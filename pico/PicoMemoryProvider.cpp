#include "PicoMemoryProvider.h"

#include <cstdio>
#include <cstdlib>

#if defined(DEKI_PICO)
#include <malloc.h>
#include <unistd.h>

// The end of the space the heap may grow into, from the Pico SDK's linker
// script: the stacks start there.
extern "C" char __StackLimit;
#endif

namespace DekiRaspberryPi
{

namespace
{

// Blocks are aligned to 8 bytes: enough for any type the engine stores and
// for word-wide DMA.
constexpr size_t kAlignment = 8;

// Says what a failed allocation asked for and what the heap had left, with
// printf: logging could itself allocate.
void ReportFailure(size_t bytes)
{
    std::printf("Deki: %u bytes could not be allocated. Heap free: %u bytes.\n", static_cast<unsigned>(bytes),
                static_cast<unsigned>(PicoMemoryProvider::FreeHeapBytes()));
}

}  // namespace

bool PicoMemoryProvider::Serves(Deki::Memory::Region region) const
{
    return region == Deki::Memory::Internal;
}

void* PicoMemoryProvider::Allocate(Deki::Memory::Region region, size_t bytes, bool needsDma)
{
    // All of SRAM is in reach of DMA, so needsDma changes nothing.
    (void)needsDma;
    if (region != Deki::Memory::Internal)
    {
        return nullptr;
    }
#if defined(DEKI_PICO)
    void* ptr = memalign(kAlignment, bytes);
#else
    void* ptr = std::malloc(bytes);
#endif
    if (!ptr)
    {
        ReportFailure(bytes);
    }
    return ptr;
}

void PicoMemoryProvider::Free(Deki::Memory::Region region, void* ptr)
{
    // memalign's blocks go back with free, like malloc's.
    (void)region;
    std::free(ptr);
}

size_t PicoMemoryProvider::GetAvailable(Deki::Memory::Region region) const
{
    return region == Deki::Memory::Internal ? FreeHeapBytes() : 0;
}

void* PicoMemoryProvider::AllocateRaw(size_t size)
{
    void* ptr = std::malloc(size);
    if (!ptr && size > 0)
    {
        ReportFailure(size);
    }
    return ptr;
}

void PicoMemoryProvider::FreeRaw(void* ptr)
{
    std::free(ptr);
}

size_t PicoMemoryProvider::FreeHeapBytes()
{
#if defined(DEKI_PICO)
    // The heap grows up from the end of the program's data towards the
    // stacks. Free is the room it has not grown into yet, plus the blocks
    // given back inside it.
    const char* top = static_cast<const char*>(sbrk(0));
    const size_t unclaimed = top < &__StackLimit ? static_cast<size_t>(&__StackLimit - top) : 0;
    return unclaimed + static_cast<size_t>(mallinfo().fordblks);
#else
    return 0;
#endif
}

}  // namespace DekiRaspberryPi
