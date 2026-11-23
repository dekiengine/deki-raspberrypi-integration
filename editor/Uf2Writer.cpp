#include "Uf2Writer.h"

#include <algorithm>
#include <cstring>

namespace DekiEditor
{

namespace
{

void PutU32(uint8_t* at, uint32_t value)
{
    at[0] = static_cast<uint8_t>(value);
    at[1] = static_cast<uint8_t>(value >> 8);
    at[2] = static_cast<uint8_t>(value >> 16);
    at[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t GetU32(const uint8_t* at)
{
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) | (static_cast<uint32_t>(at[2]) << 16) |
           (static_cast<uint32_t>(at[3]) << 24);
}

std::string Hex(uint64_t value)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    do
    {
        out.insert(out.begin(), kDigits[value & 0xF]);
        value >>= 4;
    } while (value != 0);
    return "0x" + out;
}

size_t BlocksFor(size_t bytes)
{
    return (bytes + kUf2PayloadSize - 1) / kUf2PayloadSize;
}

}  // namespace

bool WriteUf2(std::vector<Uf2Segment> segments, uint32_t familyId, std::vector<uint8_t>& out, std::string& error)
{
    std::sort(segments.begin(), segments.end(),
              [](const Uf2Segment& a, const Uf2Segment& b) { return a.address < b.address; });

    size_t numBlocks = 0;
    uint64_t previousEnd = 0;
    for (const Uf2Segment& s : segments)
    {
        if (s.size == 0 || s.data == nullptr)
        {
            error = "the segment at " + Hex(s.address) + " is empty";
            return false;
        }
        if (s.address % kUf2PayloadSize != 0)
        {
            error = "the segment at " + Hex(s.address) + " does not start on a 256-byte page";
            return false;
        }
        // Whole pages: the padding of a segment's last block is written too.
        const uint64_t end = static_cast<uint64_t>(s.address) + BlocksFor(s.size) * kUf2PayloadSize;
        if (end > 0x100000000ull)
        {
            error = "the segment at " + Hex(s.address) + " runs past the end of the address space";
            return false;
        }
        if (s.address < previousEnd)
        {
            error =
                "the segment at " + Hex(s.address) + " overlaps the one before it, which ends at " + Hex(previousEnd);
            return false;
        }
        previousEnd = end;
        numBlocks += BlocksFor(s.size);
    }
    if (numBlocks == 0)
    {
        error = "nothing to write";
        return false;
    }

    out.assign(numBlocks * kUf2BlockSize, 0);
    uint32_t blockNumber = 0;
    for (const Uf2Segment& s : segments)
    {
        for (size_t offset = 0; offset < s.size; offset += kUf2PayloadSize)
        {
            uint8_t* block = out.data() + static_cast<size_t>(blockNumber) * kUf2BlockSize;
            PutU32(block + 0, kUf2MagicStart0);
            PutU32(block + 4, kUf2MagicStart1);
            PutU32(block + 8, kUf2FlagFamilyIdPresent);
            PutU32(block + 12, s.address + static_cast<uint32_t>(offset));
            PutU32(block + 16, static_cast<uint32_t>(kUf2PayloadSize));
            PutU32(block + 20, blockNumber);
            PutU32(block + 24, static_cast<uint32_t>(numBlocks));
            PutU32(block + 28, familyId);
            std::memcpy(block + 32, s.data + offset, std::min(kUf2PayloadSize, s.size - offset));
            PutU32(block + 508, kUf2MagicEnd);
            ++blockNumber;
        }
    }
    return true;
}

bool ReadUf2(const std::vector<uint8_t>& file, std::vector<Uf2Block>& blocks, std::string& error)
{
    blocks.clear();
    if (file.empty() || file.size() % kUf2BlockSize != 0)
    {
        error = "a UF2 file is whole 512-byte blocks, and this one is " + std::to_string(file.size()) + " bytes";
        return false;
    }
    for (size_t at = 0; at < file.size(); at += kUf2BlockSize)
    {
        const uint8_t* b = file.data() + at;
        const size_t index = at / kUf2BlockSize;
        if (GetU32(b) != kUf2MagicStart0 || GetU32(b + 4) != kUf2MagicStart1 || GetU32(b + 508) != kUf2MagicEnd)
        {
            error = "block " + std::to_string(index) + " has the wrong magic numbers";
            return false;
        }
        Uf2Block block;
        block.flags = GetU32(b + 8);
        block.targetAddress = GetU32(b + 12);
        block.payloadSize = GetU32(b + 16);
        block.blockNumber = GetU32(b + 20);
        block.numBlocks = GetU32(b + 24);
        block.familyId = GetU32(b + 28);
        if (block.payloadSize > kUf2DataAreaSize)
        {
            error = "block " + std::to_string(index) + " says it carries " + std::to_string(block.payloadSize) +
                    " bytes, more than a block holds";
            return false;
        }
        block.payload.assign(b + 32, b + 32 + block.payloadSize);
        blocks.push_back(std::move(block));
    }
    return true;
}

uint32_t Uf2FamilyOfChip(const std::string& chip)
{
    if (chip == "rp2040")
    {
        return kUf2FamilyRp2040;
    }
    if (chip == "rp2350")
    {
        return kUf2FamilyRp2350ArmSecure;
    }
    return 0;
}

std::vector<uint8_t> Uf2Rp2350ErrataBlock()
{
    std::vector<uint8_t> block(kUf2BlockSize, 0);
    PutU32(block.data() + 0, kUf2MagicStart0);
    PutU32(block.data() + 4, kUf2MagicStart1);
    PutU32(block.data() + 8, kUf2FlagFamilyIdPresent | kUf2FlagExtensionTags);
    PutU32(block.data() + 12, 0x10FFFF00);
    PutU32(block.data() + 16, static_cast<uint32_t>(kUf2PayloadSize));
    PutU32(block.data() + 20, 0);
    PutU32(block.data() + 24, 2);
    PutU32(block.data() + 28, kUf2FamilyAbsolute);
    std::memset(block.data() + 32, 0xEF, kUf2PayloadSize);
    // An extension tag after the payload: its size in bytes (4) and its id.
    PutU32(block.data() + 32 + kUf2PayloadSize, 4u | (kUf2TagRp2IgnoreBlock << 8));
    PutU32(block.data() + 508, kUf2MagicEnd);
    return block;
}

}  // namespace DekiEditor
