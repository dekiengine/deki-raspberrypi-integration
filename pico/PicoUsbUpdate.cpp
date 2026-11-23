#include "PicoUsbUpdate.h"

#if defined(DEKI_PICO)
#include "UsbUpdatePlan.h"

#include <deki/Engine.h>
#include <deki/FileImage.h>

#include <hardware/flash.h>
#include <hardware/regs/addressmap.h>
#include <hardware/structs/scb.h>
#include <hardware/sync.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>

#include <cstdarg>
#include <cstdio>

// Where the program starts and ends in flash, from the Pico SDK's linker script.
extern "C" char __flash_binary_start;
extern "C" char __flash_binary_end;
#endif

namespace DekiRaspberryPi
{

#if defined(DEKI_PICO) && defined(DEKI_PICO_FLASH_SIZE)

namespace
{

using namespace UsbUpdate;

// A line of what the host sends, until its end.
char s_Line[64];
int s_LineLength = 0;
// One chunk as it arrives; in RAM, where the copy below can use it too.
uint8_t s_Chunk[kChunk];

void Reply(const char* format, ...)
{
    std::printf("%s ", kCommand);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
    stdio_flush();
}

// Restarts the board: the boot loader starts whatever game is in flash.
void __no_inline_not_in_flash_func(Restart)()
{
    scb_hw->aircr = 0x05FA0004u;  // the key, and SYSRESETREQ
    while (true)
    {
    }
}

// Copies the checked image from `staging` over the running game at `app`
// (offsets from the start of flash) and restarts. All of it runs from RAM
// with interrupts off: the program it would return to is being replaced.
// A sector at a time (CopyImage): erasing all of `app` first wiped the
// start of `staging` whenever the new image was longer than the gap.
void __no_inline_not_in_flash_func(CopyAndRestart)(uint32_t staging, uint32_t app, uint32_t size)
{
    save_and_disable_interrupts();
    struct Flash
    {
        // Through the flash cache, which each write leaves on again.
        [[gnu::always_inline]] void Read(uint32_t offset, uint8_t* out, uint32_t n)
        {
            const auto* from = reinterpret_cast<const volatile uint8_t*>(XIP_BASE + offset);
            for (uint32_t i = 0; i < n; ++i)
            {
                out[i] = from[i];
            }
        }
        [[gnu::always_inline]] void Erase(uint32_t offset, uint32_t n) { flash_range_erase(offset, n); }
        [[gnu::always_inline]] void Program(uint32_t offset, const uint8_t* data, uint32_t n)
        {
            flash_range_program(offset, data, n);
        }
    } flash;
    CopyImage(flash, staging, app, size, s_Chunk);
    Restart();
}

// Flash writes stop the flash cache for a moment: nothing may run from flash
// meanwhile, so interrupts are off and core 1 is stopped for the whole update.
void Erase(uint32_t offset, uint32_t length)
{
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(offset, length);
    restore_interrupts(interrupts);
}

void Program(uint32_t offset, const uint8_t* data, uint32_t length)
{
    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_program(offset, data, length);
    restore_interrupts(interrupts);
}

// Where the running image (program and game data) ends, from the start of
// flash.
uint32_t RunningImageEnd()
{
    const uintptr_t data = (reinterpret_cast<uintptr_t>(&__flash_binary_end) + kSector - 1) / kSector * kSector;
    const uint32_t dataSize =
        Deki::FileImage::SizeFromHeader(reinterpret_cast<const uint8_t*>(data), Deki::FileImage::kHeaderSize);
    return static_cast<uint32_t>(data - XIP_BASE) + dataSize;
}

void Receive(uint32_t size, uint32_t crc)
{
    const uint32_t app = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__flash_binary_start) - XIP_BASE);
    uint32_t staging = 0;
    if (!PlanStaging(RunningImageEnd(), size, DEKI_PICO_FLASH_SIZE, staging))
    {
        Reply("ERR the new build (%u bytes) does not fit beside the running one", static_cast<unsigned>(size));
        return;
    }
    // From here the game is stopped for good: it restarts into the new
    // build, or into the old one if this goes wrong.
    multicore_reset_core1();
    Reply("ERASING %u bytes at 0x%x", static_cast<unsigned>(AlignUp(size)), static_cast<unsigned>(staging));
    Erase(staging, AlignUp(size));
    Reply("READY %u", static_cast<unsigned>(kChunk));

    uint32_t received = 0;
    uint32_t runningCrc = 0;
    while (received < size)
    {
        const uint32_t count = size - received < kChunk ? size - received : kChunk;
        for (uint32_t i = 0; i < count; ++i)
        {
            const int c = getchar_timeout_us(5000000);
            if (c < 0)
            {
                Reply("ERR nothing arrived for 5 s at byte %u; restarting the old game",
                      static_cast<unsigned>(received + i));
                Restart();
            }
            s_Chunk[i] = static_cast<uint8_t>(c);
        }
        for (uint32_t i = count; i < kChunk; ++i)
        {
            s_Chunk[i] = 0xFF;  // what erased flash reads as
        }
        runningCrc = Crc32(runningCrc, s_Chunk, count);
        Program(staging + received, s_Chunk, kChunk);
        received += count;
        Reply("OK %u", static_cast<unsigned>(received));
    }

    // What arrived, and what flash now holds, must both be what was sent.
    const auto* written = reinterpret_cast<const uint8_t*>(XIP_BASE + staging);
    const uint32_t writtenCrc = Crc32(0, written, size);
    if (runningCrc != crc || writtenCrc != crc)
    {
        Reply("ERR checksum: expected %08x, received %08x, written %08x; restarting the old game",
              static_cast<unsigned>(crc), static_cast<unsigned>(runningCrc), static_cast<unsigned>(writtenCrc));
        Restart();
    }
    Reply("INSTALLING %u bytes", static_cast<unsigned>(size));
    busy_wait_ms(50);  // the reply out before the USB stops with everything else
    CopyAndRestart(staging, app, size);
}

void Poll(uint32_t /*deltaTimeMs*/)
{
    // What the host sent since the last frame, a line at a time; a bounded
    // amount, so a chatty host cannot stall the game.
    for (int n = 0; n < 128; ++n)
    {
        const int c = getchar_timeout_us(0);
        if (c < 0)
        {
            return;
        }
        if (c == '\n' || c == '\r')
        {
            s_Line[s_LineLength] = 0;
            s_LineLength = 0;
            uint32_t size = 0;
            uint32_t crc = 0;
            if (ParseRequest(s_Line, size, crc))
            {
                Receive(size, crc);
            }
            continue;
        }
        if (s_LineLength < static_cast<int>(sizeof(s_Line)) - 1)
        {
            s_Line[s_LineLength++] = static_cast<char>(c);
        }
    }
}

}  // namespace

void StartUsbUpdates()
{
    // A game flashed from BOOTSEL owns the whole flash, boot code included:
    // replacing that from inside the game could leave a board that does not
    // start. Those update through BOOTSEL.
    if (reinterpret_cast<uintptr_t>(&__flash_binary_start) == XIP_BASE)
    {
        return;
    }
    Deki::Engine::GetInstance().RegisterUpdate(&Poll);
}

#else

void StartUsbUpdates()
{
    // Not a Pico: nothing to listen on.
}

#endif

}  // namespace DekiRaspberryPi
