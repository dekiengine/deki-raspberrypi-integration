// Our FAT32/FAT16 code on a block of RAM: formatting, files, directories and
// long names, what lands on the card byte by byte (both FATs, FSInfo, long
// name entries), and Deki::IFileSystem on top of it.
//
// A FAT bug on the Pico shows up as a save that is gone after a reboot, or a
// card a PC calls damaged, so the on-disk layout is checked here, not only
// what our own reader makes of it.

#include <gtest/gtest.h>

#include "pico/sd/FatFileSystem.h"
#include "pico/sd/FatVolume.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace DekiRaspberryPi;

namespace
{

// Sectors for a FAT32 volume with 512-byte clusters: just over the 65525
// clusters FAT32 needs.
constexpr uint32_t kFat32Sectors = 68000;
constexpr uint32_t kFat16Sectors = 16384;

class RamBlockDevice : public IBlockDevice
{
public:
    explicit RamBlockDevice(uint32_t sectors)
        : bytes(static_cast<size_t>(sectors) * kSectorSize, 0xA5)
    {
    }

    bool ReadSectors(uint32_t sector, uint32_t count, uint8_t* out) override
    {
        if (!InRange(sector, count))
        {
            return false;
        }
        ++reads;
        std::memcpy(out, bytes.data() + static_cast<size_t>(sector) * kSectorSize,
                    static_cast<size_t>(count) * kSectorSize);
        return true;
    }

    bool WriteSectors(uint32_t sector, uint32_t count, const uint8_t* data) override
    {
        if (!InRange(sector, count) || failWrites)
        {
            return false;
        }
        ++writes;
        std::memcpy(bytes.data() + static_cast<size_t>(sector) * kSectorSize, data,
                    static_cast<size_t>(count) * kSectorSize);
        return true;
    }

    uint32_t SectorCount() const override { return static_cast<uint32_t>(bytes.size() / kSectorSize); }
    bool Sync() override { return !failWrites; }

    const uint8_t* Sector(uint32_t sector) const { return bytes.data() + static_cast<size_t>(sector) * kSectorSize; }

    std::vector<uint8_t> bytes;
    bool failWrites = false;
    int reads = 0;
    int writes = 0;

private:
    bool InRange(uint32_t sector, uint32_t count) const
    {
        return static_cast<uint64_t>(sector) + count <= SectorCount();
    }
};

uint16_t U16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t U32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Where things are on a formatted image, read from its boot sector.
struct Layout
{
    uint32_t start = 0;
    uint32_t sectorsPerCluster = 0;
    uint32_t fatStart = 0;
    uint32_t fatSize = 0;
    uint32_t fatCount = 0;
    uint32_t dataStart = 0;
    uint32_t rootCluster = 0;
    uint32_t rootDirSector = 0;
    uint32_t fsInfo = 0;
};

Layout ReadLayout(const RamBlockDevice& device, uint32_t start)
{
    const uint8_t* boot = device.Sector(start);
    Layout layout;
    layout.start = start;
    layout.sectorsPerCluster = boot[13];
    layout.fatCount = boot[16];
    layout.fatStart = start + U16(boot + 14);
    const uint32_t rootEntries = U16(boot + 17);
    layout.fatSize = U16(boot + 22) != 0 ? U16(boot + 22) : U32(boot + 36);
    layout.rootDirSector = layout.fatStart + layout.fatCount * layout.fatSize;
    layout.dataStart = layout.rootDirSector + rootEntries * 32 / kSectorSize;
    if (U16(boot + 22) == 0)
    {
        layout.rootCluster = U32(boot + 44);
        layout.fsInfo = start + U16(boot + 48);
    }
    return layout;
}

std::vector<uint8_t> Pattern(size_t size, uint32_t seed)
{
    std::vector<uint8_t> bytes(size);
    uint32_t state = seed * 2654435761u + 1;
    for (size_t i = 0; i < size; ++i)
    {
        state = state * 1103515245u + 12345u;
        bytes[i] = static_cast<uint8_t>(state >> 16);
    }
    return bytes;
}

FatFormatOptions Options(FatType type, uint8_t sectorsPerCluster, bool partitionTable = false)
{
    FatFormatOptions options;
    options.type = type;
    options.sectorsPerCluster = sectorsPerCluster;
    options.partitionTable = partitionTable;
    return options;
}

bool WriteFile(FatVolume& volume, const char* path, const std::vector<uint8_t>& data,
               FatOpenMode mode = FatOpenMode::Write)
{
    FatFile file;
    if (!volume.Open(file, path, mode))
    {
        return false;
    }
    const size_t written = volume.Write(file, data.data(), data.size());
    return volume.Close(file) && written == data.size();
}

bool ReadFile(FatVolume& volume, const char* path, std::vector<uint8_t>& out)
{
    FatFile file;
    if (!volume.Open(file, path, FatOpenMode::Read))
    {
        return false;
    }
    out.assign(file.size, 0);
    const size_t read = volume.Read(file, out.data(), out.size());
    volume.Close(file);
    return read == out.size();
}

std::vector<uint8_t> Bytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::set<std::string> List(FatVolume& volume, const char* path)
{
    std::set<std::string> names;
    FatDir dir;
    if (!volume.OpenDir(dir, path))
    {
        return names;
    }
    FatEntryInfo info;
    while (volume.ReadDir(dir, info))
    {
        names.insert(info.name);
    }
    return names;
}

uint8_t Checksum(const uint8_t* shortName)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i)
    {
        sum = static_cast<uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + shortName[i]);
    }
    return sum;
}

