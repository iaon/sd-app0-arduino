# Reproducing the `sd.bin` firmware analysis

This document describes how `sd.bin` was identified, split, verified,
disassembled, and used to reconstruct the Arduino project in this directory.
The steps are intended to be reproducible manually on a Debian or Ubuntu
Linux system.

The commands assume these files are available:

```text
/home/iaon/tmp/sd.bin
/home/iaon/tmp/sd-app0.bin
```

Create a working directory and enter it:

```sh
mkdir -p /home/iaon/tmp/sd-analysis
cd /home/iaon/tmp/sd-analysis
cp /home/iaon/tmp/sd.bin .
```

## 1. Install the basic analysis tools

```sh
sudo apt update
sudo apt install \
  binutils \
  binwalk \
  coreutils \
  curl \
  file \
  git \
  p7zip-full \
  python3 \
  xz-utils
```

On distributions where `strings` is supplied by `binutils`, a separate
`strings` package may not exist and can be omitted.

The AVR tools are not useful for this firmware:

```text
avrdude
binutils-avr
avr-objdump
```

ESP32-S3 uses an Xtensa LX7 CPU, not an AVR CPU.

## 2. Record the original file metadata

Always calculate a hash before modifying or extracting anything:

```sh
stat --printf='size=%s bytes\n' sd.bin
sha256sum sd.bin
file -k sd.bin
xxd -g 1 -l 256 sd.bin
```

Observed result:

```text
size=4194304 bytes
SHA-256:
281bf03f9ef23a63cba2d976334926f78efe2236b33cf0d4111ab359e56b8759
```

The exact size is:

```text
4194304 = 0x400000 = 4 MiB
```

This strongly suggests a complete 4 MB SPI flash dump rather than an
ordinary application file.

### Why `file` reports a DOS executable

The first byte is `0xE9`. This is both:

- a valid ESP image magic byte;
- an x86 near-jump opcode used at the beginning of some DOS COM programs.

Consequently, generic `file` versions may incorrectly report:

```text
DOS executable (COM)
```

The remaining header fields, ESP32 strings, partition table, and segment
addresses show that this interpretation is false.

## 3. Extract identifying strings

```sh
strings -a -n 4 sd.bin | head -n 200
```

Useful strings include:

```text
boot.esp32s3
arduino-lib-builder
ESP32S3_DEV
e-Paper busy
Starting SD card initialization...
/images.bin
Frames:
IntervalMs:
```

Locate their file offsets:

```sh
strings -a -t x -n 5 sd.bin |
  grep -Ei \
  'esp32s3|arduino|e-paper|images\.bin|frames|interval|starting sd'
```

Important application strings start near `0x10120`, immediately after the
application image at flash offset `0x10000`.

## 4. Measure entropy and erased flash space

An erased SPI NOR flash byte normally reads as `0xFF`. Calculate the overall
entropy and byte distribution:

```sh
python3 - <<'PY'
from collections import Counter
from pathlib import Path
import math

data = Path("sd.bin").read_bytes()
counts = Counter(data)
entropy = -sum(
    (count / len(data)) * math.log2(count / len(data))
    for count in counts.values()
)

print("length:", len(data))
print("entropy:", entropy)
print("0xff:", counts[0xff], f"({counts[0xff] / len(data):.2%})")
print("0x00:", counts[0x00], f"({counts[0x00] / len(data):.2%})")
PY
```

Observed values:

```text
entropy: approximately 1.22 bits/byte
0xff: approximately 89.5%
```

The low entropy and high `0xFF` percentage indicate that most flash
partitions are empty.

## 5. Locate and decode the ESP partition table

ESP32 partition tables are normally stored at flash offset `0x8000`:

```sh
xxd -g 1 -s 0x8000 -l 512 sd.bin
```

Each normal partition entry is 32 bytes and starts with the little-endian
magic value `0x50AA`, visible in the file as:

```text
aa 50
```

The decoded table is:

| Name | Type/subtype | Offset | Size | End |
|---|---|---:|---:|---:|
| `nvs` | data/NVS | `0x009000` | `0x005000` | `0x00E000` |
| `otadata` | data/OTA | `0x00E000` | `0x002000` | `0x010000` |
| `app0` | app/OTA_0 | `0x010000` | `0x300000` | `0x310000` |
| `spiffs` | data/SPIFFS | `0x310000` | `0x0E0000` | `0x3F0000` |
| `coredump` | data/coredump | `0x3F0000` | `0x010000` | `0x400000` |

Check how much of every partition differs from erased flash:

