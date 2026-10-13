# Reconstructed ESP32-S3 Spectra 6 firmware

This project reconstructs the behavior of the supplied `sd-app0.bin` for an
ESP32-S3 board connected to a 13.3-inch, 1200x1600 Waveshare Spectra 6
e-Paper panel. It reads `/images.bin` from an SPI SD card and displays its
frames in a loop.

For a complete, command-by-command description of the binary analysis and
reverse-engineering process, see [`BINARY-ANALYSIS.md`](BINARY-ANALYSIS.md).

The project uses the Arduino framework through PlatformIO. It is not expected
to produce a byte-for-byte copy of the original binary, but it implements the
same file format, GPIO mapping, display initialization sequence, frame size,
SD interface, and display update procedure recovered from that binary.

## Compatibility and tested versions

Do not assume that every PlatformIO, Arduino core, or `esptool` version will
produce a compatible image. The committed configuration pins the PlatformIO
ESP32 platform version used for verification.

| Component | Tested version |
|---|---|
| Python | 3.12.3 |
| PlatformIO Core | 6.2.0 |
| PlatformIO `espressif32` platform | 7.1.3 |
| Arduino-ESP32 framework selected by that platform | 2.0.17 (`4.20017.260907`) |
| ESP-IDF used by the rebuilt application | 4.4.7 |
| Xtensa ESP32-S3 GCC toolchain | 8.4.0 / 2021r2-patch5 |
| `esptool.py` | 4.11.0 |

The original supplied firmware reports Arduino-ESP32 **3.3.0** and an
ESP-IDF 5.5 development revision. The reconstructed source uses only stable
Arduino APIs available in both generations, but the build documented here
was verified with the pinned PlatformIO environment above.

Changing the `platform` line in `platformio.ini`, selecting another ESP32
board, or removing the flash/PSRAM overrides can change the binary header and
may make the image unbootable on the original 4 MB device.

`avrdude` and `binutils-avr` are not used. ESP32-S3 has an Xtensa LX7 CPU,
not an AVR CPU; flashing is performed with Espressif `esptool.py`, and the
Xtensa compiler is installed automatically by PlatformIO.

## Hardware requirements

- ESP32-S3
- 4 MB QIO flash
- OPI PSRAM with at least 960000 contiguous bytes available
- Waveshare-compatible 13.3-inch 1200x1600 Spectra 6 panel and driver board
- FAT/FAT32 SD card containing `images.bin` in its root directory

### GPIO mapping recovered from the original firmware

| Signal | GPIO |
|---|---:|
| EPD SCK | 9 |
| EPD MOSI | 41 |
| EPD CS master | 18 |
| EPD CS slave | 17 |
| EPD reset | 6 |
| EPD busy | 7 |
| EPD power | 45 |
| EPD DC | 2 |
| SD SCK | 8 |
| SD MISO | 5 |
| SD MOSI | 3 |
| SD CS | 15 |

The SD bus runs at 4 MHz. The e-Paper interface is bit-banged to preserve the
timing and behavior of the recovered firmware.

## Installing the build environment

The commands below are intended for Debian or Ubuntu. A Python virtual
environment avoids the `externally-managed-environment` error used by recent
distributions.

```sh
sudo apt update
sudo apt install python3 python3-venv

cd /home/iaon/tmp/sd-app0-arduino
python3 -m venv .venv
./.venv/bin/pip install "platformio==6.2.0"
```

Check the installed version:

```sh
./.venv/bin/pio --version
```

The first build downloads the pinned Espressif platform, Arduino framework,
compiler, and `esptool`. This can take several minutes and requires an
internet connection.

## Building

Build the application:

```sh
cd /home/iaon/tmp/sd-app0-arduino
./.venv/bin/pio run
```

The direct PlatformIO application output is:

```text
.pio/build/esp32s3/firmware.bin
```

To create both distributable images, run:

```sh
chmod +x scripts/build-release.sh
PIO="$PWD/.venv/bin/pio" scripts/build-release.sh
```

This creates:

| File | Flash address | Description |
|---|---:|---|
| `sd-app0-rebuilt.bin` | `0x10000` | Application only |
| `sd-factory-rebuilt.bin` | `0x0` | Complete 4 MB flash image |

The release script merges the bootloader, partition table, OTA metadata
helper, and application, then extends the result to exactly 4194304 bytes.

## Firmware versions and releases

`VERSION` is the source of truth for the next stable firmware version and
uses `MAJOR.MINOR.PATCH` semantic versioning. Local and branch builds embed a
traceable development version such as `0.1.0-dev.a1b2c3d4e5f6`; a clean Git
tag `v0.1.0` embeds `0.1.0`.

The firmware prints its version and Git revision on the serial console during
startup. To make a release, update `VERSION`, commit the change, and push the
matching annotated Git tag:

```sh
git tag -a v0.1.0 -m "Release v0.1.0"
git push origin v0.1.0
```

The GitHub Actions workflow verifies that the tag matches `VERSION`, builds
the images, records their SHA-256 checksums, and publishes a GitHub Release.
Release assets are named `firmware-<version>.bin` and
`sd-factory-rebuilt-<version>.bin`; the latter is a complete 4 MB flash image.

### Checksums of the verified build

These checksums identify the already generated files in this directory:

```text
fa6ea780896742775ff17d131dfa66f09e389bbe9c9256b7e85f02a0b148a173  sd-app0-rebuilt.bin
fb4b5837f6c9f5d18b2e3fb8724cbabafa905ccc283983eab2a6904f98b27f6b  sd-factory-rebuilt.bin
```

A fresh build may have different checksums because timestamps and framework
metadata can be embedded in the application.

## Preparing the SD card

1. Format the card as FAT or FAT32.
2. Copy `images.bin` to the root of the card.
3. Insert the card before powering or resetting the ESP32-S3.

The expected path is:

```text
/images.bin
```

### `images.bin` structure

All integer values are little-endian. The file starts with this packed
18-byte header:

```cpp
struct ImageHeader {
    char magic[8];
    uint16_t width;
    uint16_t height;
    uint16_t frames;
    uint32_t intervalMs;
};
```

Requirements:

- the first seven bytes of `magic` must be `EPD6BIN`;
- `width` should be `1200`;
- `height` should be `1600`;
- every frame must contain exactly `1200 * 1600 / 2 = 960000` bytes;
- total file size should be `18 + frames * 960000` bytes;
- an `intervalMs` value of zero is replaced with 1000 ms.

Pixels are stored row by row, two pixels per byte, using four-bit color
indices:

| Value | Color |
|---:|---|
| `0x0` | Black |
| `0x1` | White |
| `0x2` | Yellow |
| `0x3` | Red |
| `0x5` | Blue |
| `0x6` | Green |

Each 1200-pixel row occupies 600 bytes. The first 300 bytes are sent to the
master display controller and the following 300 bytes to the slave
controller.

## Connecting and identifying the serial port

Connect the board over USB and list detected ports:

```sh
./.venv/bin/pio device list
```

Typical Linux device names are `/dev/ttyACM0` and `/dev/ttyUSB0`. If access
is denied, add the current user to the serial-port group and then log out and
back in:

```sh
sudo usermod -aG dialout "$USER"
```

Some ESP32-S3 boards must be placed into download mode manually:

1. Hold **BOOT**.
2. Press and release **RESET**.
3. Release **BOOT**.

## Flashing

Back up the existing 4 MB flash before replacing it:

```sh
./.venv/bin/python \
  "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" \
  --chip esp32s3 --port /dev/ttyACM0 \
  read_flash 0x0 0x400000 flash-backup.bin
```

Replace `/dev/ttyACM0` with the port reported on your system.

### Flashing the complete image

This replaces the bootloader, partition table, application, and all data
partitions:

```sh
./.venv/bin/python \
  "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" \
  --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  write_flash 0x0 sd-factory-rebuilt.bin
```

Use this method for a blank device or when the existing partition table is
unknown.

### Flashing only the application

This preserves the current bootloader, partition table, NVS, and other data:

```sh
./.venv/bin/python \
  "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" \
  --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  write_flash 0x10000 sd-app0-rebuilt.bin
```

Application-only flashing is safe only when the device uses the same
partition layout, with `app0` beginning at `0x10000` and having at least
3 MB available. The supplied `sd.bin` has this layout.

If flashing at 921600 baud is unreliable, retry with `--baud 460800` or omit
the baud option.

## Serial monitoring

Open the 115200-baud serial monitor:

```sh
./.venv/bin/pio device monitor \
  --port /dev/ttyACM0 \
  --baud 115200
```

Expected startup messages include:

```text
Starting SPI initialization...
SPI initialization completed
Starting SD card initialization...
SD card initialization successful
Frames: ...
IntervalMs: ...
```

During a display update, messages such as `Write PON`, `Write DRF`,
`e-Paper busy`, and `Display Done!!` are expected.

## Troubleshooting

### The chip reports that the image expects a larger flash

The application header must describe a 4 MB device. Its first four bytes
should end in `2f`:

```sh
xxd -p -l 4 sd-app0-rebuilt.bin
```

Expected prefix:

```text
e9......2f
```

Do not remove `board_upload.flash_size = 4MB` from `platformio.ini`.

### `PSRAM initialization failed`

Confirm that the board has OPI PSRAM and that
`board_build.arduino.memory_type = qio_opi` and `BOARD_HAS_PSRAM` remain
enabled. The firmware cannot operate without a 960000-byte frame buffer.

### `SD card initialization failed`

Check the SD wiring, FAT/FAT32 formatting, 3.3 V signaling, and the recovered
GPIO mapping. Very large or exFAT-formatted cards may require reformatting.

### `open images.bin failed`

The name is case-sensitive. The file must be exactly `/images.bin`, not
`/Images.bin`, `/images.bin.bin`, or a file inside a directory.

### `invalid images.bin header`

Verify the `EPD6BIN` signature, 18-byte packed header, little-endian integer
fields, and expected file size.

### Display remains busy

Check GPIO 7 and the display board power. The firmware intentionally waits
without a timeout because this matches the recovered original behavior.

### Display output is split, shifted, or has incorrect colors

Confirm that the panel is the dual-controller 1200x1600 Spectra 6 model,
that CS master/slave are connected to GPIO 18/17, and that each frame uses
600 bytes per row in the expected four-bit palette format.

## Source layout

```text
platformio.ini
scripts/build-release.sh
src/main.cpp
src/Epaper13in3e.cpp
src/Epaper13in3e.h
```

`Epaper13in3e.cpp` is adapted from the Waveshare reference driver. The GPIO
mapping and modified analog timing bytes were recovered directly from the
supplied firmware.
