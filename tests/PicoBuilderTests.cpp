// What the Raspberry Pi Pico builder writes and does without a Pico, the Pico
// SDK or a compiler: the toolchain it pins, the platform settings it takes
// and refuses, the Pico SDK project it generates, the UF2 it packs from a
// program and the game's data, and the Picos it offers to flash (fake drives
// here).
//
// A mistake here shows up on the board as a game that does not start, data
// written over the program, or a file copied onto the wrong drive, so it is
// worth pinning.

#include <gtest/gtest.h>

#include "editor/PicoBuilder.h"
#include "editor/PicoDevices.h"
#include "editor/PicoToolchain.h"
#include "editor/Uf2Writer.h"

#include <deki-editor/build/BuilderDefinition.h>
#include <deki-editor/build/BuilderDispatch.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/FileImage.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace DekiEditor;
namespace fs = std::filesystem;

namespace
{

// The platform the tests build: a Pico-shaped board with an RP2040 and 2 MB
// of flash.
constexpr const char* kTestPico = R"({
    "id": "test_pico",
    "displayName": "Test Pico",
    "family": "raspberrypi",
    "framework": "pico",
    "frameworkOptions": {"picoBoard": "pico", "picoChip": "rp2040", "flashSize": 2097152, "stdio": "both"},
    "externalMemorySize": 0,
    "screenWidth": 320,
    "screenHeight": 320,
    "colorFormat": "RGB565",
    "provides": ["filesystem", "pico", "spi", "i2c", "external-storage"],
    "requiresPackages": ["deki-raspberrypi-integration"],
    "defines": ["DEKI_PICO"]
})";

PlatformConfig TestPico()
{
    PlatformConfig config;
    EXPECT_TRUE(PlatformFromJson(kTestPico, config));
    return config;
}

