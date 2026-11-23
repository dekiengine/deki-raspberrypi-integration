#pragma once

// Builds a Deki game for a Raspberry Pi RP2040 or RP2350 board with the Pico
// SDK, and flashes it.
//
// In this package's editor/ folder, so it is compiled into the package's
// editor-side DLL and registered when a project has the package installed,
// and never into a game, which leaves editor/ out. It claims the framework id
// "pico"; a platform names it with "framework": "pico".
//
// What it reads from the platform's frameworkOptions:
//   picoChip        "rp2040" or "rp2350" (ARM cores). Required. Only the
//                   RP2040 has been run on a device so far.
//   picoBoard       the Pico SDK's PICO_BOARD ("pico", "pico2", "pico_w"...).
//                   Default "pico" for an RP2040, "pico2" for an RP2350.
//   flashSize       the board's flash in bytes, a multiple of 4096, up to
//                   16 MB. Required.
//   stdio           where printf goes: "uart", "usb" or "both" (default).
//                   With USB, Flash can restart a running game into BOOTSEL.
//   sysClockKhz     the system clock the game asks for, in kHz (optional);
//                   the package's runtime sets it.
//   loaderOffset    for a board whose flash starts with a boot menu that loads
//                   programs from an SD card (the PicoCalc's): where that menu
//                   puts a program, in bytes from the start of the flash, a
//                   multiple of 4096. The program is then linked to start
//                   there with no boot2 in front (the menu jumps straight to
//                   its vector table), and the build makes DekiGame.bin for
//                   the menu instead of a UF2. 0 or absent: the whole flash
//                   is the game's, from BOOTSEL.
//
// What the game is compiled with, beyond the platform's own defines:
//   DEKI_PICO, DEKI_PICO_RP2040 or DEKI_PICO_RP2350,
//   DEKI_PICO_FLASH_SIZE=<bytes>,
//   DEKI_PICO_SYS_CLOCK_KHZ=<kHz> when sysClockKhz is set, DEKI_FAST_ATTR=
//   (empty), the transform width and the package and capability defines.
//
// The build (generated/build/<platform id>/) is a Pico SDK CMake project: the
// engine, the packages the platform has and the project's code in one static
// library, and the program linked from it, with the Pico SDK libraries the
// packages name under "dependencies": { "pico": [...] }. The program's main()
// is the Raspberry Pi package's. After it is built, the game's data (the boot
// payload and the internal assets) is packed into a file image and the two go
// into one DekiGame.uf2: the program at the start of the flash, the data on
// the first sector after it (GameDataAt). The runtime finds the data the same
// way, from where the linker ended the program, so there is no line between
// the two to set: either can grow while they fit the flash together.
//
// Deploy ("Flash") copies DekiGame.uf2 onto a Pico in BOOTSEL mode, or first
// restarts a Pico running a Deki game into BOOTSEL through its USB serial
// port (PicoDevices.h).

#include <deki-editor/IconsTabler.h>
#include <deki-editor/Localization.h>
#include <deki-editor/build/FirmwareBuilderBase.h>

