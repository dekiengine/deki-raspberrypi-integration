#include "FatVolume.h"

#include <deki/LogSystem.h>

#include <algorithm>
#include <cstring>

namespace DekiRaspberryPi
{

namespace
{

// FAT values, as FAT32 spells them. FAT16 values are widened on reading.
constexpr uint32_t kEndOfChain = 0x0FFFFFFF;
constexpr uint32_t kEndOfChainMin = 0x0FFFFFF8;
constexpr uint32_t kBadCluster = 0x0FFFFFF7;
constexpr uint32_t kFatError = 0xFFFFFFFF;  // a read failed; no FAT value looks like this
constexpr uint32_t kUnknownCount = 0xFFFFFFFF;

constexpr uint32_t kEntrySize = 32;
constexpr uint32_t kEntriesPerSector = kSectorSize / kEntrySize;
constexpr uint32_t kMaxDirEntries = 65536;  // the FAT limit for one directory

constexpr uint8_t kAttrReadOnly = 0x01;
constexpr uint8_t kAttrVolumeId = 0x08;
constexpr uint8_t kAttrDirectory = 0x10;
constexpr uint8_t kAttrArchive = 0x20;
constexpr uint8_t kAttrLongName = 0x0F;
constexpr uint8_t kAttrLongNameMask = 0x3F;

constexpr uint8_t kEntryEnd = 0x00;
constexpr uint8_t kEntryFree = 0xE5;
constexpr uint8_t kLastLongEntry = 0x40;
constexpr uint8_t kLongSequenceMask = 0x1F;
constexpr uint32_t kMaxLongEntries = 20;
constexpr uint32_t kUnitsPerLongEntry = 13;
constexpr uint32_t kMaxNameUnits = 255;
constexpr uint32_t kMaxAliasNumber = kMaxDirEntries;

// Windows NT keeps the letter case of an 8.3 name in these bits.
constexpr uint8_t kLowerCaseBase = 0x08;
constexpr uint8_t kLowerCaseExtension = 0x10;

// Where the 13 UTF-16 code units of a long name entry are.
constexpr uint8_t kLongNameOffsets[kUnitsPerLongEntry] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

constexpr uint32_t kFsInfoLeadSignature = 0x41615252;
constexpr uint32_t kFsInfoStructSignature = 0x61417272;
constexpr uint32_t kFsInfoTrailSignature = 0xAA550000;

constexpr uint32_t kPartitionStart = 2048;  // 1 MB, the usual alignment for flash media

// There is no clock to stamp files with, so every entry gets 2026-01-01 00:00.
constexpr uint16_t kFileDate = ((2026 - 1980) << 9) | (1 << 5) | 1;
constexpr uint16_t kFileTime = 0;

constexpr uint8_t kDotName[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
constexpr uint8_t kDotDotName[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };

uint16_t Read16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t Read32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void Write16(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void Write32(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

bool IsSeparator(char c)
{
    return c == '/' || c == '\\';
}

bool IsDotName(const char* name, size_t length)
{
    return (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.');
}

char ToUpper(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

uint16_t FoldCase(uint16_t unit)
{
    return (unit >= 'a' && unit <= 'z') ? static_cast<uint16_t>(unit - 'a' + 'A') : unit;
}

// Characters an 8.3 name may hold, apart from upper-case letters and digits.
// Bytes above 0x7F would need a code page, so they are left out.
bool IsShortChar(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
    {
        return true;
    }
    return c != '\0' && std::strchr("!#$%&'()-@^_`{}~", c) != nullptr;
}

// Fills `out` with the 11-byte form of `name` when the name is a valid 8.3
// name. With `foldCase` any letter case is taken (for lookups); without it
// each half must be all one case, which `lowerBits` records.
bool ShortNameFor(const char* name, size_t length, uint8_t* out, uint8_t& lowerBits, bool foldCase)
{
    if (length == 0 || length > 12 || name[0] == '.')
    {
        return false;
    }

    size_t dot = length;
    for (size_t i = 0; i < length; ++i)
    {
        if (name[i] == '.')
        {
            if (dot != length)
            {
                return false;
            }
            dot = i;
        }
    }
    const size_t baseLength = dot;
    const size_t extensionLength = dot == length ? 0 : length - dot - 1;
    if (baseLength > 8 || extensionLength > 3 || (dot != length && extensionLength == 0))
    {
        return false;
    }

    std::memset(out, ' ', 11);
    bool lower[2] = { false, false };
    bool upper[2] = { false, false };
    for (size_t i = 0; i < length; ++i)
    {
        if (i == dot)
        {
            continue;
        }
        const int half = i < dot ? 0 : 1;
        const char c = name[i];
        if (c >= 'a' && c <= 'z')
        {
            lower[half] = true;
        }
        else if (c >= 'A' && c <= 'Z')
        {
            upper[half] = true;
        }
        const char stored = ToUpper(c);
        if (!IsShortChar(stored))
        {
            return false;
        }
        out[i < dot ? i : 8 + (i - dot - 1)] = static_cast<uint8_t>(stored);
    }

    if (!foldCase && ((lower[0] && upper[0]) || (lower[1] && upper[1])))
    {
        return false;
    }
    lowerBits = static_cast<uint8_t>((lower[0] ? kLowerCaseBase : 0) | (lower[1] ? kLowerCaseExtension : 0));
    return true;
}

// Walks a UTF-8 name as the UTF-16 code units a long name entry stores.
class NameUnits
{
public:
    static constexpr uint16_t kInvalid = 0xFFFD;

    NameUnits(const char* text, size_t length)
        : m_Text(text),
          m_End(text + length)
    {
    }

    /// The next code unit, or 0 at the end.
    uint16_t Next()
    {
        if (m_Pending != 0)
        {
            const uint16_t unit = m_Pending;
            m_Pending = 0;
            return unit;
        }
        if (m_Text >= m_End)
        {
            return 0;
        }

        const uint8_t lead = static_cast<uint8_t>(*m_Text++);
        if (lead < 0x80)
        {
            return lead;
        }

        int extra = 0;
        uint32_t code = 0;
        if ((lead & 0xE0) == 0xC0)
        {
            extra = 1;
            code = lead & 0x1F;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            extra = 2;
            code = lead & 0x0F;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            extra = 3;
            code = lead & 0x07;
        }
        else
        {
            return kInvalid;
        }

        for (int i = 0; i < extra; ++i)
        {
            if (m_Text >= m_End || (static_cast<uint8_t>(*m_Text) & 0xC0) != 0x80)
            {
                return kInvalid;
            }
            code = (code << 6) | (static_cast<uint8_t>(*m_Text++) & 0x3F);
        }

        // Past the first plane a character takes two units (a surrogate pair).
        if (code >= 0x10000)
        {
            if (code > 0x10FFFF)
            {
                return kInvalid;
            }
            code -= 0x10000;
            m_Pending = static_cast<uint16_t>(0xDC00 | (code & 0x3FF));
            return static_cast<uint16_t>(0xD800 | (code >> 10));
        }
        return static_cast<uint16_t>(code);
    }

    void Skip(uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            Next();
        }
    }

private:
    const char* m_Text;
    const char* m_End;
    uint16_t m_Pending = 0;
};

// How many code units `name` takes as a long name, or 0 when it cannot be one.
uint32_t LongNameUnitCount(const char* name, size_t length)
{
    // A NUL byte would end the walk below early, so it is refused here.
    if (length == 0 || IsDotName(name, length) || name[length - 1] == '.' || name[length - 1] == ' ' ||
        std::memchr(name, '\0', length) != nullptr)
    {
        return 0;
    }

    NameUnits units(name, length);
    uint32_t count = 0;
    for (uint16_t unit = units.Next(); unit != 0; unit = units.Next())
    {
        if (unit < 0x20 || unit == NameUnits::kInvalid || (unit < 0x80 && std::strchr("\"*/:<>?\\|", unit) != nullptr))
        {
            return 0;
        }
        if (++count > kMaxNameUnits)
        {
            return 0;
        }
    }
    return count;
}

char AliasChar(char c)
{
    if (static_cast<uint8_t>(c) >= 0x80)
    {
        return '_';
    }
    const char upper = ToUpper(c);
    return IsShortChar(upper) ? upper : '_';
}

// The 8.3 alias of a long name: up to six characters of it, "~" and `number`.
void MakeAlias(const char* name, size_t length, uint32_t number, uint8_t* out)
{
    std::memset(out, ' ', 11);

    // The extension is what follows the last dot, unless that dot starts the name.
    const char* end = name + length;
    const char* lastDot = nullptr;
    for (const char* p = name + 1; p < end; ++p)
    {
        if (*p == '.')
        {
            lastDot = p;
        }
    }
    const char* baseEnd = lastDot != nullptr ? lastDot : end;

    char base[8] = {};
    size_t baseLength = 0;
    for (const char* p = name; p < baseEnd && baseLength < sizeof(base); ++p)
    {
        // Spaces and dots are dropped, and so are the continuation bytes of a
        // UTF-8 character, which becomes one '_'.
        if (*p == ' ' || *p == '.' || (static_cast<uint8_t>(*p) & 0xC0) == 0x80)
        {
            continue;
        }
        base[baseLength++] = AliasChar(*p);
    }
    if (lastDot != nullptr)
    {
        size_t extensionLength = 0;
        for (const char* p = lastDot + 1; p < end && extensionLength < 3; ++p)
        {
            if (*p == ' ' || *p == '.' || (static_cast<uint8_t>(*p) & 0xC0) == 0x80)
            {
                continue;
            }
            out[8 + extensionLength++] = static_cast<uint8_t>(AliasChar(*p));
        }
    }
    if (baseLength == 0)
    {
        base[baseLength++] = '_';
    }

    char tail[8] = {};
    size_t tailLength = 0;
    char digits[8] = {};
    size_t digitCount = 0;
    do
    {
        digits[digitCount++] = static_cast<char>('0' + number % 10);
        number /= 10;
    } while (number != 0 && digitCount < 7);
    tail[tailLength++] = '~';
    while (digitCount > 0)
    {
        tail[tailLength++] = digits[--digitCount];
    }

    const size_t keep = std::min(baseLength, 8 - tailLength);
    std::memcpy(out, base, keep);
    std::memcpy(out + keep, tail, tailLength);
}

uint8_t ShortNameChecksum(const uint8_t* shortName)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i)
    {
        sum = static_cast<uint8_t>(((sum & 1) << 7) + (sum >> 1) + shortName[i]);
    }
    return sum;
}

// Whether a long name entry holds its part of `name`. Letter case is ignored
// for ASCII. The padding after the terminator is not checked.
bool LongEntryMatches(const uint8_t* entry, const char* name, size_t length, uint32_t unitCount)
{
    const uint32_t first = ((entry[0] & kLongSequenceMask) - 1u) * kUnitsPerLongEntry;
    NameUnits units(name, length);
    units.Skip(first);
    for (uint32_t k = 0; k < kUnitsPerLongEntry; ++k)
    {
        const uint32_t index = first + k;
        const uint16_t stored = Read16(entry + kLongNameOffsets[k]);
        if (index < unitCount)
        {
            if (FoldCase(stored) != FoldCase(units.Next()))
            {
                return false;
            }
        }
        else
        {
            return index > unitCount || stored == 0;
        }
    }
    return true;
}

void BuildLongEntry(uint8_t* entry, const char* name, size_t length, uint32_t unitCount, uint32_t sequence, bool last,
                    uint8_t checksum)
{
    std::memset(entry, 0, kEntrySize);
    entry[0] = static_cast<uint8_t>(sequence | (last ? kLastLongEntry : 0));
    entry[11] = kAttrLongName;
    entry[13] = checksum;

    const uint32_t first = (sequence - 1) * kUnitsPerLongEntry;
    NameUnits units(name, length);
    units.Skip(first);
    for (uint32_t k = 0; k < kUnitsPerLongEntry; ++k)
    {
        // The name ends with a 0 unit when there is room, then 0xFFFF padding.
        const uint32_t index = first + k;
        const uint16_t unit = index < unitCount ? units.Next() : (index == unitCount ? 0x0000 : 0xFFFF);
        Write16(entry + kLongNameOffsets[k], unit);
    }
}

void BuildShortEntry(uint8_t* entry, const uint8_t* shortName, uint8_t attributes, uint8_t lowerBits,
                     uint32_t firstCluster, bool fat32)
{
    std::memset(entry, 0, kEntrySize);
    std::memcpy(entry, shortName, 11);
    entry[11] = attributes;
    entry[12] = lowerBits;
    Write16(entry + 14, kFileTime);
    Write16(entry + 16, kFileDate);
    Write16(entry + 18, kFileDate);
    Write16(entry + 20, fat32 ? firstCluster >> 16 : 0);
    Write16(entry + 22, kFileTime);
    Write16(entry + 24, kFileDate);
    Write16(entry + 26, firstCluster & 0xFFFF);
}

// "NAME.EXT" from an 8.3 entry, with the NT lower-case bits applied.
void FormatShortName(const uint8_t* entry, char* out)
{
    size_t length = 0;
    for (int i = 0; i < 8 && entry[i] != ' '; ++i)
    {
        char c = static_cast<char>(entry[i]);
        if (i == 0 && entry[0] == 0x05)
        {
            c = '?';  // 0x05 stands for a first byte of 0xE5
        }
        if ((entry[12] & kLowerCaseBase) != 0 && c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
        out[length++] = static_cast<uint8_t>(c) < 0x80 ? c : '?';
    }
    if (entry[8] != ' ')
    {
        out[length++] = '.';
        for (int i = 8; i < 11 && entry[i] != ' '; ++i)
        {
            char c = static_cast<char>(entry[i]);
            if ((entry[12] & kLowerCaseExtension) != 0 && c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
            out[length++] = static_cast<uint8_t>(c) < 0x80 ? c : '?';
        }
    }
    out[length] = '\0';
}

uint32_t EntryCluster(const uint8_t* entry, bool fat32)
{
    const uint32_t high = fat32 ? Read16(entry + 20) : 0;
    return (high << 16) | Read16(entry + 26);
}

bool IsLongEntry(const uint8_t* entry)
{
    return (entry[11] & kAttrLongNameMask) == kAttrLongName;
}

bool LooksLikeBootSector(const uint8_t* sector)
{
    const uint32_t sectorsPerCluster = sector[13];
    return (sector[0] == 0xEB || sector[0] == 0xE9) && Read16(sector + 11) == kSectorSize && sectorsPerCluster != 0 &&
           (sectorsPerCluster & (sectorsPerCluster - 1)) == 0 && Read16(sector + 14) != 0 &&
           (sector[16] == 1 || sector[16] == 2) && sector[510] == 0x55 && sector[511] == 0xAA;
}

bool IsFatPartitionType(uint8_t type)
{
    return type == 0x04 || type == 0x06 || type == 0x0E || type == 0x0B || type == 0x0C;
}

FatDir DirAt(uint32_t startCluster)
{
    FatDir dir;
    dir.startCluster = startCluster;
    return dir;
}

uint8_t ClusterShiftOf(uint32_t sectorsPerCluster)
{
    uint8_t shift = 0;
    while ((1u << shift) < sectorsPerCluster)
    {
        ++shift;
    }
    return shift;
}

// Sectors per cluster for a volume of `sectors`, from Microsoft's table.
uint32_t DefaultClusterSectors(uint32_t sectors, bool fat32)
{
    if (fat32)
    {
        if (sectors <= 532480)
        {
            return 1;
        }
        if (sectors <= 16777216)
        {
            return 8;
        }
        if (sectors <= 33554432)
        {
            return 16;
        }
        if (sectors <= 67108864)
        {
            return 32;
        }
        return 64;
    }
    if (sectors <= 32680)
    {
        return 2;
    }
    if (sectors <= 262144)
    {
        return 4;
    }
    if (sectors <= 524288)
    {
        return 8;
    }
    if (sectors <= 1048576)
    {
        return 16;
    }
    if (sectors <= 2097152)
    {
        return 32;
    }
    if (sectors <= 4194304)
    {
        return 64;
    }
    return 0;
}

}  // namespace

FatVolume::~FatVolume()
{
    Unmount();
}

// ---- Mounting ----

bool FatVolume::Mount(IBlockDevice& device)
{
    Unmount();

    auto fail = [this](const char* why)
    {
        DEKI_LOG_ERROR("FatVolume: %s", why);
        m_Device = nullptr;
        m_Cache.Reset();
        m_Type = FatType::None;
        return false;
    };

    if (!m_Cache.Allocate(kCacheSectors * kSectorSize, Deki::Memory::Internal))
    {
        DEKI_LOG_ERROR("FatVolume: no memory for the sector cache");
        return false;
    }
    m_Device = &device;
    for (CacheSlot& slot : m_Slots)
    {
        slot = CacheSlot{};
    }
    m_UseCounter = 0;

    // Sector 0 is either the volume's boot sector (a card formatted without
    // partitions) or an MBR.
    uint32_t start = 0;
    const uint8_t* boot = GetSector(0, Access::Read);
    if (boot == nullptr)
    {
        return fail("could not read sector 0");
    }
    if (!LooksLikeBootSector(boot))
    {
        if (boot[510] != 0x55 || boot[511] != 0xAA)
        {
            return fail("no FAT volume or partition table");
        }
        bool found = false;
        for (int i = 0; i < 4 && !found; ++i)
        {
            const uint8_t* partition = boot + 446 + 16 * i;
            if (IsFatPartitionType(partition[4]) && Read32(partition + 8) != 0)
            {
                start = Read32(partition + 8);
                found = true;
            }
        }
        if (!found)
        {
            return fail("no FAT partition");
        }
        boot = GetSector(start, Access::Read);
        if (boot == nullptr || !LooksLikeBootSector(boot))
        {
            return fail("the partition holds no FAT volume");
        }
    }

    // The BPB. Every field is copied out before the cache is used again.
    const uint32_t sectorsPerCluster = boot[13];
    const uint32_t reserved = Read16(boot + 14);
    const uint32_t fatCount = boot[16];
    const uint32_t rootEntries = Read16(boot + 17);
    const uint32_t totalSectors = Read16(boot + 19) != 0 ? Read16(boot + 19) : Read32(boot + 32);
    const uint32_t fatSize16 = Read16(boot + 22);
    const bool fat32 = fatSize16 == 0;
    const uint32_t fatSize = fat32 ? Read32(boot + 36) : fatSize16;
    const uint32_t extendedFlags = fat32 ? Read16(boot + 40) : 0;
    const uint32_t rootCluster = fat32 ? Read32(boot + 44) : 0;
    const uint32_t fsInfo = fat32 ? Read16(boot + 48) : 0;

    const uint32_t rootDirSectors = (rootEntries * kEntrySize + kSectorSize - 1) / kSectorSize;
    const uint64_t metaSectors =
        static_cast<uint64_t>(reserved) + static_cast<uint64_t>(fatCount) * fatSize + rootDirSectors;
    if (fatSize == 0 || totalSectors <= metaSectors)
    {
        return fail("the boot sector gives sizes that do not fit");
    }
    if (static_cast<uint64_t>(start) + totalSectors > device.SectorCount())
    {
        return fail("the volume is larger than the card");
    }

    m_ClusterShift = ClusterShiftOf(sectorsPerCluster);
    uint32_t clusters = static_cast<uint32_t>((totalSectors - metaSectors) >> m_ClusterShift);
    uint32_t clustersInFat = 0;
    if (fat32)
    {
        // The layout says FAT32 (no 16-bit FAT size). Volumes with fewer
        // clusters than the specification asks for exist and are read anyway.
        if (rootEntries != 0)
        {
            return fail("a FAT32 boot sector with a fixed root directory");
        }
        m_Type = FatType::Fat32;
        clustersInFat = fatSize * (kSectorSize / 4) - 2;
    }
    else
    {
        if (clusters < 4085)
        {
            return fail("FAT12 is not supported");
        }
        if (clusters >= 65525)
        {
            return fail("a FAT16 volume with too many clusters");
        }
        m_Type = FatType::Fat16;
        clustersInFat = fatSize * (kSectorSize / 2) - 2;
    }
    clusters = std::min(clusters, clustersInFat);

    m_FatStart = start + reserved;
    m_FatSize = fatSize;
    m_FatCopies = fatCount;
    if (fat32 && (extendedFlags & 0x80) != 0)
    {
        // Mirroring is off: only the active FAT is used.
        const uint32_t active = extendedFlags & 0x0F;
        if (active >= fatCount)
        {
            return fail("the active FAT does not exist");
        }
        m_FatStart += active * fatSize;
        m_FatCopies = 1;
    }
    m_RootDirSector = start + static_cast<uint32_t>(reserved + fatCount * fatSize);
    m_RootEntryCount = rootEntries;
    m_DataStart = m_RootDirSector + rootDirSectors;
    m_ClusterCount = clusters;
    m_RootCluster = rootCluster;
    m_FreeCount = kUnknownCount;
    m_NextFree = 2;
    m_FsInfoSector = 0;
    m_FsInfoInvalidated = false;

    if (fat32 && !IsValidCluster(m_RootCluster))
    {
        return fail("the root directory cluster is out of range");
    }

    // FSInfo keeps the free count and where the last allocation ended, so a
    // mount need not read the whole FAT. Both are hints and are checked.
    if (fat32 && fsInfo != 0 && fsInfo < reserved)
    {
        const uint8_t* info = GetSector(start + fsInfo, Access::Read);
        if (info != nullptr && Read32(info) == kFsInfoLeadSignature && Read32(info + 484) == kFsInfoStructSignature)
        {
            m_FsInfoSector = start + fsInfo;
            const uint32_t freeCount = Read32(info + 488);
            const uint32_t nextFree = Read32(info + 492);
            if (freeCount <= m_ClusterCount)
            {
                m_FreeCount = freeCount;
            }
            if (IsValidCluster(nextFree))
            {
                m_NextFree = nextFree;
            }
        }
    }

    DEKI_LOG_DEBUG("FatVolume: FAT%d, %u clusters of %u bytes", fat32 ? 32 : 16, m_ClusterCount, ClusterBytes());
    return true;
}

void FatVolume::Unmount()
{
    if (m_Device == nullptr)
    {
        return;
    }
    Sync();
    m_Device = nullptr;
    m_Cache.Reset();
    m_Type = FatType::None;
}

uint64_t FatVolume::TotalBytes() const
{
    return m_Device != nullptr ? static_cast<uint64_t>(m_ClusterCount) * ClusterBytes() : 0;
}

uint64_t FatVolume::FreeBytes()
{
    if (m_Device == nullptr)
    {
        return 0;
    }
    if (m_FreeCount == kUnknownCount)
    {
        uint32_t count = 0;
        for (uint32_t cluster = 2; cluster < m_ClusterCount + 2; ++cluster)
        {
            const uint32_t value = ReadFat(cluster);
            if (value == kFatError)
            {
                return 0;
            }
            if (value == 0)
            {
                ++count;
            }
        }
        m_FreeCount = count;
    }
    return static_cast<uint64_t>(m_FreeCount) * ClusterBytes();
}

bool FatVolume::Sync()
{
    if (m_Device == nullptr)
    {
        return false;
    }

    // The FAT goes out before FSInfo gets the free count that matches it.
    bool ok = FlushCache();
    if (ok && m_FsInfoInvalidated)
    {
        if (m_FsInfoSector != 0)
        {
            ok = WriteFsInfo(m_FreeCount, m_NextFree);
        }
        m_FsInfoInvalidated = !ok;
    }
    return m_Device->Sync() && ok;
}

// ---- Files ----

bool FatVolume::Open(FatFile& file, const char* path, FatOpenMode mode)
{
    file = FatFile{};
    if (m_Device == nullptr)
    {
        return false;
    }

    uint32_t dirStart = 0;
    const char* leaf = nullptr;
    size_t leafLength = 0;
    if (!FindParent(path, dirStart, leaf, leafLength) || leaf == nullptr)
    {
        return false;
    }

    EntryRef entry;
    const bool found = FindEntry(dirStart, leaf, leafLength, entry);
    if (found && (entry.attributes & kAttrDirectory) != 0)
    {
        return false;
    }

    if (mode == FatOpenMode::Read)
    {
        if (!found)
        {
            return false;
        }
        file.firstCluster = entry.firstCluster;
        file.size = entry.size;
        file.entrySector = entry.sector;
        file.entryOffset = entry.offset;
        file.flags = kFatFileOpen | kFatFileRead;
        return true;
    }

    if (!found)
    {
        if (!AddEntry(dirStart, leaf, leafLength, kAttrArchive, 0, entry))
        {
            return false;
        }
    }
    else if ((entry.attributes & kAttrReadOnly) != 0)
    {
        return false;
    }

    file.firstCluster = entry.firstCluster;
    file.size = entry.size;
    file.entrySector = entry.sector;
    file.entryOffset = entry.offset;
    file.flags = kFatFileOpen | kFatFileWrite;

    if (mode == FatOpenMode::Write && (file.firstCluster != 0 || file.size != 0))
    {
        // Empty the file. The entry is written first, so a failure after it
        // leaves lost clusters, never an entry that points at free ones.
        const uint32_t oldChain = file.firstCluster;
        file.firstCluster = 0;
        file.size = 0;
        if (!WriteEntry(file) || !FlushCache())
        {
            file = FatFile{};
            return false;
        }
        if (oldChain != 0 && !FreeChain(oldChain))
        {
            file = FatFile{};
            return false;
        }
    }
    if (mode == FatOpenMode::Append)
    {
        file.position = file.size;
    }
    return true;
}

bool FatVolume::Close(FatFile& file)
{
    if ((file.flags & kFatFileOpen) == 0)
    {
        return false;
    }
    const bool ok = (file.flags & kFatFileWrite) == 0 || Flush(file);
    file = FatFile{};
    return ok;
}

bool FatVolume::Flush(FatFile& file)
{
    if (m_Device == nullptr || (file.flags & kFatFileOpen) == 0)
    {
        return false;
    }
    if ((file.flags & kFatFileWrite) == 0)
    {
        return true;
    }
    if ((file.flags & kFatFileDirty) != 0)
    {
        if (!WriteEntry(file))
        {
            return false;
        }
        file.flags = static_cast<uint8_t>(file.flags & ~kFatFileDirty);
    }
    return FlushCache() && m_Device->Sync();
}

size_t FatVolume::Read(FatFile& file, void* buffer, size_t size)
{
    if (m_Device == nullptr || (file.flags & kFatFileRead) == 0 || buffer == nullptr || file.position >= file.size)
    {
        return 0;
    }

    uint8_t* out = static_cast<uint8_t*>(buffer);
    const size_t wanted = std::min<size_t>(size, file.size - file.position);
    const uint32_t clusterMask = ClusterBytes() - 1;
    const uint32_t sectorsPerCluster = 1u << m_ClusterShift;
    size_t done = 0;
    while (done < wanted)
    {
        uint32_t cluster = 0;
        if (!ClusterAt(file, file.position, false, cluster))
        {
            break;
        }
        const uint32_t sectorInCluster = (file.position & clusterMask) / kSectorSize;
        const uint32_t sector = ClusterSector(cluster) + sectorInCluster;
        const uint32_t inSector = file.position % kSectorSize;
        const size_t remaining = wanted - done;

        size_t step = 0;
        if (inSector == 0 && remaining >= kSectorSize)
        {
            // Whole sectors go straight into the caller's buffer.
            const uint32_t count =
                static_cast<uint32_t>(std::min<size_t>(remaining / kSectorSize, sectorsPerCluster - sectorInCluster));
            if (!ReadDirect(sector, count, out + done))
            {
                break;
            }
            step = static_cast<size_t>(count) * kSectorSize;
        }
        else
        {
            const uint8_t* cached = GetSector(sector, Access::Read);
            if (cached == nullptr)
            {
                break;
            }
            step = std::min<size_t>(kSectorSize - inSector, remaining);
            std::memcpy(out + done, cached + inSector, step);
        }
        done += step;
        file.position += static_cast<uint32_t>(step);
    }
    return done;
}

size_t FatVolume::Write(FatFile& file, const void* data, size_t size)
{
    if (m_Device == nullptr || (file.flags & kFatFileWrite) == 0 || data == nullptr)
    {
        return 0;
    }

    // A seek past the end leaves a gap, which reads back as zeros.
    while (file.size < file.position)
    {
        static constexpr uint8_t kZeros[64] = {};
        const uint32_t target = file.position;
        const size_t gap = std::min<size_t>(target - file.size, sizeof(kZeros));
        file.position = file.size;
        const size_t written = WriteBytes(file, kZeros, gap);
        file.position = target;
        if (written != gap)
        {
            return 0;
        }
    }

    // A FAT file holds at most 4 GB - 1.
    const size_t room = 0xFFFFFFFFu - file.position;
    return WriteBytes(file, static_cast<const uint8_t*>(data), std::min(size, room));
}

bool FatVolume::Seek(FatFile& file, uint32_t position)
{
    if ((file.flags & kFatFileOpen) == 0)
    {
        return false;
    }
    file.position = position;
    return true;
}

size_t FatVolume::WriteBytes(FatFile& file, const uint8_t* data, size_t size)
{
    const uint32_t clusterMask = ClusterBytes() - 1;
    const uint32_t sectorsPerCluster = 1u << m_ClusterShift;
    size_t done = 0;
    while (done < size)
    {
        uint32_t cluster = 0;
        if (!ClusterAt(file, file.position, true, cluster))
        {
            break;
        }
        const uint32_t sectorInCluster = (file.position & clusterMask) / kSectorSize;
        const uint32_t sector = ClusterSector(cluster) + sectorInCluster;
        const uint32_t inSector = file.position % kSectorSize;
        const size_t remaining = size - done;

        size_t step = 0;
        if (inSector == 0 && remaining >= kSectorSize)
        {
            const uint32_t count =
                static_cast<uint32_t>(std::min<size_t>(remaining / kSectorSize, sectorsPerCluster - sectorInCluster));
            if (!WriteDirect(sector, count, data + done))
            {
                break;
            }
            step = static_cast<size_t>(count) * kSectorSize;
        }
        else
        {
            // A sector that starts at or past the end of the file holds none
            // of its bytes, so it need not be read first.
            const Access access = file.position - inSector >= file.size ? Access::Overwrite : Access::Modify;
            uint8_t* cached = GetSector(sector, access);
            if (cached == nullptr)
            {
                break;
            }
            step = std::min<size_t>(kSectorSize - inSector, remaining);
            std::memcpy(cached + inSector, data + done, step);
        }
        done += step;
        file.position += static_cast<uint32_t>(step);
        file.size = std::max(file.size, file.position);
    }
    if (done > 0)
    {
        file.flags |= kFatFileDirty;
    }
    return done;
}

bool FatVolume::ClusterAt(FatFile& file, uint32_t position, bool allocate, uint32_t& out)
{
    const uint32_t wanted = position >> (9 + m_ClusterShift);
    if (file.firstCluster == 0)
    {
        if (!allocate)
        {
            return false;
        }
        uint32_t first = 0;
        if (!AllocateCluster(0, first))
        {
            return false;
        }
        file.firstCluster = first;
        file.cluster = first;
        file.clusterIndex = 0;
        file.flags |= kFatFileDirty;
    }

    // Walk on from where the last call stopped, or from the start after a
    // seek backwards.
    if (file.cluster == 0 || file.clusterIndex > wanted)
    {
        file.cluster = file.firstCluster;
        file.clusterIndex = 0;
    }
    while (file.clusterIndex < wanted)
    {
        uint32_t next = ReadFat(file.cluster);
        if (next == kFatError)
        {
            return false;
        }
        if (next == kEndOfChain)
        {
            if (!allocate || !AllocateCluster(file.cluster, next))
            {
                return false;
            }
        }
        else if (!IsValidCluster(next))
        {
            DEKI_LOG_ERROR("FatVolume: a file's cluster chain is broken");
            return false;
        }
        file.cluster = next;
        ++file.clusterIndex;
    }
    out = file.cluster;
    return true;
}

bool FatVolume::WriteEntry(FatFile& file)
{
    uint8_t* sector = GetSector(file.entrySector, Access::Modify);
    if (sector == nullptr)
    {
        return false;
    }
    uint8_t* entry = sector + file.entryOffset;
    Write16(entry + 18, kFileDate);
    Write16(entry + 20, m_Type == FatType::Fat32 ? file.firstCluster >> 16 : 0);
    Write16(entry + 22, kFileTime);
    Write16(entry + 24, kFileDate);
    Write16(entry + 26, file.firstCluster & 0xFFFF);
    Write32(entry + 28, file.size);
    entry[11] |= kAttrArchive;
    return true;
}

// ---- Paths and directories ----

bool FatVolume::Stat(const char* path, FatStat& out)
{
    out = FatStat{};
    if (m_Device == nullptr)
    {
        return false;
    }
    uint32_t dirStart = 0;
    const char* leaf = nullptr;
    size_t leafLength = 0;
    if (!FindParent(path, dirStart, leaf, leafLength))
    {
        return false;
    }
    if (leaf == nullptr)
    {
        out.isDirectory = true;
        return true;
    }
    EntryRef entry;
    if (!FindEntry(dirStart, leaf, leafLength, entry))
    {
        return false;
    }
    out.isDirectory = (entry.attributes & kAttrDirectory) != 0;
    out.size = out.isDirectory ? 0 : entry.size;
    return true;
}

bool FatVolume::Exists(const char* path)
{
    FatStat stat;
    return Stat(path, stat);
}

bool FatVolume::CreateDirectory(const char* path)
{
    if (m_Device == nullptr || path == nullptr)
    {
        return false;
    }

    uint32_t dirStart = RootStart();
    const char* p = path;
    while (true)
    {
        while (IsSeparator(*p))
        {
            ++p;
        }
        if (*p == '\0')
        {
            break;
        }
        const char* part = p;
        while (*p != '\0' && !IsSeparator(*p))
        {
            ++p;
        }
        const size_t length = static_cast<size_t>(p - part);
        if (length == 1 && part[0] == '.')
        {
            continue;
        }

        EntryRef entry;
        if (FindEntry(dirStart, part, length, entry))
        {
            if ((entry.attributes & kAttrDirectory) == 0)
            {
                return false;
            }
            dirStart = EntryDirStart(entry.firstCluster);
            continue;
        }
        if (IsDotName(part, length))
        {
            return false;
        }

        // The new directory's cluster is ready, with "." and "..", before the
        // parent gets an entry that points at it.
        uint32_t cluster = 0;
        if (!AllocateCluster(0, cluster))
        {
            return false;
        }
        if (!ZeroCluster(cluster))
        {
            FreeChain(cluster);
            return false;
        }
        uint8_t* first = GetSector(ClusterSector(cluster), Access::Overwrite);
        if (first == nullptr)
        {
            FreeChain(cluster);
            return false;
        }
        const bool fat32 = m_Type == FatType::Fat32;
        BuildShortEntry(first, kDotName, kAttrDirectory, 0, cluster, fat32);
        // ".." of a directory in the root holds 0, also on FAT32.
        BuildShortEntry(first + kEntrySize, kDotDotName, kAttrDirectory, 0, dirStart == RootStart() ? 0 : dirStart,
                        fat32);

        if (!AddEntry(dirStart, part, length, kAttrDirectory, cluster, entry))
        {
            FreeChain(cluster);
            return false;
        }
        dirStart = cluster;
    }
    return FlushCache() && m_Device->Sync();
}

bool FatVolume::Remove(const char* path)
{
    if (m_Device == nullptr)
    {
        return false;
    }
    uint32_t dirStart = 0;
    const char* leaf = nullptr;
    size_t leafLength = 0;
    if (!FindParent(path, dirStart, leaf, leafLength) || leaf == nullptr || IsDotName(leaf, leafLength))
    {
        return false;
    }
    EntryRef entry;
    if (!FindEntry(dirStart, leaf, leafLength, entry))
    {
        return false;
    }
    if ((entry.attributes & kAttrDirectory) != 0 &&
        (entry.firstCluster == 0 || !IsDirectoryEmpty(EntryDirStart(entry.firstCluster))))
    {
        return false;
    }

    // The entries go first: a failure after that leaves lost clusters, never
    // an entry that points at free ones.
    FatDir dir = DirAt(dirStart);
    for (uint32_t i = 0; i < entry.entryCount; ++i)
    {
        uint8_t* slot = DirEntry(dir, entry.firstIndex + i, Access::Modify, nullptr);
        if (slot == nullptr)
        {
            return false;
        }
        slot[0] = kEntryFree;
    }
    if (!FlushCache())
    {
        return false;
    }
    if (entry.firstCluster != 0 && !FreeChain(entry.firstCluster))
    {
        return false;
    }
    return FlushCache() && m_Device->Sync();
}

bool FatVolume::OpenDir(FatDir& dir, const char* path)
{
    if (m_Device == nullptr)
    {
        return false;
    }
    uint32_t dirStart = 0;
    const char* leaf = nullptr;
    size_t leafLength = 0;
    if (!FindParent(path, dirStart, leaf, leafLength))
    {
        return false;
    }
    if (leaf != nullptr)
    {
        EntryRef entry;
        if (!FindEntry(dirStart, leaf, leafLength, entry) || (entry.attributes & kAttrDirectory) == 0)
        {
            return false;
        }
        dirStart = EntryDirStart(entry.firstCluster);
    }
    dir = DirAt(dirStart);
    return true;
}

bool FatVolume::ReadDir(FatDir& dir, FatEntryInfo& out)
{
    if (m_Device == nullptr)
    {
        return false;
    }

    // Long name entries come before their 8.3 entry, last part first. Each
    // one writes its 13 units into place in out.name.
    bool chainActive = false;
    uint8_t chainNext = 0;
    uint8_t chainSum = 0;
    uint32_t nameLength = 0;
    const uint32_t maxName = sizeof(out.name) - 1;
    while (true)
    {
        const uint8_t* entry = DirEntry(dir, dir.index, Access::Read, nullptr);
        if (entry == nullptr || entry[0] == kEntryEnd)
        {
            return false;
        }
        ++dir.index;

        if (entry[0] == kEntryFree)
        {
            chainActive = false;
            continue;
        }
        if (IsLongEntry(entry))
        {
            const uint8_t sequence = entry[0] & kLongSequenceMask;
            if ((entry[0] & kLastLongEntry) != 0)
            {
                chainActive = sequence != 0 && sequence <= kMaxLongEntries;
                chainSum = entry[13];
                nameLength = std::min<uint32_t>(sequence * kUnitsPerLongEntry, maxName);
            }
            else if (!chainActive || sequence != chainNext || entry[13] != chainSum)
            {
                chainActive = false;
            }
            if (!chainActive)
            {
                continue;
            }
            chainNext = static_cast<uint8_t>(sequence - 1);
            for (uint32_t k = 0; k < kUnitsPerLongEntry; ++k)
            {
                const uint32_t index = (sequence - 1u) * kUnitsPerLongEntry + k;
                const uint16_t unit = Read16(entry + kLongNameOffsets[k]);
                if (unit == 0x0000 || unit == 0xFFFF)
                {
                    nameLength = std::min(nameLength, index);
                    break;
                }
                if (index < maxName)
                {
                    out.name[index] = unit < 0x80 ? static_cast<char>(unit) : '?';
                }
            }
            continue;
        }

        const bool chainOk = chainActive && chainNext == 0 && ShortNameChecksum(entry) == chainSum;
        chainActive = false;
        if ((entry[11] & kAttrVolumeId) != 0 || entry[0] == '.')
        {
            continue;
        }
        if (chainOk)
        {
            out.name[nameLength] = '\0';
        }
        else
        {
            FormatShortName(entry, out.name);
        }
        out.isDirectory = (entry[11] & kAttrDirectory) != 0;
        out.size = out.isDirectory ? 0 : Read32(entry + 28);
        return true;
    }
}

bool FatVolume::FindParent(const char* path, uint32_t& dirStart, const char*& leaf, size_t& leafLength)
{
    dirStart = RootStart();
    leaf = nullptr;
    leafLength = 0;
    if (path == nullptr)
    {
        return false;
    }

    const char* p = path;
    while (true)
    {
        while (IsSeparator(*p))
        {
            ++p;
        }
        if (*p == '\0')
        {
            return true;
        }
        const char* part = p;
        while (*p != '\0' && !IsSeparator(*p))
        {
            ++p;
        }
        const size_t length = static_cast<size_t>(p - part);
        const bool isDot = length == 1 && part[0] == '.';

        const char* rest = p;
        while (IsSeparator(*rest))
        {
            ++rest;
        }
        if (*rest == '\0')
        {
            // The last part. "." names the directory itself, like the root.
            if (!isDot)
            {
                leaf = part;
                leafLength = length;
            }
            return true;
        }
        if (isDot)
        {
            continue;
        }

        EntryRef entry;
        if (!FindEntry(dirStart, part, length, entry) || (entry.attributes & kAttrDirectory) == 0)
        {
            return false;
        }
        dirStart = EntryDirStart(entry.firstCluster);
    }
}

bool FatVolume::FindEntry(uint32_t dirStart, const char* name, size_t length, EntryRef& out)
{
    uint8_t shortName[11] = {};
    uint8_t lowerBits = 0;
    bool shortValid = false;
    uint32_t unitCount = 0;
    if (IsDotName(name, length))
    {
        std::memcpy(shortName, length == 1 ? kDotName : kDotDotName, 11);
        shortValid = true;
    }
    else
    {
        shortValid = ShortNameFor(name, length, shortName, lowerBits, true);
        unitCount = LongNameUnitCount(name, length);
        if (!shortValid && unitCount == 0)
        {
            return false;
        }
    }

    bool chainActive = false;
    bool chainMatches = false;
    uint8_t chainNext = 0;
    uint8_t chainSum = 0;
    uint32_t chainStart = 0;
    FatDir dir = DirAt(dirStart);
    for (uint32_t index = 0;; ++index)
    {
        uint32_t sector = 0;
        const uint8_t* entry = DirEntry(dir, index, Access::Read, &sector);
        if (entry == nullptr || entry[0] == kEntryEnd)
        {
            return false;
        }
        if (entry[0] == kEntryFree)
        {
            chainActive = false;
            continue;
        }

        if (IsLongEntry(entry))
        {
            const uint8_t sequence = entry[0] & kLongSequenceMask;
            if ((entry[0] & kLastLongEntry) != 0)
            {
                chainActive = sequence != 0 && sequence <= kMaxLongEntries;
                chainSum = entry[13];
                chainStart = index;
                // The chain must be exactly as long as the name needs.
                chainMatches =
                    unitCount > (sequence - 1u) * kUnitsPerLongEntry && unitCount <= sequence * kUnitsPerLongEntry;
            }
            else if (!chainActive || sequence != chainNext || entry[13] != chainSum)
            {
                chainActive = false;
            }
            if (chainActive)
            {
                chainNext = static_cast<uint8_t>(sequence - 1);
                chainMatches = chainMatches && LongEntryMatches(entry, name, length, unitCount);
            }
            continue;
        }

        const bool chainOk = chainActive && chainNext == 0 && ShortNameChecksum(entry) == chainSum;
        chainActive = false;
        if ((entry[11] & kAttrVolumeId) != 0)
        {
            continue;
        }
        if ((chainOk && chainMatches) || (shortValid && std::memcmp(entry, shortName, 11) == 0))
        {
            out.dirStart = dirStart;
            out.sector = sector;
            out.offset = static_cast<uint16_t>((index % kEntriesPerSector) * kEntrySize);
            out.firstIndex = chainOk ? chainStart : index;
            out.entryCount = chainOk ? index - chainStart + 1 : 1;
            out.attributes = entry[11];
            out.firstCluster = EntryCluster(entry, m_Type == FatType::Fat32);
            out.size = Read32(entry + 28);
            return true;
        }
    }
}

bool FatVolume::AddEntry(uint32_t dirStart, const char* name, size_t length, uint8_t attributes, uint32_t firstCluster,
                         EntryRef& out)
{
    uint8_t shortName[11] = {};
    uint8_t lowerBits = 0;
    uint32_t unitCount = 0;
    uint32_t longEntries = 0;
    if (!ShortNameFor(name, length, shortName, lowerBits, false))
    {
        unitCount = LongNameUnitCount(name, length);
        if (unitCount == 0)
        {
            return false;
        }
        longEntries = (unitCount + kUnitsPerLongEntry - 1) / kUnitsPerLongEntry;
        lowerBits = 0;

        // The 8.3 alias: the name itself in upper case when that is a free
        // 8.3 name (as for "Save.Bin"), otherwise NAME~1.EXT, NAME~2.EXT...
        uint8_t ignored = 0;
        bool haveAlias = ShortNameFor(name, length, shortName, ignored, true) && !ShortNameInUse(dirStart, shortName);
        for (uint32_t number = 1; !haveAlias && number <= kMaxAliasNumber; ++number)
        {
            MakeAlias(name, length, number, shortName);
            haveAlias = !ShortNameInUse(dirStart, shortName);
        }
        if (!haveAlias)
        {
            return false;
        }
    }

    // A run of free entries long enough for the long name and the 8.3 entry.
    // The directory grows by a cluster when it has none.
    const uint32_t needed = longEntries + 1;
    FatDir dir = DirAt(dirStart);
    uint32_t runStart = 0;
    uint32_t runLength = 0;
    for (uint32_t index = 0; runLength < needed; ++index)
    {
        const uint32_t sector = DirSector(dir, index, true);
        if (sector == 0)
        {
            return false;
        }
        const uint8_t* data = GetSector(sector, Access::Read);
        if (data == nullptr)
        {
            return false;
        }
        const uint8_t first = data[(index % kEntriesPerSector) * kEntrySize];
        if (first == kEntryEnd || first == kEntryFree)
        {
            if (runLength == 0)
            {
                runStart = index;
            }
            ++runLength;
        }
        else
        {
            runLength = 0;
        }
    }

    const uint8_t checksum = ShortNameChecksum(shortName);
    for (uint32_t k = 0; k < longEntries; ++k)
    {
        uint8_t* entry = DirEntry(dir, runStart + k, Access::Modify, nullptr);
        if (entry == nullptr)
        {
            return false;
        }
        BuildLongEntry(entry, name, length, unitCount, longEntries - k, k == 0, checksum);
    }

    const uint32_t shortIndex = runStart + longEntries;
    uint32_t sector = 0;
    uint8_t* entry = DirEntry(dir, shortIndex, Access::Modify, &sector);
    if (entry == nullptr)
    {
        return false;
    }
    BuildShortEntry(entry, shortName, attributes, lowerBits, firstCluster, m_Type == FatType::Fat32);

    out.dirStart = dirStart;
    out.sector = sector;
    out.offset = static_cast<uint16_t>((shortIndex % kEntriesPerSector) * kEntrySize);
    out.firstIndex = runStart;
    out.entryCount = needed;
    out.attributes = attributes;
    out.firstCluster = firstCluster;
    out.size = 0;
    return true;
}

bool FatVolume::ShortNameInUse(uint32_t dirStart, const uint8_t* shortName)
{
    FatDir dir = DirAt(dirStart);
    for (uint32_t index = 0;; ++index)
    {
        const uint8_t* entry = DirEntry(dir, index, Access::Read, nullptr);
        if (entry == nullptr || entry[0] == kEntryEnd)
        {
            return false;
        }
        if (entry[0] != kEntryFree && !IsLongEntry(entry) && std::memcmp(entry, shortName, 11) == 0)
        {
            return true;
        }
    }
}

bool FatVolume::IsDirectoryEmpty(uint32_t dirStart)
{
    FatDir dir = DirAt(dirStart);
    for (uint32_t index = 0;; ++index)
    {
        const uint8_t* entry = DirEntry(dir, index, Access::Read, nullptr);
        if (entry == nullptr || entry[0] == kEntryEnd)
        {
            return true;
        }
        if (entry[0] == kEntryFree || IsLongEntry(entry) || entry[0] == '.' || (entry[11] & kAttrVolumeId) != 0)
        {
            continue;
        }
        return false;
    }
}

uint32_t FatVolume::EntryDirStart(uint32_t firstCluster) const
{
    // An entry for the root (".." in a top-level directory) holds 0.
    return firstCluster == 0 ? RootStart() : firstCluster;
}

uint32_t FatVolume::DirSector(FatDir& dir, uint32_t index, bool extend)
{
    if (index >= kMaxDirEntries)
    {
        return 0;
    }
    if (dir.startCluster == 0)
    {
        // The FAT16 root directory: a fixed run of sectors.
        return index < m_RootEntryCount ? m_RootDirSector + index / kEntriesPerSector : 0;
    }

    const uint32_t entriesPerCluster = kEntriesPerSector << m_ClusterShift;
    const uint32_t wanted = index / entriesPerCluster;
    if (dir.cluster == 0 || dir.clusterIndex > wanted)
    {
        dir.cluster = dir.startCluster;
        dir.clusterIndex = 0;
    }
    while (dir.clusterIndex < wanted)
    {
        uint32_t next = ReadFat(dir.cluster);
        if (next == kFatError)
        {
            return 0;
        }
        if (next == kEndOfChain)
        {
            if (!extend || !AllocateCluster(dir.cluster, next))
            {
                return 0;
            }
            if (!ZeroCluster(next))
            {
                return 0;
            }
        }
        else if (!IsValidCluster(next))
        {
            DEKI_LOG_ERROR("FatVolume: a directory's cluster chain is broken");
            return 0;
        }
        dir.cluster = next;
        ++dir.clusterIndex;
    }
    return ClusterSector(dir.cluster) + (index % entriesPerCluster) / kEntriesPerSector;
}

uint8_t* FatVolume::DirEntry(FatDir& dir, uint32_t index, Access access, uint32_t* sectorOut)
{
    const uint32_t sector = DirSector(dir, index, false);
    if (sector == 0)
    {
        return nullptr;
    }
    uint8_t* data = GetSector(sector, access);
    if (data == nullptr)
    {
        return nullptr;
    }
    if (sectorOut != nullptr)
    {
        *sectorOut = sector;
    }
    return data + (index % kEntriesPerSector) * kEntrySize;
}

uint32_t FatVolume::RootStart() const
{
    return m_Type == FatType::Fat32 ? m_RootCluster : 0;
}

// ---- The FAT ----

bool FatVolume::IsValidCluster(uint32_t cluster) const
{
    return cluster >= 2 && cluster < m_ClusterCount + 2;
}

uint32_t FatVolume::ClusterSector(uint32_t cluster) const
{
    return m_DataStart + ((cluster - 2) << m_ClusterShift);
}

uint32_t FatVolume::ReadFat(uint32_t cluster)
{
    if (!IsValidCluster(cluster))
    {
        return kFatError;
    }
    const bool fat32 = m_Type == FatType::Fat32;
    const uint32_t offset = cluster * (fat32 ? 4 : 2);
    const uint8_t* data = GetSector(m_FatStart + offset / kSectorSize, Access::Read);
    if (data == nullptr)
    {
        return kFatError;
    }
    const uint8_t* entry = data + offset % kSectorSize;
    if (fat32)
    {
        // The top four bits are reserved.
        const uint32_t value = Read32(entry) & 0x0FFFFFFF;
        return value >= kEndOfChainMin ? kEndOfChain : value;
    }
    const uint32_t value = Read16(entry);
    if (value >= 0xFFF8)
    {
        return kEndOfChain;
    }
    return value == 0xFFF7 ? kBadCluster : value;
}

bool FatVolume::WriteFat(uint32_t cluster, uint32_t value)
{
    if (!IsValidCluster(cluster))
    {
        return false;
    }
    // Before taking the sector: this can use the cache.
    MarkFatChanged();

    const bool fat32 = m_Type == FatType::Fat32;
    const uint32_t offset = cluster * (fat32 ? 4 : 2);
    uint8_t* data = GetSector(m_FatStart + offset / kSectorSize, Access::Modify);
    if (data == nullptr)
    {
        return false;
    }
    uint8_t* entry = data + offset % kSectorSize;
    if (fat32)
    {
        Write32(entry, (Read32(entry) & 0xF0000000) | (value & 0x0FFFFFFF));
    }
    else
    {
        Write16(entry, value == kEndOfChain ? 0xFFFF : value);
    }
    return true;
}

bool FatVolume::AllocateCluster(uint32_t previous, uint32_t& out)
{
    if (m_FreeCount == 0)
    {
        return false;
    }

    // Search on from where the last allocation stopped, wrapping round once.
    uint32_t candidate = IsValidCluster(m_NextFree) ? m_NextFree : 2;
    for (uint32_t i = 0; i < m_ClusterCount; ++i)
    {
        const uint32_t value = ReadFat(candidate);
        if (value == kFatError)
        {
            return false;
        }
        if (value == 0)
        {
            if (!WriteFat(candidate, kEndOfChain))
            {
                return false;
            }
            if (previous != 0 && !WriteFat(previous, candidate))
            {
                return false;
            }
            if (m_FreeCount != kUnknownCount)
            {
                --m_FreeCount;
            }
            m_NextFree = IsValidCluster(candidate + 1) ? candidate + 1 : 2;
            out = candidate;
            return true;
        }
        candidate = IsValidCluster(candidate + 1) ? candidate + 1 : 2;
    }
    m_FreeCount = 0;
    return false;
}

bool FatVolume::FreeChain(uint32_t first)
{
    uint32_t cluster = first;
    for (uint32_t guard = 0; IsValidCluster(cluster) && guard < m_ClusterCount; ++guard)
    {
        const uint32_t next = ReadFat(cluster);
        if (next == kFatError || !WriteFat(cluster, 0))
        {
            return false;
        }
        if (m_FreeCount != kUnknownCount)
        {
            ++m_FreeCount;
        }
        if (next == kEndOfChain)
        {
            break;
        }
        cluster = next;
    }
    return true;
}

bool FatVolume::ZeroCluster(uint32_t cluster)
{
    uint8_t* zeros = ScratchSector();
    if (zeros == nullptr)
    {
        return false;
    }
    std::memset(zeros, 0, kSectorSize);
    const uint32_t first = ClusterSector(cluster);
    for (uint32_t i = 0; i < (1u << m_ClusterShift); ++i)
    {
        if (!WriteDirect(first + i, 1, zeros))
        {
            return false;
        }
    }
    return true;
}

void FatVolume::MarkFatChanged()
{
    // The free count in FSInfo is wrong from the first change on, so it is
    // marked unknown on the card right away. Sync writes the true count.
    if (m_FsInfoInvalidated)
    {
        return;
    }
    m_FsInfoInvalidated = true;
    if (m_FsInfoSector != 0)
    {
        WriteFsInfo(kUnknownCount, m_NextFree);
    }
}

bool FatVolume::WriteFsInfo(uint32_t freeCount, uint32_t nextFree)
{
    uint8_t* info = GetSector(m_FsInfoSector, Access::Modify);
    if (info == nullptr)
    {
        return false;
    }
    Write32(info + 488, freeCount);
    Write32(info + 492, nextFree);
    return FlushSector(m_FsInfoSector);
}

// ---- The sector cache ----

int FatVolume::PickSlot()
{
    int pick = 0;
    for (uint32_t i = 0; i < kCacheSectors; ++i)
    {
        if (!m_Slots[i].valid)
        {
            pick = static_cast<int>(i);
            break;
        }
        if (m_Slots[i].lastUse < m_Slots[pick].lastUse)
        {
            pick = static_cast<int>(i);
        }
    }
    CacheSlot& slot = m_Slots[pick];
    if (slot.valid && slot.dirty && !WriteSlot(slot, m_Cache.Data() + pick * kSectorSize))
    {
        return -1;
    }
    slot.valid = false;
    slot.dirty = false;
    return pick;
}

uint8_t* FatVolume::GetSector(uint32_t sector, Access access)
{
    for (uint32_t i = 0; i < kCacheSectors; ++i)
    {
        CacheSlot& slot = m_Slots[i];
        if (slot.valid && slot.sector == sector)
        {
            uint8_t* data = m_Cache.Data() + i * kSectorSize;
            slot.lastUse = ++m_UseCounter;
            if (access != Access::Read)
            {
                slot.dirty = true;
            }
            if (access == Access::Overwrite)
            {
                std::memset(data, 0, kSectorSize);
            }
            return data;
        }
    }

    const int pick = PickSlot();
    if (pick < 0)
    {
        return nullptr;
    }
    uint8_t* data = m_Cache.Data() + pick * kSectorSize;
    if (access == Access::Overwrite)
    {
        std::memset(data, 0, kSectorSize);
    }
    else if (!m_Device->ReadSectors(sector, 1, data))
    {
        return nullptr;
    }
    CacheSlot& slot = m_Slots[pick];
    slot.sector = sector;
    slot.lastUse = ++m_UseCounter;
    slot.valid = true;
    slot.dirty = access != Access::Read;
    return data;
}

uint8_t* FatVolume::ScratchSector()
{
    const int pick = PickSlot();
    return pick < 0 ? nullptr : m_Cache.Data() + pick * kSectorSize;
}

bool FatVolume::WriteSlot(CacheSlot& slot, const uint8_t* data)
{
    // A FAT sector goes to every copy, so the copies stay the same.
    if (slot.sector >= m_FatStart && slot.sector < m_FatStart + m_FatSize)
    {
        for (uint32_t copy = 0; copy < m_FatCopies; ++copy)
        {
            if (!m_Device->WriteSectors(slot.sector + copy * m_FatSize, 1, data))
            {
                return false;
            }
        }
    }
    else if (!m_Device->WriteSectors(slot.sector, 1, data))
    {
        return false;
    }
    slot.dirty = false;
    return true;
}

bool FatVolume::FlushCache()
{
    bool ok = true;
    for (uint32_t i = 0; i < kCacheSectors; ++i)
    {
        CacheSlot& slot = m_Slots[i];
        if (slot.valid && slot.dirty && !WriteSlot(slot, m_Cache.Data() + i * kSectorSize))
        {
            ok = false;
        }
    }
    return ok;
}

bool FatVolume::FlushSector(uint32_t sector)
{
    for (uint32_t i = 0; i < kCacheSectors; ++i)
    {
        CacheSlot& slot = m_Slots[i];
        if (slot.valid && slot.sector == sector && slot.dirty)
        {
            return WriteSlot(slot, m_Cache.Data() + i * kSectorSize);
        }
    }
    return true;
}

bool FatVolume::ReadDirect(uint32_t sector, uint32_t count, uint8_t* out)
{
    if (!m_Device->ReadSectors(sector, count, out))
    {
        return false;
    }
    // A cached copy is never older than the card's.
    for (uint32_t i = 0; i < kCacheSectors; ++i)
    {
        const CacheSlot& slot = m_Slots[i];
        if (slot.valid && slot.sector >= sector && slot.sector - sector < count)
        {
            std::memcpy(out + (slot.sector - sector) * kSectorSize, m_Cache.Data() + i * kSectorSize, kSectorSize);
        }
    }
    return true;
}

bool FatVolume::WriteDirect(uint32_t sector, uint32_t count, const uint8_t* data)
{
    // The new data replaces whatever the cache held for these sectors.
    for (CacheSlot& slot : m_Slots)
    {
        if (slot.valid && slot.sector >= sector && slot.sector - sector < count)
        {
            slot.valid = false;
            slot.dirty = false;
        }
    }
    return m_Device->WriteSectors(sector, count, data);
}

// ---- Formatting ----

bool FatVolume::Format(IBlockDevice& device, const FatFormatOptions& options)
{
    if (options.type == FatType::None)
    {
        return false;
    }
    const bool fat32 = options.type == FatType::Fat32;
    const uint32_t deviceSectors = device.SectorCount();
    const uint32_t start = options.partitionTable ? kPartitionStart : 0;
    if (deviceSectors <= start)
    {
        DEKI_LOG_ERROR("FatVolume: the device is too small to format");
        return false;
    }
    const uint32_t sectors = deviceSectors - start;

    const uint32_t sectorsPerCluster =
        options.sectorsPerCluster != 0 ? options.sectorsPerCluster : DefaultClusterSectors(sectors, fat32);
    if (sectorsPerCluster == 0 || sectorsPerCluster > 128 || (sectorsPerCluster & (sectorsPerCluster - 1)) != 0)
    {
        DEKI_LOG_ERROR("FatVolume: no cluster size fits this device");
        return false;
    }

    const uint32_t reserved = fat32 ? 32 : 1;
    const uint32_t rootEntries = fat32 ? 0 : 512;
    const uint32_t rootDirSectors = rootEntries * kEntrySize / kSectorSize;
    const uint32_t fatCount = 2;
    if (sectors <= reserved + rootDirSectors)
    {
        DEKI_LOG_ERROR("FatVolume: the device is too small to format");
        return false;
    }

    // The FAT size, by the formula in Microsoft's FAT specification. It can
    // come out a little large, never too small.
    uint32_t divisor = 256 * sectorsPerCluster + fatCount;
    if (fat32)
    {
        divisor /= 2;
    }
    const uint32_t fatSize = (sectors - reserved - rootDirSectors + divisor - 1) / divisor;
    const uint64_t metaSectors = reserved + static_cast<uint64_t>(fatCount) * fatSize + rootDirSectors;
    if (sectors <= metaSectors)
    {
        DEKI_LOG_ERROR("FatVolume: the device is too small to format");
        return false;
    }
    const uint32_t clusters = static_cast<uint32_t>((sectors - metaSectors) / sectorsPerCluster);
    const bool clustersFit = fat32 ? clusters >= 65525 && clusters < 0x0FFFFFF5 : clusters >= 4085 && clusters < 65525;
    if (!clustersFit)
    {
        DEKI_LOG_ERROR("FatVolume: %u clusters do not make a FAT%d volume; try another cluster size", clusters,
                       fat32 ? 32 : 16);
        return false;
    }

    Deki::Buffer<uint8_t> buffer(kSectorSize, Deki::Memory::Internal);
    if (!buffer)
    {
        DEKI_LOG_ERROR("FatVolume: no memory to format");
        return false;
    }
    uint8_t* sector = buffer.Data();

    // Zeros over everything in front of the data, and the FAT32 root directory.
    std::memset(sector, 0, kSectorSize);
    const uint32_t zeroEnd = start + static_cast<uint32_t>(metaSectors) + (fat32 ? sectorsPerCluster : 0);
    for (uint32_t s = start; s < zeroEnd; ++s)
    {
        if (!device.WriteSectors(s, 1, sector))
        {
            return false;
        }
    }

    // The boot sector and its BPB.
    sector[0] = 0xEB;
    sector[1] = fat32 ? 0x58 : 0x3C;
    sector[2] = 0x90;
    std::memcpy(sector + 3, "DEKI    ", 8);
    Write16(sector + 11, kSectorSize);
    sector[13] = static_cast<uint8_t>(sectorsPerCluster);
    Write16(sector + 14, reserved);
    sector[16] = static_cast<uint8_t>(fatCount);
    Write16(sector + 17, rootEntries);
    const bool smallCount = !fat32 && sectors < 65536;
    Write16(sector + 19, smallCount ? sectors : 0);
    sector[21] = 0xF8;  // fixed media
    Write16(sector + 22, fat32 ? 0 : fatSize);
    Write16(sector + 24, 63);   // sectors per track, for old BIOS code only
    Write16(sector + 26, 255);  // heads, the same
    Write32(sector + 28, start);
    Write32(sector + 32, smallCount ? 0 : sectors);
    uint8_t* tail = sector + 36;
    if (fat32)
    {
        Write32(sector + 36, fatSize);
        Write32(sector + 44, 2);  // the root directory's cluster
        Write16(sector + 48, 1);  // FSInfo
        Write16(sector + 50, 6);  // the boot sector's backup
        tail = sector + 64;
    }
    tail[0] = 0x80;  // drive number
    tail[2] = 0x29;  // the next three fields are there
    Write32(tail + 3, options.volumeId);
    std::memcpy(tail + 7, "NO NAME    ", 11);
    std::memcpy(tail + 18, fat32 ? "FAT32   " : "FAT16   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;
    if (!device.WriteSectors(start, 1, sector) || (fat32 && !device.WriteSectors(start + 6, 1, sector)))
    {
        return false;
    }

    if (fat32)
    {
        std::memset(sector, 0, kSectorSize);
        Write32(sector, kFsInfoLeadSignature);
        Write32(sector + 484, kFsInfoStructSignature);
        Write32(sector + 488, clusters - 1);  // all but the root directory's
        Write32(sector + 492, 3);
        Write32(sector + 508, kFsInfoTrailSignature);
        if (!device.WriteSectors(start + 1, 1, sector) || !device.WriteSectors(start + 7, 1, sector))
        {
            return false;
        }
    }

    // FAT[0] holds the media byte, FAT[1] the end-of-chain mark, and on FAT32
    // FAT[2] ends the root directory's one-cluster chain.
    std::memset(sector, 0, kSectorSize);
    if (fat32)
    {
        Write32(sector, 0x0FFFFFF8);
        Write32(sector + 4, 0x0FFFFFFF);
        Write32(sector + 8, 0x0FFFFFFF);
    }
    else
    {
        Write16(sector, 0xFFF8);
        Write16(sector + 2, 0xFFFF);
    }
    for (uint32_t copy = 0; copy < fatCount; ++copy)
    {
        if (!device.WriteSectors(start + reserved + copy * fatSize, 1, sector))
        {
            return false;
        }
    }

    if (options.partitionTable)
    {
        // One primary partition; the CHS fields say "use the LBA ones".
        std::memset(sector, 0, kSectorSize);
        uint8_t* partition = sector + 446;
        partition[1] = 0xFE;
        partition[2] = 0xFF;
        partition[3] = 0xFF;
        partition[4] = fat32 ? 0x0C : 0x0E;
        partition[5] = 0xFE;
        partition[6] = 0xFF;
        partition[7] = 0xFF;
        Write32(partition + 8, start);
        Write32(partition + 12, sectors);
        sector[510] = 0x55;
        sector[511] = 0xAA;
        if (!device.WriteSectors(0, 1, sector))
        {
            return false;
        }
    }
    return device.Sync();
}

}  // namespace DekiRaspberryPi
