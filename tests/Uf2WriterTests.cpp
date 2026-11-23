// The UF2 files the Pico builder writes: every block as the Pico's boot ROM
// reads it, and the bytes back out where they were put.
//
// A wrong block here shows up as a Pico that takes the file and runs nothing,
// or half a game, so the layout is pinned byte by byte.

#include <gtest/gtest.h>

#include "editor/Uf2Writer.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace DekiEditor;

namespace
{

uint32_t U32At(const std::vector<uint8_t>& bytes, size_t at)
{
    return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

std::vector<uint8_t> Pattern(size_t size, uint8_t seed)
{
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
    {
        bytes[i] = static_cast<uint8_t>(seed + i * 7);
    }
    return bytes;
}

}  // namespace

TEST(Uf2Writer, EachBlockIsLaidOutAsTheBootRomReadsIt)
{
    const std::vector<uint8_t> program = Pattern(600, 1);
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(WriteUf2({ { kPicoFlashBase, program.data(), program.size() } }, kUf2FamilyRp2040, file, error))
        << error;
    // 600 bytes: three 256-byte pages.
    ASSERT_EQ(file.size(), 3 * kUf2BlockSize);
    for (size_t i = 0; i < 3; ++i)
    {
        const size_t at = i * kUf2BlockSize;
        EXPECT_EQ(U32At(file, at + 0), 0x0A324655u);
        EXPECT_EQ(U32At(file, at + 4), 0x9E5D5157u);
        EXPECT_EQ(U32At(file, at + 8), 0x00002000u) << "family id present, nothing else";
        EXPECT_EQ(U32At(file, at + 12), 0x10000000u + i * 256);
        EXPECT_EQ(U32At(file, at + 16), 256u);
        EXPECT_EQ(U32At(file, at + 20), i);
        EXPECT_EQ(U32At(file, at + 24), 3u);
        EXPECT_EQ(U32At(file, at + 28), 0xe48bff56u);
        EXPECT_EQ(U32At(file, at + 508), 0x0AB16F30u);
        // After the payload, zeros up to the final magic.
        for (size_t b = 32 + 256; b < 508; ++b)
        {
            ASSERT_EQ(file[at + b], 0) << "block " << i << " byte " << b;
        }
    }
    // The last page is padded with zeros.
    for (size_t b = 600 - 512; b < 256; ++b)
    {
        EXPECT_EQ(file[2 * kUf2BlockSize + 32 + b], 0);
    }
}

TEST(Uf2Writer, TwoSegmentsAreNumberedTogetherInAddressOrder)
{
    const std::vector<uint8_t> program = Pattern(1000, 3);
    const std::vector<uint8_t> data = Pattern(300, 9);
    std::vector<uint8_t> file;
    std::string error;
    // Given data first: the file still starts with the program.
    ASSERT_TRUE(WriteUf2(
        { { kPicoFlashBase + 0x100000, data.data(), data.size() }, { kPicoFlashBase, program.data(), program.size() } },
        kUf2FamilyRp2350ArmSecure, file, error))
        << error;

    std::vector<Uf2Block> blocks;
    ASSERT_TRUE(ReadUf2(file, blocks, error)) << error;
    ASSERT_EQ(blocks.size(), 4u + 2u);
    for (size_t i = 0; i < blocks.size(); ++i)
    {
        EXPECT_EQ(blocks[i].blockNumber, i);
        EXPECT_EQ(blocks[i].numBlocks, 6u);
        EXPECT_EQ(blocks[i].familyId, 0xe48bff59u);
        EXPECT_EQ(blocks[i].flags, kUf2FlagFamilyIdPresent);
        if (i > 0)
        {
            EXPECT_GT(blocks[i].targetAddress, blocks[i - 1].targetAddress);
        }
    }
    EXPECT_EQ(blocks[3].targetAddress, 0x10000300u);
    EXPECT_EQ(blocks[4].targetAddress, 0x10100000u);
    EXPECT_EQ(blocks[5].targetAddress, 0x10100100u);

    // The bytes come back where they were put.
    std::vector<uint8_t> programBack, dataBack;
    for (const Uf2Block& b : blocks)
    {
        std::vector<uint8_t>& to = b.targetAddress < 0x10100000u ? programBack : dataBack;
        to.insert(to.end(), b.payload.begin(), b.payload.end());
    }
    programBack.resize(program.size());
    dataBack.resize(data.size());
    EXPECT_EQ(programBack, program);
    EXPECT_EQ(dataBack, data);
}

TEST(Uf2Writer, WhatCannotBeFlashedIsRefused)
{
    const std::vector<uint8_t> bytes = Pattern(512, 0);
    std::vector<uint8_t> file;
    std::string error;
    EXPECT_FALSE(WriteUf2({ { kPicoFlashBase + 1, bytes.data(), bytes.size() } }, kUf2FamilyRp2040, file, error));
    EXPECT_NE(error.find("256-byte page"), std::string::npos) << error;
    EXPECT_FALSE(WriteUf2({ { kPicoFlashBase, bytes.data(), 0 } }, kUf2FamilyRp2040, file, error));
    EXPECT_FALSE(WriteUf2({}, kUf2FamilyRp2040, file, error));
    // 512 bytes from 0x10000000 run to 0x10000200; the next may not start before.
    EXPECT_FALSE(WriteUf2(
        { { kPicoFlashBase, bytes.data(), bytes.size() }, { kPicoFlashBase + 0x100, bytes.data(), bytes.size() } },
        kUf2FamilyRp2040, file, error));
    EXPECT_NE(error.find("overlaps"), std::string::npos) << error;
    // A segment's padded last page counts too.
    EXPECT_FALSE(WriteUf2({ { kPicoFlashBase, bytes.data(), 300 }, { kPicoFlashBase + 0x100, bytes.data(), 10 } },
                          kUf2FamilyRp2040, file, error));
    EXPECT_FALSE(WriteUf2({ { 0xFFFFFF00u, bytes.data(), bytes.size() } }, kUf2FamilyRp2040, file, error));
    // Back to back is fine.
    EXPECT_TRUE(WriteUf2(
        { { kPicoFlashBase, bytes.data(), bytes.size() }, { kPicoFlashBase + 0x200, bytes.data(), bytes.size() } },
        kUf2FamilyRp2040, file, error))
        << error;
}

TEST(Uf2Writer, ReadingRefusesWhatIsNotUf2)
{
    std::vector<Uf2Block> blocks;
    std::string error;
    EXPECT_FALSE(ReadUf2({}, blocks, error));
    EXPECT_FALSE(ReadUf2(std::vector<uint8_t>(513, 0), blocks, error));
    EXPECT_FALSE(ReadUf2(std::vector<uint8_t>(512, 0), blocks, error));
    EXPECT_NE(error.find("magic"), std::string::npos) << error;
}

TEST(Uf2Writer, TheRp2350ErrataBlockIsPicotoolsIgnoredAbsoluteBlock)
{
    const std::vector<uint8_t> block = Uf2Rp2350ErrataBlock();
    ASSERT_EQ(block.size(), kUf2BlockSize);
    std::vector<Uf2Block> blocks;
    std::string error;
    ASSERT_TRUE(ReadUf2(block, blocks, error)) << error;
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].flags, 0xa000u);
    EXPECT_EQ(blocks[0].targetAddress, 0x10FFFF00u);
    EXPECT_EQ(blocks[0].blockNumber, 0u);
    EXPECT_EQ(blocks[0].numBlocks, 2u);
    EXPECT_EQ(blocks[0].familyId, 0xe48bff57u);
    EXPECT_EQ(blocks[0].payload, std::vector<uint8_t>(256, 0xEF));
    // The extension tag after the payload, as picotool 2.3.1 writes it.
    EXPECT_EQ(U32At(block, 288), 0x9957e304u);
    for (size_t b = 292; b < 508; ++b)
    {
        ASSERT_EQ(block[b], 0) << b;
    }
}

TEST(Uf2Writer, EachChipHasItsFamily)
{
    EXPECT_EQ(Uf2FamilyOfChip("rp2040"), 0xe48bff56u);
    EXPECT_EQ(Uf2FamilyOfChip("rp2350"), 0xe48bff59u);
    EXPECT_EQ(Uf2FamilyOfChip("esp32"), 0u);
    EXPECT_EQ(Uf2FamilyOfChip(""), 0u);
}
