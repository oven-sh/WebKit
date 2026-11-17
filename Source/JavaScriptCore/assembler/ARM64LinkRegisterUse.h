/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <cstdint>
#include <optional>

// What an A64 instruction does with x30, by the encoding groups of the Arm Architecture Reference Manual ("A64 instruction set
// encoding"), so that it also holds for instructions that ARM64Assembler does not have yet.
namespace JSC { namespace ARM64LinkRegister {

inline constexpr unsigned linkRegister = 30;
inline constexpr unsigned zeroOrStackPointer = 31;

constexpr unsigned bitsOf(uint32_t instruction, unsigned high, unsigned low) { return instruction >> low & ((2u << (high - low)) - 1); }
constexpr bool bitOf(uint32_t instruction, unsigned bit) { return instruction >> bit & 1; }
constexpr unsigned registerAt(uint32_t instruction, unsigned low) { return bitsOf(instruction, low + 4, low); }

constexpr bool mayName(uint32_t instruction)
{
    return registerAt(instruction, 0) == linkRegister || registerAt(instruction, 5) == linkRegister || registerAt(instruction, 10) == linkRegister || registerAt(instruction, 16) == linkRegister;
}

constexpr bool signsAuthenticatesOrStrips(uint32_t instruction)
{
    constexpr uint32_t hintsOnPointers = 0xff005580; // XPACLRI, {PAC,AUT}I{A,B}{1716,Z,SP}
    if ((instruction & 0xfffff01f) == 0xd503201f)
        return bitsOf(instruction, 11, 5) < 32 && bitOf(hintsOnPointers, bitsOf(instruction, 9, 5));
    return (instruction & 0xffff0000) == 0xdac10000 || (instruction & 0xffc0001f) == 0xf380001f;
}

constexpr bool jumpsToIt(uint32_t instruction)
{
    return (instruction & 0xff9fffff) == 0xd61f03c0; // BR, BLR, RET
}

// Does not fault, leave, or depend on where it lies.
constexpr bool onlyComputesWithoutIt(uint32_t instruction)
{
    if (mayName(instruction) || signsAuthenticatesOrStrips(instruction))
        return false;
    if (bitsOf(instruction, 28, 26) == 0b100)
        return bitsOf(instruction, 25, 24);
    return bitsOf(instruction, 27, 25) == 0b101 || bitsOf(instruction, 27, 25) == 0b111;
}

// Neither is set where it is compared, jumped to, used as an address, or made of itself, constants and the program counter.
struct Use {
    friend constexpr bool operator==(const Use&, const Use&) = default;

