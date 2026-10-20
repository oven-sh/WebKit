/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRelocation.h"
#include "AirInstObserver.h"
#include "B3Origin.h"
#include "MacroAssembler.h"
#include "RegisterSet.h"
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct CompiledCode;

struct CodeUnit {
    enum class Kind : uint8_t {
        Pinned,
        Plain,
        Call,
        End,
    };

    uint32_t offset;
    uint16_t length;
    Kind kind;
};
static_assert(sizeof(CodeUnit) == 8);

struct RecordedTrap {
    MacroAssembler::Label instruction;
    B3::Origin origin;
};

class CodeUnitRecorder final : public B3::Air::InstObserver {
public:
    void setColdBlocks(BitVector&& coldAirBlocks);

    void willGenerate(const B3::Air::BasicBlock&) final;
    void didGenerate(const B3::Air::Inst&, MacroAssembler::Label begin, MacroAssembler::Label end) final;

    void addPlain(MacroAssembler::Label begin, MacroAssembler::Label end, const RegisterSet& named);
    void addEnd(MacroAssembler::Label begin, MacroAssembler::Label end);
    void pin(MacroAssembler::Label begin, MacroAssembler::Label end);

    Vector<CodeUnit> link(ImageLinkBuffer&, std::span<const Relocation>);
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
    };

    void addRange(MacroAssembler::Label begin, MacroAssembler::Label end, CodeUnit::Kind);

    BitVector m_coldAirBlocks;
    BlockState m_blockState { BlockState::NoBlock };
    Vector<std::pair<MacroAssembler::Label, MacroAssembler::Label>> m_hotBlockRanges;
    Vector<Range> m_ranges;
    Vector<MacroAssembler::Label> m_boundaries;
    Vector<std::pair<MacroAssembler::Label, MacroAssembler::Label>> m_pins;
    Vector<RecordedTrap> m_traps;
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