std::string Read(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

void Write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

void WriteBytes(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

bool HasProblem(const std::vector<std::string>& problems, const std::string& part)
{
    return std::any_of(problems.begin(), problems.end(), [&](const std::string& p) { return Contains(p, part); });
}

// A folder of its own per test, removed afterwards.
class TempDir
{
public:
    TempDir()
    {
        m_Path = fs::temp_directory_path() /
                 ("deki-pico-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(m_Path);
        fs::create_directories(m_Path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_Path, ec);
    }
    const fs::path& Path() const { return m_Path; }

private:
    fs::path m_Path;
};

// Only the framework matters to the dispatcher.
class StubBuilder : public ITargetBuilder
{
public:
    explicit StubBuilder(std::string framework)
        : m_Framework(std::move(framework))
    {
    }

    const char* GetName() const override { return "Stub"; }
    std::string GetFrameworkId() const override { return m_Framework; }
    void Build(const std::string&, BuildOutputCallback, BuildProgressCallback) override {}
    bool SupportsDeploy() const override { return false; }
    void Clean(const std::string&, BuildOutputCallback, BuildProgressCallback) override {}
    void Cancel() override {}
    bool IsBuilding() const override { return false; }
    BuildState GetState() const override { return BuildState::Idle; }
    BuildProgress GetProgress() const override { return {}; }
    void SetBuildOptions(const BuildOptions&) override {}
    const BuildOptions& GetBuildOptions() const override { return m_Options; }
    void SetPlatformConfig(const PlatformConfig&) override {}
    void ClearPlatformConfig() override {}
    void SetPackageDefines(const std::vector<std::string>&) override {}
    bool IsToolchainInstalled() const override { return true; }
    std::string GetToolchainStatus() const override { return "stub"; }
    std::string GetEnginePath(const std::string&) const override { return ""; }
    bool GenerateBuildFiles(const std::string&, const PlatformConfig&, const std::vector<std::string>&) override
    {
        return true;
    }
    std::string GetBuildDirectory(const std::string&) const override { return ""; }
    std::string GetBootPayloadDirectory(const std::string&) const override { return ""; }
    std::string GetPlatformKey() const override { return m_Framework; }

private:
    std::string m_Framework;
    BuildOptions m_Options;
};

}  // namespace

// --- The toolchain ---

TEST(PicoBuilder, TheToolchainDefinitionPinsEveryComponent)
{
    BuilderDefinition def;
    std::string error;
    ASSERT_TRUE(ParseBuilderDefinition(PicoToolchainDefinitionJson(), def, error)) << error;
    EXPECT_EQ(def.id, "pico");
    EXPECT_EQ(def.family, "raspberrypi");

    std::vector<std::string> ids;
    std::set<std::string> paths;
    for (const ComponentDef& c : def.components)
    {
        ids.push_back(c.id);
        EXPECT_TRUE(c.required) << c.id;
        EXPECT_TRUE(paths.insert(c.installPath).second) << c.id << " shares its folder";
        if (!PicoHostHasPinnedToolchain())
        {
            EXPECT_FALSE(c.canInstall) << c.id;
            continue;
        }
        EXPECT_TRUE(c.canInstall) << c.id;
        EXPECT_EQ(c.fallback.url.rfind("https://", 0), 0u) << c.id;
        EXPECT_EQ(c.sha256.size(), 64u) << c.id;
        EXPECT_TRUE(std::all_of(c.sha256.begin(), c.sha256.end(),
                                [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }))
            << c.id;
        EXPECT_EQ(c.detection.count("windows"), 1u) << c.id;
    }
    const std::vector<std::string> expected = { kPicoCompilerComponent, kPicoSdkComponent, kPicoTinyUsbComponent,
                                                kPicoPioasmComponent, kPicoPicotoolComponent };
    EXPECT_EQ(ids, expected);

    if (PicoHostHasPinnedToolchain())
    {
        const auto find = [&](const std::string& id)
        { return *std::find_if(def.components.begin(), def.components.end(), [&](auto& c) { return c.id == id; }); };
        EXPECT_EQ(find(kPicoCompilerComponent).installPath, "arm-gnu-toolchain/15.2.rel1-arm-none-eabi");
        EXPECT_EQ(find(kPicoCompilerComponent).detection.at("windows"), "{installPath}/bin/arm-none-eabi-g++.exe");
        EXPECT_EQ(find(kPicoCompilerComponent).sha256,
                  "7936cac895611023ffb22a64b8e426098c7104cb689778c1894572ca840b9ece");
        EXPECT_EQ(find(kPicoSdkComponent).installPath, "raspberrypi/pico-sdk-2.3.1");
        EXPECT_EQ(find(kPicoSdkComponent).sha256, "4cd9f1f36feb34b6853ca866b5e0d977e3d344a361b9b7c94ae383693303591a");
        // TinyUSB at the commit Pico SDK 2.3.1's lib/tinyusb points at, in a
        // folder of its own: installing the SDK again clears the SDK's folder.
        const ComponentDef tinyusb = find(kPicoTinyUsbComponent);
        EXPECT_TRUE(Contains(tinyusb.fallback.url, kPicoTinyUsbCommit));
        EXPECT_NE(tinyusb.installPath.rfind(kPicoSdkInstallPath, 0), 0u);
        EXPECT_EQ(find(kPicoPioasmComponent).detection.at("windows"), "{installPath}/pioasm/pioasmConfig.cmake");
        EXPECT_EQ(find(kPicoPicotoolComponent).detection.at("windows"), "{installPath}/picotool/picotoolConfig.cmake");
    }
}

TEST(PicoBuilder, TheHostToolsGccVersionIsReadFromDumpVersion)
{
    EXPECT_EQ(PicoHostTools::GccMajor("16.1.0"), 16);
    EXPECT_EQ(PicoHostTools::GccMajor("15"), 15);
    EXPECT_EQ(PicoHostTools::GccMajor("gcc"), -1);
}

TEST(PicoBuilder, ItConfiguresWithEveryPinnedToolNamed)
{
    PicoBuilder builder;
    const std::string line = builder.ConfigureCommand();
    EXPECT_TRUE(Contains(line, " -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel"));
    EXPECT_TRUE(Contains(line, "-DCMAKE_MAKE_PROGRAM="));
    EXPECT_TRUE(Contains(line, "/raspberrypi/pico-sdk-2.3.1\""));
    EXPECT_TRUE(Contains(line, "-DPICO_TOOLCHAIN_PATH="));
    EXPECT_TRUE(Contains(line, "/arm-gnu-toolchain/15.2.rel1-arm-none-eabi\""));
    EXPECT_TRUE(Contains(line, "-DPICO_TINYUSB_PATH="));
    EXPECT_TRUE(Contains(line, "/raspberrypi/pico-sdk-tools-2.3.1/pioasm\""));
    EXPECT_TRUE(Contains(line, "/raspberrypi/picotool-2.3.1/picotool\""));
    EXPECT_TRUE(Contains(line, "-DDEKI_GXX16="));
    EXPECT_TRUE(Contains(line, "-DDEKI_ENGINE_PATH="));
    EXPECT_FALSE(Contains(line, "\\")) << "CMake paths use forward slashes";
}

// --- The platform ---

TEST(PicoBuilder, TheTestPlatformIsValidAndThePicoBuildersOwn)
{
    const PlatformConfig config = TestPico();
    PicoBuilder pico;
    for (const auto& problem : pico.ValidatePlatform(config))
    {
        ADD_FAILURE() << problem;
    }
    StubBuilder native("native");
    StubBuilder espidf("espidf");
    const std::vector<ITargetBuilder*> all{ &native, &espidf, &pico };
    EXPECT_EQ(SelectBuilder(all, config.framework), &pico);
    EXPECT_EQ(pico.GetFrameworkId(), "pico");
    EXPECT_STREQ(pico.GetDeployLabel(), "Flash");
    EXPECT_TRUE(Contains(pico.GetDescription(), "Pico SDK"));
}

TEST(PicoBuilder, OptionsGetTheirDefaults)
{
    PlatformConfig config = TestPico();
    config.frameworkOptions = R"({"picoChip": "rp2040", "flashSize": 4194304})";
    PicoOptions o = PicoBuilder::OptionsOf(config);
    EXPECT_EQ(o.board, "pico");
    EXPECT_EQ(o.stdio, "both");
    EXPECT_TRUE(o.UsbSerial());
    EXPECT_TRUE(o.UartSerial());
    EXPECT_EQ(o.sysClockKhz, 0u);
    EXPECT_TRUE(PicoBuilder().ValidatePlatform(config).empty());

    config.frameworkOptions = R"({"picoChip": "rp2350", "flashSize": 4194304, "stdio": "uart"})";
    o = PicoBuilder::OptionsOf(config);
    EXPECT_EQ(o.board, "pico2");
    EXPECT_FALSE(o.UsbSerial());
    // The RP2350 is accepted, though only the RP2040 has run on a board.
    EXPECT_TRUE(PicoBuilder().ValidatePlatform(config).empty());
}

TEST(PicoBuilder, BadSettingsAreRefused)
{
    PicoBuilder builder;
    const auto problemsWith = [&](const std::string& options)
    {
        PlatformConfig c = TestPico();
        c.frameworkOptions = options;
        return builder.ValidatePlatform(c);
    };
    EXPECT_TRUE(HasProblem(problemsWith(R"({"flashSize": 2097152})"), "picoChip is not set"));
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "esp32", "flashSize": 2097152})"), "picoChip 'esp32'"));
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "rp2040"})"), "flashSize is not set"));
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "rp2040", "flashSize": "2MB"})"), "flashSize is not set"));
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "rp2040", "flashSize": 2097000})"), "multiple of 4096"));
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "rp2040", "flashSize": 33554432})"), "16 MB"));
    EXPECT_TRUE(
        HasProblem(problemsWith(R"({"picoChip": "rp2040", "flashSize": 2097152, "stdio": "swd"})"), "stdio 'swd'"));
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "rp2040", "flashSize": 2097152, "picoBoard": "pico\"; x"})"),
                           "picoBoard"));
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "rp2040", "flashSize": 2097152, "sysClockKhz": 900000})"),
                           "sysClockKhz"));
    EXPECT_TRUE(problemsWith(R"({"picoChip": "rp2040", "flashSize": 2097152, "sysClockKhz": 200000})").empty());
}

