#pragma once

// A FAT32 or FAT16 volume on an IBlockDevice: files, directories and long
// file names, written for small RAM. The whole volume uses one allocation of
// two 512-byte sectors as a cache; open files are plain structs owned by the
// caller and hold no buffer of their own.
//
// Names are UTF-8. Lookups ignore ASCII letter case and match either the long
// name or the 8.3 name. ReadDir gives long names with every character outside
// ASCII shown as '?'.

#include "BlockDevice.h"

#include <deki/providers/Buffer.h>

#include <cstddef>
#include <cstdint>

namespace DekiRaspberryPi
{

enum class FatType : uint8_t
{
    None,
    Fat16,
    Fat32,
};

enum class FatOpenMode : uint8_t
{
    Read,    // an existing file, from the start
    Write,   // creates the file, or empties one that exists
    Append,  // creates the file, or keeps one that exists; writes go at the end
};

/// What Format writes.
struct FatFormatOptions
{
    FatType type = FatType::Fat32;
    /// Sectors per cluster, a power of two up to 128. 0 picks one from the size.
    uint8_t sectorsPerCluster = 0;
    /// Puts an MBR with one partition at sector 2048 in front of the volume.
    /// Without it the volume starts at sector 0, with no partition table.
    bool partitionTable = false;
    uint32_t volumeId = 0x44454B49;
};

// FatFile::flags bits.
constexpr uint8_t kFatFileOpen = 0x01;
constexpr uint8_t kFatFileRead = 0x02;
constexpr uint8_t kFatFileWrite = 0x04;
constexpr uint8_t kFatFileDirty = 0x08;  // the directory entry needs writing

/// An open file. The caller owns it; FatVolume fills it in.
struct FatFile
{
    uint32_t firstCluster = 0;
    uint32_t size = 0;
    uint32_t position = 0;
    uint32_t cluster = 0;  // the cluster at clusterIndex in the chain, 0 when not known
    uint32_t clusterIndex = 0;
    uint32_t entrySector = 0;  // where the directory entry is
    uint16_t entryOffset = 0;
    uint8_t flags = 0;
};

/// What Stat finds.
struct FatStat
{
    uint32_t size = 0;
    bool isDirectory = false;
};

/// One entry that ReadDir gives.
struct FatEntryInfo
{
    char name[256] = {};
    uint32_t size = 0;
    bool isDirectory = false;
};

/// A place in a directory: for listing it with ReadDir.
struct FatDir
{
    uint32_t startCluster = 0;  // 0 is the fixed FAT16 root directory
    uint32_t cluster = 0;       // cluster number clusterIndex of the directory, 0 when not known
    uint32_t clusterIndex = 0;
    uint32_t index = 0;  // the next entry ReadDir looks at
};

class FatVolume
{
public:
    /// Sectors the cache holds.
    static constexpr uint32_t kCacheSectors = 2;

    FatVolume() = default;
    ~FatVolume();

    FatVolume(const FatVolume&) = delete;
    FatVolume& operator=(const FatVolume&) = delete;

    /// Reads the volume on `device`: a bare FAT volume at sector 0, or the
    /// first FAT partition of an MBR. Returns false when there is none.
    bool Mount(IBlockDevice& device);

    /// Writes everything out and lets go of the device.
    void Unmount();

    bool IsMounted() const { return m_Device != nullptr; }
    FatType Type() const { return m_Type; }
    uint32_t ClusterBytes() const { return kSectorSize << m_ClusterShift; }
    uint64_t TotalBytes() const;

    /// Free space. The first call after mounting can read the whole FAT when
    /// the volume did not record its free count.
    uint64_t FreeBytes();

    /// Writes cached sectors and the free count, then syncs the device.
    /// Returns false when a write failed.
    bool Sync();

    /// Opens `path` ("dir/file.bin") into `file`. Returns false when the file
    /// is missing (Read), a directory on the way is missing, or the path names
    /// a directory.
    bool Open(FatFile& file, const char* path, FatOpenMode mode);

    /// Writes the file's directory entry and every cached sector, then closes
    /// it. Returns false when a write failed.
    bool Close(FatFile& file);

    /// Writes the file's directory entry and every cached sector, and keeps
    /// it open. Returns false when a write failed.
    bool Flush(FatFile& file);

    /// Reads up to `size` bytes at the file's position. Returns how many.
    size_t Read(FatFile& file, void* buffer, size_t size);

    /// Writes `size` bytes at the file's position. Returns how many; fewer
    /// means the volume is full or a write failed. A position past the end
    /// fills the gap with zeros first.
    size_t Write(FatFile& file, const void* data, size_t size);

    /// Moves the file's position. It may go past the end.
    bool Seek(FatFile& file, uint32_t position);

