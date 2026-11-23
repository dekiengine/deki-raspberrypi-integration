#pragma once

// The rules of an update over USB (PicoUsbUpdate), with nothing of the Pico
// in them so they can be tested anywhere: the request line, the checksum,
// and where the new image waits in flash while it arrives.
//
// The conversation, one line each way (the game's log may come between):
//   host:   DEKI-UPDATE <size> <crc32 in hex>
//   device: DEKI-UPDATE READY <chunk>        (or DEKI-UPDATE ERR <why>)
//   host:   <chunk> bytes                     (the last one: what is left)
//   device: DEKI-UPDATE OK <bytes so far>    (after each)
//   device: DEKI-UPDATE INSTALLING           (then it restarts)

#include <cstddef>
#include <cstdint>

namespace DekiRaspberryPi::UsbUpdate
{

constexpr char kCommand[] = "DEKI-UPDATE";
// Flash is erased a sector at a time, and the image is sent a chunk at a
// time, each written as it arrives.
constexpr uint32_t kSector = 4096;
constexpr uint32_t kChunk = 4096;

/// The CRC-32 zlib and most tools use, carried on from `crc` (0 to start).
uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size);

/// Reads "DEKI-UPDATE <size> <crc32 in hex>". False for anything else.
bool ParseRequest(const char* line, uint32_t& size, uint32_t& crc);

/// `value` rounded up to a whole sector.
uint32_t AlignUp(uint32_t value);

/// Where a new image of `size` bytes waits while it arrives: from the first
/// sector after the running image (which ends at `imageEnd`), so the game
/// keeps running from its own until the new one is whole. Offsets from the
/// start of flash; false if it does not fit below `flashEnd`.
bool PlanStaging(uint32_t imageEnd, uint32_t size, uint32_t flashEnd, uint32_t& staging);

/// Copies the new image of `size` bytes from `staging` over the game at
/// `app` (offsets from the start of flash, `app` below `staging`), a sector
/// at a time through `buffer` (kSector bytes): each sector is read before
/// the one it lands on is erased. A new image longer than the gap between
/// the two runs into `staging`, but only over sectors already copied.
/// `Flash` reads, erases and programs: Read(offset, out, n), Erase(offset,
/// n), Program(offset, data, n). Always inlined, so on the Pico it runs from
/// RAM with the function that calls it, while flash is being rewritten.
template <typename Flash>
[[gnu::always_inline]] inline void CopyImage(Flash& flash, uint32_t staging, uint32_t app, uint32_t size,
                                             uint8_t* buffer)
{
    const uint32_t length = (size + kSector - 1) / kSector * kSector;
    for (uint32_t done = 0; done < length; done += kSector)
    {
        flash.Read(staging + done, buffer, kSector);
        flash.Erase(app + done, kSector);
        flash.Program(app + done, buffer, kSector);
    }
}

}  // namespace DekiRaspberryPi::UsbUpdate
