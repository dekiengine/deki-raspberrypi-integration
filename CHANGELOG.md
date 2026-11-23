# Changelog

Notable changes to `deki-raspberrypi-integration`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

## Unreleased

### Added
- **`PicoPwmAudio`**: sound on two PWM pins. A game that makes its own
  samples gives a function that fills a buffer with them; it is called from
  the PWM's interrupt, which is also the sample clock. Not deki-audio, which
  does not run on a Pico yet.
- **The Pico SDK as a build backend** (framework `pico`), for RP2040 and
  RP2350 boards. The toolchain installs from pinned downloads (Arm GNU
  Toolchain 15.2.rel1, Pico SDK 2.3.1, TinyUSB 0.18.0, pioasm and picotool).
  A build makes one `.uf2` with the program and the game's data, written by
  our own UF2 writer.
- **Flash** to a Pico in BOOTSEL mode, or to a running Deki game over USB,
  which the 1200-baud touch restarts into BOOTSEL.
- **The Pico at run time**: memory, time, `F:/` read straight out of flash,
  I2C, SPI with DMA, GPIO with edge counting, and the SD card with our own
  FAT32 and FAT16 code.
