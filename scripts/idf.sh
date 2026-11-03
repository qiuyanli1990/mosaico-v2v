#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_PATH="$project_root/.tools/esp-idf"
export IDF_TOOLS_PATH="$project_root/.tools/espressif"
export ESP_IDF_VERSION="6.2.0"
# bootstrap initializes the submodules needed by this audio-only application.
export IDF_SKIP_CHECK_SUBMODULES=1
export IDF_COMPONENT_CACHE_PATH="$project_root/.tools/component-cache"
# Conditional display dependencies trigger a fresh solve on a clean checkout.
export IDF_COMPONENT_CONSTRAINT_FILES="$project_root/scripts/component-constraints.txt"
python_bins=("$IDF_TOOLS_PATH"/python_env/idf6.2_py*_env/bin/python)
compiler_bins=("$IDF_TOOLS_PATH"/tools/riscv32-esp-elf/esp-16.1.0_20260609/riscv32-esp-elf/bin)
if [[ ! -x "${python_bins[0]}" || ! -d "${compiler_bins[0]}" ]]; then
    echo "Run python3 scripts/bootstrap.py --install-idf first." >&2
    exit 1
fi
actual_idf="$(git -C "$IDF_PATH" rev-parse HEAD)"
if [[ "$actual_idf" != 7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe ]]; then
    echo "The ESP-IDF checkout does not match Mosaico's pinned commit." >&2
    exit 1
fi
export IDF_PYTHON_ENV_PATH="$(dirname "$(dirname "${python_bins[0]}")")"
export ESP_ROM_ELF_DIR="$IDF_TOOLS_PATH/tools/esp-rom-elfs/20260528"
export PATH="$IDF_PYTHON_ENV_PATH/bin:${compiler_bins[0]}:$IDF_TOOLS_PATH/tools/esp-idf-configdep/0.2.3/esp-idf-configdep-0.2.3/bin:$PATH"
cd "$project_root"
"${python_bins[0]}" "$project_root/scripts/apply_local_config.py"
exec "${python_bins[0]}" "$IDF_PATH/tools/idf.py" "$@"

