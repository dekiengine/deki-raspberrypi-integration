#pragma once

// UF2, the file a Pico in BOOTSEL mode takes on its USB drive and writes into
// its flash (github.com/microsoft/uf2).
//
// A UF2 file is 512-byte blocks, each carrying up to 476 bytes for one
// address. The Pico's boot ROM writes flash a 256-byte page at a time, so
// every block here carries exactly 256 bytes at a 256-aligned address, as
// Raspberry Pi's own tools write them. A block is little-endian:
//
//   0   magic 0x0A324655 ("UF2\n")      4   magic 0x9E5D5157
//   8   flags (0x2000: family id set)   12  target address
//   16  payload size (256)              20  block number, from 0
//   24  number of blocks in the file    28  family id
//   32  payload, then zeros up to 508   508 magic 0x0AB16F30
//
// The family id says which chip a block is for: the boot ROM skips blocks of
// another family, so an RP2040 file does nothing on an RP2350.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace DekiEditor
{

inline constexpr uint32_t kUf2MagicStart0 = 0x0A324655;
inline constexpr uint32_t kUf2MagicStart1 = 0x9E5D5157;
inline constexpr uint32_t kUf2MagicEnd = 0x0AB16F30;
inline constexpr uint32_t kUf2FlagFamilyIdPresent = 0x00002000;
inline constexpr size_t kUf2BlockSize = 512;
inline constexpr size_t kUf2PayloadSize = 256;
inline constexpr size_t kUf2DataAreaSize = 476;

/// The family ids of the chips this package builds for.
inline constexpr uint32_t kUf2FamilyRp2040 = 0xe48bff56;
inline constexpr uint32_t kUf2FamilyRp2350ArmSecure = 0xe48bff59;

/// Where the flash starts in the Pico's address space (XIP).
inline constexpr uint32_t kPicoFlashBase = 0x10000000;

/// Bytes to write at an address.
struct Uf2Segment
{
    uint32_t address = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

/// One block of a UF2 file, as it was read.
struct Uf2Block
{
    uint32_t flags = 0;
    uint32_t targetAddress = 0;
    uint32_t payloadSize = 0;
    uint32_t blockNumber = 0;
    uint32_t numBlocks = 0;
    uint32_t familyId = 0;
    std::vector<uint8_t> payload;
};

/// Writes `segments` as one UF2 file for `familyId` into `out`: 256 bytes a
/// block, the last block of a segment padded with zeros, every block
/// numbered across the whole file, in address order. False, with the reason
/// in `error`, when a segment is empty or not 256-aligned, two overlap, or
/// one runs past the 32-bit address space.
bool WriteUf2(std::vector<Uf2Segment> segments, uint32_t familyId, std::vector<uint8_t>& out, std::string& error);

/// Reads every block of a UF2 file. False, with the reason in `error`, when
/// the size is not whole blocks or a block's magic numbers or payload size
/// are wrong.
bool ReadUf2(const std::vector<uint8_t>& file, std::vector<Uf2Block>& blocks, std::string& error);

/// The family id of a chip name ("rp2040", "rp2350"), or 0 for another name.
uint32_t Uf2FamilyOfChip(const std::string& chip);

/// The "absolute" family, and the flag and tag of the block below.
inline constexpr uint32_t kUf2FamilyAbsolute = 0xe48bff57;
inline constexpr uint32_t kUf2FlagExtensionTags = 0x00008000;
inline constexpr uint32_t kUf2TagRp2IgnoreBlock = 0x9957e3;

/// The block Raspberry Pi's picotool puts first in every RP2350 file it
/// writes for the Pico SDK (PICO_RP2350_A2_SUPPORTED, for the A2 chip's
/// erratum E10): one block of the absolute family at 0x10FFFF00, filled with
/// 0xEF and tagged for the boot ROM to ignore, numbered 0 of 2. Put before an
/// RP2350 program's blocks, the file is byte for byte what picotool writes.
std::vector<uint8_t> Uf2Rp2350ErrataBlock();

}  // namespace DekiEditor
