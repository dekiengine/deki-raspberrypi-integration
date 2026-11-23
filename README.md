# Deki Raspberry Pi Integration

Build targets for Raspberry Pi boards. For now that is the Pico SDK, for boards with an RP2040 or RP2350: the Pico, the Pico 2 and boards built on them, such as the ClockworkPi PicoCalc (`deki-clockworkpi-boards`). **Build** makes one `.uf2` that holds the game and its data; **Flash** puts it on the board over USB.

[Deki](https://dekiengine.com) is a modular C++ engine with a visual editor, made first for embedded devices. Every capability is a package, and a platform is one too: this package has the Pico's build, its memory, time and files, and its buses.

## Install

A board package brings it (`deki-clockworkpi-boards` does). To use it on a board of your own, add it with the Package Manager or `DekiEditor --packages-add deki-raspberrypi-integration <project>`, and make a platform with `"framework": "pico"`.

## Toolchain

The Build panel lists what a build needs. **Install** downloads these, checks each against its pinned SHA-256 and puts it in `~/.deki-editor/toolchains/`:

- **Arm GNU Toolchain 15.2.rel1** (`arm-none-eabi`), from developer.arm.com
- **Pico SDK 2.3.1** and **TinyUSB 0.18.0** (the version the SDK pins), from GitHub
- **pioasm** and **picotool 2.3.1**, Raspberry Pi's prebuilt tools

Only Windows x86_64 archives are pinned so far. CMake, Ninja and a GCC 16 or newer for this machine (it compiles the reflection step) are found on PATH; install those yourself.

## The platform

A platform names the chip and the flash in its `frameworkOptions`:

| Option | What it is |
| --- | --- |
| `picoChip` | `rp2040` or `rp2350` (only the RP2040 has run so far) |
| `picoBoard` | The Pico SDK's board name, `pico` or `pico2` by default |
| `flashSize` | The board's flash, in bytes |
| `stdio` | Where the log goes: `uart`, `usb` or `both` |
| `sysClockKhz` | The chip's clock, if not the SDK's |

## What a build does

The engine, the packages the game uses and the game's code go into one program, built with the Pico SDK (`generated/build/<platform>/`). The game's data (the boot scene, the project settings and the assets on internal storage) is packed into one file image and placed on the first flash sector after the program, so the program and the data can each grow as long as both fit. Both go into `DekiGame.uf2`. The build output says how big each is and how much flash is left.

On the board, `F:/` is that image, read straight out of flash: it cannot be written. A game keeps its saves on the SD card (`S:/`).

## Flash

The target list offers:

- **A Pico in BOOTSEL mode**: a drive that holds `INFO_UF2.TXT` (`RPI-RP2` or `RP2350`). The `.uf2` is copied onto it, and the Pico restarts into the game.
- **A serial port**: a Deki game running on the Pico, on its USB. The port is opened at 1200 baud, which makes the Pico restart in BOOTSEL mode; its drive is then flashed as above.

A Pico that has never run a Deki game needs BOOTSEL by hand the first time: hold its BOOTSEL button while plugging it in. After that, double-tapping its reset button also gets BOOTSEL.

## What the package provides on the board

| | |
| --- | --- |
| Memory | The chip's SRAM, every byte of it in reach of DMA. There is no external memory |
| Files | `F:/`, the game's data in flash; the SD card is `S:/` through `deki-sdcard` |
| Buses | `deki-i2c`, `deki-spi` (DMA for long writes), `deki-gpio` (edge counting) |
| SD card | SPI mode, with our own FAT32 and FAT16 code (long names, folders) |

## Not done yet

- A second core sending frames while the next is drawn. Frames are sent as they are drawn.
- The engine's audio (deki-audio) on a Pico. `pico/audio/PicoPwmAudio` plays samples a game makes itself, out of one or two PWM pins.
- Saves in flash: `F:/` is read-only.
- exFAT cards (over 32 GB); format them as FAT32.
- Hosts other than Windows x86_64 for the toolchain.