TEST(PicoBuilder, TheDefinesSayChipAndLayout)
{
    PicoOptions o = PicoBuilder::OptionsOf(TestPico());
    std::vector<std::string> defines = PicoBuilder::PicoDefines(o);
    const std::vector<std::string> expected = { "DEKI_PICO", "DEKI_PICO_RP2040", "DEKI_PICO_FLASH_SIZE=2097152",
                                                "DEKI_FAST_ATTR=" };
    EXPECT_EQ(defines, expected);

    o.chip = "rp2350";
    o.sysClockKhz = 150000;
    defines = PicoBuilder::PicoDefines(o);
    EXPECT_EQ(defines[1], "DEKI_PICO_RP2350");
    EXPECT_NE(std::find(defines.begin(), defines.end(), "DEKI_PICO_SYS_CLOCK_KHZ=150000"), defines.end());
}

TEST(PicoBuilder, ThePlatformIsDescribedFromItsOptions)
{
    const auto rows = PicoBuilder().DescribePlatform(TestPico());
    ASSERT_GE(rows.size(), 4u);
    EXPECT_EQ(rows[0], (std::pair<std::string, std::string>{ "Chip", "RP2040" }));
    EXPECT_EQ(rows[1], (std::pair<std::string, std::string>{ "Board", "pico" }));
    EXPECT_EQ(rows[2].second, "2048 KB: the program, then the game's data");
    EXPECT_EQ(rows[3], (std::pair<std::string, std::string>{ "Serial", "USB and UART" }));
    EXPECT_EQ(rows.back(), (std::pair<std::string, std::string>{ "Output", "DekiGame.uf2" }));
}

// --- The generated CMake ---

