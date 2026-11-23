#include "PicoBuilder.h"

#include "Uf2Writer.h"

#include <deki-editor/EditorTheme.h>
#include <deki-editor/EditorUI.h>
#include "imgui.h"

#include <deki-editor/EditorPaths.h>
#include <deki-editor/EditorSettings.h>
#include <deki-editor/FeatureResolver.h>
#include <deki-editor/Paths.h>
#include <deki-editor/SafeNames.h>
#include <deki-editor/SerialPorts.h>
#include <deki-editor/ShellCommand.h>
#include <deki-editor/build/BuilderDefinition.h>
#include <deki-editor/build/CMakeGenUtils.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/ProjectPaths.h>
#include <deki/FileImage.h>
#include <deki/LogSystem.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{

constexpr const char* kPicoProgramName = "DekiGame";
constexpr const char* kPicoLibraryName = "DekiGameLib";
constexpr const char* kPicoUf2Name = "DekiGame.uf2";
// What a boot-menu build makes instead (frameworkOptions loaderOffset).
constexpr const char* kPicoMenuBinName = "DekiGame.bin";
// The board and chip the build directory was configured for: the SDK picks
// its compiler flags at the first configure, so a change needs a fresh cache.
constexpr const char* kPicoTargetStamp = "pico_target.txt";
constexpr uint32_t kPicoSector = 4096;
constexpr uint32_t kPicoMaxFlash = 16u * 1024u * 1024u;
// Seconds a Pico takes to restart into BOOTSEL and show its drive, at most.
constexpr int kPicoBootselWaitMs = 10000;

std::string PicoCMakePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::string PicoToolchainPath(const char* installPath)
{
    return PicoCMakePath(EditorPaths::GetToolchainsDir()) + "/" + installPath;
}

// frameworkOptions as JSON, to tell a value that is missing from one of the
// wrong type.
nlohmann::json PicoOptionsJson(const PlatformConfig& config)
{
    nlohmann::json j = nlohmann::json::parse(config.frameworkOptions.empty() ? "{}" : config.frameworkOptions, nullptr,
                                             /*allow_exceptions*/ false);
    return j.is_object() ? j : nlohmann::json::object();
}

// True when `key` is a whole number from 0 to 2^32 - 1.
bool PicoIsU32(const nlohmann::json& options, const char* key)
{
    if (!options.contains(key))
    {
        return false;
    }
    const nlohmann::json& v = options[key];
    return v.is_number_unsigned() ||
           (v.is_number_integer() && v.get<int64_t>() >= 0 && v.get<int64_t>() <= 0xFFFFFFFFll);
}

// A board name as the Pico SDK's boards/ headers are named.
bool PicoIsBoardName(const std::string& s)
{
    if (s.empty() || s.size() > 64)
    {
        return false;
    }
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}

std::string PicoKb(size_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    return text;
}

std::string PicoHex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

std::string PicoChipLabel(const std::string& chip)
{
    return chip == "rp2350" ? "RP2350" : chip == "rp2040" ? "RP2040" : chip;
}

std::string PicoStdioLabel(const std::string& stdio)
{
    return stdio == "usb" ? "USB" : stdio == "uart" ? "UART" : "USB and UART";
}

