#pragma once

// Main header of the Raspberry Pi package: the Pico SDK backend for RP2040 and
// RP2350 boards. Everything for the Pico itself is in pico/ and is compiled
// only into a game built with the Pico SDK (DEKI_PICO); in the editor the
// package is a DLL that offers the build backend (editor/).

#ifdef _WIN32
#ifdef DEKI_RASPBERRYPI_EXPORTS
#define DEKI_RASPBERRYPI_API __declspec(dllexport)
#else
#define DEKI_RASPBERRYPI_API __declspec(dllimport)
#endif
#else
#define DEKI_RASPBERRYPI_API __attribute__((visibility("default")))
#endif