    /// Fills `out` for the file or directory at `path`. "" is the root.
    /// Returns false when nothing is there.
    bool Stat(const char* path, FatStat& out);

    bool Exists(const char* path);

    /// Makes the directory at `path` and every missing one above it. Returns
    /// true when it exists afterwards.
    bool CreateDirectory(const char* path);

    /// Deletes the file or empty directory at `path`. It must not be open.
    bool Remove(const char* path);

    /// Starts listing the directory at `path`. Returns false when it is not one.
    bool OpenDir(FatDir& dir, const char* path);

    /// The next entry in `dir`, without "." and "..". Returns false at the end.
    bool ReadDir(FatDir& dir, FatEntryInfo& out);

    /// Writes an empty FAT volume over the whole device. Returns false when
    /// the device's size does not suit the type and cluster size asked for.
    static bool Format(IBlockDevice& device, const FatFormatOptions& options);

private:
    enum class Access : uint8_t
    {
        Read,       // load the sector
        Modify,     // load it and mark it changed
        Overwrite,  // zero it without reading and mark it changed
    };

    struct CacheSlot
    {
        uint32_t sector = 0;
        uint32_t lastUse = 0;
        bool valid = false;
        bool dirty = false;
    };

    // A directory entry that was found: where its short entry is, and the
    // run of entries (long name ones first) that belong to it.
    struct EntryRef
    {
        uint32_t dirStart = 0;
        uint32_t sector = 0;
        uint16_t offset = 0;
        uint32_t firstIndex = 0;
        uint32_t entryCount = 0;
        uint32_t firstCluster = 0;
        uint32_t size = 0;
        uint8_t attributes = 0;
    };

    IBlockDevice* m_Device = nullptr;
    Deki::Buffer<uint8_t> m_Cache;
    CacheSlot m_Slots[kCacheSectors];
    uint32_t m_UseCounter = 0;

    FatType m_Type = FatType::None;
    uint32_t m_FatStart = 0;  // absolute sector of the first FAT in use
    uint32_t m_FatSize = 0;   // sectors per FAT
    uint32_t m_FatCopies = 0;
    uint32_t m_DataStart = 0;
    uint32_t m_RootDirSector = 0;  // FAT16 only
    uint32_t m_RootEntryCount = 0;
    uint32_t m_RootCluster = 0;  // FAT32 only
    uint32_t m_ClusterCount = 0;
    uint32_t m_FsInfoSector = 0;  // 0 when there is none
    uint32_t m_FreeCount = 0;
    uint32_t m_NextFree = 2;
    uint8_t m_ClusterShift = 0;  // log2 of sectors per cluster
    bool m_FsInfoInvalidated = false;

    // Cache
    uint8_t* GetSector(uint32_t sector, Access access);
    uint8_t* ScratchSector();
    int PickSlot();
    bool WriteSlot(CacheSlot& slot, const uint8_t* data);
    bool FlushCache();
    bool FlushSector(uint32_t sector);
    bool ReadDirect(uint32_t sector, uint32_t count, uint8_t* out);
    bool WriteDirect(uint32_t sector, uint32_t count, const uint8_t* data);

    // FAT
    uint32_t ReadFat(uint32_t cluster);
    bool WriteFat(uint32_t cluster, uint32_t value);
    bool AllocateCluster(uint32_t previous, uint32_t& out);
    bool FreeChain(uint32_t first);
    bool ZeroCluster(uint32_t cluster);
    bool IsValidCluster(uint32_t cluster) const;
    uint32_t ClusterSector(uint32_t cluster) const;
    void MarkFatChanged();
    bool WriteFsInfo(uint32_t freeCount, uint32_t nextFree);
    uint32_t RootStart() const;

    // Directories
    uint32_t DirSector(FatDir& dir, uint32_t index, bool extend);
    uint8_t* DirEntry(FatDir& dir, uint32_t index, Access access, uint32_t* sectorOut);
    bool FindEntry(uint32_t dirStart, const char* name, size_t length, EntryRef& out);
    bool FindParent(const char* path, uint32_t& dirStart, const char*& leaf, size_t& leafLength);
    bool AddEntry(uint32_t dirStart, const char* name, size_t length, uint8_t attributes, uint32_t firstCluster,
                  EntryRef& out);
    bool ShortNameInUse(uint32_t dirStart, const uint8_t* shortName);
    bool IsDirectoryEmpty(uint32_t dirStart);
    uint32_t EntryDirStart(uint32_t firstCluster) const;

    // Files
    bool ClusterAt(FatFile& file, uint32_t position, bool allocate, uint32_t& out);
    size_t WriteBytes(FatFile& file, const uint8_t* data, size_t size);
    bool WriteEntry(FatFile& file);
};

}  // namespace DekiRaspberryPi
