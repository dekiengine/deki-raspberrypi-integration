#include "PicoToolchain.h"

#include <deki-editor/Localization.h>
#include <deki-editor/ShellCommand.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace DekiEditor
{

bool PicoHostHasPinnedToolchain()
{
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return true;
#else
    return false;
#endif
}

namespace
{

// One pinned archive: what it is, where it comes from and its digest.
struct PicoPin
{
    const char* id = "";
    const char* displayName = "";
    const char* version = "";
    const char* installPath = "";
    const char* url = "";
    const char* sha256 = "";
    // The file that is there once it is installed, under installPath.
    const char* detect = "";
    const char* tooltip = "";
};

// The archives. Each digest was checked against the download. The Arm
// toolchain's zip has no folder of its own inside (bin/, lib/ at the top) and
// the tools' zips have a .keep beside their folder, so those unpack as they
// are; the SDK's tarball and TinyUSB's zip wrap everything in one folder,
// which the installer lifts up.
const PicoPin kPicoPins[] = {
    {
        kPicoCompilerComponent,
        "Arm GNU Toolchain (arm-none-eabi)",
        kPicoCompilerVersion,
        kPicoCompilerInstallPath,
        "https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/"
        "arm-gnu-toolchain-15.2.rel1-mingw-w64-x86_64-arm-none-eabi.zip",
        "7936cac895611023ffb22a64b8e426098c7104cb689778c1894572ca840b9ece",
        "bin/arm-none-eabi-g++.exe",
        "The compiler for Raspberry Pi Pico boards (bare-metal Arm), from Arm",
    },
    {
        kPicoSdkComponent,
        "Pico SDK",
        kPicoSdkVersion,
        kPicoSdkInstallPath,
        "https://github.com/raspberrypi/pico-sdk/releases/download/2.3.1/pico-sdk-2.3.1.tar.gz",
        "4cd9f1f36feb34b6853ca866b5e0d977e3d344a361b9b7c94ae383693303591a",
        "pico_sdk_init.cmake",
        "Raspberry Pi's SDK for the RP2040 and RP2350",
    },
    {
        kPicoTinyUsbComponent,
        "TinyUSB",
        kPicoTinyUsbVersion,
        kPicoTinyUsbInstallPath,
        "https://codeload.github.com/hathach/tinyusb/zip/86ad6e56c1700e85f1c5678607a762cfe3aa2f47",
        "3011c90c128988012b553e5d2f0a90bc0b64046591c964bc1f9f6659edcd7e4b",
        "src/tusb.h",
        "The USB stack the Pico SDK uses for USB serial, at the version Pico SDK 2.3.1 pins",
    },
    {
        kPicoPioasmComponent,
        "pioasm",
        kPicoSdkVersion,
        kPicoPioasmInstallPath,
        "https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.3.1-0/pico-sdk-tools-2.3.1-x64-win.zip",
        "c5d18dae371eb088da61a038ccf56a23fbd19171a319461efebeae1646c06ade",
        "pioasm/pioasmConfig.cmake",
        "Assembles the programs of the Pico's PIO blocks, from Raspberry Pi",
    },
    {
        kPicoPicotoolComponent,
        "picotool",
        kPicoSdkVersion,
        kPicoPicotoolInstallPath,
        "https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.3.1-0/picotool-2.3.1-x64-win.zip",
        "68730be0813f8f35be2cca147cf7f1572662d5dcf0f5ba468e02a6dd9e85db2b",
        "picotool/picotoolConfig.cmake",
        "Raspberry Pi's tool the Pico SDK runs on a finished program",
    },
};

constexpr const char* kPicoNotPinnedTooltip =
    "No archive is pinned for this host yet; Raspberry Pi Pico builds need a Windows x86_64 machine for now";

}  // namespace

std::string PicoToolchainDefinitionJson()
{
    nlohmann::json components = nlohmann::json::array();
    for (const PicoPin& pin : kPicoPins)
    {
        nlohmann::json c = {
            { "id", pin.id },
            { "displayName", pin.displayName },
            { "required", true },
            { "installPath", pin.installPath },
            { "versionCheck", { { "type", "static" }, { "version", pin.version } } },
        };
        if (PicoHostHasPinnedToolchain())
        {
            c["canInstall"] = true;
            c["fallback"] = { { "version", pin.version }, { "url", pin.url }, { "sha256", pin.sha256 } };
            c["detection"] = { { "windows", std::string("{installPath}/") + pin.detect } };
            c["tooltip"] = pin.tooltip;
        }
        else
        {
            // Nothing to download and nothing to detect: taking whatever is
            // on the machine would build with tools nobody checked.
            c["canInstall"] = false;
            c["detection"] = nlohmann::json::object();
            c["tooltip"] = kPicoNotPinnedTooltip;
        }
        components.push_back(c);
    }

    nlohmann::json def = {
        { "id", "pico" },
        { "name", "Raspberry Pi Pico" },
        { "family", "raspberrypi" },
        { "components", components },
    };
    return def.dump();
}

// The tooltips above, marked so the translation table has them. The Platforms
// panel shows them through Tr().
[[maybe_unused]] static constexpr const char* kPicoToolchainTooltips[] = {
    TrMark("The compiler for Raspberry Pi Pico boards (bare-metal Arm), from Arm"),
    TrMark("Raspberry Pi's SDK for the RP2040 and RP2350"),
    TrMark("The USB stack the Pico SDK uses for USB serial, at the version Pico SDK 2.3.1 pins"),
    TrMark("Assembles the programs of the Pico's PIO blocks, from Raspberry Pi"),
    TrMark("Raspberry Pi's tool the Pico SDK runs on a finished program"),
    TrMark("No archive is pinned for this host yet; Raspberry Pi Pico builds need a Windows x86_64 machine for now"),
};

// ============================================================================
// Host tools
// ============================================================================

namespace
{
#ifdef _WIN32
constexpr const char* kPicoExe = ".exe";
#else
constexpr const char* kPicoExe = "";
#endif
}  // namespace

std::string PicoFindOnPath(const std::string& name)
{
    const char* path = std::getenv("PATH");
    if (path == nullptr)
    {
        return "";
    }
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    const std::string file = name + kPicoExe;
    std::stringstream dirs(path);
    std::string dir;
    while (std::getline(dirs, dir, separator))
    {
        if (dir.empty())
        {
            continue;
        }
        std::error_code ec;
        const fs::path candidate = fs::path(dir) / file;
        if (fs::is_regular_file(candidate, ec))
        {
            return candidate.generic_string();
        }
    }
    return "";
}

namespace
{
// The first line a program prints for `args`, or empty when it cannot run.
std::string PicoFirstLine(const std::string& program, const std::string& args)
{
    std::string first;
    const int code = RunShellCommand("\"" + program + "\" " + args, "",
                                     [&first](const std::string& line)
                                     {
                                         if (first.empty())
                                         {
                                             first = line;
                                         }
                                     });
    return code == 0 ? first : std::string();
}

// Where a tool may be when it is not on PATH: an editor started from the
// Start menu does not see a shell's PATH.
std::vector<std::string> PicoKnownLocations(const std::string& name)
{
    std::vector<std::string> out;
#ifdef _WIN32
    out.push_back("C:/msys64/mingw64/bin/" + name + kPicoExe);
    if (name == "cmake")
    {
        out.push_back("C:/Program Files/CMake/bin/cmake.exe");
    }
#else
    out.push_back("/usr/local/bin/" + name);
    out.push_back("/usr/bin/" + name);
#endif
    return out;
}

std::string PicoLocate(const std::string& name)
{
    std::string found = PicoFindOnPath(name);
    if (!found.empty())
    {
        return found;
    }
    for (const auto& candidate : PicoKnownLocations(name))
    {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec))
        {
            return candidate;
        }
    }
    return "";
}
}  // namespace