#include "PicoDevices.h"
#include "PicoToolchain.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace DekiEditor
{

/// A platform's frameworkOptions as the builder reads them, defaults filled in.
struct PicoOptions
{
    std::string chip;   // "rp2040", "rp2350"
    std::string board;  // PICO_BOARD
    uint32_t flashSize = 0;
    std::string stdio = "both";
    uint32_t sysClockKhz = 0;   // 0 when not set
    uint32_t loaderOffset = 0;  // 0: no boot menu, the program starts the flash

    /// True when a boot menu on the board starts the program.
    bool BootMenu() const { return loaderOffset != 0; }
    bool UsbSerial() const { return stdio == "usb" || stdio == "both"; }
    bool UartSerial() const { return stdio == "uart" || stdio == "both"; }
};

/// What a finished build takes of the flash, in bytes.
struct PicoFlashUse
{
    size_t program = 0;
    size_t gameData = 0;
    uint32_t gameDataAt = 0;  // where it went, in bytes from the flash's start
    size_t freeFlash = 0;
};

class PicoBuilder : public FirmwareBuilderBase
{
public:
    PicoBuilder();
    ~PicoBuilder() override;

    // Core operations
    void Build(const std::string& projectPath, BuildOutputCallback outputCallback = nullptr,
               BuildProgressCallback progressCallback = nullptr) override;
    void Clean(const std::string& projectPath, BuildOutputCallback outputCallback = nullptr,
               BuildProgressCallback progressCallback = nullptr) override;

    // Deploy: flash the UF2 onto a Pico
    bool SupportsDeploy() const override { return true; }
    const char* GetDeployLabel() const override { return "Flash"; }
    std::vector<DeployTarget> EnumerateDeployTargets() const override;
    void Deploy(const std::string& projectPath, const std::string& deployTargetId,
                BuildOutputCallback outputCallback = nullptr,
                BuildProgressCallback progressCallback = nullptr) override;

    // Toolchain: the pinned components (installable) and the host tools (found)
    bool IsToolchainInstalled() const override;
    std::string GetToolchainStatus() const override;
    std::vector<ToolchainComponent> GetToolchainComponents() const override;
    void CheckForToolchainUpdates(std::function<void()> onComplete = nullptr) override;

    // Build file generation
    bool GenerateBuildFiles(const std::string& projectPath, const PlatformConfig& config,
                            const std::vector<std::string>& packageDefines) override;

    // Identity
    const char* GetName() const override { return "Raspberry Pi Pico"; }
    const char* GetIcon() const override { return ICON_TI_CPU_2; }
    const char* GetDescription() const override
    {
        return TrMark("Build for Raspberry Pi RP2040 and RP2350 boards with the Pico SDK, and flash them over USB");
    }
    std::string GetFrameworkId() const override { return "pico"; }
    std::vector<std::pair<std::string, std::string>> DescribePlatform(const PlatformConfig& config) const override;
    std::vector<std::string> ValidatePlatform(const PlatformConfig& config) const override;
    // The chip, board, flash layout and serial output, in the platform editor.
    std::unique_ptr<IPlatformEditorUI> CreateEditorUI(const PlatformConfig& config) const override;

    std::string GetBuildDirectory(const std::string& projectPath) const override;
    // The boot payload, packed into the game's data in the flash.
    std::string GetBootPayloadDirectory(const std::string& projectPath) const override
    {
        return GetBuildDirectory(projectPath) + "/gamedata";
    }

    // ---- the parts a test reaches -------------------------------------------

    /// The platform's frameworkOptions with the defaults filled in. Not
    /// checked: ValidatePlatform says what is wrong with them.
    static PicoOptions OptionsOf(const PlatformConfig& config);

    /// The compile definitions the options give the game (see above).
    static std::vector<std::string> PicoDefines(const PicoOptions& options);

    /// The CMake configure line run in the build directory.
    std::string ConfigureCommand() const;

    /// The build directory of platform `platformId`: generated/build/<id>.
    static std::filesystem::path BuildDirectoryFor(const std::string& projectPath, const std::string& platformId);

    /// Packs every file under `dir` into the read-only file image the game
    /// reads its data from (Deki::FileImage), named by their paths under it.
    static bool MakeGameDataImage(const std::filesystem::path& dir, std::vector<uint8_t>& image, std::string& error);

    /// Where the game's data goes in the flash, in bytes from its start: the
    /// first sector after a program of `programSize` bytes. The runtime
    /// (PicoFileSystem) finds it the same way.
    static uint32_t GameDataAt(const PicoOptions& options, size_t programSize);

    /// Writes `uf2` from the program `programBin` (the build's DekiGame.bin)
    /// and the game's data packed from `payloadDir` (none when it does not
    /// exist), each where `options` puts it in the flash. False, with the
    /// reason in `error`, when either does not fit.
    static bool PackFirmware(const std::filesystem::path& programBin, const std::filesystem::path& payloadDir,
                             const PicoOptions& options, const std::filesystem::path& uf2, PicoFlashUse& use,
                             std::string& error);

    /// The deploy targets for these drives and serial ports: every Pico in
    /// BOOTSEL mode, and the serial ports when the game has USB serial.
    static std::vector<DeployTarget> DeployTargetsFrom(const std::vector<PicoBootDrive>& drives,
                                                       const std::vector<std::string>& ports, bool usbSerial);

private:
    PicoHostTools m_HostTools;

    void DoBuild(const std::string& projectPath, BuildOutputCallback out, BuildProgressCallback progress);
    void DoClean(const std::string& projectPath, BuildOutputCallback out, BuildProgressCallback progress);
    void DoFlashDrive(const std::string& projectPath, const PicoBootDrive& drive, BuildOutputCallback out,
                      BuildProgressCallback progress);
    void DoFlashOverSerial(const std::string& projectPath, const std::string& port, BuildOutputCallback out,
                           BuildProgressCallback progress);

    bool ComponentsInstalled() const;
    // Runs `command` in `workDir`, its output to `out`. The exit code.
    int Run(const std::string& command, const std::string& workDir, const BuildOutputCallback& out);
    void Fail(const std::string& error, const BuildOutputCallback& out, const BuildProgressCallback& progress);
    void Report(BuildState state, const std::string& text, float value, const BuildProgressCallback& progress);
};

}  // namespace DekiEditor
