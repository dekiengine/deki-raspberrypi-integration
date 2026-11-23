// Package entry point. In the editor it is a DLL that exports the standard
// Deki plugin interface (and, from editor/, the Pico SDK build backend). In a
// game built with the Pico SDK it installs the Pico's providers and bus
// drivers before anything else runs, and holds the program's main().

#include "RaspberryPiPackage.h"

#include <deki/interop/Plugin.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/ComponentRegistry.h>

#if defined(DEKI_PICO)
#include "pico/PicoFileSystem.h"
#include "pico/PicoMemoryProvider.h"
#include "pico/PicoTimeProvider.h"
#include "pico/PicoUsbUpdate.h"
#include "pico/gpio/PicoGPIO.h"
#include "pico/i2c/PicoI2C.h"
#include "pico/sd/PicoSDCard.h"
#include "pico/spi/PicoSPI.h"
#if __has_include("PanelDisplay.h")
#include "pico/display/PicoPresentWorker.h"
#endif

#include "DekiGPIO.h"    // from deki-gpio
#include "DekiI2C.h"     // from deki-i2c
#include "DekiSDCard.h"  // from deki-sdcard
#if __has_include("DekiSPI.h")
#include "DekiSPI.h"  // from deki-spi
#endif

#include <deki/Main.h>
#include <deki/Time.h>
#include <deki/providers/FileSystem.h>
#include <deki/providers/Memory.h>

#include <hardware/clocks.h>
#include <hardware/vreg.h>
#include <pico/stdlib.h>

#include <memory>
#endif

extern void DekiRaspberryPiRegisterComponents();
extern int DekiRaspberryPiGetAutoComponentCount();
extern const Deki::ComponentMeta* DekiRaspberryPiGetAutoComponentMeta(int index);

namespace DekiRaspberryPi
{

#if defined(DEKI_PICO)

namespace
{

// Installs the Pico's providers and bus factories at static-init time, so
// they are in place before any other static constructor or the engine asks
// for memory, time or files. Nothing here prints: the console starts in
// main().
struct PicoBackendInit
{
    PicoBackendInit()
    {
        // The engine owns these two and never frees them while the game runs.
        Deki::Memory::SetBackend(new PicoMemoryProvider());
        Deki::FileSystem::SetFileSystem(new PicoFileSystem());
        Deki::Time::SetTimeProvider(std::make_unique<PicoTimeProvider>());

        DekiI2c::DekiI2C::SetFactory([]() -> DekiI2c::IDekiI2C* { return new PicoI2C(); });
#if __has_include("DekiSPI.h")
        DekiSpi::DekiSPI::SetFactory([]() -> DekiSpi::IDekiSPIBus* { return new PicoSPIBus(); });
#endif
        DekiSdCard::DekiSDCard::SetFactory(&CreatePicoSDCard);
#if __has_include("PanelDisplay.h")
        // A frame is sent from core 1 while core 0 draws the next.
        DekiDisplay::SetPanelSyncHooks(&PicoPanelSyncHooks());
        DekiDisplay::PanelDisplay::SetWorkerBackend(&PicoPresentWorker());
#endif

        // Pins: one for the whole chip.
        static PicoGPIO s_Gpio;
        DekiGpio::DekiGPIO::SetCurrent(&s_Gpio);
    }
};
PicoBackendInit s_PicoInit;

}  // namespace

#endif  // DEKI_PICO

#ifdef DEKI_EDITOR

static bool s_RaspberryPiRegistered = false;

extern "C"
{
    // Registers the package's components once. Returns how many there are.
    DEKI_RASPBERRYPI_API int DekiRaspberryPiEnsureRegistered(void)
    {
        if (s_RaspberryPiRegistered)
        {
            return ::DekiRaspberryPiGetAutoComponentCount();
        }
        s_RaspberryPiRegistered = true;
        ::DekiRaspberryPiRegisterComponents();
        return ::DekiRaspberryPiGetAutoComponentCount();
    }

    DEKI_PLUGIN_API const char* DekiPluginGetName(void)
    {
        return "Deki Raspberry Pi Package";
    }

    DEKI_PLUGIN_API const char* DekiPluginGetVersion(void)
    {
#ifdef DEKI_PACKAGE_VERSION
        return DEKI_PACKAGE_VERSION;
#else
        return "0.0.0-dev";
#endif
    }

    DEKI_PLUGIN_API int DekiPluginInit(void)
    {
        return 0;
    }

    DEKI_PLUGIN_API void DekiPluginShutdown(void)
    {
        s_RaspberryPiRegistered = false;
    }

    DEKI_PLUGIN_API int DekiPluginGetComponentCount(void)
    {
        return ::DekiRaspberryPiGetAutoComponentCount();
    }

    DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPluginGetComponentMeta(int index)
    {
        return ::DekiRaspberryPiGetAutoComponentMeta(index);
    }

    DEKI_PLUGIN_API void DekiPluginRegisterComponents(void)
    {
        DekiRaspberryPiEnsureRegistered();
    }

    DEKI_RASPBERRYPI_API const char* DekiRaspberryPiGetName(void)
    {
        return "Raspberry Pi";
    }

}  // extern "C"

#endif  // DEKI_EDITOR

}  // namespace DekiRaspberryPi

#if defined(DEKI_PICO)

// The program's entry. The console first (UART and, when the build has it,
// USB), so the engine's log from here on is seen; then the engine, which
// loads the boot scene from the game's data and runs until it is stopped.
int main()
{
#if defined(DEKI_PICO_SYS_CLOCK_KHZ)
    // The platform's clock (frameworkOptions sysClockKhz), before anything
    // that times itself off it starts. Above 200 MHz an RP2040 needs its core
    // a little above the 1.10 V it starts at; 1.20 V is what Raspberry Pi's
    // own overclocked examples use.
#if defined(DEKI_PICO_RP2040) && DEKI_PICO_SYS_CLOCK_KHZ > 200000
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    busy_wait_us(1000);
#endif
    set_sys_clock_khz(DEKI_PICO_SYS_CLOCK_KHZ, true);
#if defined(DEKI_PICO_RP2040)
    // The SDK moves the peripheral clock to the 48 MHz USB clock when the
    // system clock goes past what it is rated for, which caps SPI at 24 MHz.
    // Overclocked RP2040 boards run it from the system clock instead
    // (PicoMite does on the PicoCalc), so SPI can reach half of it.
    const uint32_t sysHz = clock_get_hz(clk_sys);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, sysHz, sysHz);
#endif
#endif
    stdio_init_all();
    // A new build can come over USB while the game runs (behind a boot
    // loader only; see PicoUsbUpdate.h).
    DekiRaspberryPi::StartUsbUpdates();
    return Deki::Main();
}

#endif  // DEKI_PICO