```sh
python3 - <<'PY'
from pathlib import Path

data = Path("sd.bin").read_bytes()
regions = [
    ("nvs",      0x009000, 0x00e000),
    ("otadata",  0x00e000, 0x010000),
    ("app0",     0x010000, 0x310000),
    ("spiffs",   0x310000, 0x3f0000),
    ("coredump", 0x3f0000, 0x400000),
]

for name, start, end in regions:
    region = data[start:end]
    used = sum(byte != 0xff for byte in region)
    print(
        f"{name:8} "
        f"range=0x{start:06x}-0x{end:06x} "
        f"non_ff={used} ({used / len(region):.2%})"
    )
PY
```

Observed state:

```text
nvs       empty
otadata   contains only OTA selection metadata
app0      contains the application
spiffs    empty
coredump  empty
```

There are therefore no saved Wi-Fi credentials or user files in the NVS and
SPIFFS regions of this dump.

## 6. Inspect the bootloader and application ESP headers

The bootloader starts at `0x0`, and the application starts at `0x10000`:

```sh
xxd -g 1 -s 0x00000 -l 64 sd.bin
xxd -g 1 -s 0x10000 -l 128 sd.bin
```

Both images begin with `0xE9`, the ESP image magic.

The 24-byte ESP image header contains:

- magic byte;
- segment count;
- flash mode;
- flash size/frequency byte;
- entry address;
- target chip ID;
- minimum and maximum chip revisions;
- hash-appended flag.

Observed application header:

```text
magic:         0xE9
segments:      7
flash mode:    QIO
flash size:    4 MB
entry address: 0x40375F34
chip ID:       9, ESP32-S3
SHA appended:  yes
```

## 7. Parse all ESP image segments

Use the following parser:

```sh
python3 - <<'PY'
from pathlib import Path
import hashlib
import struct

flash = Path("sd.bin").read_bytes()

for label, base in (("bootloader", 0), ("app0", 0x10000)):
    magic, segment_count, flash_mode, size_frequency, entry = \
        struct.unpack_from("<BBBBI", flash, base)
    chip_id = struct.unpack_from("<H", flash, base + 12)[0]
    hash_appended = flash[base + 23]

    print(
        f"[{label}] magic=0x{magic:02x} "
        f"segments={segment_count} "
        f"flash_mode={flash_mode} "
        f"size_frequency=0x{size_frequency:02x} "
        f"entry=0x{entry:08x} "
        f"chip_id={chip_id} "
        f"hash_appended={hash_appended}"
    )

    position = base + 24
    checksum = 0xEF

    for index in range(segment_count):
        load_address, length = struct.unpack_from("<II", flash, position)
        position += 8
        segment_data = flash[position:position + length]

        for byte in segment_data:
            checksum ^= byte

        print(
            f"  segment {index}: "
            f"load=0x{load_address:08x} "
            f"length=0x{length:x} "
            f"file=0x{position:x}-0x{position + length:x}"
        )
        position += length

    # The XOR checksum is the byte immediately before the next
    # 16-byte-aligned boundary. Padding before it is zero.
    digest_position = (position + 16) & ~15
    checksum_position = digest_position - 1
    stored_checksum = flash[checksum_position]

    print(
        f"  checksum=0x{stored_checksum:02x} "
        f"calculated=0x{checksum:02x} "
        f"valid={stored_checksum == checksum}"
    )

    if hash_appended:
        stored_hash = flash[digest_position:digest_position + 32]
        calculated_hash = hashlib.sha256(
            flash[base:digest_position]
        ).digest()
        print("  SHA-256 valid:", stored_hash == calculated_hash)
        image_end = digest_position + 32
    else:
        image_end = digest_position

    print(
        f"  image size=0x{image_end - base:x} "
        f"({image_end - base} bytes)"
    )
PY
```

Application segments:

| Segment | File data offset in `sd.bin` | Load address | Length |
|---:|---:|---:|---:|
| 0 | `0x010020` | `0x3C040020` | `0x168BC` |
| 1 | `0x0268E4` | `0x3FC97300` | `0x036C8` |
| 2 | `0x029FB4` | `0x40374000` | `0x06064` |
| 3 | `0x030020` | `0x42000020` | `0x3AB40` |
| 4 | `0x06AB68` | `0x4037A064` | `0x0D250` |
| 5 | `0x077DC0` | `0x50000200` | `0x00004` |
| 6 | `0x077DCC` | `0x600FE000` | `0x00080` |

The embedded application SHA-256 is valid. The application ends at flash
offset `0x77E70`, so its compact size is:

```text
0x77E70 - 0x10000 = 0x67E70 = 425584 bytes
```

## 8. Extract the compact application image

```sh
dd if=sd.bin \
  of=sd-app0.bin \
  bs=1 \
  skip=$((0x10000)) \
  count=$((0x67e70)) \
  status=progress
```

Verify:

```sh
stat --printf='%n: %s bytes\n' sd-app0.bin
sha256sum sd-app0.bin
xxd -g 1 -l 32 sd-app0.bin
```

Expected hash:

```text
af2f5f9d3372447c93d5afc389dc8301a4cf486609cf9eb0bdc1c1981a5c0a65
```

