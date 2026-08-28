"""Content-addressed shader compile cache (UE5-style DDC) for the PT cook.

Two key levels, deliberately kept separate:

  L1 - runtime lookup key. SHA-256 of the normalized DXC command line, produced by
       `ShaderCompilerUtils::buildDxcCommand()` in C++ and mirrored by
       `precompile_pt_shader_bins.build_hash_command()`. It is computable without
       shader sources, which is what lets a load-only build resolve a bin path with
       no compiler and no HLSL tree deployed. L1 addresses `ShaderBin/<api>/xx/yy.bin`.

  L2 - compile identity. SHA-256 of the *preprocessed* source plus every compile
       flag that preprocessing does not already capture. Two jobs whose macros
       differ but that preprocess to the same text share an L2 entry, so the
       expensive DXC invocation happens once. Because the key is pure content, it
       is immune to file mtimes: touching a comment, or a branch switch that
       rewrites timestamps, no longer invalidates anything.

The cook resolves every job's L2 key, compiles each distinct L2 entry at most once,
then materializes the blob at each L1 path (hard link when possible).

Preprocessing all jobs is cheap enough to do unconditionally: ~40 ms per job, a few
seconds for the whole matrix, versus ~4 s for a full compile.
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path

L2_KEY_NAMESPACE = "caustica-shader-ddc-v1"

# `#line <n> "<path>"` markers in DXC preprocessor output enumerate the exact
# include closure, so the dependency graph comes for free with the L2 key.
_LINE_DIRECTIVE = re.compile(rb'^#line\s+\d+\s+"(.+)"', re.MULTILINE)


def _sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def normalize_include_path(raw: str) -> str:
    """Collapse the mixed separators and `..` segments DXC emits in `#line`."""
    unescaped = raw.replace("\\\\", "\\").replace("\\", "/")
    while unescaped.startswith("./"):
        unescaped = unescaped[2:]
    return os.path.normpath(unescaped).replace("\\", "/")


@dataclass(frozen=True)
class PreprocessResult:
    source_sha: str
    includes: tuple[str, ...]


MANIFEST_MAGIC = "CAUSDEP1"


class DependencyManifest:
    """Records what each cooked bin was built from, for exact staleness checks.

    The runtime cannot ask "did this shader change?" without knowing which files
    the shader actually includes. Scanning the whole shader tree's timestamps —
    what the engine did before — answers a much coarser question and reports every
    RT shader as stale whenever any unrelated shader is touched.

    The cook already learns the exact include closure while preprocessing, so it
    writes it down here. Deliberately, the closure lives only on this side: C++ just
    hashes the listed files and combines them, so there is no `#include` parser to
    keep byte-compatible across two languages.

    Line format, one record per line:
        D <source-relpath> <closure-file-relpath>
        B <l1-hash> <source-relpath> <fingerprint>

    All paths are relative to the shader root, not the repository, so the same
    manifest resolves whether the runtime reads shaders from the source tree or from
    a deployed ShaderDev copy. It also makes `<source-relpath>` identical to the
    engine's `shaderSrcFileName`, which is what the runtime looks records up by.

    `fingerprint` is stored per bin rather than per source on purpose: a partial
    recook must not make bins built against older sources look current.
    """

    def __init__(self) -> None:
        self.closures: dict[str, set[str]] = {}
        self.bins: dict[str, tuple[str, str]] = {}

    def add_closure(self, source: str, includes: tuple[str, ...] | set[str]) -> None:
        # Union across variants keeps the closure macro-independent: a superset can
        # only cause an extra recompile, never a missed one.
        self.closures.setdefault(source, set()).update(includes)

    def fingerprint(self, source: str, root: Path) -> str:
        parts = []
        for relpath in sorted(self.closures.get(source, ())):
            absolute = root / relpath
            try:
                parts.append(f"{relpath}:{_sha256_hex(absolute.read_bytes())}")
            except OSError:
                # A file DXC reported but that we cannot read now: fold the absence
                # into the fingerprint so it still registers as a change.
                parts.append(f"{relpath}:<missing>")
        return _sha256_hex("\n".join(parts).encode("utf-8"))

    def add_bin(self, l1_hash: str, source: str, fingerprint: str) -> None:
        self.bins[l1_hash] = (source, fingerprint)

    def write(self, path: Path) -> None:
        lines = [MANIFEST_MAGIC]
        for source in sorted(self.closures):
            for relpath in sorted(self.closures[source]):
                lines.append(f"D {source} {relpath}")
        for l1_hash in sorted(self.bins):
            source, fingerprint = self.bins[l1_hash]
            lines.append(f"B {l1_hash} {source} {fingerprint}")
        path.parent.mkdir(parents=True, exist_ok=True)
        staging = path.with_name(f"{path.name}.tmp{os.getpid()}")
        staging.write_text("\n".join(lines) + "\n", encoding="utf-8")
        os.replace(staging, path)


@lru_cache(maxsize=None)
def shader_relative(raw: str, shader_root: Path, repo_root: Path) -> str:
    """Normalize a DXC-reported include path to a path relative to the shader root.

    Memoized because the whole matrix reports the same few hundred include paths
    over and over, and `resolve()` is a syscall per component.

    An include from outside the shader tree cannot be expressed in a form the
    runtime can resolve after deployment, so it is reported rather than silently
    dropped: it would otherwise become a dependency nothing ever checks.
    """
    candidate = Path(normalize_include_path(raw))
    if not candidate.is_absolute():
        candidate = repo_root / candidate
    candidate = candidate.resolve()
    try:
        return candidate.relative_to(shader_root).as_posix()
    except ValueError:
        print(
            f"[caustica] WARNING: include '{candidate}' lives outside the shader root; "
            "edits to it will not invalidate cooked shaders."
        )
        return candidate.as_posix()


def collect_pdbs(scratch_dir: Path, pdb_root: Path) -> None:
    """Move DXC's auto-named PDBs into a shared, content-addressed PDB directory.

    Passing `-Fd <dir>/` (rather than a file) makes DXC name each PDB after the
    shader hash. That matters for two reasons: the emitted container is then
    byte-identical regardless of where the cook ran, which is what makes L2 dedup
    exact; and the name is exactly what a debugger searches for, so a single
    directory on the PDB search path serves every cooked shader.
    """
    for produced in scratch_dir.glob("*.pdb"):
        target = pdb_root / produced.name
        if not target.is_file():
            atomic_replace(produced, target)


@dataclass
class CookStats:
    jobs: int = 0
    preprocessed: int = 0
    distinct: int = 0
    compiled: int = 0
    ddc_hits: int = 0
    l1_reused: int = 0
    l1_written: int = 0
    errors: list[tuple[str, str]] = field(default_factory=list)

    @property
    def deduped(self) -> int:
        return max(0, self.preprocessed - self.distinct)


def preprocess(
    dxc: Path,
    *,
    source: Path,
    macro_args: list[str],
    include_args: list[str],
    profile_args: list[str],
) -> PreprocessResult:
    """Run DXC in preprocess-only mode and hash the result.

    Raises RuntimeError with the compiler diagnostic when preprocessing fails,
    which is how genuinely broken sources surface before any expensive compile.
    """
    with tempfile.TemporaryDirectory(prefix="caus_pp_") as tmp:
        out = Path(tmp) / "pp.hlsl"
        cmd = [str(dxc), str(source), *profile_args, *macro_args, *include_args,
               "-P", "-Fi", str(out)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0 or not out.exists():
            raise RuntimeError((result.stderr or result.stdout or "preprocess failed").strip())
        data = out.read_bytes()

    includes = sorted({
        normalize_include_path(match.group(1).decode("utf-8", "replace"))
        for match in _LINE_DIRECTIVE.finditer(data)
    })
    return PreprocessResult(source_sha=_sha256_hex(data), includes=tuple(includes))


def compute_l2_key(*, api: str, flag_signature: str, source_sha: str) -> str:
    """Compile identity: preprocessed content plus flags preprocessing cannot capture.

    Macros and include directories are deliberately absent: their entire effect is
    already baked into `source_sha`. Anything else that changes codegen (profile,
    entry point, optimization level, debug-info mode, SPIR-V layout switches) must
    be part of `flag_signature` or distinct outputs would collide.
    """
    payload = "|".join((L2_KEY_NAMESPACE, api, flag_signature, source_sha))
    return _sha256_hex(payload.encode("utf-8"))


class ShaderDdc:
    """Content-addressed store of compiled blobs, keyed by L2 key.

    Layout mirrors ShaderBin (`xx/yy.bin`) so the store can be rsynced or shared
    without any index. An entry is only published after a successful compile, so a
    crashed or killed cook never leaves a truncated blob that a later run trusts.
    """

    def __init__(self, root: Path):
        self.root = root

    def entry_path(self, l2_key: str, suffix: str = ".bin") -> Path:
        return self.root / "bin" / l2_key[:2] / (l2_key[2:] + suffix)

    def has(self, l2_key: str) -> bool:
        return self.entry_path(l2_key).is_file()

    def publish(self, l2_key: str, blob: Path) -> None:
        atomic_replace(blob, self.entry_path(l2_key))

    def materialize(self, l2_key: str, destination: Path) -> bool:
        """Place the cached blob at `destination` (an L1 path). Hard link if possible.

        Returns whether anything was written, so a no-op cook can report honestly.
        """
        source = self.entry_path(l2_key)
        if not source.is_file():
            return False
        if destination.is_file() and _same_file_or_content(source, destination):
            return False
        destination.parent.mkdir(parents=True, exist_ok=True)
        _link_or_copy(source, destination)
        return True


def atomic_replace(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    staging = target.with_name(f"{target.name}.tmp{os.getpid()}")
    shutil.copyfile(source, staging)
    os.replace(staging, target)


def _same_file_or_content(a: Path, b: Path) -> bool:
    sa, sb = a.stat(), b.stat()
    # Already the same hard link: the overwhelmingly common case on a warm tree,
    # and checking it first keeps a no-op cook from hashing the whole bin set.
    if sa.st_ino and (sa.st_ino, sa.st_dev) == (sb.st_ino, sb.st_dev):
        return True
    if sa.st_size != sb.st_size:
        return False
    return _sha256_hex(a.read_bytes()) == _sha256_hex(b.read_bytes())


def _link_or_copy(source: Path, destination: Path) -> None:
    """Hard link to keep the L1 tree free, falling back to a copy across volumes."""
    if destination.exists():
        destination.unlink()
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)
