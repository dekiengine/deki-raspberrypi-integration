#pragma once

// Storage that reads and writes whole 512-byte sectors: an SD card, or a
// block of RAM in the tests. FatVolume works on top of it.

#include <cstdint>

namespace DekiRaspberryPi
{

constexpr uint32_t kSectorSize = 512;

class IBlockDevice
{
public:
    virtual ~IBlockDevice() = default;

    /// Reads `count` sectors starting at `sector` into `out`. Returns false on error.
    virtual bool ReadSectors(uint32_t sector, uint32_t count, uint8_t* out) = 0;

    /// Writes `count` sectors starting at `sector` from `data`. Returns false on error.
    virtual bool WriteSectors(uint32_t sector, uint32_t count, const uint8_t* data) = 0;

    /// How many sectors the device holds.
    virtual uint32_t SectorCount() const = 0;

    /// Waits until every write so far is stored. Returns false on error.
    virtual bool Sync() = 0;
};

}  // namespace DekiRaspberryPi