This application image is intended to be flashed at address `0x10000`.

## 9. Read the ESP application descriptor

The first data segment begins at application offset `0x20`. Its beginning
contains `esp_app_desc_t`.

```sh
python3 - <<'PY'
from pathlib import Path
import struct

data = Path("sd-app0.bin").read_bytes()
offset = 0x20

magic = struct.unpack_from("<I", data, offset)[0]
secure_version = struct.unpack_from("<I", data, offset + 4)[0]

def text(start, size):
    return data[start:start + size].split(b"\0", 1)[0].decode(
        "ascii", "replace"
    )

print(f"magic: 0x{magic:08x}")
print("secure version:", secure_version)
print("version:", text(offset + 16, 32))
print("project:", text(offset + 48, 32))
print("time:", text(offset + 80, 16))
print("date:", text(offset + 96, 16))
print("ESP-IDF:", text(offset + 112, 32))
print("ELF SHA-256:", data[offset + 144:offset + 176].hex())
PY
```

Observed metadata:

```text
magic:   0xABCD5432
version: afa5cdd
project: arduino-lib-builder
time:    12:15:52
date:    Jul 22 2025
ESP-IDF: v5.5-1-gb66b5448e0
```

Additional strings reveal:

```text
Arduino version: 3.3.0
board: ESP32S3_DEV
flash: 4 MB QIO
CPU: 240 MHz
PSRAM: OPI
partition scheme: huge_app
```

## 10. Run Binwalk and reject false positives

```sh
binwalk sd.bin
binwalk -E -N sd.bin
```

Binwalk 2.3.3 reports an ELF signature near `0x31470`:

```text
201840  0x31470  ELF, 32-bit LSB processor-specific
```

Inspect it:

```sh
xxd -g 1 -s 0x31440 -l 96 sd.bin
```

The bytes `7F 45 4C 46` occur inside a table of 32-bit addresses and are not
followed by a coherent ELF header. This is a signature collision, not an
embedded ELF file.

Do not automatically trust every Binwalk result. Confirm the surrounding
structure and whether offsets, headers, and section tables are coherent.

## 11. Install an Xtensa ESP32 disassembler

The system `objdump` and `avr-objdump` normally do not support Xtensa.
Download Espressif's official unified Xtensa toolchain:

```sh
mkdir -p tools/xtensa-esp

curl -fL \
  https://github.com/espressif/crosstool-NG/releases/download/esp-14.2.0_20241119/xtensa-esp-elf-14.2.0_20241119-x86_64-linux-gnu.tar.xz \
  -o tools/xtensa-esp.tar.xz

tar -xJf tools/xtensa-esp.tar.xz \
  -C tools/xtensa-esp \
  --strip-components=1
```

Verify architecture support:

```sh
tools/xtensa-esp/bin/xtensa-esp-elf-objdump -i |
  grep -Ei 'xtensa|elf32'
```

Expected target:

```text
elf32-xtensa-le
```

Radare2 5.5 from the tested Ubuntu repository did not contain an Xtensa
disassembly plugin, so it was not used for the final analysis.

## 12. Extract each application segment

Offsets below are relative to `sd-app0.bin`:

```sh
mkdir -p segments

dd if=sd-app0.bin of=segments/drom.bin \
  bs=1 skip=$((0x00020)) count=$((0x168bc)) status=none

dd if=sd-app0.bin of=segments/dram.bin \
  bs=1 skip=$((0x168e4)) count=$((0x036c8)) status=none

dd if=sd-app0.bin of=segments/iram.bin \
  bs=1 skip=$((0x19fb4)) count=$((0x06064)) status=none

dd if=sd-app0.bin of=segments/irom.bin \
  bs=1 skip=$((0x20020)) count=$((0x3ab40)) status=none

dd if=sd-app0.bin of=segments/iram2.bin \
  bs=1 skip=$((0x5ab68)) count=$((0x0d250)) status=none

dd if=sd-app0.bin of=segments/rtc.bin \
  bs=1 skip=$((0x67dc0)) count=$((0x00004)) status=none

dd if=sd-app0.bin of=segments/peripheral.bin \
  bs=1 skip=$((0x67dcc)) count=$((0x00080)) status=none
```

## 13. Disassemble executable segments at their load addresses

```sh
OBJDUMP=tools/xtensa-esp/bin/xtensa-esp-elf-objdump

"$OBJDUMP" -D -b binary -m xtensa \
  --adjust-vma=0x40374000 \
  segments/iram.bin > segments/iram.asm

"$OBJDUMP" -D -b binary -m xtensa \
  --adjust-vma=0x42000020 \
  segments/irom.bin > segments/irom.asm

"$OBJDUMP" -D -b binary -m xtensa \
  --adjust-vma=0x4037A064 \
  segments/iram2.bin > segments/iram2.asm
```