std::vector<uint8_t> PicoReadBytes(const fs::path& path, bool& ok)
{
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

PicoBuilder::PicoBuilder()
{
    BuilderDefinition def;
    std::string error;
    if (ParseBuilderDefinition(PicoToolchainDefinitionJson(), def, error))
    {
        m_ToolchainMgr.Initialize(def);
    }
    else
    {
        DEKI_LOG_ERROR("PicoBuilder: its toolchain definition does not parse: %s", error.c_str());
    }
}

PicoBuilder::~PicoBuilder()
{
    Cancel();
    if (m_BuildThread.joinable())
    {
        m_BuildThread.join();
    }
}

// ============================================================================
// Paths and options
// ============================================================================

fs::path PicoBuilder::BuildDirectoryFor(const std::string& projectPath, const std::string& platformId)
{
    // Per platform, as every target's build/<id>: two Pico platforms in one
    // project must not share a build tree.
    return ProjectPaths::Build(projectPath) / (platformId.empty() ? std::string("pico") : platformId);
}

std::string PicoBuilder::GetBuildDirectory(const std::string& projectPath) const
{
    return BuildDirectoryFor(projectPath, m_PlatformConfig.id).string();
}

PicoOptions PicoBuilder::OptionsOf(const PlatformConfig& config)
{
    PicoOptions o;
    o.chip = config.Option("picoChip");
    o.board = config.Option("picoBoard", o.chip == "rp2350" ? "pico2" : "pico");
    o.flashSize = config.OptionU32("flashSize");
    o.stdio = config.Option("stdio", "both");
    o.sysClockKhz = config.OptionU32("sysClockKhz");
    o.loaderOffset = config.OptionU32("loaderOffset");
    return o;
}

std::vector<std::string> PicoBuilder::PicoDefines(const PicoOptions& options)
{
    std::vector<std::string> defines = { "DEKI_PICO",
                                         options.chip == "rp2350" ? "DEKI_PICO_RP2350" : "DEKI_PICO_RP2040",
                                         "DEKI_PICO_FLASH_SIZE=" + std::to_string(options.flashSize) };
    if (options.sysClockKhz != 0)
    {
        defines.push_back("DEKI_PICO_SYS_CLOCK_KHZ=" + std::to_string(options.sysClockKhz));
    }
    defines.push_back("DEKI_FAST_ATTR=");
    return defines;
}

// ============================================================================
// Toolchain
// ============================================================================

bool PicoBuilder::ComponentsInstalled() const
{
    const std::vector<ToolchainComponent> components = m_ToolchainMgr.GetComponents();
    if (components.empty())
    {
        return false;
    }
    for (const auto& c : components)
    {
        if (c.status != ToolchainComponentStatus::Installed && c.status != ToolchainComponentStatus::UpdateAvailable)
        {
            return false;
        }
    }
    return true;
}

bool PicoBuilder::IsToolchainInstalled() const
{
    if (!ComponentsInstalled())
    {
        return false;
    }
    for (const auto& tool : m_HostTools.Get())
    {
        if (!tool.ok)
        {
            return false;
        }
    }
    return true;
}

std::string PicoBuilder::GetToolchainStatus() const
{
    std::vector<std::string> problems;
    if (!PicoHostHasPinnedToolchain())
    {
        problems.push_back("No Pico toolchain is pinned for this host yet, so the editor cannot install one: "
                           "Raspberry Pi Pico builds need a Windows x86_64 machine for now.");
    }
    else
    {
        for (const auto& c : m_ToolchainMgr.GetComponents())
        {
            if (c.status != ToolchainComponentStatus::Installed &&
                c.status != ToolchainComponentStatus::UpdateAvailable)
            {
                problems.push_back(c.displayName +
                                   " is not installed: install it from the Build panel's toolchain list or with "
                                   "install_toolchain_component.");
            }
        }
    }
    for (const auto& tool : m_HostTools.Get())
    {
        if (!tool.ok)
        {
            problems.push_back(tool.problem);
        }
    }
    if (problems.empty())
    {
        std::string status = std::string("Pico SDK ") + kPicoSdkVersion + ", Arm GNU Toolchain " + kPicoCompilerVersion;
        for (const auto& tool : m_HostTools.Get())
        {
            status += ", " + tool.displayName + (tool.version.empty() ? "" : " " + tool.version);
        }
        return status;
    }
    std::string status;
    for (const auto& p : problems)
    {
        status += (status.empty() ? "" : " ") + p;
    }
    return status;
}

std::vector<ToolchainComponent> PicoBuilder::GetToolchainComponents() const
{
    std::vector<ToolchainComponent> components = m_ToolchainMgr.GetComponents();
    // The host tools the editor cannot install: shown so a missing one is
    // seen, with what to do as the tooltip.
    for (const auto& tool : m_HostTools.Get())
    {
        ToolchainComponent c;
        c.id = tool.id;
        c.displayName = tool.displayName;
        c.status = tool.ok ? ToolchainComponentStatus::Installed : ToolchainComponentStatus::NotInstalled;
        c.installedVersion = tool.version;
        c.canInstall = false;
        c.tooltip = tool.ok ? tool.path : tool.problem;
        components.push_back(c);
    }
    return components;
}

void PicoBuilder::CheckForToolchainUpdates(std::function<void()> onComplete)
{
    // The host tools may have been installed since the editor looked.
    m_HostTools.Rescan();
    FirmwareBuilderBase::CheckForToolchainUpdates(std::move(onComplete));
}

// ============================================================================
// Platform
// ============================================================================

std::vector<std::string> PicoBuilder::ValidatePlatform(const PlatformConfig& config) const
{
    std::vector<std::string> problems;
    const nlohmann::json raw = PicoOptionsJson(config);
    const PicoOptions o = OptionsOf(config);

    if (o.chip.empty())
    {
        problems.push_back("frameworkOptions.picoChip is not set: \"rp2040\" or \"rp2350\"");
    }
    else if (Uf2FamilyOfChip(o.chip) == 0)
    {
        problems.push_back("frameworkOptions.picoChip '" + o.chip +
                           "' is not a chip this builder knows: \"rp2040\" or \"rp2350\"");
    }

    if (!PicoIsBoardName(o.board))
    {
        problems.push_back("frameworkOptions.picoBoard '" + o.board +
                           "' is not a Pico SDK board name (lowercase letters, digits, '_' and '-', as \"pico2\")");
    }

    bool flashOk = false;
    if (!PicoIsU32(raw, "flashSize") || o.flashSize == 0)
    {
        problems.push_back("frameworkOptions.flashSize is not set: the board's flash in bytes (2097152 for 2 MB)");
    }
    else if (o.flashSize % kPicoSector != 0)
    {
        problems.push_back("frameworkOptions.flashSize " + std::to_string(o.flashSize) +
                           " is not a multiple of 4096 bytes, the flash's sector");
    }
    else if (o.flashSize > kPicoMaxFlash)
    {
        problems.push_back("frameworkOptions.flashSize " + std::to_string(o.flashSize) +
                           " is more than the 16 MB a Pico maps");
    }
    else
    {
        flashOk = true;
    }

    if (raw.contains("loaderOffset") && !PicoIsU32(raw, "loaderOffset"))
    {
        problems.push_back("frameworkOptions.loaderOffset is not a number of bytes");
    }
    else if (o.loaderOffset % kPicoSector != 0)
    {
        problems.push_back("frameworkOptions.loaderOffset " + std::to_string(o.loaderOffset) +
                           " is not a multiple of 4096 bytes: a boot menu loads programs on a flash sector");
    }
    else if (flashOk && o.loaderOffset >= o.flashSize)
    {
        problems.push_back("frameworkOptions.loaderOffset " + std::to_string(o.loaderOffset) +
                           " is not inside the flash (" + std::to_string(o.flashSize) + " bytes)");
    }

    if (o.stdio != "uart" && o.stdio != "usb" && o.stdio != "both")
    {
        problems.push_back("frameworkOptions.stdio '" + o.stdio + "' is not \"uart\", \"usb\" or \"both\"");
    }

    if (raw.contains("sysClockKhz") &&
        (!PicoIsU32(raw, "sysClockKhz") || o.sysClockKhz < 48000 || o.sysClockKhz > 300000))
    {
        problems.push_back("frameworkOptions.sysClockKhz is not a clock from 48000 (USB needs 48 MHz) to 300000 kHz");
    }
    return problems;
}

std::vector<std::pair<std::string, std::string>> PicoBuilder::DescribePlatform(const PlatformConfig& config) const
{
    const PicoOptions o = OptionsOf(config);
    std::vector<std::pair<std::string, std::string>> rows;
    rows.push_back({ "Chip", PicoChipLabel(o.chip) });
    rows.push_back({ "Board", o.board });
    rows.push_back({ "Flash", std::to_string(o.flashSize / 1024) + " KB: the program, then the game's data" });
    rows.push_back({ "Serial", PicoStdioLabel(o.stdio) });
    if (o.sysClockKhz != 0)
    {
        rows.push_back({ "Clock", std::to_string(o.sysClockKhz / 1000) + " MHz" });
    }
    rows.push_back({ "Toolchain", std::string("Pico SDK ") + kPicoSdkVersion + ", Arm GNU " + kPicoCompilerVersion });
    rows.push_back({ "Output", o.BootMenu() ? kPicoMenuBinName : kPicoUf2Name });
    if (o.BootMenu())
    {
        rows.push_back({ "Boot menu", "program at " + std::to_string(o.loaderOffset / 1024) + " KB" });
    }
    return rows;
}

namespace
{
// The chip, board, flash layout and serial output.
class PicoEditorUI : public IPlatformEditorUI
{
public:
    explicit PicoEditorUI(const PlatformConfig& config)
    {
        const PicoOptions o = PicoBuilder::OptionsOf(config);
        m_Chip = o.chip == "rp2350" ? 1 : 0;
        Copy(m_Board, o.board);
        m_FlashKb = static_cast<int>(o.flashSize / 1024);
        m_Stdio = o.stdio == "usb" ? 1 : o.stdio == "uart" ? 2 : 0;
        m_ClockKhz = static_cast<int>(o.sysClockKhz);
    }

    void Draw() override
    {
        if (SchematicSectionBegin("Raspberry Pi Pico"))
        {
            BeginPropertyContext();
            EditorUI& ui = EditorUI::Get();
            static const char* const kChips[] = { "RP2040", "RP2350" };
            ui.PropertyRow("Chip");
            ui.Combo("##PicoChip", &m_Chip, kChips, 2);
            ui.PropertyRow("Board");
            ui.InputTextWithHint("##PicoBoard", m_Chip == 1 ? "pico2" : "pico", m_Board, sizeof(m_Board));
            ui.PropertyRow("Flash Size (KB)");
            SchematicDragInt("##PicoFlash", &m_FlashKb, 4.0f, 4, 16384);
            const char* const stdio[] = { Tr(kStdio[0]), Tr(kStdio[1]), Tr(kStdio[2]) };
            ui.PropertyRow("Serial Output");
            ui.Combo("##PicoStdio", &m_Stdio, stdio, 3);
            ui.PropertyRow("Clock (kHz)");
            SchematicDragInt("##PicoClock", &m_ClockKhz, 1000.0f, 0, 300000);
            EndPropertyContext();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Palette::Dim, "%s",
                               Tr("The game's data goes right after the program, so either can grow while both fit "
                                  "the flash. With USB serial, Flash can restart a running game to flash it. A clock "
                                  "of 0 keeps the chip's own."));
            ImGui::PopTextWrapPos();
            SchematicSectionEnd();
        }
    }

    void ApplyToConfig(PlatformConfig& config) const override
    {
        static const char* const kStdioIds[] = { "both", "usb", "uart" };
        const uint32_t flash = static_cast<uint32_t>(std::max(m_FlashKb, 4)) / 4u * 4u * 1024u;
        config.SetOption("picoChip", m_Chip == 1 ? "rp2350" : "rp2040");
        const std::string board = Trimmed(m_Board);
        config.SetOption("picoBoard", board.empty() ? (m_Chip == 1 ? "pico2" : "pico") : board);
        config.SetOptionU32("flashSize", flash);
        config.SetOption("stdio", kStdioIds[std::clamp(m_Stdio, 0, 2)]);
        if (m_ClockKhz > 0)
        {
            config.SetOptionU32("sysClockKhz", static_cast<uint32_t>(m_ClockKhz));
        }
    }

    static constexpr const char* kStdio[] = { TrMark("USB and UART"), TrMark("USB"), TrMark("UART") };

private:
    static void Copy(char (&to)[64], const std::string& from)
    {
        const size_t n = std::min(from.size(), sizeof(to) - 1);
        std::memcpy(to, from.data(), n);
        to[n] = '\0';
    }
    static std::string Trimmed(const char* text)
    {
        std::string s = text;
        const size_t first = s.find_first_not_of(" \t");
        const size_t last = s.find_last_not_of(" \t");
        return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
    }

    int m_Chip = 0;
    char m_Board[64] = "";
    int m_FlashKb = 2048;
    int m_OffsetKb = 1024;
    int m_Stdio = 0;
    int m_ClockKhz = 0;
};
}  // namespace