class FatVolumeTest : public ::testing::Test
{
protected:
    RamBlockDevice m_Device{ kFat32Sectors };
    FatVolume m_Volume;

    void SetUp() override
    {
        ASSERT_TRUE(FatVolume::Format(m_Device, Options(FatType::Fat32, 1)));
        ASSERT_TRUE(m_Volume.Mount(m_Device));
    }
};

}  // namespace

TEST(FatFormat, Fat32BootSectorFsInfoAndFatAreLaidOutAsSpecified)
{
    RamBlockDevice device(kFat32Sectors);
    ASSERT_TRUE(FatVolume::Format(device, Options(FatType::Fat32, 1)));

    const uint8_t* boot = device.Sector(0);
    EXPECT_EQ(boot[0], 0xEB);
    EXPECT_EQ(U16(boot + 11), 512u);
    EXPECT_EQ(boot[13], 1);
    EXPECT_EQ(U16(boot + 17), 0u);  // no fixed root on FAT32
    EXPECT_EQ(U16(boot + 22), 0u);
    EXPECT_EQ(U32(boot + 32), kFat32Sectors);
    EXPECT_EQ(U32(boot + 44), 2u);
    EXPECT_EQ(std::memcmp(boot + 82, "FAT32   ", 8), 0);
    EXPECT_EQ(boot[510], 0x55);
    EXPECT_EQ(boot[511], 0xAA);
    EXPECT_EQ(std::memcmp(device.Sector(6), boot, kSectorSize), 0) << "backup boot sector";

    const uint8_t* info = device.Sector(1);
    EXPECT_EQ(U32(info), 0x41615252u);
    EXPECT_EQ(U32(info + 484), 0x61417272u);
    EXPECT_EQ(U32(info + 508), 0xAA550000u);

    const Layout layout = ReadLayout(device, 0);
    const uint8_t* fat = device.Sector(layout.fatStart);
    EXPECT_EQ(U32(fat), 0x0FFFFFF8u);
    EXPECT_EQ(U32(fat + 8) & 0x0FFFFFFF, 0x0FFFFFFFu) << "the root directory's chain ends at once";

    const uint32_t clusters = (kFat32Sectors - (layout.dataStart - layout.start)) / layout.sectorsPerCluster;
    EXPECT_GE(clusters, 65525u);
    EXPECT_EQ(U32(info + 488), clusters - 1);

    FatVolume volume;
    ASSERT_TRUE(volume.Mount(device));
    EXPECT_EQ(volume.Type(), FatType::Fat32);
    EXPECT_EQ(volume.ClusterBytes(), 512u);
    EXPECT_EQ(volume.FreeBytes(), static_cast<uint64_t>(clusters - 1) * 512);
}