Disassemble the image entry point:

```sh
"$OBJDUMP" -D -b binary -m xtensa \
  --adjust-vma=0x40374000 \
  --start-address=0x40375EF0 \
  --stop-address=0x40375FC0 \
  segments/iram.bin
```

The entry instruction is at:

```text
0x40375F34
```

## 14. Map strings from file offsets to runtime addresses

The first DROM segment:

```text
file data begins: 0x10020 in sd.bin
runtime address:  0x3C040020
```

For a string found inside this segment:

```text
runtime_address =
    0x3C040020 + (string_file_offset - 0x10020)
```

Example:

```text
"/images.bin" file offset: 0x102B1
runtime address:
0x3C040020 + (0x102B1 - 0x10020) = 0x3C0402B1
```

Search for references in the disassembly:

```sh
grep -n -C 5 '0x3c0402b1' segments/irom.asm
```

The relevant code contains:

```text
l32r ..., (... 0x3c0402b1)
```

This is a literal-pool load of the `/images.bin` pointer.

Other useful runtime string addresses:

| String | Address |
|---|---:|
| `e-Paper busy` | `0x3C040120` |
| `Write DRF` | `0x3C040152` |
| `Write POF` | `0x3C04015E` |
| `Display Done!!` | `0x3C04016A` |
| `Starting SPI initialization...` | `0x3C04017B` |
| `Starting SD card initialization...` | `0x3C0401B7` |
| `/images.bin` | `0x3C0402B1` |
| `invalid images.bin header` | `0x3C0402D4` |
| `Frames:` | `0x3C0402EE` |
| `IntervalMs:` | `0x3C0402F7` |
| `Failed to read frame:` | `0x3C040304` |

## 15. Locate the main application functions

Following the string references and their callers identifies:

| Address | Reconstructed purpose |
|---:|---|
| `0x4200237C` | Configure e-Paper GPIO |
| `0x42002408` | Bit-banged byte transfer |
| `0x42002454` | Set both controller chip-select pins |
| `0x42002480` | Wait for BUSY to become inactive |
| `0x42002514` | Initialize the Spectra 6 controllers |
| `0x420026B8` | Transfer one frame and refresh |
| `0x42002724` | Parse the `images.bin` header |
| `0x420027C4` | Seek to and read one frame |
| `0x42002854` | Initialize SPI and SD |
| `0x420029CC` | Arduino `setup()` |
| `0x42002B1C` | Arduino `loop()` |

Disassemble an individual routine with:

```sh
"$OBJDUMP" -D -b binary -m xtensa \
  --adjust-vma=0x42000020 \
  --start-address=0x42002724 \
  --stop-address=0x420027C4 \
  segments/irom.bin
```

When a function boundary begins one or two bytes after apparent invalid
instructions, restart `objdump` exactly at the suspected address. Xtensa uses
variable-length instructions, so beginning at the wrong byte causes the rest
of the output to be decoded incorrectly.

## 16. Recover the `images.bin` header

The function at `0x42002724` reads exactly 18 bytes and checks these first
seven values:

```text
0x45  E
0x50  P
0x44  D
0x36  6
0x42  B
0x49  I
0x4E  N
```

It then reads:

```text
offset  8: uint16_t width
offset 10: uint16_t height
offset 12: uint16_t frames
offset 14: uint32_t intervalMs
```

Recovered packed structure:

```cpp
#pragma pack(push, 1)
struct ImageHeader {
    char magic[8];
    uint16_t width;
    uint16_t height;
    uint16_t frames;
    uint32_t intervalMs;
};
#pragma pack(pop)
```

Only the first seven magic bytes are validated. Width and height are parsed
but not used to calculate the frame size.

## 17. Recover the frame size and file offset calculation

The frame-reading function loads:

```text
0x753 << 9
```

Calculate it:

```sh
python3 - <<'PY'
print(hex(0x753 << 9), 0x753 << 9)
PY
```

Result:

```text
0xEA600 = 960000 bytes
```

The seek offset is:

```text
18 + frame_index * 960000
```

For a 1200x1600 panel:

```text
1200 * 1600 / 2 = 960000
```

Therefore, every byte stores two four-bit color indices.

## 18. Recover the dual-controller transfer layout

The display routine uses:

```text
row size:         600 bytes
controller half:  300 bytes
height:           1600 rows
```

For every row:

- bytes `0..299` are sent to the master controller;
- bytes `300..599` are sent to the slave controller.

Both controllers receive command `0x10` before their pixel data.

After data transfer, the firmware sends:

```text
0x04  power on
0x12  display refresh
0x02  power off
```

The panel busy signal is active low.

## 19. Recover the e-Paper GPIO mapping

The GPIO configuration function calls Arduino `pinMode()` with constant
arguments. The byte-transfer function calls `digitalWrite()` for every
output bit.