std::unique_ptr<IPlatformEditorUI> PicoBuilder::CreateEditorUI(const PlatformConfig& config) const
{
    return std::make_unique<PicoEditorUI>(config);
}

// ============================================================================
// Build files
// ============================================================================

std::string PicoBuilder::ConfigureCommand() const
{
    const PicoHostTool cmake = m_HostTools.Find("cmake");
    const PicoHostTool ninja = m_HostTools.Find("ninja");
    const PicoHostTool gcc = m_HostTools.Find("host-gcc");
    // MinSizeRel, with the platform's cxxFlags as the optimisation (the
    // generated CMakeLists says so). Every tool is named rather than searched
    // for, so the build uses the ones the toolchain status checked, and the
    // SDK never fetches pioasm, picotool or TinyUSB itself.
    return "\"" + cmake.path + "\" -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel" +
           " \"-DCMAKE_MAKE_PROGRAM=" + ninja.path + "\"" +
           " \"-DPICO_SDK_PATH=" + PicoToolchainPath(kPicoSdkInstallPath) + "\"" +
           " \"-DPICO_TOOLCHAIN_PATH=" + PicoToolchainPath(kPicoCompilerInstallPath) + "\"" +
           " \"-DPICO_TINYUSB_PATH=" + PicoToolchainPath(kPicoTinyUsbInstallPath) + "\"" +
           " \"-Dpioasm_DIR=" + PicoToolchainPath(kPicoPioasmInstallPath) + "/pioasm\"" +
           " \"-Dpicotool_DIR=" + PicoToolchainPath(kPicoPicotoolInstallPath) + "/picotool\"" +
           " \"-DDEKI_GXX16=" + gcc.path + "\"" + EditorSettings::GetCMakeLocationArgs();
}

