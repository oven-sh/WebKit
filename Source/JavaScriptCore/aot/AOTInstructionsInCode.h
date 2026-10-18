#pragma once

#if ENABLE(AOT)

#include "AOTImage.h"
#include <array>
#include <span>

namespace JSC::AOT {

struct InstructionInCode {
    enum class Kind : uint8_t {
        Plain,
        Pinned,
        StubCall,
        Jump,
        ConditionalJump,
        TableAddress,
        TableEntry,
        End,
    };
    uint32_t offset;
    uint32_t target;
    uint8_t length;
    Kind kind;
};

#if CPU(X86_64)
struct DecodedInstruction {
    uint8_t length;
    uint8_t modRMOffset;
};

inline DecodedInstruction decodeInstructionSlow(const uint8_t* bytes, size_t available)
{
    constexpr DecodedInstruction unknown { 0, 0 };
    static constexpr uint32_t isKnown[7][8] = {
        { 0x02022b2b, 0x2a2b2b0b, 0xffff0000, 0xffff0308, 0x02ffabfb, 0xff00030a, 0x220a12ca, 0x84d00b00 },
        { 0x80770800, 0x00025700, 0xfff3ffff, 0x40ffffff, 0xffffffff, 0xf5cbc00c, 0x7fbeff77, 0x7f3eff7f },
        { 0x70000001, 0xff290a29, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
        { 0x00704c00, 0x00000007, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
        { 0x00770000, 0x00005700, 0xfff30000, 0x40ffffff, 0x00000000, 0x00000000, 0x7fbe0054, 0x7f3ef77f },
        { 0x71800811, 0xffa90b29, 0x03000001, 0x03000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
        { 0x0070cf00, 0x00000007, 0x00000800, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    };
    static constexpr uint32_t hasModRMInFirstMap[8] = { 0x0f0f0f0f, 0x0f0f0f0f, 0x00000000, 0x00000a08, 0x0000ffff, 0x00000000, 0xff0f00c3, 0xc0c00000 };
    static constexpr uint32_t hasNoModRMInSecondMap[8] = { 0x00004be0, 0x00ff0000, 0x00000000, 0x00800000, 0x0000ffff, 0x00000707, 0x0000ff00, 0x00000000 };
    auto isIn = [](const uint32_t (&set)[8], uint8_t opcode) {
        return set[opcode >> 5] >> (opcode & 31) & 1;
    };
    available = std::min<size_t>(available, 15);
    unsigned at = 0;
    bool hasOperandSizePrefix = false;
    for (; at < available; ++at) {
        uint8_t byte = bytes[at];
        if (byte == 0x66)
            hasOperandSizePrefix = true;
        else if (byte != 0xf2 && byte != 0xf3 && byte != 0xf0 && byte != 0x2e && byte != 0x36 && byte != 0x3e && byte != 0x26 && byte != 0x64 && byte != 0x65)
            break;
    }
    if (at + 1 > available)
        return unknown;
    unsigned map = 1;
    bool hasVex = false;
    bool isWide = false;
    if (bytes[at] == 0xc5 || bytes[at] == 0xc4) {
        hasVex = true;
        if (at + 3 > available)
            return unknown;
        if (bytes[at] == 0xc5) {
            map = 2;
            at += 2;
        } else {
            unsigned encodedMap = bytes[at + 1] & 31;
            if (!encodedMap || encodedMap > 3)
                return unknown;
            map = encodedMap + 1;
            at += 3;
        }
    } else {
        if ((bytes[at] & 0xf0) == 0x40)
            isWide = bytes[at++] & 8;
        if (at + 1 > available)
            return unknown;
        if (bytes[at] == 0x0f) {
            map = 2;
            if (++at + 1 > available)
                return unknown;
            if (bytes[at] == 0x38 || bytes[at] == 0x3a)
                map = bytes[at++] == 0x38 ? 3 : 4;
        }
    }
    if (at + 1 > available)
        return unknown;
    uint8_t opcode = bytes[at++];
    if (!isIn(isKnown[(hasVex ? 3 : 0) + map - 1], opcode))
        return unknown;
    unsigned wordOrDoubleWord = hasOperandSizePrefix && !isWide ? 2 : 4;
    bool hasModRM = true;
    unsigned immediate = 0;
    switch (map) {
    case 1:
        hasModRM = isIn(hasModRMInFirstMap, opcode);
        if (opcode < 0x40)
            immediate = (opcode & 7) == 4 ? 1 : (opcode & 7) == 5 ? wordOrDoubleWord : 0;
        else if (opcode == 0x68 || opcode == 0x69 || opcode == 0x81 || opcode == 0xa9 || opcode == 0xc7)
            immediate = wordOrDoubleWord;
        else if (opcode == 0x6a || opcode == 0x6b || (opcode >= 0x70 && opcode <= 0x7f) || opcode == 0x80 || opcode == 0x83 || opcode == 0xa8 || (opcode >= 0xb0 && opcode <= 0xb7)
            || opcode == 0xc0 || opcode == 0xc1 || opcode == 0xc6 || opcode == 0xcd || (opcode >= 0xe0 && opcode <= 0xe7) || opcode == 0xeb)
            immediate = 1;
        else if (opcode >= 0xa0 && opcode <= 0xa3)
            immediate = 8;
        else if (opcode >= 0xb8 && opcode <= 0xbf)
            immediate = isWide ? 8 : wordOrDoubleWord;
        else if (opcode == 0xc2 || opcode == 0xca)
            immediate = 2;
        else if (opcode == 0xc8)
            immediate = 3;
        else if (opcode == 0xe8 || opcode == 0xe9) {
            if (hasOperandSizePrefix)
                return unknown;
            immediate = 4;
        }
        break;
    case 2:
        hasModRM = hasVex ? opcode != 0x77 : !isIn(hasNoModRMInSecondMap, opcode);
        if (!hasVex && opcode >= 0x80 && opcode <= 0x8f) {
            if (hasOperandSizePrefix)
                return unknown;
            immediate = 4;
        } else if ((opcode >= 0x70 && opcode <= 0x73) || opcode == 0xa4 || opcode == 0xac || opcode == 0xba || opcode == 0xc2 || (opcode >= 0xc4 && opcode <= 0xc6))
            immediate = 1;
        break;
    case 3:
        break;
    default:
        immediate = 1;
        break;
    }
    unsigned modRMOffset = 0;
    if (hasModRM) {
        modRMOffset = at;
        if (at + 1 > available)
            return unknown;
        uint8_t modRM = bytes[at++];
        unsigned mode = modRM >> 6;
        if (mode != 3) {
            unsigned displacement = mode == 1 ? 1 : mode == 2 ? 4 : 0;
            if ((modRM & 7) == 4) {
                if (at + 1 > available)
                    return unknown;
                if (!mode && (bytes[at] & 7) == 5)
                    displacement = 4;
                ++at;
            } else if (!mode && (modRM & 7) == 5)
                displacement = 4;
            at += displacement;
        }
        if (map == 1 && !hasVex && (opcode == 0xf6 || opcode == 0xf7) && (modRM >> 3 & 7) < 2)
            immediate = opcode == 0xf6 ? 1 : wordOrDoubleWord;
    }
    at += immediate;
    if (at > available || modRMOffset > 7)
        return unknown;
    return { static_cast<uint8_t>(at), static_cast<uint8_t>(modRMOffset) };
}

struct X86OpcodeTraits {
    static constexpr uint8_t immediateMask = 7;
    static constexpr uint8_t hasModRM = 8;
    static constexpr uint8_t hasWideImmediate = 16;
    static constexpr uint8_t isFast = 32;

    static constexpr std::array<uint8_t, 256> makeFirstMap()
    {
        constexpr uint32_t isKnown[8] = { 0x02022b2b, 0x2a2b2b0b, 0xffff0000, 0xffff0308, 0x02ffabfb, 0xff00030a, 0x220a12ca, 0x84d00b00 };
        constexpr uint32_t hasModRMInFirstMap[8] = { 0x0f0f0f0f, 0x0f0f0f0f, 0x00000000, 0x00000a08, 0x0000ffff, 0x00000000, 0xff0f00c3, 0xc0c00000 };
        std::array<uint8_t, 256> result { };
        for (unsigned opcode = 0; opcode < 256; ++opcode) {
            if (!(isKnown[opcode >> 5] >> (opcode & 31) & 1))
                continue;
            if (opcode == 0x0f || opcode == 0xc4 || opcode == 0xc5 || opcode == 0xf6 || opcode == 0xf7 || (opcode >= 0xa0 && opcode <= 0xa3))
                continue;
            unsigned immediate = 0;
            uint8_t traits = isFast;
            if (opcode < 0x40)
                immediate = (opcode & 7) == 4 ? 1 : (opcode & 7) == 5 ? 4 : 0;
            else if (opcode == 0x68 || opcode == 0x69 || opcode == 0x81 || opcode == 0xa9 || opcode == 0xc7)
                immediate = 4;
            else if (opcode == 0x6a || opcode == 0x6b || (opcode >= 0x70 && opcode <= 0x7f) || opcode == 0x80 || opcode == 0x83 || opcode == 0xa8 || (opcode >= 0xb0 && opcode <= 0xb7)
                || opcode == 0xc0 || opcode == 0xc1 || opcode == 0xc6 || opcode == 0xcd || (opcode >= 0xe0 && opcode <= 0xe7) || opcode == 0xeb)
                immediate = 1;
            else if (opcode >= 0xb8 && opcode <= 0xbf) {
                immediate = 4;
                traits |= hasWideImmediate;
            } else if (opcode == 0xc2 || opcode == 0xca)
                immediate = 2;
            else if (opcode == 0xc8)
                immediate = 3;
            else if (opcode == 0xe8 || opcode == 0xe9)
                immediate = 4;
            if (hasModRMInFirstMap[opcode >> 5] >> (opcode & 31) & 1)
                traits |= hasModRM;
            result[opcode] = traits | immediate;
        }
        return result;
    }

    static constexpr std::array<uint8_t, 256> makeSecondMap()
    {
        constexpr uint32_t isKnown[8] = { 0x80770800, 0x00025700, 0xfff3ffff, 0x40ffffff, 0xffffffff, 0xf5cbc00c, 0x7fbeff77, 0x7f3eff7f };
        constexpr uint32_t hasNoModRMInSecondMap[8] = { 0x00004be0, 0x00ff0000, 0x00000000, 0x00800000, 0x0000ffff, 0x00000707, 0x0000ff00, 0x00000000 };
        std::array<uint8_t, 256> result { };
        for (unsigned opcode = 0; opcode < 256; ++opcode) {
            if (!(isKnown[opcode >> 5] >> (opcode & 31) & 1))
                continue;
            if (opcode == 0x38 || opcode == 0x3a)
                continue;
            unsigned immediate = 0;
            if (opcode >= 0x80 && opcode <= 0x8f)
                immediate = 4;
            else if ((opcode >= 0x70 && opcode <= 0x73) || opcode == 0xa4 || opcode == 0xac || opcode == 0xba || opcode == 0xc2 || (opcode >= 0xc4 && opcode <= 0xc6))
                immediate = 1;
            uint8_t traits = isFast;
            if (!(hasNoModRMInSecondMap[opcode >> 5] >> (opcode & 31) & 1))
                traits |= hasModRM;
            result[opcode] = traits | immediate;
        }
        return result;
    }

    static constexpr std::array<uint8_t, 256> makeBytesAfterModRM()
    {
        std::array<uint8_t, 256> result { };
        for (unsigned modRM = 0; modRM < 0xc0; ++modRM) {
            unsigned mode = modRM >> 6;
            unsigned bytes = mode == 1 ? 1 : mode == 2 ? 4 : 0;
            if ((modRM & 7) == 4)
                ++bytes;
            else if (!mode && (modRM & 7) == 5)
                bytes = 4;
            result[modRM] = bytes;
        }
        return result;
    }
};

inline DecodedInstruction decodeInstruction(const uint8_t* bytes, size_t available)
{
    static constexpr std::array<uint8_t, 256> firstMap = X86OpcodeTraits::makeFirstMap();
    static constexpr std::array<uint8_t, 256> secondMap = X86OpcodeTraits::makeSecondMap();
    static constexpr std::array<uint8_t, 256> bytesAfterModRM = X86OpcodeTraits::makeBytesAfterModRM();

    if (available < 15) [[unlikely]]
        return decodeInstructionSlow(bytes, available);
    unsigned at = 0;
    bool isWide = false;
    if ((bytes[0] & 0xf0) == 0x40)
        isWide = bytes[at++] & 8;
    uint8_t traits = firstMap[bytes[at]];
    if (bytes[at] == 0x0f)
        traits = secondMap[bytes[++at]];
    if (!(traits & X86OpcodeTraits::isFast)) [[unlikely]]
        return decodeInstructionSlow(bytes, available);
    ++at;
    unsigned modRMOffset = 0;
    if (traits & X86OpcodeTraits::hasModRM) {
        modRMOffset = at;
        uint8_t modRM = bytes[at++];
        at += bytesAfterModRM[modRM];
        if (modRM < 0x40 && (modRM & 7) == 4 && (bytes[modRMOffset + 1] & 7) == 5)
            at += 4;
    }
    at += traits & X86OpcodeTraits::immediateMask;
    if (isWide && (traits & X86OpcodeTraits::hasWideImmediate))
        at += 4;
    return { static_cast<uint8_t>(at), static_cast<uint8_t>(modRMOffset) };
}

inline Vector<uint8_t> instructionRecords(std::span<const uint8_t> bytes, std::span<const DataInCode> data)
{
    Vector<uint8_t> records;
    size_t size = bytes.size();
    records.reserveInitialCapacity(size / 3 + 8);
    size_t range = 0;
    for (size_t at = 0; at < size;) {
        size_t endOfInstructions = range < data.size() ? data[range].offset : size;
        if (at == endOfInstructions) {
            for (unsigned i = data[range].numberOfWords; i--;)
                records.append(static_cast<uint8_t>(0x80 | sizeof(uint32_t)));
            at += data[range++].numberOfWords * sizeof(uint32_t);
            continue;
        }
        DecodedInstruction decoded = at < endOfInstructions && endOfInstructions <= size ? decodeInstruction(bytes.data() + at, endOfInstructions - at) : DecodedInstruction { 0, 0 };
        if (!decoded.length)
            return { };
        records.append(static_cast<uint8_t>(decoded.length | decoded.modRMOffset << 4));
        at += decoded.length;
    }
    return records;
}
#endif

#define FOR_EACH_WALK_FAILURE(macro) \
    macro(None, "it can be walked") \
    macro(NoRecords, "no lengths were kept") \
    macro(LengthsDoNotSum, "the lengths do not sum to the bytes") \
    macro(StubCallIsNotAnInstruction, "a stub call is opaque, lies in data or is not as long as a near call") \
    macro(DataIsNotInWords, "data does not lie in records of four bytes") \
    macro(TableEntryLeadsOut, "an entry of a table leads out of the function") \
    macro(UnknownInstruction, "an instruction is not known") \
    macro(ModRMOffsetIsWrong, "the ModRM byte does not follow the opcode") \
    macro(DirectCallIsNotStubCall, "a direct call is no stub call") \
    macro(AbsoluteAddress, "a move from or to an absolute address") \
    macro(JumpWithAPrefix, "a jump has a prefix or a REX ahead") \
    macro(TargetOutsideFunction, "a target lies out of the function") \
    macro(PCRelativeOperand, "an operand relative to the pc is no address of a range") \
    macro(AddressOutsideRanges, "an address relative to the pc leads to no start of a range") \
    macro(KeyOnABranch, "a faulting access or an index reference is a branch") \
    macro(UnconsumedStubCalls, "a stub call is at no start, or the list is not sorted") \
    macro(UnconsumedFaultingAccesses, "a faulting access is at no start, or the list is not sorted") \
    macro(UnconsumedIndexReferences, "an index reference is at no start, or the list is not sorted") \
    macro(DataIsNotUsedUp, "a range of data lies behind the end")

enum class WalkFailure : uint8_t {
#define AOT_WALK_FAILURE(name, words) name,
    FOR_EACH_WALK_FAILURE(AOT_WALK_FAILURE)
#undef AOT_WALK_FAILURE
};

static constexpr ASCIILiteral walkFailureMessages[] = {
#define AOT_WALK_FAILURE(name, words) words ## _s,
    FOR_EACH_WALK_FAILURE(AOT_WALK_FAILURE)
#undef AOT_WALK_FAILURE
};
static constexpr unsigned numberOfWalkFailures = std::size(walkFailureMessages);

namespace InstructionsInCodeInternal {

using Kind = InstructionInCode::Kind;
using Cause = WalkFailure;

inline int32_t readInt32(const uint8_t* at)
{
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

inline void writeInt32(uint8_t* at, int64_t value)
{
    RELEASE_ASSERT(value == static_cast<int32_t>(value));
    int32_t narrow = static_cast<int32_t>(value);
    memcpy(at, &narrow, sizeof(narrow));
}

#if CPU(X86_64)

inline constexpr bool regFieldExtendsOpcode(unsigned map, uint8_t opcode, bool hasVex)
{
    if (hasVex)
        return map == 2 && opcode >= 0x71 && opcode <= 0x73;
    if (map == 1) {
        switch (opcode) {
        case 0x80: case 0x81: case 0x83: case 0x8f: case 0xc0: case 0xc1: case 0xc6: case 0xc7:
        case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xf6: case 0xf7: case 0xfe: case 0xff:
            return true;
        default:
            return opcode >= 0xd8 && opcode <= 0xdf;
        }
    }
    if (map == 2) {
        switch (opcode) {
        case 0x00: case 0x01: case 0x18: case 0x1f: case 0x71: case 0x72: case 0x73: case 0xae: case 0xba: case 0xc7:
            return true;
        default:
            return false;
        }
    }
    return false;
}

inline constexpr bool isLegacyPrefix(uint8_t byte)
{
    switch (byte) {
    case 0x66: case 0xf2: case 0xf3: case 0xf0: case 0x2e: case 0x36: case 0x3e: case 0x26: case 0x64: case 0x65: case 0x67:
        return true;
    default:
        return false;
    }
}

inline Cause classifySlow(const uint8_t* bytes, InstructionInCode& instruction, unsigned modRMOffset)
{
    constexpr unsigned stackPointer = 4;
    unsigned length = instruction.length;
    unsigned at = 0;
    while (at < length && isLegacyPrefix(bytes[at]))
        ++at;
    if (at >= length)
        return Cause::UnknownInstruction;
    unsigned r = 0;
    unsigned b = 0;
    unsigned map = 1;
    bool hasVex = false;
    if ((bytes[at] == 0xc4 || bytes[at] == 0xc5) && modRMOffset) {
        hasVex = true;
        if (at + 3 >= length)
            return Cause::UnknownInstruction;
        r = ~bytes[at + 1] >> 7 & 1;
        if (bytes[at] == 0xc5) {
            map = 2;
            at += 2;
        } else {
            b = ~bytes[at + 1] >> 5 & 1;
            unsigned encodedMap = bytes[at + 1] & 31;
            if (!encodedMap || encodedMap > 3)
                return Cause::UnknownInstruction;
            map = encodedMap + 1;
            at += 3;
        }
    } else {
        if ((bytes[at] & 0xf0) == 0x40) {
            r = bytes[at] >> 2 & 1;
            b = bytes[at] & 1;
            ++at;
        }
        if (at < length && bytes[at] == 0x0f) {
            map = 2;
            ++at;
            if (at < length && (bytes[at] == 0x38 || bytes[at] == 0x3a))
                map = bytes[at++] == 0x38 ? 3 : 4;
        }
    }
    if (at >= length)
        return Cause::UnknownInstruction;
    uint8_t opcode = bytes[at];
    auto jumps = [&](Kind kind, unsigned width) {
        if (at != (map == 2 ? 1u : 0u) || at + 1 + width != length)
            return Cause::JumpWithAPrefix;
        int64_t distance = width == 1 ? static_cast<int8_t>(bytes[length - 1]) : readInt32(bytes + length - 4);
        int64_t target = static_cast<int64_t>(instruction.offset) + length + distance;
        if (target < 0 || target > std::numeric_limits<uint32_t>::max())
            return Cause::TargetOutsideFunction;
        instruction.kind = kind;
        instruction.target = static_cast<uint32_t>(target);
        return Cause::None;
    };
    bool namesStackPointer = false;
    if (map == 1 && !hasVex) {
        if (opcode == 0xe8)
            return Cause::DirectCallIsNotStubCall;
        if (opcode >= 0xa0 && opcode <= 0xa3)
            return Cause::AbsoluteAddress;
        if (opcode == 0xe9)
            return jumps(Kind::Jump, 4);
        if (opcode == 0xeb)
            return jumps(Kind::Jump, 1);
        if (opcode >= 0x70 && opcode <= 0x7f)
            return jumps(Kind::ConditionalJump, 1);
        if (opcode == 0xc3 || opcode == 0xc2 || opcode == 0xcc || opcode == 0xf4 || opcode == 0xfa) {
            instruction.kind = Kind::End;
            return Cause::None;
        }
        if ((opcode >= 0x50 && opcode <= 0x5f) || opcode == 0x68 || opcode == 0x6a || opcode == 0x9c || opcode == 0x9d || opcode == 0xc9)
            namesStackPointer = true;
        else if ((opcode >= 0xb0 && opcode <= 0xbf) || (opcode >= 0x91 && opcode <= 0x97))
            namesStackPointer = ((opcode & 7) | b << 3) == stackPointer;
    } else if (map == 2 && !hasVex) {
        if (opcode >= 0x80 && opcode <= 0x8f)
            return jumps(Kind::ConditionalJump, 4);
        if (opcode == 0x0b) {
            instruction.kind = Kind::End;
            return Cause::None;
        }
        if (opcode >= 0xc8 && opcode <= 0xcf)
            namesStackPointer = ((opcode & 7) | b << 3) == stackPointer;
    }
    if (modRMOffset) {
        if (modRMOffset != at + 1 || modRMOffset >= length)
            return Cause::ModRMOffsetIsWrong;
        uint8_t modRM = bytes[modRMOffset];
        unsigned mode = modRM >> 6;
        unsigned extension = modRM >> 3 & 7;
        if (regFieldExtendsOpcode(map, opcode, hasVex)) {
            if (map == 1 && !hasVex && opcode == 0xff) {
                if (extension == 4 || extension == 5) {
                    instruction.kind = Kind::End;
                    return Cause::None;
                }
                if (extension == 2 || extension == 3 || extension == 6)
                    namesStackPointer = true;
            }
            if (map == 1 && !hasVex && opcode == 0x8f)
                namesStackPointer = true;
        } else if ((extension | r << 3) == stackPointer)
            namesStackPointer = true;
        if (mode == 3)
            namesStackPointer |= ((modRM & 7) | b << 3) == stackPointer;
        else if ((modRM & 7) == 4) {
            if (modRMOffset + 1 >= length)
                return Cause::UnknownInstruction;
            uint8_t sib = bytes[modRMOffset + 1];
            if (!(!mode && (sib & 7) == 5))
                namesStackPointer |= ((sib & 7) | b << 3) == stackPointer;
        } else if (!mode && (modRM & 7) == 5)
            return Cause::PCRelativeOperand;
    }
    instruction.kind = namesStackPointer ? Kind::Pinned : Kind::Plain;
    return Cause::None;
}

struct X86OpcodeClasses {
    static constexpr uint8_t isFast = 1;
    static constexpr uint8_t isEnd = 2;
    static constexpr uint8_t namesStackPointer = 4;
    static constexpr uint8_t hasRegisterInOpcode = 8;
    static constexpr uint8_t regFieldExtendsOpcode = 16;
    static constexpr uint8_t isJump = 32;
    static constexpr uint8_t isConditionalJump = 64;
    static constexpr uint8_t hasWideDistance = 128;

    static constexpr std::array<uint8_t, 256> makeFirstMap()
    {
        std::array<uint8_t, 256> result { };
        for (unsigned opcode = 0; opcode < 256; ++opcode) {
            if (isLegacyPrefix(opcode) || (opcode & 0xf0) == 0x40 || opcode == 0x0f || opcode == 0xc4 || opcode == 0xc5)
                continue;
            if (opcode == 0xe8 || (opcode >= 0xa0 && opcode <= 0xa3) || opcode == 0xff || opcode == 0x8f)
                continue;
            uint8_t classes = isFast;
            if (opcode == 0xe9)
                classes |= isJump | hasWideDistance;
            else if (opcode == 0xeb)
                classes |= isJump;
            else if (opcode >= 0x70 && opcode <= 0x7f)
                classes |= isConditionalJump;
            else if (opcode == 0xc3 || opcode == 0xc2 || opcode == 0xcc || opcode == 0xf4 || opcode == 0xfa)
                classes |= isEnd;
            else if ((opcode >= 0x50 && opcode <= 0x5f) || opcode == 0x68 || opcode == 0x6a || opcode == 0x9c || opcode == 0x9d || opcode == 0xc9)
                classes |= namesStackPointer;
            else if ((opcode >= 0xb0 && opcode <= 0xbf) || (opcode >= 0x91 && opcode <= 0x97))
                classes |= hasRegisterInOpcode;
            if (InstructionsInCodeInternal::regFieldExtendsOpcode(1, opcode, false))
                classes |= regFieldExtendsOpcode;
            result[opcode] = classes;
        }
        return result;
    }

    static constexpr std::array<uint8_t, 256> makeSecondMap()
    {
        std::array<uint8_t, 256> result { };
        for (unsigned opcode = 0; opcode < 256; ++opcode) {
            if (opcode == 0x38 || opcode == 0x3a)
                continue;
            uint8_t classes = isFast;
            if (opcode >= 0x80 && opcode <= 0x8f)
                classes |= isConditionalJump | hasWideDistance;
            else if (opcode == 0x0b)
                classes |= isEnd;
            else if (opcode >= 0xc8 && opcode <= 0xcf)
                classes |= hasRegisterInOpcode;
            if (InstructionsInCodeInternal::regFieldExtendsOpcode(2, opcode, false))
                classes |= regFieldExtendsOpcode;
            result[opcode] = classes;
        }
        return result;
    }
};

inline Cause classify(const uint8_t* bytes, InstructionInCode& instruction, unsigned modRMOffset)
{
    static constexpr std::array<uint8_t, 256> firstMap = X86OpcodeClasses::makeFirstMap();
    static constexpr std::array<uint8_t, 256> secondMap = X86OpcodeClasses::makeSecondMap();
    constexpr unsigned stackPointer = 4;

    unsigned length = instruction.length;
    unsigned at = 0;
    unsigned r = 0;
    unsigned b = 0;
    if ((bytes[0] & 0xf0) == 0x40) {
        r = bytes[0] >> 2 & 1;
        b = bytes[0] & 1;
        at = 1;
    }
    unsigned firstOpcodeByte = at;
    uint8_t classes = 0;
    if (at < length) [[likely]] {
        classes = firstMap[bytes[at]];
        if (bytes[at] == 0x0f && at + 1 < length)
            classes = secondMap[bytes[++at]];
    }
    if (!(classes & X86OpcodeClasses::isFast)) [[unlikely]]
        return classifySlow(bytes, instruction, modRMOffset);
    uint8_t opcode = bytes[at];
    if (classes & (X86OpcodeClasses::isJump | X86OpcodeClasses::isConditionalJump | X86OpcodeClasses::isEnd)) {
        if (classes & X86OpcodeClasses::isEnd) {
            instruction.kind = Kind::End;
            return Cause::None;
        }
        unsigned width = classes & X86OpcodeClasses::hasWideDistance ? 4 : 1;
        if (firstOpcodeByte || at + 1 + width != length)
            return Cause::JumpWithAPrefix;
        int64_t distance = width == 1 ? static_cast<int8_t>(bytes[length - 1]) : readInt32(bytes + length - 4);
        int64_t target = static_cast<int64_t>(instruction.offset) + length + distance;
        if (target < 0 || target > std::numeric_limits<uint32_t>::max())
            return Cause::TargetOutsideFunction;
        instruction.kind = classes & X86OpcodeClasses::isJump ? Kind::Jump : Kind::ConditionalJump;
        instruction.target = static_cast<uint32_t>(target);
        return Cause::None;
    }
    bool namesStackPointer = classes & X86OpcodeClasses::namesStackPointer;
    if (classes & X86OpcodeClasses::hasRegisterInOpcode)
        namesStackPointer = ((opcode & 7) | b << 3) == stackPointer;
    if (modRMOffset) {
        if (modRMOffset != at + 1 || modRMOffset >= length)
            return Cause::ModRMOffsetIsWrong;
        uint8_t modRM = bytes[modRMOffset];
        unsigned mode = modRM >> 6;
        if (!(classes & X86OpcodeClasses::regFieldExtendsOpcode) && ((modRM >> 3 & 7) | r << 3) == stackPointer)
            namesStackPointer = true;
        if (mode == 3)
            namesStackPointer |= ((modRM & 7) | b << 3) == stackPointer;
        else if ((modRM & 7) == 4) {
            if (modRMOffset + 1 >= length)
                return Cause::UnknownInstruction;
            uint8_t sib = bytes[modRMOffset + 1];
            if (!(!mode && (sib & 7) == 5))
                namesStackPointer |= ((sib & 7) | b << 3) == stackPointer;
        } else if (!mode && (modRM & 7) == 5)
            return Cause::PCRelativeOperand;
    }
    instruction.kind = namesStackPointer ? Kind::Pinned : Kind::Plain;
    return Cause::None;
}

#elif CPU(ARM64)

inline bool namesLinkRegister(uint32_t word)
{
    constexpr unsigned linkRegister = 30;
    unsigned d = word & 31;
    unsigned n = word >> 5 & 31;
    unsigned a = word >> 10 & 31;
    unsigned m = word >> 16 & 31;
    if ((word & 0x1f800000) == 0x12800000)
        return d == linkRegister;
    if ((word & 0x1f000000) == 0x11000000 || (word & 0x1f800000) == 0x12000000 || (word & 0x1f800000) == 0x13000000)
        return d == linkRegister || n == linkRegister;
    bool isVector = word >> 26 & 1;
    if ((word & 0x3b000000) == 0x39000000 || (word & 0x3b200000) == 0x38000000)
        return (!isVector && d == linkRegister) || n == linkRegister;
    if ((word & 0x3a000000) == 0x28000000)
        return (!isVector && (d == linkRegister || a == linkRegister)) || n == linkRegister;
    if ((word & 0x3b200c00) == 0x38200800)
        return (!isVector && d == linkRegister) || n == linkRegister || m == linkRegister;
    if ((word & 0x5e000000) == 0x1e000000)
        return d == linkRegister || n == linkRegister;
    return d == linkRegister || n == linkRegister || a == linkRegister || m == linkRegister;
}

inline Cause classify(const uint8_t* bytes, InstructionInCode& instruction)
{
    uint32_t word = static_cast<uint32_t>(readInt32(bytes));
    auto jumps = [&](Kind kind, unsigned shift, unsigned width) {
        int64_t distance = static_cast<int64_t>(static_cast<uint64_t>(word >> shift) << (64 - width)) >> (64 - width);
        int64_t target = static_cast<int64_t>(instruction.offset) + distance * 4;
        if (target < 0 || target > std::numeric_limits<uint32_t>::max())
            return Cause::TargetOutsideFunction;
        instruction.kind = kind;
        instruction.target = static_cast<uint32_t>(target);
        return Cause::None;
    };
    if ((word & 0xfc000000) == 0x14000000)
        return jumps(Kind::Jump, 0, 26);
    if ((word & 0xfc000000) == 0x94000000)
        return Cause::DirectCallIsNotStubCall;
    if ((word & 0xff000010) == 0x54000000 || (word & 0x7e000000) == 0x34000000)
        return jumps(Kind::ConditionalJump, 5, 19);
    if ((word & 0x7e000000) == 0x36000000)
        return jumps(Kind::ConditionalJump, 5, 14);
    if ((word & 0x9f000000) == 0x10000000) {
        int64_t distance = static_cast<int64_t>(static_cast<uint64_t>((word >> 5 & 0x7ffff) << 2 | (word >> 29 & 3)) << 43) >> 43;
        int64_t target = static_cast<int64_t>(instruction.offset) + distance;
        if (target < 0 || target > std::numeric_limits<uint32_t>::max())
            return Cause::TargetOutsideFunction;
        instruction.kind = Kind::TableAddress;
        instruction.target = static_cast<uint32_t>(target);
        return Cause::None;
    }
    if ((word & 0x9f000000) == 0x90000000 || (word & 0x3b000000) == 0x18000000)
        return Cause::PCRelativeOperand;
    if ((word & 0xfffffc1f) == 0xd65f0000 || (word & 0xfffffc1f) == 0xd61f0000 || (word & 0xff000000) == 0xd4000000 || !(word & 0xffff0000)) {
        instruction.kind = Kind::End;
        return Cause::None;
    }
    if ((word & 0xfe000000) == 0xd6000000) {
        instruction.kind = Kind::Pinned;
        return Cause::None;
    }
    instruction.kind = namesLinkRegister(word) ? Kind::Pinned : Kind::Plain;
    return Cause::None;
}

#endif

template<typename Code, typename Functor>
Cause tryDecode(const Code& code, const Functor& functor)
{
    const uint8_t* bytes = code.bytes.span().data();
    size_t size = code.bytes.size();
    auto& calls = code.info.stubCalls;
    auto& faults = code.info.faultingAccesses;
    auto& references = code.info.indexRelocations;
    auto& keptInline = code.info.rangesKeptInline;
    size_t nextKeptInline = 0;
    auto& ranges = code.dataInCode;
    size_t call = 0;
    size_t faultIndex = 0;
    size_t reference = 0;
    size_t range = 0;
    size_t at = 0;
    constexpr size_t never = std::numeric_limits<size_t>::max();
    size_t nextCall = calls.isEmpty() ? never : calls[0].offset;
    size_t nextFault = faults.isEmpty() ? never : faults[0].codeOffset;
    size_t nextReference = references.isEmpty() ? never : references[0].offset;
    const bool hasRanges = !ranges.isEmpty();
    auto isStartOfData = [&](uint32_t offset) {
        auto found = std::ranges::lower_bound(ranges, offset, { }, [](auto& data) {
            return data.offset;
        });
        return found != ranges.end() && found->offset == offset;
    };
    auto step = [&](InstructionInCode& instruction, bool isOpaque, unsigned modRMOffset) {
        UNUSED_PARAM(modRMOffset);
        bool isInData = false;
        if (hasRanges) [[unlikely]] {
            while (range < ranges.size() && ranges[range].offset + ranges[range].numberOfWords * sizeof(uint32_t) <= at)
                ++range;
            isInData = range < ranges.size() && ranges[range].offset <= at;
        }
        if (nextCall == at) {
            if (isOpaque || isInData || instruction.length != sizeOfNearCall)
                return Cause::StubCallIsNotAnInstruction;
            instruction.kind = Kind::StubCall;
            instruction.target = static_cast<uint32_t>(call++);
            nextCall = call < calls.size() ? calls[call].offset : never;
        } else if (isInData) {
            if (instruction.length != sizeof(uint32_t) || (at - ranges[range].offset) % sizeof(uint32_t))
                return Cause::DataIsNotInWords;
            instruction.kind = Kind::Pinned;
            if (ranges[range].isTable) {
                int64_t target = static_cast<int64_t>(ranges[range].offset) + readInt32(bytes + at);
                if (target < 0 || target > static_cast<int64_t>(size))
                    return Cause::TableEntryLeadsOut;
                instruction.kind = Kind::TableEntry;
                instruction.target = static_cast<uint32_t>(target);
            }
        } else if (isOpaque)
            instruction.kind = Kind::Pinned;
        else {
#if CPU(X86_64)
            if (Cause cause = classify(bytes + at, instruction, modRMOffset); cause != Cause::None)
                return cause;
#else
            if (Cause cause = classify(bytes + at, instruction); cause != Cause::None)
                return cause;
#endif
            if ((instruction.kind == Kind::Jump || instruction.kind == Kind::ConditionalJump) && instruction.target > size)
                return Cause::TargetOutsideFunction;
            if (instruction.kind == Kind::TableAddress && !isStartOfData(instruction.target))
                return Cause::AddressOutsideRanges;
        }
        bool isKey = false;
        if (nextFault == at) {
            ++faultIndex;
            nextFault = faultIndex < faults.size() ? faults[faultIndex].codeOffset : never;
            isKey = true;
        }
#if CPU(X86_64)
        if (nextReference == at + instruction.length) {
            ++reference;
            nextReference = reference < references.size() ? references[reference].offset : never;
            isKey = true;
        }
#else
        if (nextReference != never && nextReference + sizeof(uint32_t) == at) {
            ++reference;
            nextReference = reference < references.size() ? references[reference].offset : never;
            isKey = true;
        } else if (nextReference == at)
            isKey = true;
#endif
        if (isKey && instruction.kind != Kind::End) {
            if (instruction.kind != Kind::Plain && instruction.kind != Kind::Pinned && instruction.kind != Kind::StubCall)
                return Cause::KeyOnABranch;
            instruction.kind = Kind::Pinned;
        }
        while (nextKeptInline < keptInline.size() && keptInline[nextKeptInline].second <= at)
            ++nextKeptInline;
        if (nextKeptInline < keptInline.size() && keptInline[nextKeptInline].first <= at && instruction.kind == Kind::Plain)
            instruction.kind = Kind::Pinned;
#if CPU(ARM64)
        if (at >= code.appendedEntriesStart && instruction.kind == Kind::Plain)
            instruction.kind = Kind::Pinned;
#endif
        functor(static_cast<const InstructionInCode&>(instruction));
        at += instruction.length;
        return Cause::None;
    };
#if CPU(X86_64)
    if (code.instructions.isEmpty())
        return Cause::NoRecords;
    for (uint8_t record : code.instructions) {
        InstructionInCode instruction { static_cast<uint32_t>(at), 0, static_cast<uint8_t>(record & 15), Kind::Plain };
        if (!instruction.length || at + instruction.length > size)
            return Cause::LengthsDoNotSum;
        if (Cause cause = step(instruction, record & 0x80, record >> 4 & 7); cause != Cause::None)
            return cause;
    }
#else
    if (size % sizeof(uint32_t))
        return Cause::LengthsDoNotSum;
    while (at < size) {
        InstructionInCode instruction { static_cast<uint32_t>(at), 0, sizeof(uint32_t), Kind::Plain };
        if (Cause cause = step(instruction, false, 0); cause != Cause::None)
            return cause;
    }
#endif
    while (range < ranges.size() && ranges[range].offset + ranges[range].numberOfWords * sizeof(uint32_t) <= at)
        ++range;
    if (at != size)
        return Cause::LengthsDoNotSum;
    if (call != calls.size())
        return Cause::UnconsumedStubCalls;
    if (faultIndex != faults.size())
        return Cause::UnconsumedFaultingAccesses;
    if (reference != references.size())
        return Cause::UnconsumedIndexReferences;
    if (range != ranges.size())
        return Cause::DataIsNotUsedUp;
    return Cause::None;
}

template<typename Code, typename Functor>
bool walk(const Code& code, const Functor& functor)
{
    return tryDecode(code, functor) == Cause::None;
}

} // namespace InstructionsInCodeInternal

inline bool canBeWalked(const CompiledCode& code)
{
    return code.hasCodeAnnotations && !code.isRewritten && InstructionsInCodeInternal::walk(code, [](const InstructionInCode&) { });
}

template<typename Functor>
void forEachInstructionInCode(const CompiledCode& code, const Functor& functor)
{
    RELEASE_ASSERT(code.hasCodeAnnotations && !code.isRewritten);
    bool wasWalked = InstructionsInCodeInternal::walk(code, functor);
    RELEASE_ASSERT(wasWalked);
}

inline void setTarget(std::span<uint8_t> newBytes, const InstructionInCode& instruction, uint32_t newOffset, uint32_t newTarget, uint32_t newStartOfTable)
{
    using namespace InstructionsInCodeInternal;
    RELEASE_ASSERT(static_cast<size_t>(newOffset) + instruction.length <= newBytes.size());
    uint8_t* at = newBytes.data() + newOffset;
    if (instruction.kind == Kind::TableEntry) {
        writeInt32(at, static_cast<int64_t>(newTarget) - newStartOfTable);
        return;
    }
#if CPU(X86_64)
    int64_t distance = static_cast<int64_t>(newTarget) - (static_cast<int64_t>(newOffset) + instruction.length);
    switch (instruction.kind) {
    case Kind::Jump:
    case Kind::ConditionalJump:
        if (instruction.length == 2) {
            RELEASE_ASSERT(distance == static_cast<int8_t>(distance));
            at[1] = static_cast<uint8_t>(static_cast<int8_t>(distance));
            return;
        }
        writeInt32(at + instruction.length - sizeof(int32_t), distance);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
#else
    int64_t distance = static_cast<int64_t>(newTarget) - newOffset;
    uint32_t word = static_cast<uint32_t>(readInt32(at));
    auto set = [&](unsigned shift, unsigned width) {
        RELEASE_ASSERT(!(distance & 3) && distance >> 2 >= -(1ll << (width - 1)) && distance >> 2 < (1ll << (width - 1)));
        uint32_t mask = ((1u << width) - 1) << shift;
        word = (word & ~mask) | (static_cast<uint32_t>(distance >> 2) << shift & mask);
    };
    switch (instruction.kind) {
    case Kind::Jump:
        set(0, 26);
        break;
    case Kind::ConditionalJump:
        set(5, (word & 0x7e000000) == 0x36000000 ? 14 : 19);
        break;
    case Kind::TableAddress:
        RELEASE_ASSERT(distance >= -(1ll << 20) && distance < (1ll << 20));
        word = (word & 0x9f00001f) | (static_cast<uint32_t>(distance) & 3) << 29 | (static_cast<uint32_t>(distance >> 2) & 0x7ffff) << 5;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    memcpy(at, &word, sizeof(word));
#endif
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
