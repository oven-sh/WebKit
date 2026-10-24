/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompactList.h"
#include "AOTRelocation.h"
#include "AOTVector.h"
#include "AirInstObserver.h"
#include "B3Origin.h"
#include "MacroAssembler.h"
#include "RegisterSet.h"
#include <span>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

struct CompiledCode;

struct CodeUnit {
    enum class Kind : uint8_t {
        Pinned,
        Plain,
        // One x86-64 instruction with an operand at a displacement off the stack pointer, which names the stack pointer in no other way.
        PlainOffStackPointer,
        Call,
        End,
    };

    bool isPlain() const { return kind == Kind::Plain || kind == Kind::PlainOffStackPointer; }

    uint32_t offset;
    uint16_t length;
    Kind kind;
    // Lies in a block that is cold by an estimate only: a path without a rare edge leads there, and the code may run more than once.
    bool mayRun { false };
};
static_assert(sizeof(CodeUnit) == 8);

// One byte per unit: the length in the low four bits, then the kind, then mayRun.
class PackedCodeUnits {
public:
    class Iterator {
    public:
        Iterator(const uint8_t* byte, uint32_t offset)
            : m_byte(byte)
            , m_offset(offset)
        {
        }

        CodeUnit operator*() const { return { m_offset, length(), static_cast<CodeUnit::Kind>(*m_byte >> kindShift & kindMask), !!(*m_byte & mayRunBit) }; }
        Iterator& operator++()
        {
            m_offset += length();
            m_byte += *m_byte & shortLengthMask ? 1 : 3;
            return *this;
        }
        friend bool operator==(const Iterator& a, const Iterator& b) { return a.m_byte == b.m_byte; }

    private:
        uint16_t length() const { return static_cast<uint16_t>(*m_byte & shortLengthMask ? *m_byte & shortLengthMask : m_byte[1] | m_byte[2] << 8); }

        const uint8_t* m_byte;
        uint32_t m_offset;
    };

    PackedCodeUnits() = default;
    PackedCodeUnits(std::span<const CodeUnit>);
    PackedCodeUnits(const Vector<CodeUnit>& units)
        : PackedCodeUnits(units.span())
    {
    }

    bool isEmpty() const { return !m_size; }
    size_t size() const { return m_size; }
    uint32_t codeSize() const { return m_codeSize; }
    Iterator begin() const LIFETIME_BOUND { return { m_bytes.begin(), 0 }; }
    Iterator end() const LIFETIME_BOUND { return { m_bytes.end(), m_codeSize }; }
    CodeUnit first() const { return *begin(); }
    CodeUnit last() const;
    Vector<CodeUnit> unpack() const;

    void append(const CodeUnit&);
    void prepend(std::span<const CodeUnit>);
    // The unit that newCodeSize cuts is Pinned.
    void shrinkCodeSize(uint32_t newCodeSize);
    void clear() { *this = { }; }

private:
    static constexpr uint8_t shortLengthMask = 0xf;
    static constexpr unsigned kindShift = 4;
    static constexpr uint8_t kindMask = 0x7;
    static constexpr uint8_t mayRunBit = 0x80;

    Typed::CompactList<uint8_t> m_bytes;
    uint32_t m_size { 0 };
    uint32_t m_codeSize { 0 };
};
static_assert(sizeof(PackedCodeUnits) == 16);

struct RecordedTrap {
    MacroAssembler::Label instruction;
    B3::Origin origin;
};

class CodeUnitRecorder final : public B3::Air::InstObserver {
public:
    // A cold block that is not rarely run is cold by an estimate only.
    void setColdBlocks(BitVector&& coldAirBlocks, BitVector&& rarelyRunAirBlocks);

    void willGenerate(const B3::Air::BasicBlock&) final;
    void didGenerate(const B3::Air::Inst&, MacroAssembler::Label begin, MacroAssembler::Label end) final;

    void willGenerateBlock(bool isCold, bool isRarelyRun);
    void didGenerateInBlock(MacroAssembler::Label begin, MacroAssembler::Label end, CodeUnit::Kind);

    void addPlain(MacroAssembler::Label begin, MacroAssembler::Label end, const RegisterSet& named);
    void addEnd(MacroAssembler::Label begin, MacroAssembler::Label end);
    void pin(MacroAssembler::Label begin, MacroAssembler::Label end);

    Vector<CodeUnit> link(ImageLinkBuffer&, std::span<const Relocation>);
    uint32_t sizeOfHotBlocks() const { return m_sizeOfHotBlocks; }
    Vector<RecordedTrap> takeTraps() { return std::exchange(m_traps, { }); }

private:
    struct Range {
        MacroAssembler::Label begin;
        MacroAssembler::Label end;
        CodeUnit::Kind kind;
    };
    enum class BlockState : uint8_t {
        NoBlock,
        StartsHotBlock,
        InHotBlock,
        InColdBlock,
        StartsColdBlockThatMayRun,
        InColdBlockThatMayRun,
    };

    void addRange(MacroAssembler::Label begin, MacroAssembler::Label end, CodeUnit::Kind);

    BitVector m_coldAirBlocks;
    BitVector m_rarelyRunAirBlocks;
    BlockState m_blockState { BlockState::NoBlock };
    Vector<std::pair<MacroAssembler::Label, MacroAssembler::Label>> m_hotBlockRanges;
    Vector<std::pair<MacroAssembler::Label, MacroAssembler::Label>> m_rangesOfColdBlocksThatMayRun;
    Vector<Range> m_ranges;
    WTF::Vector<MacroAssembler::Label> m_boundaries;
    Vector<std::pair<MacroAssembler::Label, MacroAssembler::Label>> m_pins;
    Vector<RecordedTrap> m_traps;
    uint32_t m_sizeOfHotBlocks { 0 };
};

enum class CodeUnitsError : uint8_t {
    None,
    UnitsDoNotTileCode,
    CallIsNotAUnit,
    SiteIsNotPinned,
    TargetIsNotABoundary,
    ReturnAddressIsNotABoundary,
    OffsetsDoNotAscend,
    OffsetOutsideCode,
};

CodeUnitsError verifyCodeUnits(const CompiledCode&);

// bytes are a copy of the code from firstOffset on that is reached by a near call: its units of Kind::PlainOffStackPointer address what they addressed in front of the call.
PackedCodeUnits::Iterator adjustForNearCall(std::span<uint8_t> bytes, PackedCodeUnits::Iterator from, PackedCodeUnits::Iterator end, uint32_t firstOffset);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