bool PicoBuilder::GenerateBuildFiles(const std::string& projectPath, const PlatformConfig& config,
                                     const std::vector<std::string>& packageDefines)
{
    const std::vector<std::string> problems = ValidatePlatform(config);
    for (const auto& problem : problems)
    {
        DEKI_LOG_ERROR("PicoBuilder: platform '%s': %s", config.id.c_str(), problem.c_str());
    }
    if (!problems.empty())
    {
        return false;
    }
    const PicoOptions o = OptionsOf(config);

    const fs::path buildDir = BuildDirectoryFor(projectPath, config.id);
    std::error_code ec;
    fs::create_directories(buildDir, ec);
    if (ec)
    {
        DEKI_LOG_ERROR("PicoBuilder: cannot create %s: %s", buildDir.string().c_str(), ec.message().c_str());
        return false;
    }

    // A platform needs its hardware layer whether or not any scene names one
    // of its components, and says which package that is.
    const auto allPackages = CMakeGen::ScanPackageManifests(projectPath);
    std::vector<std::string> allDefines = packageDefines;
    for (const auto& id : config.requiresPackages)
    {
        const auto it = std::find_if(allPackages.begin(), allPackages.end(), [&](const CMakeGen::PackageEntry& pkg)
                                     { return pkg.id == id || pkg.dirName == id; });
        if (it == allPackages.end())
        {
            DEKI_LOG_ERROR("Platform '%s' needs the package '%s', which is not installed. Install it from the Package "
                           "Manager.",
                           config.id.c_str(), id.c_str());
            return false;
        }
        if (!it->buildDefine.empty() &&
            std::find(allDefines.begin(), allDefines.end(), it->buildDefine) == allDefines.end())
        {
            allDefines.push_back(it->buildDefine);
        }
    }
    const auto activeIds = CMakeGen::ResolveActivePackages(allPackages, allDefines, config.Capabilities());
    const std::vector<std::string> capabilityDefines =
        CMakeGen::CapabilityDefinesFor(config.Capabilities(), allPackages);

    std::string transformWhy;
    const std::vector<std::string> transformDefines = CMakeGen::TransformDefines(
        CMakeGen::ResolveProjectTransformWidth(allPackages, CMakeGen::ReadProjectTags(projectPath), &transformWhy));

    // A scene on this target naming a component from a package this target
    // cannot build is a mistake, reported now instead of as a compiler or
    // linker error later.
    const auto conflicts = FindTargetContentConflicts(projectPath, config.id);
    if (!conflicts.empty())
    {
        DEKI_LOG_ERROR("Cannot build for '%s':", config.id.c_str());
        for (const auto& c : conflicts)
        {
            DEKI_LOG_ERROR("  %s", c.Describe().c_str());
        }
        DEKI_LOG_ERROR("  Remove it from this target's content, or use a platform that provides what it needs "
                       "(set_platform_provides).");
        return false;
    }

    const StripPlan strip = ComputeStripPlan(projectPath, config.id);
    for (const auto& w : strip.warnings)
    {
        DEKI_LOG_WARNING("%s", w.c_str());
    }

    // The Pico SDK libraries: the stdlib, then what the active packages name.
    std::vector<std::string> picoLibs = { "pico_stdlib" };
    for (const auto& pkg : allPackages)
    {
        if (activeIds.count(pkg.id) == 0)
        {
            continue;
        }
        const auto mine = pkg.frameworkDeps.find(GetFrameworkId());
        if (mine == pkg.frameworkDeps.end())
        {
            continue;
        }
        for (const auto& dep : mine->second)
        {
            std::string why;
            if (!SafeNames::IsSafeName(dep.name, why))
            {
                DEKI_LOG_ERROR("PicoBuilder: package '%s' names the Pico SDK library '%s': %s", pkg.id.c_str(),
                               dep.name.c_str(), why.c_str());
                return false;
            }
            if (std::find(picoLibs.begin(), picoLibs.end(), dep.name) == picoLibs.end())
            {
                picoLibs.push_back(dep.name);
            }
        }
    }

    // A platform's cxxFlags may not choose the language standard: the build
    // sets C++23, and the engine's headers need it.
    std::vector<std::string> cxxFlags;
    for (const auto& flag : config.cxxFlags)
    {
        if (flag.rfind("-std=", 0) == 0)
        {
            DEKI_LOG_WARNING("PicoBuilder: ignoring %s from platform '%s' cxxFlags: the build sets C++23", flag.c_str(),
                             config.id.c_str());
            continue;
        }
        cxxFlags.push_back(flag);
    }
    if (cxxFlags.empty())
    {
        cxxFlags.push_back("-Os");
    }

    std::ostringstream file;
    file << "# Generated by Deki Editor — do not edit\n";
    file << "# Platform: " << config.displayName << " (Raspberry Pi Pico SDK, " << PicoChipLabel(o.chip) << ")\n";
    file << "cmake_minimum_required(VERSION 3.16)\n\n";

    // Every path comes at configure time (ConfigureCommand): the editor and
    // its toolchains can move, and nothing regenerates this file when they do.
    file << "if(NOT PICO_SDK_PATH OR NOT DEKI_ENGINE_PATH)\n";
    file << "    message(FATAL_ERROR \"PICO_SDK_PATH and DEKI_ENGINE_PATH must be supplied by the Deki Editor. "
            "Build this project from the editor rather than invoking CMake directly.\")\n";
    file << "endif()\n\n";

    file << "set(PICO_BOARD \"" << o.board << "\")\n";
    file << "set(PICO_PLATFORM \"" << (o.chip == "rp2350" ? "rp2350-arm-s" : "rp2040") << "\")\n";
    file << "include(\"${PICO_SDK_PATH}/external/pico_sdk_import.cmake\")\n\n";

    file << "project(" << kPicoProgramName << " C CXX ASM)\n";
    file << "set(CMAKE_C_STANDARD 11)\n";
    file << "set(CMAKE_CXX_STANDARD 23)\n";
    file << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

    file << "# The platform's flags decide the optimisation: MinSizeRel keeps NDEBUG but not\n";
    file << "# CMake's own optimisation flag, and the flags (below) reach the engine and the SDK alike.\n";
    file << "set(CMAKE_C_FLAGS_MINSIZEREL \"-DNDEBUG\")\n";
    file << "set(CMAKE_CXX_FLAGS_MINSIZEREL \"-DNDEBUG\")\n\n";

    file << "# The engine has its own operator new and delete, so the SDK leaves its own out.\n";
    file << "add_compile_definitions(PICO_CXX_DISABLE_ALLOCATION_OVERRIDES=1)\n";
    file << "pico_sdk_init()\n\n";

    file << "add_compile_options(\n";
    file << "    -ffunction-sections\n";
    file << "    -fdata-sections\n";
    file << "    \"$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>\"\n";
    file << "    \"$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>\"\n";
    file << "    \"$<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>\"\n";
    for (const auto& flag : config.cFlags)
    {
        file << "    \"$<$<COMPILE_LANGUAGE:C>:" << CMakeGen::EscapeCMakeString(flag) << ">\"\n";
    }
    for (const auto& flag : cxxFlags)
    {
        file << "    \"$<$<COMPILE_LANGUAGE:CXX>:" << CMakeGen::EscapeCMakeString(flag) << ">\"\n";
    }
    file << ")\n\n";

    file << "# Project root: three levels up from generated/build/<platform>\n";
    file << "get_filename_component(DEKI_PROJECT_PATH \"${CMAKE_SOURCE_DIR}/../../..\" ABSOLUTE)\n\n";

    CMakeGen::EmitEngineCoreSourceCollection(file);
    // When something is stripped, the reflection tables in each package's
    // generated/ (made with every feature present) would refer to components
    // this build does not compile, so this build regenerates its own with the
    // same exclusions, as the ESP-IDF build does.
    file << "message(STATUS \"Deki stripping: " << CMakeGen::EscapeCMakeString(strip.summary) << "\")\n";
    CMakeGen::EmitActivePackageSources(file, allPackages, activeIds, strip.SourceExcludeRegexes(),
                                       strip.AnythingStripped());
    CMakeGen::EmitGeneratedFileSources(file, GetEnginePath(projectPath), allPackages, activeIds);

    // The engine's entry (Deki::Main), which the package's main() calls.
    file << "list(APPEND DEKI_ENGINE_SOURCES \"${DEKI_ENGINE_PATH}/entry/Main.cpp\")\n";
    const std::string packageInitPath =
        CMakeGen::GeneratePackageInitFile(buildDir, allPackages, activeIds, fs::path(GetSourceDirectory(projectPath)));
    file << "\n# Package registration (calls each package's RegisterComponents)\n";
    file << "list(APPEND DEKI_ENGINE_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/"
         << fs::path(packageInitPath).filename().string() << "\")\n";

    file << "\n";
    file << "# =============================================================================\n";
    file << "# Project Sources\n";
    file << "# =============================================================================\n";
    CMakeGen::EmitProjectSourceCollection(file, "${DEKI_PROJECT_PATH}/src", true);

    // One static library: each package's entry defines the same DekiPlugin*
    // functions, which a library only links where something asks for them.
    file << "\n# The engine, the packages and the game, as one library the program links\n";
    file << "add_library(" << kPicoLibraryName << " STATIC ${DEKI_ENGINE_SOURCES} ${PROJECT_SOURCES})\n";

    if (strip.AnythingStripped())
    {
        std::string dirs, tags, prefixes, outdirs;
        for (const auto& pkg : allPackages)
        {
            if (activeIds.count(pkg.id) == 0 || pkg.packagePrefix.empty())
            {
                continue;
            }
            dirs += " \"${DEKI_PROJECT_PATH}/packages/" + pkg.dirName + "\"";
            tags += " \"" + pkg.dirName + "\"";
            prefixes += " \"" + pkg.packagePrefix + "\"";
            outdirs += " \"${CMAKE_BINARY_DIR}/refl/" + pkg.dirName + "/generated\"";
        }
        file << "\n# Reflection regenerated for this configuration (stripping is on)\n";
        // Only a non-empty environment value: an empty normal variable would
        // make the find_program below skip its search (CMP0125).
        file << "if(NOT DEKI_GXX16 AND NOT \"$ENV{DEKI_GXX16}\" STREQUAL \"\")\n"
                "    set(DEKI_GXX16 \"$ENV{DEKI_GXX16}\")\nendif()\n";
        file << "if(NOT DEKI_GXX16)\n    find_program(DEKI_GXX16 NAMES g++-16 g++)\nendif()\n";
        file << "if(NOT DEKI_GXX16)\n"
                "    message(FATAL_ERROR \"Stripping regenerates the reflection tables and needs GCC 16.1+: "
                "set DEKI_GXX16 (variable or environment), or turn stripping off in the Build panel.\")\n"
                "endif()\n";
        file << "include(\"${DEKI_ENGINE_PATH}/cmake/DekiReflectionCodegen.cmake\")\n";
        file << "deki_reflection_codegen(\n";
        file << "    UNIT_PREFIX \"pico\"\n";
        file << "    PACKAGE_DIRS" << dirs << "\n";
        file << "    PACKAGE_TAGS" << tags << "\n";
        file << "    PACKAGE_PREFIXES" << prefixes << "\n";
        file << "    PACKAGE_OUTDIRS" << outdirs << "\n";
        file << "    GENERATOR_SRC \"${DEKI_ENGINE_PATH}/tools/reflection_codegen.cpp\" GXX \"${DEKI_GXX16}\"\n";
        file << "    INCLUDE_DIRS ${DEKI_PACKAGE_INCLUDE_DIRS} \"${DEKI_ENGINE_PATH}/include\" "
                "\"${DEKI_ENGINE_PATH}/third_party\" \"${DEKI_PROJECT_PATH}/packages\" \"${DEKI_PROJECT_PATH}/src\"\n";
        const auto codegenExcludes = strip.CodegenExcludeRegexes();
        if (!codegenExcludes.empty())
        {
            file << "    EXCLUDE_REGEX";
            for (const auto& rx : codegenExcludes)
            {
                file << " \"" << CMakeGen::EscapeCMakeString(rx) << "\"";
            }
            file << "\n";
        }
        // The generator parses the package headers as a host program: the
        // configuration's defines, none of the Pico SDK's.
        file << "    DEFINES \"DEKI_FAST_ATTR=\"";
        for (const auto& define : transformDefines)
        {
            file << " " << define;
        }
        for (const auto& define : allDefines)
        {
            file << " " << define;
        }
        file << ")\n";
        file << "set(_DEKI_PICO_GEN_SRCS \"\")\n";
        file << "foreach(_OUTDIR" << outdirs << ")\n";
        file << "    file(GLOB _g \"${_OUTDIR}/*.gen.cpp\")\n";
        file << "    list(APPEND _DEKI_PICO_GEN_SRCS ${_g})\n";
        file << "endforeach()\n";
        file << "target_sources(" << kPicoLibraryName << " PRIVATE ${_DEKI_PICO_GEN_SRCS})\n";
        // Generate before compiling what is generated, or ninja may compile
        // the previous build's tables first.
        file << "foreach(_TAG" << tags << ")\n";
        file << "    string(MAKE_C_IDENTIFIER \"${_TAG}\" _RC_TAGID)\n";
        file << "    add_dependencies(" << kPicoLibraryName << " ${DEKI_RC_TARGETS_${_RC_TAGID}})\n";
        file << "endforeach()\n";
        file << "foreach(_OUTDIR" << outdirs << ")\n";
        file << "    get_filename_component(_REFL_DIR \"${_OUTDIR}\" DIRECTORY)\n";
        file << "    target_include_directories(" << kPicoLibraryName << " PRIVATE \"${_REFL_DIR}\")\n";
        file << "endforeach()\n";
    }

    CMakeGen::EmitIncludePaths(file, kPicoLibraryName, "${DEKI_PROJECT_PATH}/src");

    file << "\n";
    file << "# =============================================================================\n";
    file << "# Defines (" << config.displayName << ")\n";
    file << "# =============================================================================\n";
    CMakeGen::EmitPlatformDefines(file, kPicoLibraryName, config, allDefines);
    file << "target_compile_definitions(" << kPicoLibraryName << " PRIVATE\n";
    for (const auto& define : PicoDefines(o))
    {
        file << "    \"" << CMakeGen::EscapeCMakeString(define) << "\"\n";
    }
    for (const auto& define : capabilityDefines)
    {
        file << "    " << define << "\n";
    }
    file << ")\n";
    file << "message(STATUS \"Deki transform: " << CMakeGen::EscapeCMakeString(transformWhy) << "\")\n";
    for (const auto& define : transformDefines)
    {
        file << "target_compile_definitions(" << kPicoLibraryName << " PRIVATE " << define << ")\n";
    }
    if (m_BuildOptions.enableLogging)
    {
        file << "target_compile_definitions(" << kPicoLibraryName << " PRIVATE DEKI_LOG_ENABLED)\n";
    }
    if (m_BuildOptions.enableInternalLogging)
    {
        file << "target_compile_definitions(" << kPicoLibraryName << " PRIVATE DEKI_LOG_INTERNAL_ENABLED)\n";
    }

    file << "\n";
    file << "# =============================================================================\n";
    file << "# The program\n";
    file << "# =============================================================================\n";
    file << "set(DEKI_PICO_LIBS";
    for (const auto& lib : picoLibs)
    {
        file << " " << lib;
    }
    file << ")\n";
    // The SDK's libraries compile their sources into whatever links them. The
    // library takes only their headers where the SDK has a _headers target, so
    // those sources are compiled once, into the program.
    file << "foreach(_LIB IN LISTS DEKI_PICO_LIBS)\n";
    file << "    if(TARGET ${_LIB}_headers)\n";
    file << "        target_link_libraries(" << kPicoLibraryName << " PUBLIC ${_LIB}_headers)\n";
    file << "    elseif(TARGET ${_LIB})\n";
    file << "        target_link_libraries(" << kPicoLibraryName << " PRIVATE ${_LIB})\n";
    file << "    else()\n";
    file << "        message(FATAL_ERROR \"A package needs the Pico SDK library '${_LIB}', which Pico SDK "
         << kPicoSdkVersion << " does not have\")\n";
    file << "    endif()\n";
    file << "endforeach()\n\n";

    // The program's main() is the Raspberry Pi package's, in the library; the
    // program needs a source of its own.
    const std::string entry = "/* Generated by Deki Editor - do not edit.\n"
                              " * The program's main() is in the Raspberry Pi package's entry, in " +
                              std::string(kPicoLibraryName) +
                              ".\n"
                              " * This gives the program a source of its own and links that main(). */\n"
                              "extern int main(void);\n"
                              "__attribute__((used)) static int (*const deki_pico_main)(void) = main;\n";
    if (!CMakeGen::WriteIfChanged(buildDir / "game_entry.gen.c", entry))
    {
        DEKI_LOG_ERROR("PicoBuilder: cannot write %s", (buildDir / "game_entry.gen.c").string().c_str());
        return false;
    }
    file << "add_executable(" << kPicoProgramName << " \"${CMAKE_CURRENT_SOURCE_DIR}/game_entry.gen.c\")\n";
    file << "target_link_libraries(" << kPicoProgramName << " PRIVATE " << kPicoLibraryName
         << " ${DEKI_PICO_LIBS} pico_bootsel_via_double_reset)\n";
    file << "target_link_options(" << kPicoProgramName << " PRIVATE -Wl,--gc-sections -Wl,--print-memory-usage";
    for (const auto& flag : config.linkFlags)
    {
        file << " \"" << CMakeGen::EscapeCMakeString(flag) << "\"";
    }
    file << ")\n";
    file << "pico_enable_stdio_uart(" << kPicoProgramName << " " << (o.UartSerial() ? 1 : 0) << ")\n";
    file << "pico_enable_stdio_usb(" << kPicoProgramName << " " << (o.UsbSerial() ? 1 : 0) << ")\n";
    if (o.BootMenu())
    {
        // Started by a boot menu: the program's flash begins at loaderOffset,
        // and it has no boot2, as the menu has set the flash up already and
        // jumps straight to the vector table at that address. Both through the
        // SDK's linker script overrides, so the rest of its script stays the
        // SDK's own.
        const fs::path linkerDir = buildDir / "linker";
        std::error_code mkErr;
        fs::create_directories(linkerDir, mkErr);
        const std::string region = "FLASH(rx) : ORIGIN = 0x10000000 + " + std::to_string(o.loaderOffset) +
                                   ", LENGTH = " + std::to_string(o.flashSize - o.loaderOffset) + "\n";
        const std::string boot2 = "/* Generated by Deki Editor - do not edit.\n"
                                  "   No boot2: a boot menu starts this program, with the flash set up. */\n"
                                  "SECTIONS\n{\n    /DISCARD/ : { *(.boot2) }\n}\n";
        if (!CMakeGen::WriteIfChanged(linkerDir / "pico_flash_region.ld", region) ||
            !CMakeGen::WriteIfChanged(linkerDir / "section_boot2.incl", boot2))
        {
            DEKI_LOG_ERROR("PicoBuilder: cannot write the linker overrides in %s", linkerDir.string().c_str());
            return false;
        }
        file << "pico_add_linker_script_override_path(" << kPicoProgramName
             << " \"${CMAKE_CURRENT_SOURCE_DIR}/linker\" FILES pico_flash_region.ld section_boot2.incl)\n";
    }
    file << "pico_add_extra_outputs(" << kPicoProgramName << ")\n";

    // A changed board or chip needs a fresh cache: the SDK chose the compiler
    // and its flags at the first configure, and would keep them.
    const std::string stamp = "board=" + o.board + "\nchip=" + o.chip + "\nsdk=" + kPicoSdkVersion +
                              "\nloader=" + std::to_string(o.loaderOffset) + "\n";
    {
        std::ifstream in(buildDir / kPicoTargetStamp, std::ios::binary);
        const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in && before != stamp)
        {
            DEKI_LOG_EDITOR("PicoBuilder: the board or chip changed, so %s configures from scratch", config.id.c_str());
            fs::remove(buildDir / "build" / "CMakeCache.txt", ec);
            fs::remove_all(buildDir / "build" / "CMakeFiles", ec);
        }
    }
    if (!CMakeGen::WriteIfChanged(buildDir / kPicoTargetStamp, stamp) ||
        !CMakeGen::WriteIfChanged(buildDir / "CMakeLists.txt", file.str()))
    {
        DEKI_LOG_ERROR("PicoBuilder: cannot write the build files in %s", buildDir.string().c_str());
        return false;
    }
    return true;
}