TEST(FatFormat, RefusesClusterCountsTheTypeCannotHave)
{
    RamBlockDevice device(20000);
    EXPECT_FALSE(FatVolume::Format(device, Options(FatType::Fat32, 1))) << "too few clusters for FAT32";
    EXPECT_FALSE(FatVolume::Format(device, Options(FatType::Fat16, 8))) << "too few clusters for FAT16";
    EXPECT_FALSE(FatVolume::Format(device, Options(FatType::Fat16, 3))) << "not a power of two";
    EXPECT_TRUE(FatVolume::Format(device, Options(FatType::Fat16, 1)));
}

TEST(FatFormat, MountRefusesADeviceWithNoVolume)
{
    RamBlockDevice device(1024);
    FatVolume volume;
    EXPECT_FALSE(volume.Mount(device));
    EXPECT_FALSE(volume.IsMounted());
}

TEST_F(FatVolumeTest, WritesAFileAndReadsItBack)
{
    ASSERT_TRUE(WriteFile(m_Volume, "hello.txt", Bytes("Hello, Pico")));

    FatStat stat;
    ASSERT_TRUE(m_Volume.Stat("hello.txt", stat));
    EXPECT_FALSE(stat.isDirectory);
    EXPECT_EQ(stat.size, 11u);

    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(m_Volume, "HELLO.TXT", back)) << "lookups ignore letter case";
    EXPECT_EQ(back, Bytes("Hello, Pico"));
    EXPECT_FALSE(m_Volume.Exists("missing.txt"));
}

TEST_F(FatVolumeTest, AppendAddsAtTheEnd)
{
    ASSERT_TRUE(WriteFile(m_Volume, "log.txt", Bytes("one,")));
    ASSERT_TRUE(WriteFile(m_Volume, "log.txt", Bytes("two,"), FatOpenMode::Append));
    ASSERT_TRUE(WriteFile(m_Volume, "new.txt", Bytes("made"), FatOpenMode::Append)) << "append makes a missing file";

    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(m_Volume, "log.txt", back));
    EXPECT_EQ(back, Bytes("one,two,"));
    ASSERT_TRUE(ReadFile(m_Volume, "new.txt", back));
    EXPECT_EQ(back, Bytes("made"));
}

TEST_F(FatVolumeTest, OverwriteEmptiesTheFileAndFreesItsClusters)
{
    const uint64_t freeBefore = m_Volume.FreeBytes();
    ASSERT_TRUE(WriteFile(m_Volume, "save.bin", Pattern(40000, 1)));
    EXPECT_LT(m_Volume.FreeBytes(), freeBefore - 39000);

    ASSERT_TRUE(WriteFile(m_Volume, "save.bin", Bytes("short")));
    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(m_Volume, "save.bin", back));
    EXPECT_EQ(back, Bytes("short"));
    EXPECT_EQ(m_Volume.FreeBytes(), freeBefore - 512) << "one cluster left in use";
}

TEST_F(FatVolumeTest, SeekReadsAndWritesInThePlaceAsked)
{
    const std::vector<uint8_t> data = Pattern(5000, 2);
    ASSERT_TRUE(WriteFile(m_Volume, "data.bin", data));

    FatFile file;
    ASSERT_TRUE(m_Volume.Open(file, "data.bin", FatOpenMode::Read));
    uint8_t chunk[100] = {};
    ASSERT_TRUE(m_Volume.Seek(file, 3000));
    ASSERT_EQ(m_Volume.Read(file, chunk, sizeof(chunk)), sizeof(chunk));
    EXPECT_EQ(std::memcmp(chunk, data.data() + 3000, sizeof(chunk)), 0);
    ASSERT_TRUE(m_Volume.Seek(file, 10)) << "backwards, across clusters";
    ASSERT_EQ(m_Volume.Read(file, chunk, sizeof(chunk)), sizeof(chunk));
    EXPECT_EQ(std::memcmp(chunk, data.data() + 10, sizeof(chunk)), 0);
    ASSERT_TRUE(m_Volume.Seek(file, 4990));
    EXPECT_EQ(m_Volume.Read(file, chunk, sizeof(chunk)), 10u) << "a read stops at the end";
    m_Volume.Close(file);

    // In place, in append mode: the size stays.
    ASSERT_TRUE(m_Volume.Open(file, "data.bin", FatOpenMode::Append));
    ASSERT_TRUE(m_Volume.Seek(file, 1020));
    ASSERT_EQ(m_Volume.Write(file, "XYZWV", 5), 5u);
    ASSERT_TRUE(m_Volume.Close(file));
    std::vector<uint8_t> expected = data;
    std::memcpy(expected.data() + 1020, "XYZWV", 5);
    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(m_Volume, "data.bin", back));
    EXPECT_EQ(back, expected);

    // Past the end: the gap reads back as zeros.
    ASSERT_TRUE(m_Volume.Open(file, "data.bin", FatOpenMode::Append));
    ASSERT_TRUE(m_Volume.Seek(file, 6000));
    ASSERT_EQ(m_Volume.Write(file, "!", 1), 1u);
    ASSERT_TRUE(m_Volume.Close(file));
    ASSERT_TRUE(ReadFile(m_Volume, "data.bin", back));
    ASSERT_EQ(back.size(), 6001u);
    EXPECT_TRUE(std::all_of(back.begin() + 5000, back.begin() + 6000, [](uint8_t b) { return b == 0; }));
    EXPECT_EQ(back[6000], '!');
}

