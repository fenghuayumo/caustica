# Bundled MathLib

This directory is ordinary source in the Caustica repository, not a Git submodule.
Cloning Caustica includes MathLib without an additional download.

- Upstream: https://github.com/NVIDIA-RTX/MathLib
- Version: v7
- Commit: d0365f0256f4c081da8eb81b3cf8d28f9adf8ca3
- License: LICENSE.txt (MIT)

The upstream CMake file has been changed to use the bundled ARM compatibility
header rather than fetching sse2neon during configuration. Other MathLib source
files are unchanged.

## Bundled sse2neon

- Upstream: https://github.com/DLTcollab/sse2neon
- Commit: 60fc9391e378b58c60899791c0e9ee9cdaf43c08
- Files: sse2neon/sse2neon.h and sse2neon/LICENSE (MIT)

When updating either dependency, preserve its license and update this record.