namespace
{

class PicoBuildFilesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const fs::path p = m_Dir.Path();
        Write(p / "deki.json", R"({"name":"My Game","engine":"deki"})");
        Write(p / "deki-packages.json", R"({"packages":{}})");
        fs::create_directories(p / "src");
        fs::create_directories(p / "assets");
        AddPackage("deki-raspberrypi-integration", "DEKI_PACKAGE_RASPBERRYPI", "[]", R"(["deki-spi"])",
                   R"({"pico": ["pico_stdlib", "pico_multicore", "hardware_dma"]})");
        AddPackage("deki-spi", "DEKI_PACKAGE_SPI", "[]", "[]", R"({"pico": ["hardware_spi"]})");
        AddPackage("deki-sdl3-integration", "DEKI_PACKAGE_SDL3", R"(["sdl3"])", "[]", R"({"native": ["SDL3"]})");
        AddPackage("deki-esp32-integration", "DEKI_PACKAGE_ESP32_HAL", R"(["esp32"])", "[]",
                   R"({"espidf": ["esp_lcd"]})");
    }

    void AddPackage(const std::string& id, const std::string& define, const std::string& platformRequires,
                    const std::string& requiredPackages, const std::string& dependencies)
    {
        const fs::path dir = m_Dir.Path() / "packages" / id;
        Write(dir / "package.json",
              "{\"id\":\"" + id + "\",\"buildDefine\":\"" + define + "\",\"platformRequires\":" + platformRequires +
                  ",\"requires\":" + requiredPackages + ",\"dependencies\":" + dependencies + "}");
        Write(dir / "package.cmake", "set(PACKAGE_PREFIX \"X\")\n");
    }

    fs::path BuildDir(const PlatformConfig& config) const
    {
        return PicoBuilder::BuildDirectoryFor(m_Dir.Path().string(), config.id);
    }

    std::string Generate(const PlatformConfig& config, const std::vector<std::string>& defines,
                         const BuildOptions& options = {})
    {
        m_Builder.SetBuildOptions(options);
        EXPECT_TRUE(m_Builder.GenerateBuildFiles(m_Dir.Path().string(), config, defines));
        return Read(BuildDir(config) / "CMakeLists.txt");
    }

    PicoBuilder m_Builder;
    TempDir m_Dir;
};

}  // namespace

TEST_F(PicoBuildFilesTest, ItIsAPicoSdkProject)
{
    const std::string cmake = Generate(TestPico(), {});
    EXPECT_TRUE(Contains(cmake, "set(PICO_BOARD \"pico\")\n"));
    EXPECT_TRUE(Contains(cmake, "set(PICO_PLATFORM \"rp2040\")\n"));
    EXPECT_TRUE(Contains(cmake, "include(\"${PICO_SDK_PATH}/external/pico_sdk_import.cmake\")\n"));
    EXPECT_TRUE(Contains(cmake, "project(DekiGame C CXX ASM)\n"));
    EXPECT_TRUE(Contains(cmake, "set(CMAKE_CXX_STANDARD 23)\n"));
    // The SDK reads the board and platform before it is imported, and the
    // engine's operator new must be settled before the SDK sets up.
    EXPECT_LT(cmake.find("set(PICO_BOARD"), cmake.find("pico_sdk_import.cmake"));
    EXPECT_LT(cmake.find("pico_sdk_import.cmake"), cmake.find("project(DekiGame"));
    const size_t overrides = cmake.find("add_compile_definitions(PICO_CXX_DISABLE_ALLOCATION_OVERRIDES=1)");
    ASSERT_NE(overrides, std::string::npos);
    EXPECT_LT(overrides, cmake.find("pico_sdk_init()"));

    // Everything in one static library, the program linked from it.
    EXPECT_TRUE(Contains(cmake, "add_library(DekiGameLib STATIC ${DEKI_ENGINE_SOURCES} ${PROJECT_SOURCES})\n"));
    EXPECT_TRUE(Contains(cmake, "list(APPEND DEKI_ENGINE_SOURCES \"${DEKI_ENGINE_PATH}/entry/Main.cpp\")\n"));
    EXPECT_TRUE(Contains(cmake, "add_executable(DekiGame \"${CMAKE_CURRENT_SOURCE_DIR}/game_entry.gen.c\")\n"));
    EXPECT_TRUE(Contains(cmake, "target_link_libraries(DekiGame PRIVATE DekiGameLib ${DEKI_PICO_LIBS} "
                                "pico_bootsel_via_double_reset)\n"));
    EXPECT_TRUE(Contains(Read(BuildDir(TestPico()) / "game_entry.gen.c"), "= main;"));
    EXPECT_TRUE(fs::exists(BuildDir(TestPico()) / "deki_package_init.gen.cpp"));
    EXPECT_TRUE(Contains(cmake, "pico_enable_stdio_uart(DekiGame 1)\n"));
    EXPECT_TRUE(Contains(cmake, "pico_enable_stdio_usb(DekiGame 1)\n"));
    EXPECT_TRUE(Contains(cmake, "pico_add_extra_outputs(DekiGame)\n"));
    EXPECT_TRUE(Contains(cmake, "-Wl,--gc-sections -Wl,--print-memory-usage"));
    EXPECT_TRUE(Contains(cmake, "    -ffunction-sections\n    -fdata-sections\n"));
    EXPECT_TRUE(Contains(cmake, "\"$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>\""));
    EXPECT_TRUE(Contains(cmake, "\"$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>\""));
    EXPECT_TRUE(Contains(cmake, "\"$<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>\""));
    // No cxxFlags on the platform: -Os, once.
    EXPECT_TRUE(Contains(cmake, "\"$<$<COMPILE_LANGUAGE:CXX>:-Os>\""));
    EXPECT_TRUE(Contains(cmake, "set(CMAKE_CXX_FLAGS_MINSIZEREL \"-DNDEBUG\")\n"));
}