Recovered mapping:

| Signal | GPIO | Evidence |
|---|---:|---|
| BUSY | 7 | configured as input, read by wait routine |
| RESET | 6 | toggled five times during reset |
| DC | 2 | configured as output |
| POWER | 45 | configured as output and driven high |
| SCK | 9 | toggled high/low for every transmitted bit |
| MOSI | 41 | set from the top bit of every transmitted byte |
| CS master | 18 | selected for the first 300 bytes of each row |
| CS slave | 17 | selected for the second 300 bytes of each row |

This mapping differs from some public Waveshare examples, which is why it
must be recovered from the supplied binary rather than copied blindly.

## 20. Recover the SD interface

ESP32's windowed Xtensa ABI maps arguments around a `call8` instruction as:

```text
caller a10 -> callee a2
caller a11 -> callee a3
caller a12 -> callee a4
caller a13 -> callee a5
caller a14 -> callee a6
caller a15 -> callee a7
```

The SD initialization routine passes these constants to `SPI.begin()`:

```text
SCK  = 8
MISO = 5
MOSI = 3
SS   = -1
```

It then calls `SD.begin()` with:

```text
CS pin:       15
SPI object:   the separately initialized SPIClass
frequency:    4000000
mount point:  "/sd"
max files:    5
format flag:  false
```

This reconstructs:

```cpp
sdSpi.begin(8, 5, 3, -1);
SD.begin(15, sdSpi, 4000000, "/sd", 5, false);
```

## 21. Recover the display initialization tables

Dump the DROM area containing short constant arrays:

```sh
xxd -g 1 -s 0xb2a0 -l 80 segments/drom.bin
```

The important command payloads are:

```text
AN_TM   C0 1E 1E CE CE CE 15 15 55
CMD66   49 55 13 5D 05 10
PSR     DF 69
CDI     F7
TCON    03 03
AGID    10
PWS     22
CCSET   01
TRES    04 B0 03 20
PWR     0F 00 28 2C 28 38
EN_BUF  07
BTST_P  E8 28
BTST_N  E8 28
```

Command order recovered from `0x42002514`:

```text
74 F0 00 50 60 86 E3 E0 61 01 B6 06 B7 05 B0 B1
```

The analog timing bytes in the supplied binary differ slightly from some
public Waveshare driver revisions. The reconstructed project uses the bytes
from the binary.

## 22. Compare against the official Waveshare source

Clone the official board repository:

```sh
git clone --depth=1 \
  https://github.com/waveshareteam/ESP32-S3-ePaper-13.3E6.git \
  waveshare-reference
```

Relevant files:

```text
waveshare-reference/example/Arduino-3.2.0/examples/04_SD_Test/
  DEV_Config.cpp
  DEV_Config.h
  EPD_13in3e.cpp
  EPD_13in3e.h
```

Compare:

```sh
grep -R -n \
  'AN_TM\|CMD66\|EPD_13IN3E_Display\|EPD_13IN3E_Init' \
  waveshare-reference/example/Arduino-3.2.0/examples/04_SD_Test
```

The command sequence, panel dimensions, two-controller row split, and most
constant tables match. This confirms that the target hardware is the
Waveshare-compatible 13.3-inch 1200x1600 Spectra 6 panel.

Do not copy the public example's GPIO mapping over the recovered mapping:
the supplied firmware was built for different pin assignments.

## 23. Reconstruct the application logic

The high-level flow recovered from `setup()` is:

```text
Serial.begin(115200)
initialize e-Paper GPIO
initialize SD SPI
mount SD
initialize PSRAM
allocate 960000-byte frame buffer
open /images.bin
parse header
store frame count
use 1000 ms when intervalMs is zero
close file
```

The recovered `loop()` is:

```text
open /images.bin
parse header
currentFrame %= max(frames, 1)
seek to 18 + currentFrame * 960000
read exactly 960000 bytes
initialize the e-Paper controller
send frame to both display controllers
refresh and power off panel
delay 500 ms
close file
advance currentFrame
delay intervalMs
```

The current frame index is stored in the segment loaded at `0x50000200`,
which is RTC memory on ESP32-S3. The reconstruction therefore uses:

```cpp
RTC_DATA_ATTR uint32_t currentFrame;
```

## 24. Validate the reconstructed project

The source reconstructed from the analysis is in:

```text
/home/iaon/tmp/sd-app0-arduino
```

Build it according to `README.md`, then inspect the application header:

```sh
cd /home/iaon/tmp/sd-app0-arduino
xxd -p -l 4 sd-app0-rebuilt.bin
```

Expected final byte:

```text
2f
```

This encodes the original 4 MB flash/QIO configuration. An earlier build
using the unmodified generic PlatformIO board definition produced `3f`,
which advertised an 8 MB flash and was therefore rejected as incompatible
with the source dump.

Confirm required strings and tables:

```sh
strings -a sd-app0-rebuilt.bin |
  grep -E \
  'EPD6BIN|images.bin|Starting SPI|Starting SD|Frames:|IntervalMs:'

python3 - <<'PY'
from pathlib import Path

data = Path("sd-app0-rebuilt.bin").read_bytes()
patterns = {
    "analog timing": bytes.fromhex(
        "c0 1e 1e ce ce ce 15 15 55"
    ),
    "command 66": bytes.fromhex("49 55 13 5d 05 10"),
    "power": bytes.fromhex("0f 00 28 2c 28 38"),
}

for name, pattern in patterns.items():
    offset = data.find(pattern)
    print(name, hex(offset) if offset >= 0 else "missing")
PY
```

## 25. What cannot be recovered exactly

Static analysis can recover behavior, constants, control flow, hardware
mapping, and file formats, but not every original source-level detail.

Normally lost information includes:

- original local variable names;
- comments;
- source file organization;
- typedef and class names not preserved in symbols;
- exact formatting;
- macros after preprocessing;
- inlined function boundaries;
- compiler-generated temporary variables.

A byte-for-byte rebuild would additionally require:

- the original complete source tree;
- the exact compiler build;
- linker scripts;
- all compilation flags;
- library revisions;
- build timestamps and metadata.

The goal of this analysis was therefore a behaviorally compatible,
maintainable Arduino implementation rather than a byte-identical binary.

## 26. Derivation of offsets, addresses, and constants

This section collects the arithmetic behind the important values used
throughout the analysis. Nothing here depends on guessing an offset and then
declaring it correct.

### 26.1 Full flash size

`stat` reports:

```text
4194304 bytes
```

Convert to hexadecimal:

```sh
python3 - <<'PY'
print(hex(4194304))
print(4194304 // (1024 * 1024), "MiB")
PY
```

Result:

```text
0x400000
4 MiB
```

That is why all partition ranges are checked against an exclusive flash end
of `0x400000`.

### 26.2 Partition entry field layout

Each ESP partition-table entry is 32 bytes:

| Entry offset | Size | Meaning |
|---:|---:|---|
| `+0x00` | 2 | magic, `0x50AA` |
| `+0x02` | 1 | type |
| `+0x03` | 1 | subtype |
| `+0x04` | 4 | partition flash offset, little-endian |
| `+0x08` | 4 | partition size, little-endian |
| `+0x0C` | 16 | zero-terminated label |
| `+0x1C` | 4 | flags |

The `app0` entry begins at table address `0x8040`:

```sh
xxd -g 1 -s 0x8040 -l 32 sd.bin
```

Bytes:

```text
aa 50 00 10 00 00 01 00 00 00 30 00 61 70 70 30 ...
```

Decode the fields:

```text
aa 50       -> magic 0x50AA
00          -> application type
10          -> OTA slot 0 subtype
00 00 01 00 -> little-endian 0x00010000
00 00 30 00 -> little-endian 0x00300000
61 70 70 30 -> "app0"
```

Therefore:

```text
app0 start = 0x10000
app0 size  = 0x300000
app0 end   = 0x10000 + 0x300000 = 0x310000
```

The same addition gives every partition end:

```text
nvs:      0x009000 + 0x005000 = 0x00E000
otadata:  0x00E000 + 0x002000 = 0x010000
spiffs:   0x310000 + 0x0E0000 = 0x3F0000
coredump: 0x3F0000 + 0x010000 = 0x400000
```

### 26.3 ESP image header bytes

The first four application bytes are:

```sh
xxd -g 1 -s 0x10000 -l 4 sd.bin
```

```text
e9 07 02 2f
```

Decode:

```text
e9 -> ESP image magic
07 -> seven segments
02 -> QIO flash mode
2f -> flash-size/frequency byte
```

For `0x2F`:

```text
high nibble 0x2 -> 4 MB flash size code
low nibble  0xF -> 80 MHz flash-frequency code
```

The entry address is the next four-byte little-endian field:

```text
34 5f 37 40 -> 0x40375F34
```

### 26.4 First segment offset

An ESP image begins with a 24-byte header:

```text
application base = 0x10000
header size      = 0x18
```

The first eight-byte segment header therefore begins at:

```text
0x10000 + 0x18 = 0x10018
```

Its data begins after the segment header:

```text
0x10018 + 0x08 = 0x10020
```

The first segment header contains:

```text
load address = 0x3C040020
length       = 0x168BC
```

Its data ends at:

```text
0x10020 + 0x168BC = 0x268DC
```

That end is also the start of the second segment header.

### 26.5 Deriving every following segment offset

For each segment:

```text
data_start[n]   = header_start[n] + 8
header_start[n+1] = data_start[n] + length[n]
```

Applying the formula:

```text
segment 0:
  header = 0x10018
  data   = 0x10020
  end    = 0x10020 + 0x168BC = 0x268DC

segment 1:
  header = 0x268DC
  data   = 0x268E4
  end    = 0x268E4 + 0x036C8 = 0x29FAC

segment 2:
  header = 0x29FAC
  data   = 0x29FB4
  end    = 0x29FB4 + 0x06064 = 0x30018

segment 3:
  header = 0x30018
  data   = 0x30020
  end    = 0x30020 + 0x3AB40 = 0x6AB60

segment 4:
  header = 0x6AB60
  data   = 0x6AB68
  end    = 0x6AB68 + 0x0D250 = 0x77DB8

segment 5:
  header = 0x77DB8
  data   = 0x77DC0
  end    = 0x77DC0 + 0x00004 = 0x77DC4

segment 6:
  header = 0x77DC4
  data   = 0x77DCC
  end    = 0x77DCC + 0x00080 = 0x77E4C
```

These values come directly from the segment lengths; they are not found by
searching for the next `0xE9` byte.

### 26.6 Checksum and SHA positions

After the final segment, the checksum is placed immediately before the next
16-byte boundary.

Final segment end:

```text
position = 0x77E4C
```

Calculate the next boundary:

```text
digest_position =
    align_up(position + 1, 16)
  = align_up(0x77E4D, 16)
  = 0x77E50
```

The checksum byte is:

```text
checksum_position = 0x77E50 - 1 = 0x77E4F
```

The bytes from `0x77E4C` through `0x77E4E` are alignment padding.

The appended SHA-256 occupies 32 bytes:

```text
hash start = 0x77E50
hash end   = 0x77E50 + 0x20 = 0x77E70
```

Therefore, the application image ends at `0x77E70`.

### 26.7 Compact application size and `dd` count

The compact file starts at the partition start and ends after the SHA:

```text
start = 0x10000
end   = 0x77E70
```

Subtract:

```text
0x77E70 - 0x10000 = 0x67E70
```

Convert to decimal:

```sh
python3 - <<'PY'
print(0x67e70)
PY
```

```text
425584
```

That is the source of:

```sh
count=$((0x67e70))
```

in the `dd` command.

### 26.8 Segment offsets inside `sd-app0.bin`

`sd-app0.bin` begins at original flash offset `0x10000`. Convert any full
flash offset to an application-relative offset by subtracting `0x10000`.

Examples:

```text
DROM:
0x10020 - 0x10000 = 0x00020

DRAM:
0x268E4 - 0x10000 = 0x168E4

IRAM:
0x29FB4 - 0x10000 = 0x19FB4

IROM:
0x30020 - 0x10000 = 0x20020

IRAM2:
0x6AB68 - 0x10000 = 0x5AB68
```

These subtractions produce the `skip=` values used when splitting
`sd-app0.bin`.

### 26.9 Runtime address of a byte inside a segment

Every segment header supplies:

```text
file data start
runtime load address
length
```

For an item at a file offset within a segment:

```text
runtime =
    segment_load_address
  + (item_file_offset - segment_file_data_start)
```

For `/images.bin`:

```text
segment load address    = 0x3C040020
segment file data start = 0x00010020
string file offset      = 0x000102B1
```

Therefore:

```text
within-segment offset = 0x102B1 - 0x10020 = 0x291
runtime address       = 0x3C040020 + 0x291
                      = 0x3C0402B1
```

### 26.10 DROM file offset of a runtime constant

The inverse calculation is:

```text
segment_file_offset =
    runtime_address - segment_load_address
```

For the command-table area:

```text
runtime address  = 0x3C04B2C0
DROM load base   = 0x3C040020
```

Subtract:

```text
0x3C04B2C0 - 0x3C040020 = 0xB2A0
```

That is why the command-table dump uses:

```sh
xxd -s 0xb2a0 segments/drom.bin
```

### 26.11 Finding pointer literals

Xtensa commonly loads 32-bit constants with `L32R` from nearby literal
pools. To find references to a runtime address, first encode the address as a
little-endian 32-bit value.

For `/images.bin`:

```text
address:       0x3C0402B1
little endian: B1 02 04 3C
```

Search every extracted segment:

```sh
python3 - <<'PY'
from pathlib import Path
import struct

target = 0x3c0402b1
needle = struct.pack("<I", target)

regions = [
    ("drom", Path("segments/drom.bin"), 0x3c040020),
    ("dram", Path("segments/dram.bin"), 0x3fc97300),
    ("iram", Path("segments/iram.bin"), 0x40374000),
    ("irom", Path("segments/irom.bin"), 0x42000020),
]

for name, path, load_base in regions:
    data = path.read_bytes()
    start = 0
    while True:
        offset = data.find(needle, start)
        if offset < 0:
            break
        print(name, hex(load_base + offset))
        start = offset + 1
PY
```