TEST_F(FatVolumeTest, NestedDirectoriesHoldFilesAndOnlyEmptyOnesAreRemoved)
{
    ASSERT_TRUE(m_Volume.CreateDirectory("saves/slot1/backup"));
    ASSERT_TRUE(m_Volume.CreateDirectory("saves/slot1")) << "an existing directory is fine";
    ASSERT_TRUE(WriteFile(m_Volume, "saves/slot1/game.sav", Bytes("level 3")));
    ASSERT_TRUE(WriteFile(m_Volume, "/saves/slot1/backup/game.sav", Bytes("level 2")));
    EXPECT_FALSE(WriteFile(m_Volume, "saves/slot2/game.sav", Bytes("x"))) << "the directory must exist";

    FatStat stat;
    ASSERT_TRUE(m_Volume.Stat("saves/slot1", stat));
    EXPECT_TRUE(stat.isDirectory);
    ASSERT_TRUE(m_Volume.Stat("", stat));
    EXPECT_TRUE(stat.isDirectory);

    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(m_Volume, "saves\\slot1\\backup\\..\\game.sav", back)) << "backslashes and ..";
    EXPECT_EQ(back, Bytes("level 3"));
    ASSERT_TRUE(ReadFile(m_Volume, "saves/./slot1/backup/game.sav", back));
    EXPECT_EQ(back, Bytes("level 2"));

    EXPECT_EQ(List(m_Volume, "saves/slot1"), (std::set<std::string>{ "backup", "game.sav" }));
    EXPECT_EQ(List(m_Volume, ""), (std::set<std::string>{ "saves" }));

    EXPECT_FALSE(m_Volume.Remove("saves/slot1/backup")) << "not empty";
    ASSERT_TRUE(m_Volume.Remove("saves/slot1/backup/game.sav"));
    ASSERT_TRUE(m_Volume.Remove("saves/slot1/backup"));
    EXPECT_FALSE(m_Volume.Exists("saves/slot1/backup"));
    EXPECT_EQ(List(m_Volume, "saves/slot1"), (std::set<std::string>{ "game.sav" }));
    FatFile directory;
    EXPECT_FALSE(m_Volume.Open(directory, "saves", FatOpenMode::Read)) << "a directory";
}