TEST_F(PicoBuildFilesTest, ItHasTheChipsDefinesAndOnlyThePlatformsPackages)
{
    // Asked for SDL3 and ESP32 too, as a project with those scenes would be:
    // the platform's capabilities leave them out, and its requiresPackages
    // bring the Raspberry Pi package in even though no define named it.
    const std::string cmake = Generate(TestPico(), { "DEKI_PACKAGE_SDL3", "DEKI_PACKAGE_ESP32_HAL" });
    for (const char* define :
         { "    \"DEKI_PICO\"\n", "    \"DEKI_PICO_RP2040\"\n", "    \"DEKI_PICO_FLASH_SIZE=2097152\"\n", "    \"DEKI_FAST_ATTR=\"\n", "    DEKI_PACKAGE_RASPBERRYPI\n" })
    {
        EXPECT_TRUE(Contains(cmake, define)) << define;
    }
    // deki-spi comes in as the Raspberry Pi package's requirement.
    EXPECT_TRUE(Contains(cmake, "\"${DEKI_PROJECT_PATH}/packages/deki-spi/*.cpp\""));
    EXPECT_FALSE(Contains(cmake, "DEKI_PICO_RP2350"));
    EXPECT_FALSE(Contains(cmake, "deki-sdl3-integration"));
    EXPECT_FALSE(Contains(cmake, "deki-esp32-integration"));
    EXPECT_FALSE(Contains(cmake, "esp_lcd"));
    // The SDK libraries the active packages name, each once, the stdlib first.
    EXPECT_TRUE(Contains(cmake, "set(DEKI_PICO_LIBS pico_stdlib pico_multicore hardware_dma hardware_spi)\n"));
    EXPECT_TRUE(Contains(cmake, "target_link_libraries(DekiGameLib PUBLIC ${_LIB}_headers)"));
}

TEST_F(PicoBuildFilesTest, AnRp2350BuildsForItsArmCores)
{
    PlatformConfig config = TestPico();
    config.SetOption("picoChip", "rp2350");
    config.SetOption("picoBoard", "pico2");
    config.SetOption("stdio", "usb");
    const std::string cmake = Generate(config, {});
    EXPECT_TRUE(Contains(cmake, "set(PICO_PLATFORM \"rp2350-arm-s\")\n"));
    EXPECT_TRUE(Contains(cmake, "set(PICO_BOARD \"pico2\")\n"));
    EXPECT_TRUE(Contains(cmake, "    \"DEKI_PICO_RP2350\"\n"));
    EXPECT_TRUE(Contains(cmake, "pico_enable_stdio_uart(DekiGame 0)\n"));
    EXPECT_TRUE(Contains(cmake, "pico_enable_stdio_usb(DekiGame 1)\n"));
}

TEST_F(PicoBuildFilesTest, ThePlatformsFlagsReplaceTheDefaultOptimisation)
{
    PlatformConfig config = TestPico();
    config.cxxFlags = { "-O2", "-std=c++17" };
    config.linkFlags = { "-Wl,--no-warn-rwx-segments" };
    const std::string cmake = Generate(config, {});
    EXPECT_TRUE(Contains(cmake, "\"$<$<COMPILE_LANGUAGE:CXX>:-O2>\""));
    EXPECT_FALSE(Contains(cmake, "-Os"));
    EXPECT_FALSE(Contains(cmake, "-std=c++17")) << "the build sets the standard";
    EXPECT_TRUE(Contains(cmake, "\"-Wl,--no-warn-rwx-segments\""));
}

TEST_F(PicoBuildFilesTest, LoggingFollowsTheBuildOptions)
{
    BuildOptions quiet;
    quiet.enableLogging = false;
    EXPECT_FALSE(Contains(Generate(TestPico(), {}, quiet), "DEKI_LOG_ENABLED"));

    BuildOptions verbose;
    verbose.enableLogging = true;
    verbose.enableInternalLogging = true;
    const std::string cmake = Generate(TestPico(), {}, verbose);
    EXPECT_TRUE(Contains(cmake, "target_compile_definitions(DekiGameLib PRIVATE DEKI_LOG_ENABLED)"));
    EXPECT_TRUE(Contains(cmake, "target_compile_definitions(DekiGameLib PRIVATE DEKI_LOG_INTERNAL_ENABLED)"));
}

TEST_F(PicoBuildFilesTest, AChangedChipStartsTheConfigureAfresh)
{
    Generate(TestPico(), {});
    const fs::path cache = BuildDir(TestPico()) / "build" / "CMakeCache.txt";
    Write(cache, "old");
    Generate(TestPico(), {});
    EXPECT_TRUE(fs::exists(cache)) << "unchanged settings keep the cache";

    PlatformConfig changed = TestPico();
    changed.SetOption("picoChip", "rp2350");
    Generate(changed, {});
    EXPECT_FALSE(fs::exists(cache));
}

TEST_F(PicoBuildFilesTest, AMissingRequiredPackageOrABadPlatformFailsTheBuild)
{
    PlatformConfig bad = TestPico();
    bad.SetOption("picoChip", "rp9999");
    EXPECT_FALSE(m_Builder.GenerateBuildFiles(m_Dir.Path().string(), bad, {}));

    fs::remove_all(m_Dir.Path() / "packages" / "deki-raspberrypi-integration");
    EXPECT_FALSE(m_Builder.GenerateBuildFiles(m_Dir.Path().string(), TestPico(), {}));
}

