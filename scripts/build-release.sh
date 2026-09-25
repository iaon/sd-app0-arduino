#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pio="${PIO:-pio}"
core_dir="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
build_dir="$project_dir/.pio/build/esp32s3"

if ! command -v "$pio" >/dev/null 2>&1; then
    echo "PlatformIO not found. Set PIO=/path/to/pio or activate the venv." >&2
    exit 1
fi

pio_path="$(command -v "$pio")"
python="${PYTHON:-$(dirname "$pio_path")/python}"
if [[ ! -x "$python" ]]; then
    python=python3
fi

"$pio" run -d "$project_dir"

esptool="$core_dir/packages/tool-esptoolpy/esptool.py"
boot_app0="$(find "$core_dir/packages/framework-arduinoespressif32" \
    -name boot_app0.bin -print -quit)"

if [[ ! -f "$esptool" || -z "$boot_app0" ]]; then
    echo "Required PlatformIO packages were not found in $core_dir." >&2
    exit 1
fi

cp "$build_dir/firmware.bin" "$project_dir/sd-app0-rebuilt.bin"

"$python" "$esptool" --chip esp32s3 merge_bin \
    -o "$project_dir/sd-factory-rebuilt.bin" \
    --flash_mode qio \
    --flash_size 4MB \
    0x0 "$build_dir/bootloader.bin" \
    0x8000 "$build_dir/partitions.bin" \
    0xe000 "$boot_app0" \
    0x10000 "$build_dir/firmware.bin"

truncate -s 4194304 "$project_dir/sd-factory-rebuilt.bin"

sha256sum \
    "$project_dir/sd-app0-rebuilt.bin" \
    "$project_dir/sd-factory-rebuilt.bin"