// ============================================================================
// The game's data and the UF2
// ============================================================================

bool PicoBuilder::MakeGameDataImage(const fs::path& dir, std::vector<uint8_t>& image, std::string& error)
{
    std::vector<Deki::FileImage::File> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
        {
            continue;
        }
        Deki::FileImage::File file;
        file.name = fs::relative(it->path(), dir, ec).generic_string();
        std::ifstream in(it->path(), std::ios::binary);
        if (ec || !in)
        {
            error = "cannot read " + it->path().string();
            return false;
        }
        file.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        files.push_back(std::move(file));
    }
    if (ec)
    {
        error = "cannot list " + dir.string() + ": " + ec.message();
        return false;
    }
    return Deki::FileImage::Write(std::move(files), image, error);
}

uint32_t PicoBuilder::GameDataAt(const PicoOptions& options, size_t programSize)
{
    const size_t end = options.loaderOffset + programSize;
    return static_cast<uint32_t>((end + kPicoSector - 1) / kPicoSector * kPicoSector);
}

bool PicoBuilder::PackFirmware(const fs::path& programBin, const fs::path& payloadDir, const PicoOptions& options,
                               const fs::path& uf2, PicoFlashUse& use, std::string& error)
{
    bool read = false;
    const std::vector<uint8_t> program = PicoReadBytes(programBin, read);
    if (!read || program.empty())
    {
        error = "the build left no program at " + programBin.string();
        return false;
    }
    const uint32_t dataAt = GameDataAt(options, program.size());
    if (dataAt > options.flashSize)
    {
        error = "the program is " + PicoKb(program.size()) + " from " + PicoKb(options.loaderOffset) +
                " and the flash is " + PicoKb(options.flashSize) +
                ". Strip packages the game does not use, or use a board with more flash.";
        return false;
    }

    std::vector<uint8_t> image;
    std::error_code ec;
    if (fs::is_directory(payloadDir, ec) && !MakeGameDataImage(payloadDir, image, error))
    {
        error = "cannot pack the game's data: " + error;
        return false;
    }
    const size_t room = options.flashSize - dataAt;
    if (image.size() > room)
    {
        error = "the game's data is " + PicoKb(image.size()) + " and the flash has " + PicoKb(room) +
                " left for it after the program (" + PicoKb(program.size()) +
                "). Keep assets on an SD card, or use a board with more flash.";
        return false;
    }

    use.program = program.size();
    use.gameData = image.size();
    use.gameDataAt = dataAt;
    use.freeFlash = room - image.size();

    if (options.BootMenu())
    {
        // For the boot menu: one file it writes into the flash from
        // loaderOffset, so the program, then erased flash (0xFF) up to the
        // next sector, then the game's data.
        std::vector<uint8_t> menuBin(dataAt - options.loaderOffset + image.size(), 0xFF);
        std::copy(program.begin(), program.end(), menuBin.begin());
        std::copy(image.begin(), image.end(), menuBin.begin() + (dataAt - options.loaderOffset));
        std::ofstream out(uf2, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(menuBin.data()), static_cast<std::streamsize>(menuBin.size()));
        if (!out)
        {
            error = "cannot write " + uf2.string();
            return false;
        }
        return true;
    }

    std::vector<Uf2Segment> segments = { { kPicoFlashBase, program.data(), program.size() } };
    if (!image.empty())
    {
        segments.push_back({ kPicoFlashBase + dataAt, image.data(), image.size() });
    }
    std::vector<uint8_t> bytes;
    if (!WriteUf2(segments, Uf2FamilyOfChip(options.chip), bytes, error))
    {
        error = "cannot make " + uf2.filename().string() + ": " + error;
        return false;
    }
    if (options.chip == "rp2350")
    {
        // First, as the Pico SDK's own RP2350 files have it.
        const std::vector<uint8_t> errata = Uf2Rp2350ErrataBlock();
        bytes.insert(bytes.begin(), errata.begin(), errata.end());
    }
    std::ofstream out(uf2, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        error = "cannot write " + uf2.string();
        return false;
    }

    return true;
}

