/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Yarr { namespace ImageBody {

struct Reference {
    uint32_t offset;
    uint64_t identity;
    uint8_t size { 0 };
    bool usesRegistersOnly { false };
};

struct Body {
    const uint8_t* bytes { nullptr };
    size_t size { 0 };
    Vector<Reference> references;
};

struct PlacedReference {
    uint32_t offset;
    bool isOfWideBody;
    uint32_t index;
    uint64_t identity;
};

struct Result {
    Vector<uint8_t> bytes;
    Vector<PlacedReference> references;
    uint32_t entryForWideSubjects { 0 };
};

enum class Kind : uint8_t { Plain, ConditionalJump, Jump, Return, IndirectJump, AddressOfCode, Call, Reference, Fill, JumpIfWide };

struct Unit {
    uint32_t offset { 0 };
    uint8_t size { 0 };
    Kind kind { Kind::Plain };
    uint8_t condition { 0 };
    bool hasScratchAsOperand { false };
    bool hasScratchInAddress { false };
    bool usesRegistersOnly { false };
    int32_t target { -1 };
    int32_t reference { -1 };
    uint32_t word { 0 };
    uint64_t key { 0 };
};

struct Decoded {
    uint8_t size { 0 };
    Kind kind { Kind::Plain };
    uint8_t condition { 0 };
    bool usesCountRegister { false };
    bool hasScratchAsOperand { false };
    bool hasScratchInAddress { false };
    bool isRelativeToNextInstruction { false };
    int32_t displacement { 0 };
    uint32_t word { 0 };
};

#if CPU(ARM64)
inline bool decode(const uint8_t* p, size_t left, Decoded& result)
{
    if (left < 4)
        return false;
    uint32_t word;
    memcpy(&word, p, 4);
    result = { };
    result.size = 4;
    auto signedField = [&](unsigned lowest, unsigned bits) {
        return static_cast<int32_t>(word << (32 - lowest - bits)) >> (32 - bits);
    };
    if ((word & 0x7c000000) == 0x14000000) {
        result.kind = word >> 31 ? Kind::Reference : Kind::Jump;
        result.displacement = signedField(0, 26) * 4 - 4;
        result.word = word & 0xfc000000;
    } else if ((word & 0xff000010) == 0x54000000 || (word & 0x7e000000) == 0x34000000) {
        result.kind = Kind::ConditionalJump;
        result.displacement = signedField(5, 19) * 4 - 4;
        result.word = word & 0xff00001f;
    } else if ((word & 0x7e000000) == 0x36000000) {
        result.kind = Kind::ConditionalJump;
        result.displacement = signedField(5, 14) * 4 - 4;
        result.word = word & 0xfff8001f;
    } else if ((word & 0x9f000000) == 0x10000000) {
        result.isRelativeToNextInstruction = true;
        result.displacement = (signedField(5, 19) * 4 | static_cast<int32_t>((word >> 29) & 3)) - 4;
        result.word = word & 0x9f00001f;
    } else if ((word & 0x9f000000) == 0x90000000 || (word & 0x3b000000) == 0x18000000 || (word & 0xfffffc1f) == 0xd63f0000)
        return false;
    else if (word == 0xd65f03c0)
        result.kind = Kind::Return;
    else if ((word & 0xfffffc1f) == 0xd61f0000)
        result.kind = Kind::IndirectJump;
    else if (word == 0xd503201f)
        result.kind = Kind::Fill;
    return true;
}

inline void invertCondition(Unit& unit)
{
    unit.word ^= (unit.word & 0xff000010) == 0x54000000 ? 1 : 1 << 24;
    unit.key = 2 + static_cast<uint64_t>(unit.word) * 0x9e3779b97f4a7c15ull;
}
#else
inline void invertCondition(Unit& unit)
{
    unit.condition ^= 1;
    unit.key = 2 + unit.condition;
}

inline bool decode(const uint8_t* p, size_t left, Decoded& result)
{
    size_t i = 0;
    bool hasOperandSizePrefix = false;
    while (i < left && (p[i] == 0x66 || p[i] == 0xf2 || p[i] == 0xf3))
        hasOperandSizePrefix |= p[i++] == 0x66;
    uint8_t rex = 0;
    if (i < left && (p[i] & 0xf0) == 0x40)
        rex = p[i++];
    if (i >= left)
        return false;
    uint8_t opcode = p[i++];
    bool hasModRM = false;
    bool immediateDependsOnModRM = false;
    unsigned immediate = 0;
    unsigned wordImmediate = hasOperandSizePrefix ? 2 : 4;
    result = { };
    auto registerInOpcodeIsCount = [&] {
        if ((opcode & 7) == 3 && (rex & 1))
            result.hasScratchAsOperand = true;
        return (opcode & 7) == 1 && !(rex & 1);
    };
    if (opcode == 0x0f) {
        if (i >= left)
            return false;
        uint8_t second = p[i++];
        if ((second & 0xf0) == 0x80) {
            if (i + 4 > left)
                return false;
            result.kind = Kind::ConditionalJump;
            result.condition = second & 0x0f;
            memcpy(&result.displacement, p + i, 4);
            result.size = i + 4;
            return true;
        }
        if (second == 0x1f) {
            hasModRM = true;
            result.kind = Kind::Fill;
        } else if (second == 0x6f || second == 0x67 || second == 0x7e)
            hasModRM = true;
        else if ((second & 0xf0) == 0x40 || (second & 0xf0) == 0x90 || second == 0xa3 || second == 0xab || second == 0xb3 || second == 0xbb || second == 0xaf || second == 0xb6 || second == 0xb7 || second == 0xbe || second == 0xbf || second == 0xbc || second == 0xbd || second == 0xb8)
            hasModRM = true;
        else if (second == 0xba) {
            hasModRM = true;
            immediate = 1;
        } else
            return false;
    } else if (opcode < 0x40) {
        switch (opcode & 7) {
        case 0:
        case 1:
        case 2:
        case 3:
            hasModRM = true;
            break;
        case 4:
            immediate = 1;
            break;
        case 5:
            immediate = wordImmediate;
            break;
        default:
            return false;
        }
    } else if (opcode >= 0x50 && opcode <= 0x5f)
        result.usesCountRegister = registerInOpcodeIsCount();
    else if ((opcode & 0xf0) == 0x70) {
        if (i >= left)
            return false;
        result.kind = Kind::ConditionalJump;
        result.condition = opcode & 0x0f;
        result.displacement = static_cast<int8_t>(p[i]);
        result.size = i + 1;
        return true;
    } else {
        switch (opcode) {
        case 0x63:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8a:
        case 0x8b:
        case 0x8d:
        case 0xd0:
        case 0xd1:
        case 0xfe:
            hasModRM = true;
            break;
        case 0xff:
            hasModRM = true;
            break;
        case 0x69:
        case 0x81:
        case 0xc7:
            hasModRM = true;
            immediate = wordImmediate;
            break;
        case 0x6b:
        case 0x80:
        case 0x83:
        case 0xc0:
        case 0xc1:
        case 0xc6:
            hasModRM = true;
            immediate = 1;
            break;
        case 0xf6:
        case 0xf7:
            hasModRM = true;
            immediateDependsOnModRM = true;
            break;
        case 0x90:
            result.kind = Kind::Fill;
            break;
        case 0x98:
        case 0x99:
        case 0xc9:
        case 0xcc:
        case 0xf4:
            break;
        case 0xa8:
            immediate = 1;
            break;
        case 0xa9:
            immediate = wordImmediate;
            break;
        case 0xc3:
            result.kind = Kind::Return;
            break;
        case 0xe8:
        case 0xe9:
            if (i + 4 > left)
                return false;
            result.kind = opcode == 0xe9 ? Kind::Jump : Kind::Reference;
            memcpy(&result.displacement, p + i, 4);
            result.size = i + 4;
            return true;
        case 0xeb:
            if (i >= left)
                return false;
            result.kind = Kind::Jump;
            result.displacement = static_cast<int8_t>(p[i]);
            result.size = i + 1;
            return true;
        default:
            if (opcode >= 0xb0 && opcode <= 0xb7) {
                immediate = 1;
                result.usesCountRegister = registerInOpcodeIsCount();
            } else if (opcode >= 0xb8 && opcode <= 0xbf) {
                immediate = rex & 8 ? 8 : wordImmediate;
                result.usesCountRegister = registerInOpcodeIsCount();
            } else
                return false;
        }
    }
    if (hasModRM) {
        if (i >= left)
            return false;
        uint8_t modRM = p[i++];
        unsigned mode = modRM >> 6;
        unsigned middle = (modRM >> 3) & 7;
        unsigned last = modRM & 7;
        bool middleIsRegister = !(opcode == 0x80 || opcode == 0x81 || opcode == 0x83 || opcode == 0xc0 || opcode == 0xc1 || opcode == 0xc6 || opcode == 0xc7 || opcode == 0xd0 || opcode == 0xd1 || opcode == 0xf6 || opcode == 0xf7 || opcode == 0xfe || opcode == 0xff || (opcode == 0x0f && (p[i - 2] == 0xba || p[i - 2] == 0x1f || (p[i - 2] & 0xf0) == 0x90)));
        if (middleIsRegister && middle == 1 && !(rex & 4))
            result.usesCountRegister = true;
        if (middleIsRegister && middle == 3 && (rex & 4))
            result.hasScratchAsOperand = true;
        if (immediateDependsOnModRM)
            immediate = middle < 2 ? (opcode == 0xf6 ? 1 : wordImmediate) : 0;
        if (opcode == 0xff) {
            if (middle == 4)
                result.kind = Kind::IndirectJump;
            else if (middle > 1 && middle != 6)
                return false;
        }
        if (mode == 3) {
            if (last == 1 && !(rex & 1))
                result.usesCountRegister = true;
            if (last == 3 && (rex & 1))
                result.hasScratchAsOperand = true;
        } else {
            unsigned displacementSize = mode == 1 ? 1 : mode == 2 ? 4 : 0;
            if (last == 4) {
                if (i >= left)
                    return false;
                uint8_t scaleIndexBase = p[i++];
                unsigned base = scaleIndexBase & 7;
                unsigned index = (scaleIndexBase >> 3) & 7;
                if (!mode && base == 5)
                    displacementSize = 4;
                else if (base == 1 && !(rex & 1))
                    result.usesCountRegister = true;
                else if (base == 3 && (rex & 1))
                    result.hasScratchInAddress = true;
                if (index == 1 && !(rex & 2))
                    result.usesCountRegister = true;
                if (index == 3 && (rex & 2))
                    result.hasScratchInAddress = true;
            } else if (!mode && last == 5) {
                displacementSize = 4;
                result.isRelativeToNextInstruction = true;
                if (i + 4 > left)
                    return false;
                memcpy(&result.displacement, p + i, 4);
            } else if (last == 1 && !(rex & 1))
                result.usesCountRegister = true;
            else if (last == 3 && (rex & 1))
                result.hasScratchInAddress = true;
            i += displacementSize;
        }
    }
    i += immediate;
    if (i > left || i > 15)
        return false;
    result.size = i;
    return true;
}

struct Effects {
    uint16_t reads { 0 };
    uint16_t writes { 0 };
    bool readsFlags { false };
    bool setsAllFlags { false };
    bool touchesFlags { false };
    bool readsVectorRegister { false };
    bool writesVectorRegister { false };
    bool hasMemory { false };
    bool loads { false };
    bool stores { false };
    bool addressIsPlain { false };
    uint8_t base { 0 };
    uint8_t bytesInMemory { 0 };
    int32_t displacement { 0 };
};

inline bool computeEffects(const uint8_t* p, size_t size, Effects& result)
{
    constexpr unsigned accumulator = 0;
    constexpr unsigned stackPointer = 4;
    constexpr unsigned framePointer = 5;
    result = { };
    size_t i = 0;
    bool hasOperandSizePrefix = false;
    while (i < size && (p[i] == 0x66 || p[i] == 0xf2 || p[i] == 0xf3))
        hasOperandSizePrefix |= p[i++] == 0x66;
    uint8_t rex = 0;
    if (i < size && (p[i] & 0xf0) == 0x40)
        rex = p[i++];
    if (i >= size)
        return false;
    unsigned opcode = p[i++];
    if (opcode == 0x0f) {
        if (i >= size)
            return false;
        opcode = 0x100 | p[i++];
    }
    enum class Does : uint8_t { Nothing, Reads, Writes, ReadsAndWrites };
    Does middleDoes = Does::Nothing;
    Does lastDoes = Does::Nothing;
    bool isByte = false;
    bool hasModRM = true;
    bool isAddressOnly = false;
    auto bit = [](unsigned reg) -> uint16_t { return static_cast<uint16_t>(1u << reg); };
    unsigned middleOfModRM = i < size ? (p[i] >> 3) & 7 : 0;
    if (opcode < 0x40) {
        unsigned operation = opcode >> 3;
        bool compares = operation == 7;
        result.setsAllFlags = true;
        result.touchesFlags = true;
        result.readsFlags = operation == 2 || operation == 3;
        switch (opcode & 7) {
        case 0:
            isByte = true;
            [[fallthrough]];
        case 1:
            middleDoes = Does::Reads;
            lastDoes = compares ? Does::Reads : Does::ReadsAndWrites;
            if (operation == 6 && !isByte && !hasOperandSizePrefix && i < size && p[i] >> 6 == 3 && ((p[i] >> 3) & 7) == (p[i] & 7) && !(rex & 4) == !(rex & 1)) {
                middleDoes = Does::Nothing;
                lastDoes = Does::Writes;
            }
            break;
        case 2:
            isByte = true;
            [[fallthrough]];
        case 3:
            middleDoes = compares ? Does::Reads : Does::ReadsAndWrites;
            lastDoes = Does::Reads;
            if (operation == 6 && !isByte && !hasOperandSizePrefix && i < size && p[i] >> 6 == 3 && ((p[i] >> 3) & 7) == (p[i] & 7) && !(rex & 4) == !(rex & 1)) {
                middleDoes = Does::Writes;
                lastDoes = Does::Nothing;
            }
            break;
        case 4:
        case 5:
            hasModRM = false;
            result.reads |= bit(accumulator);
            if (!compares && (opcode & 7) == 5 && !hasOperandSizePrefix)
                result.writes |= bit(accumulator);
            else if (!compares)
                return false;
            break;
        default:
            return false;
        }
    } else if (opcode >= 0x50 && opcode <= 0x5f) {
        hasModRM = false;
        unsigned reg = (opcode & 7) | (rex & 1) << 3;
        result.reads |= bit(stackPointer);
        result.writes |= bit(stackPointer);
        if (opcode < 0x58) {
            result.reads |= bit(reg);
            result.stores = true;
        } else {
            result.writes |= bit(reg);
            result.loads = true;
        }
        result.hasMemory = true;
    } else {
        switch (opcode) {
        case 0x63:
        case 0x1b6:
        case 0x1b7:
        case 0x1be:
        case 0x1bf:
            middleDoes = Does::Writes;
            lastDoes = Does::Reads;
            break;
        case 0x16f:
            if (i >= size || p[i] >> 6 == 3)
                return false;
            lastDoes = Does::Reads;
            hasOperandSizePrefix = false;
            result.writesVectorRegister = true;
            break;
        case 0x167:
            if (i >= size || p[i] >> 6 != 3)
                return false;
            hasOperandSizePrefix = false;
            result.readsVectorRegister = true;
            result.writesVectorRegister = true;
            break;
        case 0x17e:
            if (i >= size || p[i] >> 6 != 3 || !hasOperandSizePrefix || !(rex & 8))
                return false;
            lastDoes = Does::Writes;
            hasOperandSizePrefix = false;
            result.readsVectorRegister = true;
            break;
        case 0x69:
        case 0x6b:
            middleDoes = Does::Writes;
            lastDoes = Does::Reads;
            result.setsAllFlags = true;
            result.touchesFlags = true;
            break;
        case 0x1af:
            middleDoes = Does::ReadsAndWrites;
            lastDoes = Does::Reads;
            result.setsAllFlags = true;
            result.touchesFlags = true;
            break;
        case 0x80:
            isByte = true;
            [[fallthrough]];
        case 0x81:
        case 0x83:
            lastDoes = middleOfModRM == 7 ? Does::Reads : Does::ReadsAndWrites;
            if (opcode == 0x83 && middleOfModRM == 1 && p[size - 1] == 0xff && !hasOperandSizePrefix)
                lastDoes = Does::Writes;
            result.setsAllFlags = true;
            result.touchesFlags = true;
            result.readsFlags = middleOfModRM == 2 || middleOfModRM == 3;
            break;
        case 0x84:
            isByte = true;
            [[fallthrough]];
        case 0x85:
            middleDoes = Does::Reads;
            lastDoes = Does::Reads;
            result.setsAllFlags = true;
            result.touchesFlags = true;
            break;
        case 0x88:
            isByte = true;
            [[fallthrough]];
        case 0x89:
            middleDoes = Does::Reads;
            lastDoes = Does::Writes;
            break;
        case 0x8a:
            isByte = true;
            [[fallthrough]];
        case 0x8b:
            middleDoes = Does::Writes;
            lastDoes = Does::Reads;
            break;
        case 0x8d:
            middleDoes = Does::Writes;
            isAddressOnly = true;
            break;
        case 0xc6:
            isByte = true;
            [[fallthrough]];
        case 0xc7:
            if (middleOfModRM)
                return false;
            lastDoes = Does::Writes;
            break;
        case 0xc1:
        case 0xd1:
            lastDoes = Does::ReadsAndWrites;
            result.touchesFlags = true;
            break;
        case 0xf6:
            isByte = true;
            [[fallthrough]];
        case 0xf7:
            if (middleOfModRM > 3)
                return false;
            lastDoes = middleOfModRM < 2 ? Does::Reads : Does::ReadsAndWrites;
            result.setsAllFlags = middleOfModRM != 2;
            result.touchesFlags = middleOfModRM != 2;
            break;
        case 0xff:
            if (middleOfModRM > 1)
                return false;
            lastDoes = Does::ReadsAndWrites;
            result.touchesFlags = true;
            break;
        case 0x1a3:
            middleDoes = Does::Reads;
            lastDoes = Does::Reads;
            result.touchesFlags = true;
            break;
        case 0x1ba:
            if (middleOfModRM != 4)
                return false;
            lastDoes = Does::Reads;
            result.touchesFlags = true;
            break;
        case 0xa8:
        case 0xa9:
            hasModRM = false;
            result.reads |= bit(accumulator);
            result.setsAllFlags = true;
            result.touchesFlags = true;
            break;
        case 0xc9:
            hasModRM = false;
            result.reads |= bit(framePointer);
            result.writes |= bit(framePointer) | bit(stackPointer);
            result.hasMemory = true;
            result.loads = true;
            break;
        default:
            if (opcode >= 0xb8 && opcode <= 0xbf && !hasOperandSizePrefix) {
                hasModRM = false;
                result.writes |= bit((opcode & 7) | (rex & 1) << 3);
                break;
            }
            if ((opcode & 0x1f0) == 0x140) {
                middleDoes = Does::ReadsAndWrites;
                lastDoes = Does::Reads;
                result.readsFlags = true;
                break;
            }
            return false;
        }
    }
    if (!hasModRM)
        return true;
    if (i >= size)
        return false;
    uint8_t modRM = p[i++];
    unsigned mode = modRM >> 6;
    unsigned middle = ((modRM >> 3) & 7) | (rex & 4) << 1;
    unsigned last = modRM & 7;
    bool writesPartially = isByte || hasOperandSizePrefix;
    auto note = [&](unsigned reg, Does does) {
        if (does == Does::Reads || does == Does::ReadsAndWrites || (does == Does::Writes && writesPartially))
            result.reads |= bit(reg);
        if ((does == Does::Writes || does == Does::ReadsAndWrites) && !writesPartially)
            result.writes |= bit(reg);
        return does == Does::Nothing || does == Does::Reads || !writesPartially;
    };
    if (isByte && !rex && ((middleDoes != Does::Nothing && middle >= 4) || (mode == 3 && last >= 4)))
        return false;
    if (!note(middle, middleDoes))
        return false;
    if (mode == 3) {
        if (isAddressOnly)
            return false;
        return note(last | (rex & 1) << 3, lastDoes);
    }
    unsigned displacementSize = mode == 1 ? 1 : mode == 2 ? 4 : 0;
    bool hasBase = true;
    bool hasIndex = false;
    unsigned base = last | (rex & 1) << 3;
    if (last == 4) {
        if (i >= size)
            return false;
        uint8_t scaleIndexBase = p[i++];
        base = (scaleIndexBase & 7) | (rex & 1) << 3;
        unsigned index = ((scaleIndexBase >> 3) & 7) | (rex & 2) << 2;
        if (!mode && (scaleIndexBase & 7) == 5) {
            displacementSize = 4;
            hasBase = false;
        }
        if (index != 4) {
            hasIndex = true;
            result.reads |= bit(index);
        }
    } else if (!mode && last == 5) {
        displacementSize = 4;
        hasBase = false;
    }
    if (hasBase)
        result.reads |= bit(base);
    if (i + displacementSize > size)
        return false;
    if (displacementSize == 1)
        result.displacement = static_cast<int8_t>(p[i]);
    else if (displacementSize == 4)
        memcpy(&result.displacement, p + i, 4);
    if (isAddressOnly)
        return true;
    result.hasMemory = true;
    result.loads = lastDoes == Does::Reads || lastDoes == Does::ReadsAndWrites;
    result.stores = lastDoes == Does::Writes || lastDoes == Does::ReadsAndWrites;
    result.addressIsPlain = hasBase && !hasIndex;
    result.base = base;
    bool readsNarrow = opcode == 0x1b6 || opcode == 0x1be;
    bool readsHalf = opcode == 0x1b7 || opcode == 0x1bf;
    result.bytesInMemory = isByte || readsNarrow ? 1 : hasOperandSizePrefix || readsHalf ? 2 : (rex & 8) && opcode != 0x63 ? 8 : 4;
    return true;
}
#endif

static constexpr size_t notFound = static_cast<size_t>(-1);

inline uint64_t hashOf(const uint8_t* bytes, size_t size, uint64_t seed)
{
    uint64_t hash = 0xcbf29ce484222325ull ^ seed;
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

inline uint64_t hashOfContents(const uint8_t* bytes, size_t size, uint64_t seed)
{
    uint64_t hash = seed ^ size * 0x9e3779b97f4a7c15ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        memcpy(&word, bytes + i, 8);
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 32;
    }
    for (; i < size; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

class Stream {
public:
    bool read(const Body& body, bool mayUseCountRegister = false)
    {
        m_body = &body;
        units.reserveInitialCapacity(body.size / 3);
        m_identityOfUnit.reserveInitialCapacity(body.size / 3);
        Vector<int32_t> unitIndexAtOffset;
        unitIndexAtOffset.fill(-1, body.size + 1);
        size_t nextReference = 0;
        for (size_t at = 0; at < body.size;) {
            Decoded decoded;
            bool hasListedReference = nextReference < body.references.size() && body.references[nextReference].offset == at;
#if CPU(ARM64)
            if (!hasListedReference && !decode(body.bytes + at, body.size - at, decoded))
                return false;
#else
            if (!decode(body.bytes + at, body.size - at, decoded))
                return false;
#endif
            Unit unit;
            unit.offset = at;
            unit.size = decoded.size;
            unit.kind = decoded.kind;
            unit.condition = decoded.condition;
            unit.hasScratchAsOperand = decoded.hasScratchAsOperand;
            unit.hasScratchInAddress = decoded.hasScratchInAddress;
            unit.word = decoded.word;
            int64_t target = -1;
            uint64_t identity = 0;
            if (nextReference < body.references.size() && body.references[nextReference].offset < at)
                return false;
            int64_t named = static_cast<int64_t>(at) + decoded.size + decoded.displacement;
            if (decoded.usesCountRegister && at && !mayUseCountRegister)
                return false;
            if (hasListedReference) {
#if CPU(ARM64)
                decoded.size = body.references[nextReference].size;
                unit.size = decoded.size;
                if (!decoded.size || at + decoded.size > body.size)
                    return false;
#else
                if (!decoded.isRelativeToNextInstruction && decoded.kind != Kind::Reference)
                    return false;
#endif
                unit.kind = Kind::Reference;
                unit.reference = nextReference++;
                identity = body.references[unit.reference].identity;
                unit.usesRegistersOnly = body.references[unit.reference].usesRegistersOnly;
            } else if (decoded.isRelativeToNextInstruction || decoded.kind == Kind::Reference) {
#if CPU(ARM64)
                uint32_t nextInstruction = 0;
                if (at + 8 <= body.size)
                    memcpy(&nextInstruction, body.bytes + at + 4, 4);
                bool addsPagesNext = (nextInstruction & 0xbfc00000) == 0x91400000 && (nextInstruction & 0x1f) == (decoded.word & 0x1f) && ((nextInstruction >> 5) & 0x1f) == (decoded.word & 0x1f);
                if (decoded.kind != Kind::Reference && addsPagesNext) {
                    constexpr size_t reachOfAddressInstruction = 1 << 20;
                    if (body.size >= reachOfAddressInstruction)
                        return false;
                    int64_t pages = (nextInstruction >> 10) & 0xfff;
                    named += (nextInstruction & 0x40000000 ? -pages : pages) * 4096;
                    decoded.size = 8;
                }
                bool isAddress = true;
#else
                bool isAddress = body.bytes[at + 1] == 0x8d && decoded.size == 7;
#endif
                bool isInside = named >= 0 && named < static_cast<int64_t>(body.size);
                if (!isInside || (decoded.kind != Kind::Reference && !isAddress))
                    return false;
                unit.kind = decoded.kind == Kind::Reference ? Kind::Call : Kind::AddressOfCode;
                target = named;
            } else if (decoded.kind == Kind::Jump || decoded.kind == Kind::ConditionalJump) {
                if (named < 0 || named >= static_cast<int64_t>(body.size))
                    return false;
                target = named;
            }
            switch (unit.kind) {
            case Kind::Jump:
                unit.key = 1;
                break;
            case Kind::Call:
                unit.key = 4;
                break;
#if CPU(ARM64)
            case Kind::ConditionalJump:
                unit.key = 2 + static_cast<uint64_t>(unit.word) * 0x9e3779b97f4a7c15ull;
                break;
            case Kind::AddressOfCode:
                unit.key = 3 + static_cast<uint64_t>(unit.word) * 0x9e3779b97f4a7c15ull;
                break;
            case Kind::Reference:
                unit.key = hashOf(nullptr, 0, identity);
                break;
#else
            case Kind::ConditionalJump:
                unit.key = 2 + unit.condition;
                break;
            case Kind::AddressOfCode:
                unit.key = hashOf(body.bytes + at, 3, 3);
                break;
            case Kind::Reference:
                unit.key = hashOf(body.bytes + at, decoded.size - 4, identity);
                break;
#endif
            default:
                unit.key = hashOf(body.bytes + at, decoded.size, 5);
                break;
            }
            unitIndexAtOffset[at] = units.size();
            at += decoded.size;
            if (unit.kind == Kind::Fill)
                continue;
            unit.target = static_cast<int32_t>(target);
            m_identityOfUnit.append(identity);
            units.append(unit);
        }
        if (nextReference != body.references.size())
            return false;
        for (auto& unit : units) {
            if (unit.target < 0)
                continue;
            unit.target = unitIndexAtOffset[unit.target];
            if (unit.target < 0 || static_cast<size_t>(unit.target) >= units.size())
                return false;
        }
        return !units.isEmpty();
    }

    static bool endsBlock(Kind kind)
    {
        return kind == Kind::Jump || kind == Kind::Return || kind == Kind::IndirectJump;
    }

    bool isSameExceptTarget(size_t a, size_t b) const
    {
        auto& one = units[a];
        auto& other = units[b];
        if (one.kind != other.kind || one.key != other.key)
            return false;
        if (one.kind == Kind::Jump || one.kind == Kind::ConditionalJump || one.kind == Kind::Call)
            return true;
        if (one.size != other.size)
            return false;
#if CPU(ARM64)
        size_t compared = one.kind == Kind::Plain || one.kind == Kind::Return || one.kind == Kind::IndirectJump ? one.size : 0;
#else
        size_t compared = one.kind == Kind::Plain || one.kind == Kind::Return || one.kind == Kind::IndirectJump ? one.size : one.size - 4;
#endif
        return !memcmp(bytesOf(one), bytesOf(other), compared) && m_identityOfUnit[a] == m_identityOfUnit[b];
    }

    void tidy()
    {
        size_t n = units.size();
        Unit* all = units.mutableSpan().data();
        for (size_t i = 0; i < n; ++i) {
            int32_t destination = all[i].target;
            if (destination < 0)
                continue;
            for (unsigned jumpsFollowed = 0; jumpsFollowed < 8 && all[destination].kind == Kind::Jump && all[destination].target != destination; ++jumpsFollowed)
                destination = all[destination].target;
            all[i].target = destination;
        }
        mergeDuplicateTails();
        constexpr uint8_t isJumpTarget = 1;
        constexpr uint8_t isDropped = 2;
        constexpr uint8_t isReached = 4;
        Vector<uint8_t> unitMarks;
        unitMarks.fill(0, n + 1);
        uint8_t* marks = unitMarks.mutableSpan().data();
        for (size_t i = 0; i < n; ++i) {
            if (all[i].target >= 0)
                marks[all[i].target] |= isJumpTarget;
        }
        for (size_t i = 0; i + 2 < n; ++i) {
            if (all[i].kind == Kind::ConditionalJump && all[i + 1].kind == Kind::Jump && all[i].target == static_cast<int32_t>(i + 2) && !(marks[i + 1] & isJumpTarget)) {
                invertCondition(all[i]);
                all[i].target = all[i + 1].target;
                marks[i + 1] |= isDropped;
            }
        }
#if CPU(X86_64)
        bool scratchIsKnown = false;
        uint64_t addressInScratch = 0;
        for (size_t i = 0; i < n; ++i) {
            auto& unit = all[i];
            if (marks[i] & isJumpTarget)
                scratchIsKnown = false;
            if (marks[i] & isDropped)
                continue;
            static constexpr uint8_t loadsAddressIntoScratch[] = { 0x4c, 0x8d, 0x1d };
            if (unit.kind == Kind::Reference && unit.size == 7 && !memcmp(bytesOf(unit), loadsAddressIntoScratch, sizeof(loadsAddressIntoScratch))) {
                if (scratchIsKnown && addressInScratch == m_identityOfUnit[i])
                    marks[i] |= isDropped;
                scratchIsKnown = true;
                addressInScratch = m_identityOfUnit[i];
                continue;
            }
            if (unit.hasScratchAsOperand || unit.kind == Kind::Reference || unit.kind == Kind::Call || endsBlock(unit.kind))
                scratchIsKnown = false;
        }
#endif
        Vector<uint32_t, 64> toVisit;
        toVisit.append(0);
        marks[0] |= isReached;
        while (!toVisit.isEmpty()) {
            size_t index = toVisit.takeLast();
            for (;;) {
                bool fallsThrough = (marks[index] & isDropped) || !endsBlock(all[index].kind);
                int32_t destination = marks[index] & isDropped ? -1 : all[index].target;
                if (destination >= 0 && !(marks[destination] & isReached)) {
                    marks[destination] |= isReached;
                    toVisit.append(destination);
                }
                if (!fallsThrough || ++index >= n || (marks[index] & isReached))
                    break;
                marks[index] |= isReached;
            }
        }
        size_t kept = 0;
        Vector<int32_t> newUnitIndexes;
        newUnitIndexes.fill(-1, n + 1);
        int32_t* newIndex = newUnitIndexes.mutableSpan().data();
        for (size_t i = 0; i < n; ++i) {
            if (!(marks[i] & isReached))
                marks[i] |= isDropped;
        }
        for (size_t i = 0; i < n; ++i) {
            if (marks[i] & isDropped)
                continue;
            if (all[i].kind == Kind::Jump || all[i].kind == Kind::ConditionalJump) {
                size_t next = i + 1;
                while (next < n && (marks[next] & isDropped))
                    ++next;
                if (all[i].target == static_cast<int32_t>(next)) {
                    marks[i] |= isDropped;
                    continue;
                }
            }
            newIndex[i] = kept++;
        }
        if (kept == n)
            return;
        int32_t next = -1;
        for (size_t i = n; i--;) {
            if (newIndex[i] >= 0)
                next = newIndex[i];
            else
                newIndex[i] = next;
        }
        uint64_t* identities = m_identityOfUnit.mutableSpan().data();
        kept = 0;
        for (size_t i = 0; i < n; ++i) {
            if (marks[i] & isDropped)
                continue;
            if (kept != i) {
                all[kept] = all[i];
                identities[kept] = identities[i];
            }
            if (all[kept].target >= 0)
                all[kept].target = newIndex[all[kept].target];
            ++kept;
        }
        units.shrink(kept);
        m_identityOfUnit.shrink(kept);
    }

#if CPU(X86_64)
    void eliminateDeadStores()
    {
        constexpr uint32_t flags = 1u << 16;
        constexpr uint32_t vectorRegister = 1u << 17;
        constexpr uint32_t allRegisters = 0xffff;
        constexpr uint32_t liveOnReturn = 1u << 0 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 12 | 1u << 13 | 1u << 14 | 1u << 15;
        constexpr uint32_t stackRegisters = 1u << 4 | 1u << 5;
        constexpr uint64_t allSlots = ~0ull;
        size_t n = units.size();
        unsigned output = 1;
        static constexpr uint8_t movesOutputRegister[] = { 0x49, 0x89, 0xca };
        bool hasMovedOutputRegister = units[0].size == sizeof(movesOutputRegister) && !memcmp(bytesOf(units[0]), movesOutputRegister, sizeof(movesOutputRegister));
        if (hasMovedOutputRegister)
            output = 10;
        struct Facts {
            uint32_t reads { 0 };
            uint32_t writes { 0 };
            uint32_t registersThatMustBeDead { 0 };
            uint64_t outputSlotsRead { 0 };
            uint64_t outputSlotsWritten { 0 };
            uint64_t frameSlotsRead { 0 };
            uint64_t frameSlotsWritten { 0 };
            uint64_t frameSlotsThatMustBeDead { 0 };
            bool isRemovable { false };
            bool stores { false };
            bool returnsFailure { false };
        };
        Vector<Facts> unitFacts;
        unitFacts.grow(n);
        Facts* facts = unitFacts.mutableSpan().data();
        Unit* all = units.mutableSpan().data();
        bool tracksFrame = true;
        for (size_t i = 0; i < n; ++i) {
            auto& unit = all[i];
            auto& fact = facts[i];
            const uint8_t* bytes = bytesOf(unit);
            switch (unit.kind) {
            case Kind::Jump:
                continue;
            case Kind::ConditionalJump:
                fact.reads = flags;
                continue;
            case Kind::Return:
                fact.reads = liveOnReturn;
                fact.outputSlotsRead = allSlots;
                continue;
            case Kind::IndirectJump:
                fact.reads = allRegisters;
                fact.frameSlotsRead = allSlots;
                if (unit.size == 3 && bytes[0] == 0xff && bytes[1] == 0x65 && bytes[2] >= 0x80 && !(bytes[2] & 7))
                    fact.frameSlotsRead = 1ull << ((0x100 - bytes[2]) / 8 - 1);
                else if (unit.size == 6 && bytes[0] == 0xff && bytes[1] == 0xa5) {
                    int32_t displacement;
                    memcpy(&displacement, bytes + 2, 4);
                    if (displacement < 0 && !(displacement & 7))
                        fact.frameSlotsRead = displacement >= -512 ? 1ull << (-displacement / 8 - 1) : 0;
                }
                continue;
            case Kind::Reference:
                if (bytes[0] == 0xe8) {
                    fact.reads = allRegisters;
                    if (!unit.usesRegistersOnly) {
                        fact.outputSlotsRead = allSlots;
                        fact.frameSlotsRead = allSlots;
                    }
                    continue;
                }
                break;
            case Kind::Plain:
            case Kind::AddressOfCode:
                break;
            default:
                return;
            }
            if (!i && hasMovedOutputRegister) {
                fact.reads = 1u << 1;
                fact.writes = 1u << 10;
                continue;
            }
            Effects effects;
            if (!computeEffects(bytes, unit.size, effects))
                return;
            fact.reads = effects.reads | (effects.readsFlags ? flags : 0) | (effects.readsVectorRegister ? vectorRegister : 0);
            fact.writes = effects.writes | (effects.setsAllFlags ? flags : 0) | (effects.writesVectorRegister && !effects.readsVectorRegister ? vectorRegister : 0);
            fact.registersThatMustBeDead = effects.writes | (effects.touchesFlags ? flags : 0) | (effects.writesVectorRegister ? vectorRegister : 0);
            fact.isRemovable = !(effects.writes & stackRegisters) && !effects.stores;
            fact.stores = effects.stores;
            if (effects.writes & 1u << output)
                return;
            constexpr unsigned framePointer = 5;
            if ((effects.reads & 1u << framePointer) && !(effects.writes & stackRegisters) && (unit.size != 1 || (bytes[0] & 0xf8) != 0x50)) {
                if (!effects.hasMemory || !effects.addressIsPlain || effects.base != framePointer)
                    tracksFrame = false;
                else if (effects.displacement < 0 && effects.displacement >= -512) {
                    unsigned slot = (-effects.displacement - 1) / 8;
                    bool isWhole = effects.bytesInMemory == 8 && !(effects.displacement & 7);
                    if (static_cast<unsigned>(-effects.displacement) - slot * 8 < effects.bytesInMemory)
                        tracksFrame = false;
                    if (effects.loads)
                        fact.frameSlotsRead = 1ull << slot;
                    if (effects.stores) {
                        fact.frameSlotsThatMustBeDead = 1ull << slot;
                        if (isWhole)
                            fact.frameSlotsWritten = 1ull << slot;
                        fact.isRemovable = !(effects.writes & stackRegisters);
                    }
                } else if (effects.displacement >= 0)
                    tracksFrame = false;
            }
            if (effects.reads & 1u << output) {
                if (!effects.hasMemory || !effects.addressIsPlain || effects.base != output || effects.displacement < 0 || (effects.displacement & 3) || effects.bytesInMemory < 4)
                    return;
                unsigned slot = effects.displacement / 4;
                unsigned count = effects.bytesInMemory / 4;
                if (slot + count <= 64) {
                    uint64_t slots = (count == 2 ? 3ull : 1ull) << slot;
                    if (effects.loads)
                        fact.outputSlotsRead = slots;
                    if (effects.stores) {
                        fact.outputSlotsWritten = slots;
                        fact.isRemovable = !(effects.writes & stackRegisters);
                    }
                }
            }
        }
        for (size_t i = 0; i < n; ++i) {
            static constexpr uint8_t returnsNoMatch[] = { 0x48, 0x83, 0xc8, 0xff };
            if (all[i].kind != Kind::Plain || all[i].size != sizeof(returnsNoMatch) || memcmp(bytesOf(all[i]), returnsNoMatch, sizeof(returnsNoMatch)))
                continue;
            size_t index = i + 1;
            for (unsigned steps = 0; steps < 24 && index < n; ++steps) {
                if (all[index].kind == Kind::Return) {
                    facts[i].returnsFailure = true;
                    break;
                }
                if (all[index].kind == Kind::Jump) {
                    index = all[index].target;
                    continue;
                }
                if (all[index].kind != Kind::Plain || (facts[index].writes & 1) || facts[index].stores || facts[index].outputSlotsRead)
                    break;
                ++index;
            }
        }
        Vector<uint32_t> liveRegistersBeforeUnit;
        liveRegistersBeforeUnit.fill(0, n + 1);
        Vector<uint64_t> liveOutputSlotsBeforeUnit;
        liveOutputSlotsBeforeUnit.fill(0, n + 1);
        uint32_t* liveRegisters = liveRegistersBeforeUnit.mutableSpan().data();
        uint64_t* liveOutputSlots = liveOutputSlotsBeforeUnit.mutableSpan().data();
        Vector<uint64_t> liveFrameSlotsBeforeUnit;
        liveFrameSlotsBeforeUnit.fill(0, n + 1);
        uint64_t* liveFrameSlots = liveFrameSlotsBeforeUnit.mutableSpan().data();
        if (!tracksFrame) {
            for (size_t i = 0; i < n; ++i) {
                if (facts[i].frameSlotsThatMustBeDead)
                    facts[i].isRemovable = false;
            }
        }
        Vector<uint8_t> removedUnits;
        removedUnits.fill(0, n + 1);
        uint8_t* isRemoved = removedUnits.mutableSpan().data();
        constexpr unsigned maxIterations = 24;
        bool changed = true;
        for (unsigned round = 0; changed; ++round) {
            if (round == maxIterations)
                return;
            changed = false;
            uint32_t liveRegistersAtTakenAddresses = 0;
            uint64_t liveOutputSlotsAtTakenAddresses = 0;
            uint64_t liveFrameSlotsAtTakenAddresses = 0;
            for (size_t i = 0; i < n; ++i) {
                if (all[i].kind == Kind::AddressOfCode) {
                    liveRegistersAtTakenAddresses |= liveRegisters[all[i].target];
                    liveOutputSlotsAtTakenAddresses |= liveOutputSlots[all[i].target];
                    liveFrameSlotsAtTakenAddresses |= liveFrameSlots[all[i].target];
                }
            }
            for (size_t i = n; i--;) {
                auto& unit = all[i];
                auto& fact = facts[i];
                uint32_t liveRegistersAfter = 0;
                uint64_t liveOutputSlotsAfter = 0;
                uint64_t liveFrameSlotsAfter = 0;
                if (!endsBlock(unit.kind)) {
                    liveRegistersAfter = liveRegisters[i + 1];
                    liveOutputSlotsAfter = liveOutputSlots[i + 1];
                    liveFrameSlotsAfter = liveFrameSlots[i + 1];
                }
                if (unit.kind == Kind::Jump || unit.kind == Kind::ConditionalJump) {
                    liveRegistersAfter |= liveRegisters[unit.target];
                    liveOutputSlotsAfter |= liveOutputSlots[unit.target];
                    liveFrameSlotsAfter |= liveFrameSlots[unit.target];
                } else if (unit.kind == Kind::IndirectJump) {
                    liveRegistersAfter |= liveRegistersAtTakenAddresses;
                    liveOutputSlotsAfter |= liveOutputSlotsAtTakenAddresses;
                    liveFrameSlotsAfter |= liveFrameSlotsAtTakenAddresses;
                }
                bool isDead = fact.isRemovable && !(fact.registersThatMustBeDead & liveRegistersAfter) && !(fact.outputSlotsWritten & liveOutputSlotsAfter) && !(fact.frameSlotsThatMustBeDead & liveFrameSlotsAfter);
                isRemoved[i] = isDead;
                uint32_t liveRegistersBefore = isDead ? liveRegistersAfter : (liveRegistersAfter & ~fact.writes) | fact.reads;
                uint64_t liveOutputSlotsBefore = isDead ? liveOutputSlotsAfter : (liveOutputSlotsAfter & ~fact.outputSlotsWritten) | fact.outputSlotsRead;
                uint64_t liveFrameSlotsBefore = isDead ? liveFrameSlotsAfter : (liveFrameSlotsAfter & ~fact.frameSlotsWritten) | fact.frameSlotsRead;
                if (fact.returnsFailure)
                    liveOutputSlotsBefore = 0;
                if (liveRegistersBefore != liveRegisters[i] || liveOutputSlotsBefore != liveOutputSlots[i] || liveFrameSlotsBefore != liveFrameSlots[i]) {
                    liveRegisters[i] = liveRegistersBefore;
                    liveOutputSlots[i] = liveOutputSlotsBefore;
                    liveFrameSlots[i] = liveFrameSlotsBefore;
                    changed = true;
                }
            }
        }
        static constexpr uint8_t pushesFramePointer[] = { 0x55 };
        static constexpr uint8_t setsFramePointer[] = { 0x48, 0x89, 0xe5 };
        static constexpr uint8_t reservesFrame[] = { 0x48, 0x83, 0xec };
        static constexpr uint8_t reservesLargeFrame[] = { 0x48, 0x81, 0xec };
        static constexpr uint8_t releasesFrame[] = { 0xc9 };
        auto startsWith = [&](size_t index, std::span<const uint8_t> prefix, size_t size) {
            return index < n && all[index].kind == Kind::Plain && all[index].size == size && !memcmp(bytesOf(all[index]), prefix.data(), prefix.size());
        };
        bool frameIsUnused = !hasMovedOutputRegister && startsWith(0, pushesFramePointer, 1) && startsWith(1, setsFramePointer, 3) && (startsWith(2, reservesFrame, 4) || startsWith(2, reservesLargeFrame, 7));
        for (size_t i = 0; i < n && frameIsUnused; ++i) {
            if (all[i].target >= 0 && all[i].target < 3) {
                frameIsUnused = false;
                break;
            }
            if (i < 3 || isRemoved[i] || all[i].kind == Kind::Return || startsWith(i, releasesFrame, 1))
                continue;
            frameIsUnused = !((facts[i].reads | facts[i].writes) & stackRegisters);
        }
        for (size_t i = 0; i < n && frameIsUnused; ++i) {
            if (i < 3 || startsWith(i, releasesFrame, 1))
                isRemoved[i] = 1;
        }
        removeMarkedUnits(isRemoved);
    }
#endif

    void removeMarkedUnits(const uint8_t* isRemoved)
    {
        size_t n = units.size();
        Vector<int32_t> newUnitIndexes;
        newUnitIndexes.fill(-1, n + 1);
        int32_t* newIndex = newUnitIndexes.mutableSpan().data();
        size_t kept = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!isRemoved[i])
                newIndex[i] = kept++;
        }
        if (kept == n)
            return;
        int32_t next = -1;
        for (size_t i = n; i--;) {
            if (newIndex[i] >= 0)
                next = newIndex[i];
            else
                newIndex[i] = next;
        }
        kept = 0;
        for (size_t i = 0; i < n; ++i) {
            if (isRemoved[i])
                continue;
            units[kept] = units[i];
            m_identityOfUnit[kept] = m_identityOfUnit[i];
            if (units[kept].target >= 0)
                units[kept].target = newIndex[units[kept].target];
            ++kept;
        }
        units.shrink(kept);
        m_identityOfUnit.shrink(kept);
    }

    void optimizeLoops(unsigned maxLoopSize)
    {
        size_t n = units.size();
        Vector<uint32_t> unitOffsets;
        unitOffsets.fill(0, n + 1);
        for (size_t i = 0; i < n; ++i)
            unitOffsets[i + 1] = unitOffsets[i] + units[i].size;
        Vector<uint32_t> firstJumpTo;
        firstJumpTo.fill(std::numeric_limits<uint32_t>::max(), n);
        Vector<uint32_t> lastJumpTo;
        lastJumpTo.fill(0, n);
        for (size_t i = 0; i < n; ++i) {
            if (units[i].target < 0)
                continue;
            firstJumpTo[units[i].target] = std::min<uint32_t>(firstJumpTo[units[i].target], i);
            lastJumpTo[units[i].target] = std::max<uint32_t>(lastJumpTo[units[i].target], i);
        }
        for (size_t last = 0; last < n; ++last) {
            if ((units[last].kind != Kind::Jump && units[last].kind != Kind::ConditionalJump) || units[last].target < 0 || static_cast<size_t>(units[last].target) >= last)
                continue;
            size_t head = units[last].target;
#if CPU(X86_64)
            if (unitOffsets[last + 1] - unitOffsets[head] <= maxLoopSize)
                hoistAddressOutOfLoop(head, last, firstJumpTo, lastJumpTo);
#else
            UNUSED_VARIABLE(head);
#endif
        }
        Vector<uint32_t> rotatedLoops;
        for (size_t last = 0; last < n; ++last) {
            if (units[last].kind != Kind::Jump || units[last].target < 0 || static_cast<size_t>(units[last].target) + 2 > last)
                continue;
            size_t head = units[last].target;
            if (units[head].kind != Kind::Plain || units[head + 1].kind != Kind::ConditionalJump || unitOffsets[last] - unitOffsets[head] > maxLoopSize)
                continue;
            bool containsBlockEnd = false;
            for (size_t i = head; i < last; ++i)
                containsBlockEnd |= endsBlock(units[i].kind);
            if (!containsBlockEnd)
                rotatedLoops.append(last);
        }
        if (rotatedLoops.isEmpty())
            return;
        Vector<Unit> rebuiltUnits;
        Vector<uint64_t> identities;
        Vector<uint32_t> newIndex;
        newIndex.fill(0, n);
        size_t next = 0;
        for (size_t i = 0; i < n; ++i) {
            newIndex[i] = rebuiltUnits.size();
            if (next == rotatedLoops.size() || rotatedLoops[next] != i) {
                rebuiltUnits.append(units[i]);
                identities.append(m_identityOfUnit[i]);
                continue;
            }
            ++next;
            size_t head = units[i].target;
            Unit repeatedBranch = units[head + 1];
            Unit out = units[i];
            out.target = repeatedBranch.target;
            invertCondition(repeatedBranch);
            repeatedBranch.target = head + 2;
            rebuiltUnits.append(units[head]);
            identities.append(m_identityOfUnit[head]);
            rebuiltUnits.append(repeatedBranch);
            identities.append(0);
            rebuiltUnits.append(out);
            identities.append(0);
        }
        for (auto& unit : rebuiltUnits) {
            if (unit.target >= 0)
                unit.target = newIndex[unit.target];
        }
        units = WTF::move(rebuiltUnits);
        m_identityOfUnit = WTF::move(identities);
    }

    void placeBlocksAfterTheirPredecessors()
    {
        size_t n = units.size();
        Vector<uint32_t> blockOfUnit;
        blockOfUnit.fill(0, n);
        Vector<std::pair<uint32_t, uint32_t>> blocks;
        for (size_t i = 0; i < n;) {
            size_t end = i;
            while (end + 1 < n && !endsBlock(units[end].kind))
                ++end;
            for (size_t k = i; k <= end; ++k)
                blockOfUnit[k] = blocks.size();
            blocks.append(std::pair<uint32_t, uint32_t> { static_cast<uint32_t>(i), static_cast<uint32_t>(end + 1) });
            i = end + 1;
        }
        if (blocks.size() < 3)
            return;
        Vector<bool> isPlaced;
        isPlaced.fill(false, blocks.size());
        Vector<uint32_t> order;
        Vector<uint32_t> pendingBlocks;
        for (size_t first = 0; first < blocks.size(); ++first) {
            if (isPlaced[first])
                continue;
            pendingBlocks.append(first);
            while (!pendingBlocks.isEmpty()) {
                uint32_t block = pendingBlocks.last();
                pendingBlocks.removeLast();
                if (isPlaced[block])
                    continue;
                isPlaced[block] = true;
                order.append(block);
                for (uint32_t i = blocks[block].second; i-- > blocks[block].first;) {
                    if (units[i].target >= 0 && !isPlaced[blockOfUnit[units[i].target]])
                        pendingBlocks.append(blockOfUnit[units[i].target]);
                }
                auto& last = units[blocks[block].second - 1];
                if (last.kind == Kind::Jump && !isPlaced[blockOfUnit[last.target]] && blocks[blockOfUnit[last.target]].first == static_cast<uint32_t>(last.target))
                    pendingBlocks.append(blockOfUnit[last.target]);
            }
        }
        Vector<Unit> rebuiltUnits;
        Vector<uint64_t> identities;
        Vector<uint32_t> newIndex;
        newIndex.fill(0, n);
        for (uint32_t block : order) {
            for (uint32_t i = blocks[block].first; i < blocks[block].second; ++i) {
                newIndex[i] = rebuiltUnits.size();
                rebuiltUnits.append(units[i]);
                identities.append(m_identityOfUnit[i]);
            }
        }
        for (auto& unit : rebuiltUnits) {
            if (unit.target >= 0)
                unit.target = newIndex[unit.target];
        }
        units = WTF::move(rebuiltUnits);
        m_identityOfUnit = WTF::move(identities);
    }

#if CPU(X86_64)
    using SavedUnits = std::pair<Vector<Unit>, Vector<uint64_t>>;

    bool copySmallSharedBlocksNearTheirJumps(const Vector<bool>& isLong, const Vector<uint32_t>& address, SavedUnits& unitsWithoutCopies)
    {
        constexpr unsigned maxCopiedBlockSize = 16;
        constexpr unsigned maxUnitsInCopiedBlock = 5;
        constexpr uint32_t reachOfShortJump = 110;
        size_t n = units.size();
        Vector<uint32_t> longJumpsTo;
        longJumpsTo.fill(0, n);
        for (size_t i = 0; i < n; ++i) {
            if (isLong[i])
                ++longJumpsTo[units[i].target];
        }
        Vector<std::pair<uint32_t, uint32_t>> jumps;
        for (size_t i = 0; i < n; ++i) {
            if (isLong[i] && longJumpsTo[units[i].target] > 1)
                jumps.append({ static_cast<uint32_t>(units[i].target), static_cast<uint32_t>(i) });
        }
        if (jumps.isEmpty())
            return false;
        std::ranges::sort(jumps);
        Vector<uint32_t> gaps;
        for (size_t i = 1; i <= n; ++i) {
            if (endsBlock(units[i - 1].kind))
                gaps.append(i);
        }
        struct Copy {
            uint32_t gap;
            uint32_t first;
            uint32_t end;
            uint32_t firstJump;
            uint32_t endJump;
        };
        Vector<Copy> copies;
        for (size_t groupBegin = 0; groupBegin < jumps.size();) {
            uint32_t first = jumps[groupBegin].first;
            size_t groupEnd = groupBegin;
            while (groupEnd < jumps.size() && jumps[groupEnd].first == first)
                ++groupEnd;
            unsigned size = 0;
            uint32_t end = first;
            bool canBeCopied = false;
            for (; end < n && end - first < maxUnitsInCopiedBlock; ++end) {
                auto& unit = units[end];
                if (unit.kind == Kind::Jump)
                    size += 5;
                else if (unit.kind == Kind::ConditionalJump)
                    size += 6;
                else if (unit.kind == Kind::Plain || unit.kind == Kind::Return || unit.kind == Kind::IndirectJump)
                    size += unit.size;
                else
                    break;
                if (endsBlock(unit.kind)) {
                    canBeCopied = size <= maxCopiedBlockSize;
                    ++end;
                    break;
                }
            }
            for (size_t i = groupBegin; canBeCopied && i < groupEnd;) {
                uint32_t from = address[jumps[i].second];
                auto firstGapOutOfReach = std::upper_bound(gaps.begin(), gaps.end(), from + reachOfShortJump, [&](uint32_t limit, uint32_t gap) {
                    return limit < address[gap];
                });
                if (firstGapOutOfReach == gaps.begin() || address[*(firstGapOutOfReach - 1)] + reachOfShortJump < from) {
                    ++i;
                    continue;
                }
                uint32_t gap = *(firstGapOutOfReach - 1);
                unsigned saved = 0;
                size_t served = i;
                for (; served < groupEnd && address[jumps[served].second] <= address[gap] + reachOfShortJump; ++served)
                    saved += units[jumps[served].second].kind == Kind::Jump ? 3 : 4;
                if (saved <= size) {
                    ++i;
                    continue;
                }
                copies.append(Copy { gap, first, end, static_cast<uint32_t>(i), static_cast<uint32_t>(served) });
                i = served;
            }
            groupBegin = groupEnd;
        }
        if (copies.isEmpty())
            return false;
        std::ranges::sort(copies, { }, &Copy::gap);
        Vector<Unit> rebuiltUnits;
        Vector<uint64_t> identities;
        Vector<uint32_t> newIndex;
        newIndex.fill(0, n);
        Vector<uint32_t> firstUnitOfCopy;
        size_t nextCopy = 0;
        for (size_t i = 0; i <= n; ++i) {
            for (; nextCopy < copies.size() && copies[nextCopy].gap == i; ++nextCopy) {
                firstUnitOfCopy.append(rebuiltUnits.size());
                for (uint32_t k = copies[nextCopy].first; k < copies[nextCopy].end; ++k) {
                    rebuiltUnits.append(units[k]);
                    identities.append(m_identityOfUnit[k]);
                }
            }
            if (i == n)
                break;
            newIndex[i] = rebuiltUnits.size();
            rebuiltUnits.append(units[i]);
            identities.append(m_identityOfUnit[i]);
        }
        for (auto& unit : rebuiltUnits) {
            if (unit.target >= 0)
                unit.target = newIndex[unit.target];
        }
        for (size_t copy = 0; copy < copies.size(); ++copy) {
            for (uint32_t jump = copies[copy].firstJump; jump < copies[copy].endJump; ++jump)
                rebuiltUnits[newIndex[jumps[jump].second]].target = firstUnitOfCopy[copy];
        }
        unitsWithoutCopies = { WTF::move(units), WTF::move(m_identityOfUnit) };
        units = WTF::move(rebuiltUnits);
        m_identityOfUnit = WTF::move(identities);
        return true;
    }

    void restoreUnits(SavedUnits&& saved)
    {
        units = WTF::move(saved.first);
        m_identityOfUnit = WTF::move(saved.second);
    }
#endif

    const uint8_t* bytesOf(const Unit& unit) const { return m_body->bytes + unit.offset; }
    uint64_t identityOf(size_t index) const { return m_identityOfUnit[index]; }

    Vector<Unit> units;

private:
    void hoistAddressOutOfLoop(size_t head, size_t last, Vector<uint32_t>& firstJumpTo, Vector<uint32_t>& lastJumpTo)
    {
        size_t address = notFound;
        for (size_t i = head; i <= last; ++i) {
            auto& unit = units[i];
            if (endsBlock(unit.kind) && i != last)
                return;
            if (unit.kind == Kind::Reference && unit.hasScratchAsOperand && unit.size == 7) {
                if (address != notFound)
                    return;
                address = i;
                continue;
            }
            if (unit.hasScratchAsOperand || (unit.hasScratchInAddress && address == notFound) || unit.kind == Kind::Reference || unit.kind == Kind::Call)
                return;
        }
        if (address == notFound || address == head || firstJumpTo[address] != std::numeric_limits<uint32_t>::max())
            return;
        for (size_t i = head + 1; i <= last; ++i) {
            if (firstJumpTo[i] != std::numeric_limits<uint32_t>::max() && (firstJumpTo[i] < head || lastJumpTo[i] > last))
                return;
        }
        for (size_t i = head; i <= last; ++i) {
            int32_t target = units[i].target;
            if (target >= static_cast<int32_t>(head) && target < static_cast<int32_t>(address))
                units[i].target = target + 1;
        }
        Unit moved = units[address];
        uint64_t identity = m_identityOfUnit[address];
        uint32_t firstJumpToHead = firstJumpTo[head];
        uint32_t lastJumpToHead = lastJumpTo[head];
        for (size_t i = address; i > head; --i) {
            units[i] = units[i - 1];
            m_identityOfUnit[i] = m_identityOfUnit[i - 1];
            firstJumpTo[i] = firstJumpTo[i - 1];
            lastJumpTo[i] = lastJumpTo[i - 1];
        }
        units[head] = moved;
        m_identityOfUnit[head] = identity;
        firstJumpTo[head + 1] = head;
        lastJumpTo[head + 1] = last;
        firstJumpTo[head] = firstJumpToHead < head ? firstJumpToHead : lastJumpToHead > last ? lastJumpToHead : std::numeric_limits<uint32_t>::max();
        lastJumpTo[head] = lastJumpToHead > last ? lastJumpToHead : (firstJumpToHead < head ? static_cast<uint32_t>(head) : 0);
    }

    void mergeDuplicateTails()
    {
        size_t n = units.size();
        constexpr size_t maxTailLength = 24;
        Vector<uint64_t> tailHash;
        tailHash.fill(0, n + 1);
        Vector<uint8_t> tailLength;
        tailLength.fill(0, n + 1);
        for (size_t i = n; i--;) {
            auto& unit = units[i];
            uint64_t own = unit.key * 0x9e3779b97f4a7c15ull + static_cast<uint64_t>(unit.target + 1);
            if (endsBlock(unit.kind)) {
                tailHash[i] = own;
                tailLength[i] = 1;
                continue;
            }
            if (!tailLength[i + 1] || tailLength[i + 1] >= maxTailLength)
                continue;
            tailHash[i] = (tailHash[i + 1] ^ own) * 0x100000001b3ull + own;
            tailLength[i] = tailLength[i + 1] + 1;
        }
        Vector<bool> isJumpTarget;
        isJumpTarget.fill(false, n);
        for (auto& unit : units) {
            if (unit.target >= 0)
                isJumpTarget[unit.target] = true;
        }
        Vector<std::pair<uint64_t, uint32_t>> blocks;
        for (size_t i = 0; i < n; ++i) {
            if (isJumpTarget[i] && tailLength[i])
                blocks.append(std::pair<uint64_t, uint32_t> { tailHash[i], static_cast<uint32_t>(i) });
        }
        std::sort(blocks.begin(), blocks.end());
        Vector<int32_t> canonicalTail;
        canonicalTail.fill(-1, n);
        bool found = false;
        for (size_t k = 1; k < blocks.size(); ++k) {
            if (blocks[k].first != blocks[k - 1].first)
                continue;
            uint32_t first = blocks[k - 1].second;
            if (canonicalTail[first] >= 0)
                first = canonicalTail[first];
            uint32_t other = blocks[k].second;
            if (tailLength[first] != tailLength[other])
                continue;
            bool isEqual = true;
            for (size_t step = 0; step < tailLength[first] && isEqual; ++step)
                isEqual = isSameExceptTarget(first + step, other + step) && units[first + step].target == units[other + step].target;
            if (!isEqual || first + tailLength[first] > other)
                continue;
            canonicalTail[other] = first;
            found = true;
        }
        if (!found)
            return;
        for (auto& unit : units) {
            if (unit.target >= 0 && canonicalTail[unit.target] >= 0)
                unit.target = canonicalTail[unit.target];
        }
    }

    const Body* m_body { nullptr };
    Vector<uint64_t> m_identityOfUnit;
};

inline bool endsBlock(Kind kind)
{
    return Stream::endsBlock(kind);
}

class Tidier {
public:
    static bool tidy(const Body& body, Result& result, unsigned maxOptimizedLoopSize)
    {
        Stream stream;
        if (!stream.read(body, true))
            return false;
        stream.tidy();
#if CPU(X86_64)
        stream.eliminateDeadStores();
#endif
        stream.optimizeLoops(maxOptimizedLoopSize);
        stream.placeBlocksAfterTheirPredecessors();
        stream.tidy();
        auto& units = stream.units;
        size_t n = units.size();
#if CPU(ARM64)
        Vector<uint32_t> address;
        address.fill(0, n + 1);
        Vector<bool> jumpsOverLongJump;
        jumpsOverLongJump.fill(false, n);
        for (bool hasChanged = true; hasChanged;) {
            hasChanged = false;
            for (size_t i = 0; i < n; ++i)
                address[i + 1] = address[i] + units[i].size + (jumpsOverLongJump[i] ? 4 : 0);
            for (size_t i = 0; i < n; ++i) {
                bool isTestBitBranch = units[i].kind == Kind::ConditionalJump && (units[i].word & 0x7e000000) == 0x36000000;
                if (!isTestBitBranch || jumpsOverLongJump[i])
                    continue;
                int64_t distance = static_cast<int64_t>(address[units[i].target]) - address[i];
                if (distance < -(1 << 15) || distance >= (1 << 15)) {
                    jumpsOverLongJump[i] = true;
                    hasChanged = true;
                }
            }
        }
        result.bytes.grow(address[n]);
        uint8_t* out = result.bytes.mutableSpan().data();
        for (size_t i = 0; i < n; ++i) {
            auto& unit = units[i];
            uint8_t* at = out + address[i];
            memcpy(at, stream.bytesOf(unit), unit.size);
            if (unit.kind == Kind::Reference) {
                result.references.append(PlacedReference { address[i], false, static_cast<uint32_t>(unit.reference), stream.identityOf(i) });
                continue;
            }
            if (unit.target < 0)
                continue;
            int64_t distance = static_cast<int64_t>(address[unit.target]) - address[i];
            uint32_t word = unit.word;
            auto fits = [&](unsigned bits) {
                return distance >= -(1ll << (bits - 1)) && distance < (1ll << (bits - 1));
            };
            if (unit.kind == Kind::Jump || unit.kind == Kind::Call) {
                if (!fits(28))
                    return false;
                word |= static_cast<uint32_t>(distance >> 2) & 0x3ffffff;
            } else if (unit.kind == Kind::AddressOfCode) {
                if (!fits(21))
                    return false;
                word |= (static_cast<uint32_t>(distance) & 3) << 29 | (static_cast<uint32_t>(distance >> 2) & 0x7ffff) << 5;
            } else if ((word & 0x7e000000) == 0x36000000) {
                if (jumpsOverLongJump[i]) {
                    distance -= 4;
                    if (!fits(28))
                        return false;
                    uint32_t jump = 0x14000000 | (static_cast<uint32_t>(distance >> 2) & 0x3ffffff);
                    memcpy(at + 4, &jump, 4);
                    word ^= 1 << 24;
                    distance = 8;
                }
                if (!fits(16))
                    return false;
                word |= (static_cast<uint32_t>(distance >> 2) & 0x3fff) << 5;
            } else {
                if (!fits(21))
                    return false;
                word |= (static_cast<uint32_t>(distance >> 2) & 0x7ffff) << 5;
            }
            memcpy(at, &word, 4);
        }
#else
        Vector<bool> isLong;
        Vector<uint32_t> address;
        auto sizeOf = [&](size_t i) -> unsigned {
            if (units[i].kind == Kind::Jump)
                return isLong[i] ? 5 : 2;
            if (units[i].kind == Kind::ConditionalJump)
                return isLong[i] ? 6 : 2;
            return units[i].size;
        };
        auto chooseJumpSizes = [&] {
            n = units.size();
            isLong.fill(false, n);
            address.fill(0, n + 1);
            for (;;) {
                for (size_t i = 0; i < n; ++i)
                    address[i + 1] = address[i] + sizeOf(i);
                bool changed = false;
                for (size_t i = 0; i < n; ++i) {
                    if ((units[i].kind != Kind::Jump && units[i].kind != Kind::ConditionalJump) || isLong[i])
                        continue;
                    int64_t distance = static_cast<int64_t>(address[units[i].target]) - (address[i] + 2);
                    if (distance < -128 || distance > 127) {
                        isLong[i] = true;
                        changed = true;
                    }
                }
                if (!changed)
                    return address[n];
            }
        };
        uint32_t sizeWithoutCopies = chooseJumpSizes();
        Stream::SavedUnits unitsWithoutCopies;
        if (stream.copySmallSharedBlocksNearTheirJumps(isLong, address, unitsWithoutCopies) && chooseJumpSizes() >= sizeWithoutCopies) {
            stream.restoreUnits(WTF::move(unitsWithoutCopies));
            chooseJumpSizes();
        }
        result.bytes.grow(address[n]);
        uint8_t* out = result.bytes.mutableSpan().data();
        for (size_t i = 0; i < n; ++i) {
            auto& unit = units[i];
            uint8_t* at = out + address[i];
            unsigned size = sizeOf(i);
            int32_t distance = unit.target >= 0 ? static_cast<int32_t>(static_cast<int64_t>(address[unit.target]) - (address[i] + size)) : 0;
            if (unit.kind == Kind::Jump || unit.kind == Kind::ConditionalJump) {
                if (isLong[i]) {
                    if (unit.kind == Kind::Jump)
                        at[0] = 0xe9;
                    else {
                        at[0] = 0x0f;
                        at[1] = 0x80 | unit.condition;
                    }
                    memcpy(at + size - 4, &distance, 4);
                } else {
                    at[0] = unit.kind == Kind::Jump ? 0xeb : 0x70 | unit.condition;
                    at[1] = static_cast<uint8_t>(static_cast<int8_t>(distance));
                }
                continue;
            }
            memcpy(at, stream.bytesOf(unit), size);
            if (unit.kind == Kind::AddressOfCode || unit.kind == Kind::Call)
                memcpy(at + size - 4, &distance, 4);
            else if (unit.kind == Kind::Reference)
                result.references.append(PlacedReference { address[i], false, static_cast<uint32_t>(unit.reference), stream.identityOf(i) });
        }
#endif
        return true;
    }
};

} } } // namespace JSC::Yarr::ImageBody

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