TEST_F(FatVolumeTest, LongNamesRoundTripAndKeepTheirAliases)
{
    const std::string longName(200, 'n');
    const std::vector<std::string> names = {
        "My Save Game.dat",  "readme.txt",      "UPPER.TXT",     "Mixed.Txt", "longfilename1.txt",
        "longfilename2.txt", longName + ".bin", "two.dots.here", ".hidden",
    };
    for (const std::string& name : names)
    {
        ASSERT_TRUE(WriteFile(m_Volume, name.c_str(), Bytes(name))) << name;
    }
    EXPECT_EQ(List(m_Volume, ""), std::set<std::string>(names.begin(), names.end()));

    for (const std::string& name : names)
    {
        std::vector<uint8_t> back;
        ASSERT_TRUE(ReadFile(m_Volume, name.c_str(), back)) << name;
        EXPECT_EQ(back, Bytes(name));
    }

    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(m_Volume, "my save game.DAT", back)) << "long names ignore letter case";
    EXPECT_EQ(back, Bytes("My Save Game.dat"));
    ASSERT_TRUE(ReadFile(m_Volume, "LONGFI~1.TXT", back)) << "the first alias";
    EXPECT_EQ(back, Bytes("longfilename1.txt"));
    ASSERT_TRUE(ReadFile(m_Volume, "LONGFI~2.TXT", back)) << "the second alias";
    EXPECT_EQ(back, Bytes("longfilename2.txt"));
    ASSERT_TRUE(ReadFile(m_Volume, "MIXED.TXT", back)) << "an alias that needs no number";
    EXPECT_EQ(back, Bytes("Mixed.Txt"));

    // UTF-8 goes in as UTF-16; listings show it as '?'.
    ASSERT_TRUE(WriteFile(m_Volume, "caf\xC3\xA9.txt", Bytes("coffee")));
    ASSERT_TRUE(ReadFile(m_Volume, "caf\xC3\xA9.txt", back));
    EXPECT_EQ(back, Bytes("coffee"));
    EXPECT_EQ(List(m_Volume, "").count("caf?.txt"), 1u);

    EXPECT_FALSE(WriteFile(m_Volume, "bad?.txt", Bytes("x")));
    EXPECT_FALSE(WriteFile(m_Volume, std::string(256, 'x').c_str(), Bytes("x"))) << "over 255 characters";

    ASSERT_TRUE(m_Volume.Remove("My Save Game.dat"));
    EXPECT_FALSE(m_Volume.Exists("My Save Game.dat"));
    EXPECT_FALSE(m_Volume.Exists("MYSAVE~1.DAT")) << "the alias goes with it";
}

TEST_F(FatVolumeTest, LongNameEntriesAreWrittenAsTheSpecificationSays)
{
    ASSERT_TRUE(WriteFile(m_Volume, "A long file name.txt", Bytes("x")));
    ASSERT_TRUE(WriteFile(m_Volume, "lower.txt", Bytes("y")));
    ASSERT_TRUE(m_Volume.Sync());

    const Layout layout = ReadLayout(m_Device, 0);
    const uint8_t* root = m_Device.Sector(layout.dataStart + (layout.rootCluster - 2) * layout.sectorsPerCluster);

    // "A long file name.txt" is 20 characters: two long entries, last part first.
    EXPECT_EQ(root[0], 0x42);
    EXPECT_EQ(root[11], 0x0F);
    EXPECT_EQ(root[32], 0x01);
    EXPECT_EQ(root[32 + 11], 0x0F);
    const uint8_t* shortEntry = root + 64;
    EXPECT_EQ(std::memcmp(shortEntry, "ALONGF~1TXT", 11), 0);
    EXPECT_EQ(root[13], Checksum(shortEntry));
    EXPECT_EQ(root[32 + 13], Checksum(shortEntry));
    EXPECT_EQ(U16(root + 32 + 1), 'A') << "the first character is in the entry numbered 1";
    // Seven characters in the second entry, then the terminator and padding.
    EXPECT_EQ(U16(root + 1), 'a');
    EXPECT_EQ(U16(root + 18), 0x0000);
    EXPECT_EQ(U16(root + 30), 0xFFFF);

    // "lower.txt" fits 8.3 with the NT lower-case bits, so no long entry.
    const uint8_t* lower = root + 96;
    EXPECT_EQ(std::memcmp(lower, "LOWER   TXT", 11), 0);
    EXPECT_EQ(lower[12], 0x18);
    EXPECT_EQ(U32(lower + 28), 1u);
}

TEST_F(FatVolumeTest, AFileSpanningManyClustersReadsBackInAnyChunkSize)
{
    const std::vector<uint8_t> data = Pattern(300000, 3);
    FatFile file;
    ASSERT_TRUE(m_Volume.Open(file, "big.bin", FatOpenMode::Write));
    for (size_t at = 0; at < data.size(); at += 1000)
    {
        const size_t step = std::min<size_t>(1000, data.size() - at);
        ASSERT_EQ(m_Volume.Write(file, data.data() + at, step), step);
    }
    ASSERT_TRUE(m_Volume.Close(file));

    ASSERT_TRUE(m_Volume.Open(file, "big.bin", FatOpenMode::Read));
    EXPECT_EQ(file.size, data.size());
    std::vector<uint8_t> back(data.size());
    size_t at = 0;
    const size_t steps[] = { 1, 511, 512, 513, 4096, 7777 };
    for (int i = 0; at < back.size(); ++i)
    {
        const size_t step = std::min(steps[i % 6], back.size() - at);
        ASSERT_EQ(m_Volume.Read(file, back.data() + at, step), step);
        at += step;
    }
    EXPECT_EQ(m_Volume.Read(file, back.data(), 1), 0u);
    m_Volume.Close(file);
    EXPECT_EQ(back, data);
}