    bool isCopiedOut { false };
    bool isOverwritten { false };
};

constexpr Use useBy(uint32_t instruction)
{
    auto isIt = [&](unsigned low) { return registerAt(instruction, low) == linkRegister; };
    auto isZeroOrStackPointer = [&](unsigned low) { return registerAt(instruction, low) == zeroOrStackPointer; };
    auto isNeitherItNorZero = [&](unsigned low) { return !isIt(low) && !isZeroOrStackPointer(low); };
    auto computes = [&](bool dropsResult, bool readsIt, bool readsOthers) {
        return isIt(0) ? Use { false, readsOthers } : Use { readsIt && !dropsResult, false };
    };
    auto movesData = [](bool namesIt, bool isIntoIt) { return Use { namesIt && !isIntoIt, namesIt && isIntoIt }; };
    auto mayDoEither = [](bool namesIt) { return Use { namesIt, namesIt }; };
    const bool setsFlags = bitOf(instruction, 29);

    if (bitsOf(instruction, 28, 26) == 0b100) {
        switch (bitsOf(instruction, 25, 23)) {
        case 0b000:
        case 0b001: // PC-relative address
        case 0b101: // move wide
            return { };
        case 0b010: // add, subtract
            return computes(setsFlags && isZeroOrStackPointer(0), isIt(5), !isIt(5));
        case 0b011: // add, subtract with tags; minimum, maximum
            return computes(false, isIt(5), !isIt(5));
        case 0b100: // logical
            return computes(bitsOf(instruction, 30, 29) == 0b11 && isZeroOrStackPointer(0), isIt(5), isNeitherItNorZero(5));
        case 0b110: // bitfield
            return computes(isZeroOrStackPointer(0), isIt(5), isNeitherItNorZero(5));
        default: // extract
            return computes(isZeroOrStackPointer(0), isIt(5) || isIt(16), isNeitherItNorZero(5) || isNeitherItNorZero(16));
        }
    }

    if (bitsOf(instruction, 28, 26) == 0b101) {
        if (bitsOf(instruction, 31, 24) != 0xd5) // branches, exceptions
            return { };
        if (!bitOf(instruction, 23)) // system
            return movesData(isIt(0), bitOf(instruction, 21));
        return mayDoEither(mayName(instruction));
    }

    if (bitsOf(instruction, 27, 25) == 0b101) {
        auto computesFromTwo = [&] {
            return computes(isZeroOrStackPointer(0), isIt(5) || isIt(16), isNeitherItNorZero(5) || isNeitherItNorZero(16));
        };
        if (!bitOf(instruction, 28)) {
            if (bitOf(instruction, 24) && bitOf(instruction, 21)) // add, subtract, extended register
                return computes(setsFlags && isZeroOrStackPointer(0), isIt(5) || isIt(16), !isIt(5) || isNeitherItNorZero(16));
            return computesFromTwo(); // logical, add, subtract, shifted register
        }
        switch (bitsOf(instruction, 24, 21)) {
        case 0b0000:
            if (!bitsOf(instruction, 15, 10)) // add, subtract with carry
                return computesFromTwo();
            if (bitsOf(instruction, 14, 10) == 0b00001 || bitsOf(instruction, 13, 10) == 0b0010) // into flags
                return { };
            return computes(false, isIt(5) || isIt(16), true);
        case 0b0010: // conditional compare
            return { };
        case 0b0100: // conditional select
            return computesFromTwo();
        case 0b0110:
            if (bitOf(instruction, 30)) // one source
                return computes(isZeroOrStackPointer(0), isIt(5), isNeitherItNorZero(5));
            return computes(setsFlags && isZeroOrStackPointer(0), isIt(5) || isIt(16), true); // two sources
        default: // three sources
            return computes(isZeroOrStackPointer(0), isIt(5) || isIt(16) || isIt(10), true);
        }
    }

    if (bitsOf(instruction, 27, 25) == 0b111) {
        if ((instruction & 0x5f000000) == 0x1e000000 && (!bitOf(instruction, 21) || !bitsOf(instruction, 15, 10))) { // to or from floating point
            unsigned opcode = bitsOf(instruction, 18, 16);
            bool isFromGPR = opcode == 0b010 || opcode == 0b011 || opcode == 0b111;
            return isFromGPR ? movesData(isIt(5), false) : movesData(isIt(0), true);
        }
        if ((instruction & 0xbfe08400) == 0x0e000400) { // to or from a lane
            switch (bitsOf(instruction, 14, 11)) {
            case 0b0001:
            case 0b0011:
                return movesData(isIt(5), false);
            case 0b0101:
            case 0b0111:
                return movesData(isIt(0), true);
            }
        }
        return { };
    }

    if (bitOf(instruction, 27)) {
        const bool isVector = bitOf(instruction, 26);
        auto movesRegister = [&] {
            unsigned opcode = bitsOf(instruction, 23, 22);
            bool prefetches = bitsOf(instruction, 31, 30) == 0b11 && opcode == 0b10;
            return movesData(!isVector && !prefetches && isIt(0), opcode);
        };
        switch (bitsOf(instruction, 29, 28)) {
        case 0b00:
            if (!isVector) // exclusive, ordered, compare and swap
                return mayDoEither(isIt(0) || isIt(10) || isIt(16));
            if (!bitOf(instruction, 23) || isZeroOrStackPointer(16)) // structures
                return { };
            return isIt(5) ? Use { false, !isIt(16) } : Use { isIt(16), false };
        case 0b01:
            if (!bitOf(instruction, 24)) // literal
                return movesData(!isVector && bitsOf(instruction, 31, 30) != 0b11 && isIt(0), true);
            if (!isVector && !bitOf(instruction, 21) && !bitsOf(instruction, 11, 10)) // ordered, unscaled
                return movesData(isIt(0), bitsOf(instruction, 23, 22));
            if (bitsOf(instruction, 31, 24) == 0xd9 && bitOf(instruction, 21)) // tags
                return mayDoEither(isIt(0));
            return mayDoEither(isIt(0) || isIt(5) || isIt(16));
        case 0b10: // pair
            return movesData(!isVector && (isIt(0) || isIt(10)), bitOf(instruction, 22));
        default:
            if (bitOf(instruction, 24) || !bitOf(instruction, 21) || bitsOf(instruction, 11, 10) == 0b10)
                return movesRegister();
            if (bitOf(instruction, 10)) // through an authenticated pointer
                return movesData(isIt(0), true);
            return mayDoEither(!isVector && (isIt(0) || isIt(16))); // atomic
        }
    }

    if (!bitsOf(instruction, 31, 16)) // UDF
        return { };
    return mayDoEither(mayName(instruction)); // SVE, SME, and what has no meaning yet
}

// LDR, LDUR, LDP, STR, STUR and STP of 64 bits.
struct AccessOfWords {
    // From the base as the instruction finds it, and as it leaves it.
    constexpr int offsetBefore() const { return isPostIndexed ? 0 : immediate; }
    constexpr int offsetAfter() const { return offsetBefore() - (writesBaseBack ? immediate : 0); }

    bool isLoad;
    bool isPair;
    bool hasIndex;
    bool writesBaseBack;
    bool isPostIndexed;
    int immediate;
};

constexpr std::optional<AccessOfWords> accessOfWordsBy(uint32_t instruction)
{
    const bool isLoad = bitOf(instruction, 22);
    auto signedBits = [&](unsigned high, unsigned low) { return static_cast<int32_t>(instruction << (31 - high)) >> (31 - high + low); };
    if ((instruction & 0xfe000000) == 0xa8000000)
        return AccessOfWords { isLoad, true, false, bitOf(instruction, 23), bitsOf(instruction, 24, 23) == 0b01, signedBits(21, 15) * 8 };
    if ((instruction & 0xff800000) == 0xf9000000)
        return AccessOfWords { isLoad, false, false, false, false, static_cast<int>(bitsOf(instruction, 21, 10)) * 8 };
    if ((instruction & 0xffa00000) == 0xf8000000)
        return AccessOfWords { isLoad, false, false, bitOf(instruction, 10), bitsOf(instruction, 11, 10) == 0b01, signedBits(20, 12) };
    if ((instruction & 0xffa00c00) == 0xf8200800)
        return AccessOfWords { isLoad, false, true, false, false, 0 };
    return std::nullopt;
}

} } // namespace JSC::ARM64LinkRegister