// ============================================================================
// Build, clean
// ============================================================================

void PicoBuilder::Build(const std::string& projectPath, BuildOutputCallback outputCallback,
                        BuildProgressCallback progressCallback)
{
    RunOnBuildThread([this, projectPath, outputCallback, progressCallback]()
                     { DoBuild(projectPath, outputCallback, progressCallback); });
}

void PicoBuilder::Clean(const std::string& projectPath, BuildOutputCallback outputCallback,
                        BuildProgressCallback progressCallback)
{
    RunOnBuildThread([this, projectPath, outputCallback, progressCallback]()
                     { DoClean(projectPath, outputCallback, progressCallback); });
}

void PicoBuilder::Report(BuildState state, const std::string& text, float value, const BuildProgressCallback& progress)
{
    SetProgress(state, text, value);
    if (progress)
    {
        progress(GetProgress());
    }
}

void PicoBuilder::Fail(const std::string& error, const BuildOutputCallback& out, const BuildProgressCallback& progress)
{
    if (out)
    {
        out(error, true);
    }
    SetError(error);
    if (progress)
    {
        progress(GetProgress());
    }
}

int PicoBuilder::Run(const std::string& command, const std::string& workDir, const BuildOutputCallback& out)
{
    return RunShellCommand(
        command, workDir,
        [&out](const std::string& line)
        {
            if (out)
            {
                const bool isError = line.find("error") != std::string::npos ||
                                     line.find("Error") != std::string::npos ||
                                     line.find("FAILED") != std::string::npos;
                out(line, isError);
            }
        },
        &m_CancelRequested);
}