TEST_F(FatVolumeTest, ADirectoryGrowsPastItsFirstCluster)
{
    ASSERT_TRUE(m_Volume.CreateDirectory("many"));
    std::set<std::string> expected;
    for (int i = 0; i < 120; ++i)
    {
        const std::string name = "A rather long name " + std::to_string(i) + ".txt";
        ASSERT_TRUE(WriteFile(m_Volume, ("many/" + name).c_str(), Bytes(name))) << name;
        expected.insert(name);
    }
    EXPECT_EQ(List(m_Volume, "many"), expected);
    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(m_Volume, "many/A rather long name 77.txt", back));
    EXPECT_EQ(back, Bytes("A rather long name 77.txt"));
}

TEST_F(FatVolumeTest, AFullVolumeStopsWritingAndDeletingFreesTheSpace)
{
    const uint64_t freeAtStart = m_Volume.FreeBytes();
    const std::vector<uint8_t> chunk = Pattern(65536, 4);
    FatFile file;
    ASSERT_TRUE(m_Volume.Open(file, "fill.bin", FatOpenMode::Write));
    uint64_t total = 0;
    while (true)
    {
        const size_t written = m_Volume.Write(file, chunk.data(), chunk.size());
        total += written;
        if (written < chunk.size())
        {
            break;
        }
    }
    ASSERT_TRUE(m_Volume.Close(file));
    EXPECT_EQ(total, freeAtStart);
    EXPECT_EQ(m_Volume.FreeBytes(), 0u);
    EXPECT_FALSE(WriteFile(m_Volume, "more.bin", Bytes("no room")));

    FatStat stat;
    ASSERT_TRUE(m_Volume.Stat("fill.bin", stat));
    EXPECT_EQ(stat.size, total);

    ASSERT_TRUE(m_Volume.Remove("fill.bin"));
    ASSERT_TRUE(m_Volume.Remove("more.bin")) << "its entry was made before the space ran out";
    EXPECT_EQ(m_Volume.FreeBytes(), freeAtStart);
    ASSERT_TRUE(WriteFile(m_Volume, "after.bin", Pattern(10000, 5)));
}

TEST_F(FatVolumeTest, AFreshMountAfterSyncReadsTheSameBytes)
{
    const std::vector<uint8_t> a = Pattern(12345, 6);
    const std::vector<uint8_t> b = Pattern(77, 7);
    ASSERT_TRUE(m_Volume.CreateDirectory("dir"));
    ASSERT_TRUE(WriteFile(m_Volume, "dir/a long name.bin", a));
    ASSERT_TRUE(WriteFile(m_Volume, "B.BIN", b));
    const uint64_t freeBytes = m_Volume.FreeBytes();
    ASSERT_TRUE(m_Volume.Sync());

    FatVolume again;
    ASSERT_TRUE(again.Mount(m_Device));
    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(again, "dir/a long name.bin", back));
    EXPECT_EQ(back, a);
    ASSERT_TRUE(ReadFile(again, "b.bin", back));
    EXPECT_EQ(back, b);
    EXPECT_EQ(again.FreeBytes(), freeBytes) << "FSInfo kept the free count";
}

TEST_F(FatVolumeTest, FsInfoSaysUnknownUntilSyncWritesTheTrueCount)
{
    const Layout layout = ReadLayout(m_Device, 0);
    ASSERT_TRUE(WriteFile(m_Volume, "x.bin", Pattern(3000, 8)));
    EXPECT_EQ(U32(m_Device.Sector(layout.fsInfo) + 488), 0xFFFFFFFFu) << "marked unknown at the first change";

    ASSERT_TRUE(m_Volume.Sync());
    EXPECT_EQ(static_cast<uint64_t>(U32(m_Device.Sector(layout.fsInfo) + 488)) * 512, m_Volume.FreeBytes());
}