int PicoHostTools::GccMajor(const std::string& dumpVersion)
{
    size_t i = 0;
    int major = 0;
    while (i < dumpVersion.size() && dumpVersion[i] >= '0' && dumpVersion[i] <= '9')
    {
        major = major * 10 + (dumpVersion[i] - '0');
        ++i;
    }
    return i == 0 ? -1 : major;
}

std::vector<PicoHostTool> PicoHostTools::Scan()
{
    std::vector<PicoHostTool> tools;

    PicoHostTool cmake;
    cmake.id = "cmake";
    cmake.displayName = "CMake";
    cmake.path = PicoLocate("cmake");
    cmake.ok = !cmake.path.empty();
    if (cmake.ok)
    {
        // "cmake version 3.31.2"
        const std::string line = PicoFirstLine(cmake.path, "--version");
        cmake.version = line.rfind("cmake version ", 0) == 0 ? line.substr(14) : line;
    }
    else
    {
        cmake.problem = "CMake was not found. Install CMake 3.16 or newer (cmake.org, or in MSYS2: pacman -S "
                        "mingw-w64-x86_64-cmake), put it on PATH and restart the editor.";
    }
    tools.push_back(cmake);

    PicoHostTool ninja;
    ninja.id = "ninja";
    ninja.displayName = "Ninja";
    ninja.path = PicoLocate("ninja");
    if (ninja.path.empty() && !cmake.path.empty())
    {
        // Often installed beside CMake.
        std::error_code ec;
        const fs::path beside = fs::path(cmake.path).parent_path() / (std::string("ninja") + kPicoExe);
        if (fs::is_regular_file(beside, ec))
        {
            ninja.path = beside.generic_string();
        }
    }
    ninja.ok = !ninja.path.empty();
    if (ninja.ok)
    {
        ninja.version = PicoFirstLine(ninja.path, "--version");
    }
    else
    {
        ninja.problem = "Ninja was not found. Install it (ninja-build.org, or in MSYS2: pacman -S "
                        "mingw-w64-x86_64-ninja), put it on PATH and restart the editor.";
    }
    tools.push_back(ninja);

    // The reflection codegen is compiled with -freflection, which GCC has
    // from 16. The generated CMake looks in the same places, in this order,
    // when it is not told which compiler to use; the builder always tells it.
    PicoHostTool gcc;
    gcc.id = "host-gcc";
    gcc.displayName = "Host GCC 16+ (reflection codegen)";
    std::vector<std::string> candidates;
#ifdef _WIN32
    candidates.push_back("C:/msys64/mingw64/bin/g++.exe");
#endif
    for (const char* name : { "g++-16", "g++" })
    {
        const std::string found = PicoFindOnPath(name);
        if (!found.empty())
        {
            candidates.push_back(found);
        }
    }
    std::string tooOld;
    for (const auto& candidate : candidates)
    {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec))
        {
            continue;
        }
        const std::string version = PicoFirstLine(candidate, "-dumpfullversion");
        const int major = GccMajor(version);
        if (major >= 16)
        {
            gcc.path = candidate;
            gcc.version = version;
            gcc.ok = true;
            break;
        }
        if (tooOld.empty() && major > 0)
        {
            tooOld = candidate + " is GCC " + version;
        }
    }
    if (!gcc.ok)
    {
        gcc.problem = std::string("The reflection step needs GCC 16 or newer on this machine") +
                      (tooOld.empty() ? " and none was found" : " (" + tooOld + ")") +
#ifdef _WIN32
                      ". Install MSYS2's GCC (pacman -S mingw-w64-x86_64-gcc) into C:/msys64, or put a GCC 16 "
                      "g++ on PATH, and restart the editor.";
#else
                      ". Install g++-16 and restart the editor.";
#endif
    }
    tools.push_back(gcc);

    return tools;
}

std::vector<PicoHostTool> PicoHostTools::Get() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Scanned)
    {
        m_Tools = Scan();
        m_Scanned = true;
    }
    return m_Tools;
}

void PicoHostTools::Rescan()
{
    std::vector<PicoHostTool> tools = Scan();
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Tools = std::move(tools);
    m_Scanned = true;
}

PicoHostTool PicoHostTools::Find(const std::string& id) const
{
    for (const auto& tool : Get())
    {
        if (tool.id == id)
        {
            return tool;
        }
    }
    return {};
}

}  // namespace DekiEditor