void PicoBuilder::DoBuild(const std::string& projectPath, BuildOutputCallback out, BuildProgressCallback progress)
{
    Report(BuildState::Building, "Checking the toolchain...", 0.0f, progress);
    m_HostTools.Rescan();
    if (!IsToolchainInstalled())
    {
        Fail("The Pico toolchain is not ready: " + GetToolchainStatus(), out, progress);
        return;
    }

    const std::string buildDir = GetBuildDirectory(projectPath);
    if (out)
    {
        out("Building " + m_PlatformConfig.displayName + " in " + buildDir, false);
    }

    Report(BuildState::Building, "Generating build files...", 0.05f, progress);
    if (!GenerateBuildFiles(projectPath, m_PlatformConfig, m_PackageDefines))
    {
        Fail("Could not generate the build files; the Console says why.", out, progress);
        return;
    }

    Report(BuildState::Building, "Configuring...", 0.1f, progress);
    int code = Run(ConfigureCommand(), buildDir, out);
    if (m_CancelRequested)
    {
        Fail("Build cancelled", out, progress);
        return;
    }
    if (code != 0)
    {
        Fail("CMake configure failed (exit code " + std::to_string(code) + "); the lines above say why.", out,
             progress);
        return;
    }

    Report(BuildState::Building, "Compiling...", 0.3f, progress);
    code = Run("\"" + m_HostTools.Find("cmake").path + "\" --build build", buildDir, out);
    if (m_CancelRequested)
    {
        Fail("Build cancelled", out, progress);
        return;
    }
    if (code != 0)
    {
        Fail("The build failed (exit code " + std::to_string(code) + "); the lines above say why.", out, progress);
        return;
    }

    // Checked as well as the exit code: a build that ran nothing can still
    // exit 0, and that is never a success.
    const fs::path elf = fs::path(buildDir) / "build" / (std::string(kPicoProgramName) + ".elf");
    const fs::path bin = fs::path(buildDir) / "build" / (std::string(kPicoProgramName) + ".bin");
    std::error_code ec;
    if (!fs::is_regular_file(elf, ec) || !fs::is_regular_file(bin, ec))
    {
        Fail("The build exited without an error but left no " + elf.filename().string() + " and " +
                 bin.filename().string() + " in " + (fs::path(buildDir) / "build").string() +
                 ". Clean the build and build again.",
             out, progress);
        return;
    }

    Report(BuildState::Building, "Packing the game's data...", 0.95f, progress);
    const PicoOptions options = OptionsOf(m_PlatformConfig);
    const fs::path payload = GetBootPayloadDirectory(projectPath);
    if (!fs::is_directory(payload, ec) && out)
    {
        out("No game data at " + payload.string() + ": the UF2 holds only the program.", true);
    }
    const fs::path uf2 = fs::path(buildDir) / (options.BootMenu() ? kPicoMenuBinName : kPicoUf2Name);
    // The other kind of output, from a build before the platform changed, is
    // not this build's.
    fs::remove(fs::path(buildDir) / (options.BootMenu() ? kPicoUf2Name : kPicoMenuBinName), ec);
    PicoFlashUse use;
    std::string error;
    if (!PackFirmware(bin, payload, options, uf2, use, error))
    {
        Fail("Could not make " + uf2.filename().string() + ": " + error, out, progress);
        return;
    }
    if (out)
    {
        out("Program: " + PicoKb(use.program) + " (at " + PicoHex(kPicoFlashBase + options.loaderOffset) + ")", false);
        out("Game data: " + PicoKb(use.gameData) + " (at " + PicoHex(kPicoFlashBase + use.gameDataAt) + ")", false);
        out("Free flash: " + PicoKb(use.freeFlash) + " of " + PicoKb(options.flashSize), false);
        if (options.BootMenu())
        {
            out("Firmware: " + uf2.generic_string() +
                    ", for the board's boot menu. Copy it into the firmware folder of the board's SD card and "
                    "choose it in the menu.",
                false);
        }
        else
        {
            out("Firmware: " + uf2.generic_string() +
                    ". Flash copies it onto the Pico; or hold BOOTSEL while plugging the Pico in and copy it onto "
                    "the drive that appears.",
                false);
        }
    }
    Report(BuildState::Completed, "Build succeeded", 1.0f, progress);
}

void PicoBuilder::DoClean(const std::string& projectPath, BuildOutputCallback out, BuildProgressCallback progress)
{
    Report(BuildState::Building, "Cleaning...", 0.0f, progress);
    const fs::path buildDir = GetBuildDirectory(projectPath);
    std::error_code ec;
    for (const fs::path& path : { buildDir / "build", buildDir / kPicoUf2Name, buildDir / kPicoMenuBinName })
    {
        if (!fs::exists(path, ec))
        {
            continue;
        }
        fs::remove_all(path, ec);
        if (ec)
        {
            Fail("Clean failed: cannot remove " + path.string() + ": " + ec.message(), out, progress);
            return;
        }
        if (out)
        {
            out("Removed: " + path.string(), false);
        }
    }
    Report(BuildState::Completed, "Clean succeeded", 1.0f, progress);
}

// ============================================================================
// Deploy: flash the UF2
// ============================================================================

std::vector<DeployTarget> PicoBuilder::DeployTargetsFrom(const std::vector<PicoBootDrive>& drives,
                                                         const std::vector<std::string>& ports, bool usbSerial)
{
    std::vector<DeployTarget> targets;
    for (const auto& drive : drives)
    {
        targets.push_back({ drive.root, "Pico in BOOTSEL mode (" + drive.root +
                                            (drive.boardId.empty() ? "" : " " + drive.boardId) + ")" });
    }
    // A running game restarts into BOOTSEL from its USB serial port only.
    if (usbSerial)
    {
        for (const auto& port : ports)
        {
            targets.push_back({ port, port + " (a running game: restarts it into BOOTSEL)" });
        }
    }
    return targets;
}

std::vector<DeployTarget> PicoBuilder::EnumerateDeployTargets() const
{
    // A boot-menu build goes onto the SD card, not over BOOTSEL.
    if (OptionsOf(m_PlatformConfig).BootMenu())
    {
        return {};
    }
    return DeployTargetsFrom(FindPicoBootDrives(), EnumerateSerialPorts(), OptionsOf(m_PlatformConfig).UsbSerial());
}