// --- The UF2 ---

TEST(PicoBuilder, TheProgramAndTheGameDataGoIntoOneUf2)
{
    TempDir dir;
    std::vector<uint8_t> program(5000);
    for (size_t i = 0; i < program.size(); ++i)
    {
        program[i] = static_cast<uint8_t>(i * 13);
    }
    WriteBytes(dir.Path() / "DekiGame.bin", program);
    Write(dir.Path() / "gamedata" / "boot.scene", "{\"objects\":[]}");
    Write(dir.Path() / "gamedata" / "assets" / "1234", "pixels");

    const PicoOptions options = PicoBuilder::OptionsOf(TestPico());
    const fs::path uf2 = dir.Path() / "DekiGame.uf2";
    PicoFlashUse use;
    std::string error;
    ASSERT_TRUE(
        PicoBuilder::PackFirmware(dir.Path() / "DekiGame.bin", dir.Path() / "gamedata", options, uf2, use, error))
        << error;
    EXPECT_EQ(use.program, 5000u);
    EXPECT_GT(use.gameData, 0u);
    EXPECT_EQ(use.gameDataAt, 8192u) << "the sector after the program";
    EXPECT_EQ(use.freeFlash, 2097152u - 8192u - use.gameData);

    std::vector<uint8_t> file;
    {
        std::ifstream in(uf2, std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::vector<Uf2Block> blocks;
    ASSERT_TRUE(ReadUf2(file, blocks, error)) << error;
    const size_t programBlocks = (5000 + 255) / 256;
    const size_t dataBlocks = (use.gameData + 255) / 256;
    ASSERT_EQ(blocks.size(), programBlocks + dataBlocks);
    EXPECT_EQ(blocks.front().targetAddress, 0x10000000u);
    EXPECT_EQ(blocks[programBlocks].targetAddress, 0x10002000u) << "the data on the sector after the program";
    for (const Uf2Block& b : blocks)
    {
        EXPECT_EQ(b.familyId, kUf2FamilyRp2040);
        EXPECT_EQ(b.numBlocks, blocks.size());
    }

    // The data the game finds there is the image of gamedata/.
    std::vector<uint8_t> image;
    for (size_t i = programBlocks; i < blocks.size(); ++i)
    {
        image.insert(image.end(), blocks[i].payload.begin(), blocks[i].payload.end());
    }
    Deki::FileImage files;
    ASSERT_TRUE(files.Open(image.data(), image.size()));
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    ASSERT_TRUE(files.Find("boot.scene", 10, bytes, size));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes), size), "{\"objects\":[]}");
    ASSERT_TRUE(files.Find("assets/1234", 11, bytes, size));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes), size), "pixels");

    std::vector<uint8_t> programBack;
    for (size_t i = 0; i < programBlocks; ++i)
    {
        programBack.insert(programBack.end(), blocks[i].payload.begin(), blocks[i].payload.end());
    }
    programBack.resize(program.size());
    EXPECT_EQ(programBack, program);
}

