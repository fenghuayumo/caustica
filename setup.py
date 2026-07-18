from __future__ import annotations

import os
import shutil
import sys
from types import SimpleNamespace
from pathlib import Path

from setuptools import Distribution, setup
from setuptools.command.build_py import build_py as _build_py


ROOT = Path(__file__).resolve().parent
SUPPORT_PYTHON = ROOT / "support" / "python"
sys.path.insert(0, str(SUPPORT_PYTHON))

from build_wheel import (  # noqa: E402
    BIN_DIR,
    PYTHON_PACKAGE_DIR,
    copy_runtime_files,
    directory_size,
    run_dynamic_shader_precompile,
    run_pt_shader_precompile,
)


def env_choice(name: str, default: str, choices: set[str], legacy_name: str | None = None) -> str:
    value = os.environ.get(name)
    if value is None and legacy_name is not None:
        value = os.environ.get(legacy_name)
    value = (value if value is not None else default).lower()
    if value not in choices:
        allowed = ", ".join(sorted(choices))
        raise RuntimeError(f"{name} must be one of: {allowed}")
    return value


def env_bool(name: str, default: bool = False, legacy_name: str | None = None) -> bool:
    value = os.environ.get(name)
    if value is None and legacy_name is not None:
        value = os.environ.get(legacy_name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def env_list(name: str, legacy_name: str | None = None) -> list[str]:
    value = os.environ.get(name)
    if value is None and legacy_name is not None:
        value = os.environ.get(legacy_name)
    value = value or ""
    return [item.strip() for item in value.replace(";", ",").split(",") if item.strip()]


class BinaryDistribution(Distribution):
    def has_ext_modules(self) -> bool:
        return True


class BuildPyWithRuntime(_build_py):
    def run(self) -> None:
        if not BIN_DIR.exists():
            raise FileNotFoundError(f"{BIN_DIR} does not exist. Build caustica first.")
        if not PYTHON_PACKAGE_DIR.exists():
            raise FileNotFoundError(f"{PYTHON_PACKAGE_DIR} does not exist.")

        package_dir = Path(self.build_lib) / "caustica"
        if package_dir.exists():
            shutil.rmtree(package_dir)

        super().run()

        assets = env_choice("CAUSTICA_WHEEL_ASSETS", "minimal", {"minimal", "full", "none"}, "CAUSTICA_WHEEL_ASSETS")
        dynamic_shaders = env_choice(
            "CAUSTICA_WHEEL_DYNAMIC_SHADERS",
            "bin",
            {"full", "bin", "none"},
            "CAUSTICA_WHEEL_DYNAMIC_SHADERS",
        )
        shader_api = env_choice(
            "CAUSTICA_WHEEL_SHADER_API",
            "d3d12" if os.name == "nt" else "vulkan",
            {"d3d12", "vulkan", "both"},
            "CAUSTICA_WHEEL_SHADER_API",
        )
        shader_pack = env_bool("CAUSTICA_WHEEL_SHADER_PACK", True, "CAUSTICA_WHEEL_SHADER_PACK")
        if os.name != "nt" and shader_api == "d3d12":
            raise RuntimeError(
                "CAUSTICA_WHEEL_SHADER_API=d3d12 is only valid on Windows. "
                "Use CAUSTICA_WHEEL_SHADER_API=vulkan on Linux."
            )

        if env_bool("CAUSTICA_WHEEL_PRECOMPILE_PT_SHADERS", True, "CAUSTICA_WHEEL_PRECOMPILE_PT_SHADERS"):
            if dynamic_shaders == "none":
                print("WARNING: CAUSTICA_WHEEL_PRECOMPILE_PT_SHADERS is set while dynamic shader bins are omitted.")
            pt_preset = os.environ.get(
                "CAUSTICA_WHEEL_PRECOMPILE_PT_GLOBAL_PRESET",
                "coverage",
            )
            run_pt_shader_precompile(SimpleNamespace(
                shader_api=shader_api,
                precompile_pt_force=env_bool("CAUSTICA_WHEEL_PRECOMPILE_PT_FORCE", legacy_name="CAUSTICA_WHEEL_PRECOMPILE_PT_FORCE"),
                precompile_pt_global_preset=pt_preset,
            ))
            if env_bool("CAUSTICA_WHEEL_VERIFY_PT_SHADERS", True, "CAUSTICA_WHEEL_VERIFY_PT_SHADERS"):
                from verify_pt_shader_bins import verify_apis

                if verify_apis(shader_api, pt_preset) != 0:
                    raise RuntimeError(
                        "PT shader coverage verify failed. "
                        "Run: python support/python/cook_shaders.py"
                    )

        if env_bool("CAUSTICA_WHEEL_PRECOMPILE_DYNAMIC_SHADERS", legacy_name="CAUSTICA_WHEEL_PRECOMPILE_DYNAMIC_SHADERS"):
            if dynamic_shaders == "none":
                print("WARNING: CAUSTICA_WHEEL_PRECOMPILE_DYNAMIC_SHADERS is set while dynamic shader bins are omitted.")
            run_dynamic_shader_precompile(SimpleNamespace(
                shader_api=shader_api,
                precompile_modes=os.environ.get("CAUSTICA_WHEEL_PRECOMPILE_MODES", os.environ.get("CAUSTICA_WHEEL_PRECOMPILE_MODES", "reference,realtime")),
                precompile_frames=int(os.environ.get("CAUSTICA_WHEEL_PRECOMPILE_FRAMES", os.environ.get("CAUSTICA_WHEEL_PRECOMPILE_FRAMES", "1"))),
                precompile_scene=env_list("CAUSTICA_WHEEL_PRECOMPILE_SCENES", "CAUSTICA_WHEEL_PRECOMPILE_SCENES"),
            ))

        copy_runtime_files(
            package_dir,
            dynamic_shaders=dynamic_shaders,
            shader_api=shader_api,
            assets=assets,
            shader_pack=shader_pack,
        )
        size_mib = directory_size(package_dir) / (1024 * 1024)
        print(f"Staged caustica package size: {size_mib:.1f} MiB")

    def get_outputs(self, include_bytecode: int = 1) -> list[str]:
        outputs = super().get_outputs(include_bytecode)
        package_dir = Path(self.build_lib) / "caustica"
        if package_dir.exists():
            outputs.extend(str(path) for path in package_dir.rglob("*") if path.is_file())
        return outputs


setup(
    name="caustica",
    version=os.environ.get("CAUSTICA_WHEEL_VERSION", os.environ.get("CAUSTICA_WHEEL_VERSION", "0.6.0")),
    description="Python bindings for caustica",
    long_description=(ROOT / "py_caustica.md").read_text(encoding="utf-8"),
    long_description_content_type="text/markdown",
    packages=["caustica"],
    package_dir={"caustica": "python/caustica"},
    include_package_data=True,
    license_files=["LICENSE.txt"],
    python_requires=">=3.8",
    distclass=BinaryDistribution,
    cmdclass={"build_py": BuildPyWithRuntime},
    zip_safe=False,
)