The address appears in the IROM literal pool. `objdump` resolves the
corresponding `L32R` instructions and prints the loaded value in
parentheses, allowing references to be followed without manually decoding
the signed PC-relative immediate.

### 26.12 Deriving function starts

Function addresses were established from a combination of:

1. a string reference loaded by `L32R`;
2. the basic block that uses that string;
3. callers of that block;
4. the nearest valid Xtensa `ENTRY` instruction at the function start;
5. a terminating `RETW` or tail branch;
6. coherent re-disassembly when `objdump` is started exactly at that byte.

For example:

```text
"/images.bin" -> referenced around 0x42002A46
```

Following the enclosing function backward reaches:

```text
0x420029CC: entry a1, 96
```

This function performs one-time initialization, allocation, and header
printing, so it is identified as Arduino `setup()`.

The next major function starts at:

```text
0x42002B1C: entry a1, 112
```

It repeatedly opens the file, reads one frame, refreshes the panel, advances
the frame index, and delays, so it is identified as Arduino `loop()`.

### 26.13 Header-field offsets

The parser requests exactly 18 bytes into a stack buffer. Its byte checks use
stack offsets `26` through `32`, corresponding to buffer offsets `0` through
`6`.

After the magic checks, it executes little-endian 16-bit loads at:

```text
buffer + 8
buffer + 10
buffer + 12
```

and combines two 16-bit values from:

```text
buffer + 14
buffer + 16
```

into one 32-bit value:

```text
low16 | (high16 << 16)
```

This directly yields:

```text
+8  uint16_t width
+10 uint16_t height
+12 uint16_t frames
+14 uint32_t intervalMs
```

The total is:

```text
8 + 2 + 2 + 2 + 4 = 18 bytes
```

### 26.14 Frame-size constant

The instruction sequence builds:

```text
0x753 << 9
```

In decimal:

```text
0x753 = 1875
2^9   = 512
1875 * 512 = 960000
```

The panel dimensions independently confirm it:

```text
1200 pixels * 1600 rows = 1920000 pixels
two pixels per byte      = 960000 bytes
```

These two independent derivations agree.

### 26.15 Per-controller row size

One packed row contains:

```text
1200 pixels / 2 pixels per byte = 600 bytes
```

The panel uses two controllers:

```text
600 / 2 = 300 bytes per controller per row
```

The transfer loop increments its source pointer by 600 and writes 300 bytes
per iteration. The second loop begins at `frame + 300`, proving that it sends
the second half of every row.

### 26.16 GPIO-number derivation

The bit-transfer routine performs:

```text
digitalWrite(41, current_data_bit)
digitalWrite(9, HIGH)
digitalWrite(9, LOW)
```

This identifies:

```text
MOSI = 41
SCK  = 9
```

The chip-select helper performs two writes with the same logical value:

```text
digitalWrite(18, value)
digitalWrite(17, value)
```

This identifies the two controller CS pins.

The reset routine repeatedly writes GPIO 6. The busy routine repeatedly reads
GPIO 7 until it becomes high. The initialization function drives GPIO 45
high and leaves it high as the panel power-enable pin.

### 26.17 SD clock frequency

The SD initialization routine constructs:

```text
0x7D0 = 2000
```

and multiplies it by itself:

```text
2000 * 2000 = 4000000
```

That product is passed as the SD clock-frequency argument, producing:

```text
4000000 Hz = 4 MHz
```

### 26.18 Command-table lengths

Each initialization call loads:

- a command byte;
- a pointer to a DROM constant;
- an immediate length.

For example, the analog timing call uses:

```text
command = 0x74
pointer = 0x3C04B2D3
length  = 9
```

Dumping nine bytes beginning at that address yields:

```text
C0 1E 1E CE CE CE 15 15 55
```

The next constant begins at:

```text
0x3C04B2D3 + 9 = 0x3C04B2DC
```

which matches the one-byte `PWS` payload. Repeating this pointer-plus-length
calculation verifies that the short tables are adjacent and do not overlap
incorrectly.

### 26.19 Why the reconstructed application is flashed at `0x10000`

The address is not selected by convention alone. It is supported by three
independent observations:

1. the partition table declares `app0` at `0x10000`;
2. a valid ESP image header begins at `sd.bin + 0x10000`;
3. the bootloader's partition-selection logic references that table.

Therefore:

```sh
write_flash 0x10000 sd-app0-rebuilt.bin
```

is correct only for this partition layout.

### 26.20 Why the complete image is flashed at `0x0`

`sd-factory-rebuilt.bin` contains:

```text
0x000000 bootloader
0x008000 partition table
0x00E000 boot_app0/OTA metadata helper
0x010000 application
```

Its offsets are already embedded into one merged file. Supplying an
additional nonzero base would shift every component and make the flash
layout invalid. The correct command is therefore:

```sh
write_flash 0x0 sd-factory-rebuilt.bin
```
