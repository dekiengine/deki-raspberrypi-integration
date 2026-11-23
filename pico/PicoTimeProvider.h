#pragma once

#include <deki/providers/ITimeProvider.h>

#if defined(DEKI_PICO)
#include <pico/time.h>
#endif

namespace DekiRaspberryPi
{

/// The engine's clock on a Pico: the chip's 64-bit microsecond timer, which
/// starts at boot and never wraps in practice.
class PicoTimeProvider : public Deki::ITimeProvider
{
public:
#if defined(DEKI_PICO)
    uint32_t GetTicksMs() const override { return static_cast<uint32_t>(time_us_64() / 1000u); }
    uint64_t GetTicksUs() const override { return time_us_64(); }
    void DelayMs(uint32_t ms) const override { sleep_ms(ms); }
#else
    // Outside a Pico build (the editor's DLL) nothing uses it.
    uint32_t GetTicksMs() const override { return 0; }
#endif
};

}  // namespace DekiRaspberryPi