void PicoBuilder::Deploy(const std::string& projectPath, const std::string& deployTargetId,
                         BuildOutputCallback outputCallback, BuildProgressCallback progressCallback)
{
    if (OptionsOf(m_PlatformConfig).BootMenu())
    {
        Fail("This platform's game starts from the board's boot menu: copy " +
                 (fs::path(GetBuildDirectory(projectPath)) / kPicoMenuBinName).generic_string() +
                 " into the firmware folder of its SD card, then choose it in the menu.",
             outputCallback, progressCallback);
        return;
    }
    // Only a listed drive or port: an id from somewhere else could name any
    // folder or device.
    const std::vector<PicoBootDrive> drives = FindPicoBootDrives();
    std::string problem;
    const PicoBootDrive* drive = nullptr;
    std::string port;
    if (deployTargetId.empty())
    {
        if (drives.size() == 1)
        {
            drive = &drives.front();
        }
        else if (drives.empty())
        {
            problem = "No Pico in BOOTSEL mode found. Hold BOOTSEL while plugging the Pico in, or choose the serial "
                      "port of a Pico running a Deki game.";
        }
        else
        {
            problem = "More than one Pico is in BOOTSEL mode; choose one:";
            for (const auto& d : drives)
            {
                problem += " " + d.root;
            }
        }
    }
    else
    {
        const auto it = std::find_if(drives.begin(), drives.end(),
                                     [&](const PicoBootDrive& d) { return NamesSameDrive(deployTargetId, d.root); });
        if (it != drives.end())
        {
            drive = &*it;
        }
        else
        {
            const std::vector<std::string> ports = EnumerateSerialPorts();
            std::string why;
            if (std::find(ports.begin(), ports.end(), deployTargetId) != ports.end() &&
                SafeNames::IsSafeSerialPort(deployTargetId, why))
            {
                port = deployTargetId;
            }
            else
            {
                problem = "'" + deployTargetId +
                          "' is not a Pico in BOOTSEL mode (a drive with INFO_UF2.TXT) or a serial port.";
            }
        }
    }

    const PicoOptions options = OptionsOf(m_PlatformConfig);
    if (problem.empty() && drive != nullptr)
    {
        const std::string chip = PicoChipOfBoardId(drive->boardId);
        if (!chip.empty() && chip != options.chip)
        {
            problem = "The Pico on " + drive->root + " is an " + PicoChipLabel(chip) + " and this build is for an " +
                      PicoChipLabel(options.chip) + "; it would not run there.";
        }
    }
    if (problem.empty() && !port.empty() && !options.UsbSerial())
    {
        problem = "This platform's game has no USB serial (stdio is \"uart\"), so it cannot be restarted from " + port +
                  ". Hold BOOTSEL while plugging the Pico in, and flash its drive.";
    }

    if (!problem.empty())
    {
        // Reported through the same path as a deploy that started, so the
        // Build panel and build_status show it.
        RunOnBuildThread([this, problem, outputCallback, progressCallback]()
                         { Fail(problem, outputCallback, progressCallback); });
        return;
    }
    if (drive != nullptr)
    {
        const PicoBootDrive chosen = *drive;
        RunOnBuildThread([this, projectPath, chosen, outputCallback, progressCallback]()
                         { DoFlashDrive(projectPath, chosen, outputCallback, progressCallback); });
        return;
    }
    RunOnBuildThread([this, projectPath, port, outputCallback, progressCallback]()
                     { DoFlashOverSerial(projectPath, port, outputCallback, progressCallback); });
}

void PicoBuilder::DoFlashDrive(const std::string& projectPath, const PicoBootDrive& drive, BuildOutputCallback out,
                               BuildProgressCallback progress)
{
    const fs::path uf2 = fs::path(GetBuildDirectory(projectPath)) / kPicoUf2Name;
    Report(BuildState::Deploying, "Flashing " + drive.root + "...", 0.0f, progress);
    if (out)
    {
        out("Copying " + uf2.generic_string() + " onto " + drive.root, false);
    }
    std::string error;
    const bool ok = CopyUf2ToDrive(
        uf2, drive.root, [&](float value) { Report(BuildState::Deploying, "Flashing " + drive.root, value, progress); },
        &m_CancelRequested, error);
    if (!ok)
    {
        Fail("Flash failed: " + error, out, progress);
        return;
    }
    if (out)
    {
        out("Flashed. The Pico writes it into its flash and starts the game.", false);
    }
    Report(BuildState::Completed, "Flashed " + drive.root, 1.0f, progress);
}

void PicoBuilder::DoFlashOverSerial(const std::string& projectPath, const std::string& port, BuildOutputCallback out,
                                    BuildProgressCallback progress)
{
    std::error_code ec;
    const fs::path uf2 = fs::path(GetBuildDirectory(projectPath)) / kPicoUf2Name;
    if (!fs::is_regular_file(uf2, ec))
    {
        Fail("Flash failed: " + uf2.string() + " is not there; build first.", out, progress);
        return;
    }

    Report(BuildState::Deploying, "Restarting the Pico on " + port + " into BOOTSEL...", 0.0f, progress);
    const std::vector<PicoBootDrive> before = FindPicoBootDrives();
    std::string error;
    if (!TouchPortAt1200Baud(port, error))
    {
        Fail("Flash failed: " + error, out, progress);
        return;
    }
    if (out)
    {
        out("Asked the game on " + port + " to restart into BOOTSEL; waiting for its drive...", false);
    }

    std::string root;
    const auto start = std::chrono::steady_clock::now();
    while (root.empty())
    {
        if (m_CancelRequested)
        {
            Fail("Flash cancelled", out, progress);
            return;
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(kPicoBootselWaitMs))
        {
            Fail("No Pico drive appeared within " + std::to_string(kPicoBootselWaitMs / 1000) + " seconds after " +
                     port +
                     " was asked to restart. Is it a Pico running a Deki game with USB serial? Hold BOOTSEL "
                     "while plugging it in, and flash its drive.",
                 out, progress);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        root = PickNewBootDrive(before, FindPicoBootDrives());
    }

    for (const auto& drive : FindPicoBootDrives())
    {
        if (NamesSameDrive(drive.root, root))
        {
            const std::string chip = PicoChipOfBoardId(drive.boardId);
            const std::string want = OptionsOf(m_PlatformConfig).chip;
            if (!chip.empty() && chip != want)
            {
                Fail("The Pico on " + port + " is an " + PicoChipLabel(chip) + " and this build is for an " +
                         PicoChipLabel(want) + ". It is now in BOOTSEL mode on " + root + ".",
                     out, progress);
                return;
            }
            DoFlashDrive(projectPath, drive, out, progress);
            return;
        }
    }
    Fail("The Pico's drive " + root + " went away before it could be flashed.", out, progress);
}

// The texts above, marked so the translation table has them.
[[maybe_unused]] static constexpr const char* kPicoEditorTexts[] = {
    TrMark("Raspberry Pi Pico"),
    TrMark("Chip"),
    TrMark("Board"),
    TrMark("Flash Size (KB)"),
    TrMark("Game Data At (KB)"),
    TrMark("Serial Output"),
    TrMark("Clock (kHz)"),
    TrMark("The program goes below Game Data At and the game's data from there to the end of the flash; both are "
           "rounded to 4 KB. With USB serial, Flash can restart a running game to flash it. A clock of 0 keeps the "
           "chip's own."),
};

}  // namespace DekiEditor

// ---------------------------------------------------------------------------
// Builder plugin entry points
//
// In editor/, so the editor-side package DLL exports them and a game build,
// which leaves editor/ out, does not.
// ---------------------------------------------------------------------------

#include <deki-editor/build/BuilderPlugin.h>

extern "C"
{
    DEKI_BUILDER_API const DekiBuilderAbi* DekiBuilderGetAbi(void)
    {
        static const DekiBuilderAbi kAbi =
            DekiBuilderThisAbi(static_cast<uint32_t>(sizeof(DekiEditor::PlatformConfig)),
                               static_cast<uint32_t>(sizeof(DekiEditor::CMakeGen::PackageEntry)));
        return &kAbi;
    }

    DEKI_BUILDER_API const char* DekiBuilderGetName(void)
    {
        return "Raspberry Pi Pico Builder";
    }

    DEKI_BUILDER_API const char* DekiBuilderGetVersion(void)
    {
        return "1.0.0";
    }

    DEKI_BUILDER_API int DekiBuilderGetBuilderCount(void)
    {
        return 1;
    }

    DEKI_BUILDER_API DekiEditor::ITargetBuilder* DekiBuilderCreateBuilder(int index)
    {
        return index == 0 ? new DekiEditor::PicoBuilder() : nullptr;
    }

    DEKI_BUILDER_API void DekiBuilderDestroyBuilder(DekiEditor::ITargetBuilder* builder)
    {
        delete builder;  // in THIS module: its vtable and operator delete live here
    }

}  // extern "C"