TEST(PicoBuilder, AnRp2350Uf2StartsWithTheErrataBlock)
{
    TempDir dir;
    WriteBytes(dir.Path() / "DekiGame.bin", std::vector<uint8_t>(1000, 5));
    PicoOptions options = PicoBuilder::OptionsOf(TestPico());
    options.chip = "rp2350";
    PicoFlashUse use;
    std::string error;
    ASSERT_TRUE(PicoBuilder::PackFirmware(dir.Path() / "DekiGame.bin", dir.Path() / "none", options,
                                          dir.Path() / "DekiGame.uf2", use, error))
        << error;
    std::ifstream in(dir.Path() / "DekiGame.uf2", std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Uf2Block> blocks;
    ASSERT_TRUE(ReadUf2(file, blocks, error)) << error;
    ASSERT_EQ(blocks.size(), 1u + 4u);
    EXPECT_EQ(blocks[0].familyId, kUf2FamilyAbsolute);
    for (size_t i = 1; i < blocks.size(); ++i)
    {
        EXPECT_EQ(blocks[i].familyId, kUf2FamilyRp2350ArmSecure);
        EXPECT_EQ(blocks[i].blockNumber, i - 1);
        EXPECT_EQ(blocks[i].numBlocks, 4u);
    }
}

TEST(PicoBuilder, AProgramOrGameDataThatDoesNotFitIsRefused)
{
    TempDir dir;
    PicoOptions options = PicoBuilder::OptionsOf(TestPico());
    options.flashSize = 64 * 1024;
    PicoFlashUse use;
    std::string error;

    WriteBytes(dir.Path() / "big.bin", std::vector<uint8_t>(64 * 1024 + 1, 1));
    EXPECT_FALSE(PicoBuilder::PackFirmware(dir.Path() / "big.bin", dir.Path() / "none", options, dir.Path() / "a.uf2",
                                           use, error));
    EXPECT_TRUE(Contains(error, "the flash is")) << error;

    WriteBytes(dir.Path() / "small.bin", std::vector<uint8_t>(1024, 1));
    WriteBytes(dir.Path() / "gamedata" / "assets" / "huge", std::vector<uint8_t>(61 * 1024, 2));
    EXPECT_FALSE(PicoBuilder::PackFirmware(dir.Path() / "small.bin", dir.Path() / "gamedata", options,
                                           dir.Path() / "b.uf2", use, error));
    EXPECT_TRUE(Contains(error, "game's data is")) << error;

    // No game data at all: the program alone.
    ASSERT_TRUE(PicoBuilder::PackFirmware(dir.Path() / "small.bin", dir.Path() / "none", options, dir.Path() / "c.uf2",
                                          use, error))
        << error;
    EXPECT_EQ(fs::file_size(dir.Path() / "c.uf2"), 4u * 512u);
    EXPECT_FALSE(PicoBuilder::PackFirmware(dir.Path() / "missing.bin", dir.Path() / "none", options,
                                           dir.Path() / "d.uf2", use, error));
}

// --- The Picos to flash ---

TEST(PicoDevices, TheBoardIdIsReadFromInfoUf2)
{
    EXPECT_EQ(PicoBoardIdFromInfo("UF2 Bootloader v3.0\r\nModel: Raspberry Pi RP2\r\nBoard-ID: RPI-RP2\r\n"),
              "RPI-RP2");
    EXPECT_EQ(PicoBoardIdFromInfo("UF2 Bootloader v1.0\nModel: Raspberry Pi RP2350\nBoard-ID: RP2350\n"), "RP2350");
    EXPECT_EQ(PicoBoardIdFromInfo("nothing here"), "");
    EXPECT_EQ(PicoChipOfBoardId("RPI-RP2"), "rp2040");
    EXPECT_EQ(PicoChipOfBoardId("RP2350"), "rp2350");
    EXPECT_EQ(PicoChipOfBoardId("SAMD21"), "");
}

TEST(PicoDevices, OnlyDrivesWithInfoUf2AreBootDrives)
{
    TempDir dir;
    const fs::path pico = dir.Path() / "RPI-RP2";
    const fs::path stick = dir.Path() / "STICK";
    Write(pico / "INFO_UF2.TXT", "Board-ID: RPI-RP2\n");
    Write(pico / "INDEX.HTM", "");
    Write(stick / "notes.txt", "");
    const auto drives = PicoBootDrivesAmong({ stick.string(), pico.string(), (dir.Path() / "gone").string() });
    ASSERT_EQ(drives.size(), 1u);
    EXPECT_EQ(drives[0].root, pico.string());
    EXPECT_EQ(drives[0].boardId, "RPI-RP2");
}

TEST(PicoDevices, TheDriveThatAppearedIsTheOneToFlash)
{
    const std::vector<PicoBootDrive> before = { { "E:", "RPI-RP2" } };
    EXPECT_EQ(PickNewBootDrive(before, { { "E:", "RPI-RP2" }, { "F:", "RPI-RP2" } }), "F:");
    EXPECT_EQ(PickNewBootDrive(before, { { "E:", "RPI-RP2" } }), "") << "the Pico that was already there is another";
    EXPECT_EQ(PickNewBootDrive({}, { { "F:", "" }, { "G:", "" } }), "") << "two at once: which is ours is unknown";
    EXPECT_EQ(PickNewBootDrive({}, {}), "");
#ifdef _WIN32
    EXPECT_TRUE(NamesSameDrive("e:\\", "E:"));
#endif
    EXPECT_FALSE(NamesSameDrive("E:", ""));
}

TEST(PicoDevices, APortThatIsNotThereCannotBeTouched)
{
    std::string error;
#ifdef _WIN32
    EXPECT_FALSE(TouchPortAt1200Baud("COM250", error));
#else
    EXPECT_FALSE(TouchPortAt1200Baud("/dev/deki-no-such-port", error));
#endif
    EXPECT_FALSE(error.empty());
}

TEST(PicoDevices, CopyingPutsTheUf2OnTheDrive)
{
    TempDir dir;
    const fs::path drive = dir.Path() / "RPI-RP2";
    Write(drive / "INFO_UF2.TXT", "Board-ID: RPI-RP2\n");
    WriteBytes(dir.Path() / "DekiGame.uf2", std::vector<uint8_t>(200 * 1024, 7));
    float last = 0.0f;
    std::string error;
    ASSERT_TRUE(CopyUf2ToDrive(
        dir.Path() / "DekiGame.uf2", drive.string(), [&](float v) { last = v; }, nullptr, error))
        << error;
    EXPECT_FLOAT_EQ(last, 1.0f);
    EXPECT_EQ(fs::file_size(drive / "DekiGame.uf2"), 200u * 1024u);
    EXPECT_FALSE(CopyUf2ToDrive(dir.Path() / "missing.uf2", drive.string(), nullptr, nullptr, error));
}

TEST(PicoBuilder, BootDrivesAreOfferedAndSerialPortsOnlyWithUsbSerial)
{
    const std::vector<PicoBootDrive> drives = { { "E:", "RPI-RP2" } };
    const std::vector<std::string> ports = { "COM5" };
    auto targets = PicoBuilder::DeployTargetsFrom(drives, ports, true);
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].id, "E:");
    EXPECT_EQ(targets[0].label, "Pico in BOOTSEL mode (E: RPI-RP2)");
    EXPECT_EQ(targets[1].id, "COM5");
    EXPECT_TRUE(Contains(targets[1].label, "BOOTSEL"));

    targets = PicoBuilder::DeployTargetsFrom(drives, ports, false);
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0].id, "E:");
}

