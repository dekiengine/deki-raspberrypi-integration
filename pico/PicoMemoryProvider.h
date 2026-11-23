#pragma once

#include <deki/providers/IMemoryProvider.h>

namespace DekiRaspberryPi
{

/// Memory on a Pico: the chip's SRAM, one heap that every DMA channel can
/// reach (264 KB on an RP2040, 520 KB on an RP2350, less the program's own
/// data and the stacks). There is no external memory: PSRAM on a board's
/// own bus is not mapped into the address space, so it is not memory the
/// engine can place things in.
class PicoMemoryProvider : public Deki::IMemoryProvider
{
public:
    bool Initialize() override { return true; }
    void Shutdown() override {}

    bool Serves(Deki::Memory::Region region) const override;
    void* Allocate(Deki::Memory::Region region, size_t bytes, bool needsDma) override;
    void Free(Deki::Memory::Region region, void* ptr) override;
    size_t GetAvailable(Deki::Memory::Region region) const override;

    void* AllocateRaw(size_t size) override;
    void FreeRaw(void* ptr) override;

    /// Bytes the heap can still hand out: what it has never used plus what
    /// was freed back to it.
    static size_t FreeHeapBytes();
};

}  // namespace DekiRaspberryPi