TEST_F(FatVolumeTest, BothFatCopiesMatchAfterChanges)
{
    ASSERT_TRUE(m_Volume.CreateDirectory("a/b"));
    for (int i = 0; i < 20; ++i)
    {
        ASSERT_TRUE(WriteFile(m_Volume, ("a/b/file" + std::to_string(i) + ".bin").c_str(), Pattern(1000 + i * 700, i)));
    }
    for (int i = 0; i < 20; i += 3)
    {
        ASSERT_TRUE(m_Volume.Remove(("a/b/file" + std::to_string(i) + ".bin").c_str()));
    }
    ASSERT_TRUE(WriteFile(m_Volume, "a/b/file1.bin", Bytes("smaller")));
    ASSERT_TRUE(m_Volume.Sync());

    const Layout layout = ReadLayout(m_Device, 0);
    ASSERT_EQ(layout.fatCount, 2u);
    EXPECT_EQ(std::memcmp(m_Device.Sector(layout.fatStart), m_Device.Sector(layout.fatStart + layout.fatSize),
                          static_cast<size_t>(layout.fatSize) * kSectorSize),
              0);
}

TEST(FatFat16, FilesAndDirectoriesWorkWithTheFixedRoot)
{
    RamBlockDevice device(kFat16Sectors);
    ASSERT_TRUE(FatVolume::Format(device, Options(FatType::Fat16, 1)));
    EXPECT_EQ(std::memcmp(device.Sector(0) + 54, "FAT16   ", 8), 0);

    FatVolume volume;
    ASSERT_TRUE(volume.Mount(device));
    EXPECT_EQ(volume.Type(), FatType::Fat16);

    const std::vector<uint8_t> data = Pattern(20000, 9);
    ASSERT_TRUE(volume.CreateDirectory("Level Data"));
    ASSERT_TRUE(WriteFile(volume, "Level Data/World One.bin", data));
    ASSERT_TRUE(WriteFile(volume, "top.txt", Bytes("top")));
    ASSERT_TRUE(volume.Sync());

    FatVolume again;
    ASSERT_TRUE(again.Mount(device));
    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(again, "level data/world one.bin", back));
    EXPECT_EQ(back, data);
    EXPECT_EQ(List(again, "Level Data/.."), (std::set<std::string>{ "Level Data", "top.txt" }));

    const Layout layout = ReadLayout(device, 0);
    EXPECT_EQ(std::memcmp(device.Sector(layout.fatStart), device.Sector(layout.fatStart + layout.fatSize),
                          static_cast<size_t>(layout.fatSize) * kSectorSize),
              0);
}

TEST(FatPartitions, AnMbrPartitionedCardMountsItsFirstFatPartition)
{
    RamBlockDevice device(kFat32Sectors + 2048);
    ASSERT_TRUE(FatVolume::Format(device, Options(FatType::Fat32, 1, true)));

    const uint8_t* mbr = device.Sector(0);
    EXPECT_EQ(mbr[446 + 4], 0x0C);
    EXPECT_EQ(U32(mbr + 446 + 8), 2048u);
    EXPECT_EQ(U32(mbr + 446 + 12), kFat32Sectors);
    EXPECT_EQ(mbr[510], 0x55);
    EXPECT_EQ(device.Sector(2048)[0], 0xEB);
    EXPECT_EQ(U32(device.Sector(2048) + 28), 2048u) << "hidden sectors";

    FatVolume volume;
    ASSERT_TRUE(volume.Mount(device));
    const std::vector<uint8_t> data = Pattern(4000, 10);
    ASSERT_TRUE(WriteFile(volume, "on partition.bin", data));
    volume.Unmount();

    ASSERT_TRUE(volume.Mount(device));
    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(volume, "on partition.bin", back));
    EXPECT_EQ(back, data);
    const Layout layout = ReadLayout(device, 2048);
    EXPECT_GE(layout.fatStart, 2048u + 32u) << "nothing written in front of the partition but the MBR";
}