// A boot menu (the PicoCalc's) starts the program from loaderOffset: it must
// be on a sector inside the flash.
TEST(PicoBuilder, ABootMenuOffsetIsCheckedAgainstTheLayout)
{
    PicoBuilder builder;
    const auto problemsWith = [&](const std::string& options)
    {
        PlatformConfig c = TestPico();
        c.frameworkOptions = options;
        return builder.ValidatePlatform(c);
    };
    EXPECT_TRUE(problemsWith(R"({"picoChip": "rp2040", "flashSize": 2097152, "loaderOffset": 204800})").empty());
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "rp2040", "flashSize": 2097152, "loaderOffset": 200000})"),
                           "loaderOffset 200000"));
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "rp2040", "flashSize": 2097152, "loaderOffset": 2097152})"),
                           "not inside the flash"));
    EXPECT_TRUE(HasProblem(problemsWith(R"({"picoChip": "rp2040", "flashSize": 2097152, "loaderOffset": "200k"})"),
                           "loaderOffset is not a number"));
}

// The menu's file: the program at its start, erased flash up to the game's
// data, then the data, all as the menu writes it from loaderOffset.
TEST(PicoBuilder, ABootMenuBuildIsOneBinFromTheLoaderOffset)
{
    TempDir dir;
    std::vector<uint8_t> program(3000, 0x5A);
    WriteBytes(dir.Path() / "DekiGame.bin", program);
    Write(dir.Path() / "gamedata" / "boot.scene", "{\"objects\":[]}");

    PlatformConfig config = TestPico();
    config.frameworkOptions = R"({"picoChip": "rp2040", "flashSize": 2097152, "loaderOffset": 204800})";
    const PicoOptions options = PicoBuilder::OptionsOf(config);
    ASSERT_TRUE(options.BootMenu());
    const fs::path out = dir.Path() / "menu.bin";
    PicoFlashUse use;
    std::string error;
    ASSERT_TRUE(
        PicoBuilder::PackFirmware(dir.Path() / "DekiGame.bin", dir.Path() / "gamedata", options, out, use, error))
        << error;

    std::ifstream in(out, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t dataAt = PicoBuilder::GameDataAt(options, program.size()) - options.loaderOffset;
    EXPECT_EQ(dataAt, 4096u) << "the sector after the program";
    ASSERT_EQ(bytes.size(), dataAt + use.gameData);
    EXPECT_TRUE(std::equal(program.begin(), program.end(), bytes.begin()));
    EXPECT_EQ(bytes[program.size()], 0xFF);
    EXPECT_EQ(bytes[dataAt - 1], 0xFF);
    EXPECT_EQ(std::string(bytes.begin() + dataAt, bytes.begin() + dataAt + 4), "DKFI");
    EXPECT_EQ(use.freeFlash, options.flashSize - options.loaderOffset - dataAt - use.gameData);
}

TEST_F(PicoBuildFilesTest, ABootMenuBuildStartsAtTheLoaderOffsetWithNoBoot2)
{
    PlatformConfig config = TestPico();
    config.frameworkOptions = R"({"picoChip": "rp2040", "flashSize": 2097152, "loaderOffset": 204800})";
    const std::string cmake = Generate(config, {});
    EXPECT_TRUE(Contains(cmake, "pico_add_linker_script_override_path(DekiGame \"${CMAKE_CURRENT_SOURCE_DIR}/linker\" "
                                "FILES pico_flash_region.ld section_boot2.incl)\n"));
    EXPECT_LT(cmake.find("pico_add_linker_script_override_path"), cmake.find("pico_add_extra_outputs"));
    EXPECT_EQ(Read(BuildDir(config) / "linker" / "pico_flash_region.ld"),
              "FLASH(rx) : ORIGIN = 0x10000000 + 204800, LENGTH = 1892352\n");
    EXPECT_TRUE(Contains(Read(BuildDir(config) / "linker" / "section_boot2.incl"), "/DISCARD/ : { *(.boot2) }"));

    // Without it the program starts the flash, with the SDK's own script.
    EXPECT_FALSE(Contains(Generate(TestPico(), {}), "pico_add_linker_script_override_path"));
}