TEST_F(FatVolumeTest, AFailingCardMakesCallsFailWithoutCrashing)
{
    ASSERT_TRUE(WriteFile(m_Volume, "kept.bin", Bytes("kept")));
    m_Device.failWrites = true;
    FatFile file;
    if (m_Volume.Open(file, "lost.bin", FatOpenMode::Write))
    {
        const std::vector<uint8_t> data = Pattern(5000, 11);
        EXPECT_LT(m_Volume.Write(file, data.data(), data.size()), data.size());
        EXPECT_FALSE(m_Volume.Close(file));
    }
    EXPECT_FALSE(m_Volume.Sync());
    m_Device.failWrites = false;

    FatVolume again;
    ASSERT_TRUE(again.Mount(m_Device));
    std::vector<uint8_t> back;
    ASSERT_TRUE(ReadFile(again, "kept.bin", back));
    EXPECT_EQ(back, Bytes("kept"));
}

TEST_F(FatVolumeTest, TheFileSystemTakesVirtualPathsAsTheEsp32OneDoes)
{
    FatFileSystem fs(m_Volume, "/sdcard");
    ASSERT_TRUE(m_Volume.CreateDirectory("save"));

    Deki::IFileSystem::FileHandle handle = fs.OpenFile("S:/save/slot1.bin", Deki::IFileSystem::OpenMode::WriteBinary);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(fs.WriteFile(handle, "0123456789", 10), 10u);
    EXPECT_EQ(fs.TellFile(handle), 10);
    EXPECT_EQ(fs.ReadFile(handle, nullptr, 1), 0u);
    fs.CloseFile(handle);

    EXPECT_TRUE(fs.FileExists("S:/save/slot1.bin"));
    EXPECT_TRUE(fs.FileExists("D:/save/slot1.bin")) << "the old prefix";
    EXPECT_TRUE(fs.FileExists("/sdcard/save/slot1.bin")) << "a converted path";
    EXPECT_TRUE(fs.FileExists("S:/save")) << "a directory exists too";
    EXPECT_FALSE(fs.FileExists("S:/save/slot2.bin"));
    EXPECT_EQ(fs.OpenFile("S:/nowhere/slot1.bin", Deki::IFileSystem::OpenMode::WriteBinary), nullptr);

    handle = fs.OpenFile("s:/SAVE/SLOT1.BIN", Deki::IFileSystem::OpenMode::ReadBinary);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(fs.GetFileSize(handle), 10);
    EXPECT_EQ(fs.SeekFile(handle, -3, Deki::IFileSystem::SeekOrigin::End), 7);
    char tail[4] = {};
    EXPECT_EQ(fs.ReadFile(handle, tail, 3), 3u);
    EXPECT_STREQ(tail, "789");
    EXPECT_EQ(fs.SeekFile(handle, -2, Deki::IFileSystem::SeekOrigin::Current), 8);
    EXPECT_EQ(fs.SeekFile(handle, -20, Deki::IFileSystem::SeekOrigin::Current), -1);
    EXPECT_EQ(fs.WriteFile(handle, "x", 1), 0u) << "read mode";
    fs.CloseFile(handle);
    fs.CloseFile(handle);  // a second close is ignored

    char converted[64] = {};
    ASSERT_TRUE(fs.ConvertPath("S:/save/slot1.bin", converted, sizeof(converted)));
    EXPECT_STREQ(converted, "/sdcard/save/slot1.bin");
    EXPECT_FALSE(fs.ConvertPath("S:/save/slot1.bin", converted, 8)) << "too small";
}

TEST_F(FatVolumeTest, TheFileSystemOpensAFewFilesAtOnce)
{
    FatFileSystem fs(m_Volume, "/sdcard");
    std::vector<Deki::IFileSystem::FileHandle> handles;
    for (int i = 0; i < FatFileSystem::kMaxOpenFiles; ++i)
    {
        handles.push_back(
            fs.OpenFile(("S:/f" + std::to_string(i) + ".txt").c_str(), Deki::IFileSystem::OpenMode::WriteText));
        ASSERT_NE(handles.back(), nullptr);
        fs.WriteFile(handles.back(), "abc", 3);
    }
    EXPECT_EQ(fs.OpenFile("S:/one-too-many.txt", Deki::IFileSystem::OpenMode::WriteText), nullptr);
    fs.CloseAll();
    for (int i = 0; i < FatFileSystem::kMaxOpenFiles; ++i)
    {
        FatStat stat;
        ASSERT_TRUE(m_Volume.Stat(("f" + std::to_string(i) + ".txt").c_str(), stat));
        EXPECT_EQ(stat.size, 3u);
    }
}
