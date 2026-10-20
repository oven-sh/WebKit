/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeUnits.h"

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AirBasicBlock.h"
#include "AirInst.h"
#include "AirOpcodeUtils.h"
#include "B3Value.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT {

static constexpr uint32_t wordSize = sizeof(uint32_t);
static constexpr uint32_t maxUnitLength = std::numeric_limits<uint16_t>::max();
static constexpr uint16_t maxSplitPieceLength = maxUnitLength & ~(wordSize - 1);

namespace {

struct OffsetRange {
    uint32_t begin;
    uint32_t end;
    CodeUnit::Kind kind;
};

struct LinkedRanges {
    Vector<OffsetRange> recorded;
    Vector<OffsetRange> pins;
    Vector<OffsetRange> hotBlocks;
};

} // anonymous namespace

static constexpr GPRReg nearCallClobberedRegister()
{
#if CPU(X86_64)
    return MacroAssembler::stackPointerRegister;
#elif CPU(ARM64)
    return MacroAssembler::linkRegister;
#else
#error "Code units are defined for x86-64 and ARM64."
#endif
}

static CodeUnit::Kind kindOf(const B3::Air::Inst& inst)
{
    B3::Air::Opcode opcode = inst.kind.opcode;
    if (B3::Air::isReturn(opcode))
        return CodeUnit::Kind::End;
    switch (opcode) {
    case B3::Air::Oops:
        return CodeUnit::Kind::End;
    case B3::Air::Patch:
    case B3::Air::ColdCCall:
    case B3::Air::Shuffle:
    case B3::Air::EntrySwitch:
    case B3::Air::WasmBoundsCheck:
    case B3::Air::EnterFrame:
    case B3::Air::LeaveFrame:
        return CodeUnit::Kind::Pinned;
    default:
        break;
    }
    if (inst.kind.effects)
        return CodeUnit::Kind::Pinned;
    B3::Air::Tmp clobbered(nearCallClobberedRegister());
    for (const B3::Air::Arg& arg : inst.args()) {
        if (arg.usesTmp(clobbered))
            return CodeUnit::Kind::Pinned;
    }
    return CodeUnit::Kind::Plain;
}

// A fenced access on x86-64 has kind.effects too, and the address computation of a trapping access has the access as its origin.
static bool isTrappingAccess(const B3::Air::Inst& inst)
{
    return inst.kind.effects && inst.origin && inst.origin->traps();
}

void CodeUnitRecorder::setColdBlocks(BitVector&& coldAirBlocks)
{
    m_coldAirBlocks = WTF::move(coldAirBlocks);
}

void CodeUnitRecorder::willGenerate(const B3::Air::BasicBlock& block)
{
    m_blockState = m_coldAirBlocks.get(block.index()) ? BlockState::InColdBlock : BlockState::StartsHotBlock;
}

void CodeUnitRecorder::addRange(MacroAssembler::Label begin, MacroAssembler::Label end, CodeUnit::Kind kind)
{
    if (kind == CodeUnit::Kind::Pinned) {
        m_boundaries.append(begin);
        return;
    }
    m_ranges.append({ begin, end, kind });
}

void CodeUnitRecorder::didGenerate(const B3::Air::Inst& inst, MacroAssembler::Label begin, MacroAssembler::Label end)
{
    if (begin == end)
        return;

    switch (m_blockState) {
    case BlockState::NoBlock:
    case BlockState::StartsHotBlock:
        m_hotBlockRanges.append({ begin, end });
        m_blockState = BlockState::InHotBlock;
        break;
    case BlockState::InHotBlock:
        m_hotBlockRanges.last().second = end;
        break;
    case BlockState::InColdBlock:
        break;
    }

    if (isTrappingAccess(inst))
        m_traps.append({ begin, inst.origin ? inst.origin->origin() : B3::Origin() });
    addRange(begin, end, kindOf(inst));
}

void CodeUnitRecorder::addPlain(MacroAssembler::Label begin, MacroAssembler::Label end, const RegisterSet& named)
{
    addRange(begin, end, named.contains(nearCallClobberedRegister(), IgnoreVectors) ? CodeUnit::Kind::Pinned : CodeUnit::Kind::Plain);
}

void CodeUnitRecorder::addEnd(MacroAssembler::Label begin, MacroAssembler::Label end)
{
    addRange(begin, end, CodeUnit::Kind::End);
}

void CodeUnitRecorder::pin(MacroAssembler::Label begin, MacroAssembler::Label end)
{
    m_pins.append({ begin, end });
}

static CodeUnit exactUnitAt(uint32_t offset, RelocationKind relocationKind, CodeUnit::Kind kind)
{
    return { offset, safeCast<uint16_t>(MacroAssembler::relocationSize(relocationKind)), kind };
}

static Vector<CodeUnit> collectExactUnits(std::span<const Relocation> relocations, std::span<const LinkedSite> linkedSites, uint32_t size)
{
    Vector<CodeUnit> units;
    units.reserveInitialCapacity(relocations.size() + linkedSites.size());
    size_t nextRelocation = 0;
    size_t nextLinkedSite = 0;
    while (nextRelocation < relocations.size() || nextLinkedSite < linkedSites.size()) {
        bool relocationIsNext = nextLinkedSite == linkedSites.size()
            || (nextRelocation < relocations.size() && relocations[nextRelocation].offset < linkedSites[nextLinkedSite].offset);
        if (relocationIsNext) {
            const Relocation& relocation = relocations[nextRelocation++];
            units.append(exactUnitAt(relocation.offset, relocation.kind, relocation.kind == RelocationKind::Call ? CodeUnit::Kind::Call : CodeUnit::Kind::Pinned));
            continue;
        }
        const LinkedSite& site = linkedSites[nextLinkedSite++];
        units.append(exactUnitAt(site.offset, site.kind, CodeUnit::Kind::Pinned));
    }

    uint64_t end = 0;
    for (const CodeUnit& unit : units) {
        RELEASE_ASSERT(unit.length && unit.offset >= end);
        end = static_cast<uint64_t>(unit.offset) + unit.length;
        RELEASE_ASSERT(end <= size);
    }
    return units;
}

static void appendPieces(Vector<CodeUnit>& units, uint32_t begin, uint32_t end, CodeUnit::Kind kind)
{
    uint32_t length = end - begin;
    if (kind == CodeUnit::Kind::Pinned && !units.isEmpty() && units.last().kind == CodeUnit::Kind::Pinned && units.last().length + length <= maxUnitLength) {
        units.last().length = static_cast<uint16_t>(units.last().length + length);
        return;
    }
    if (length <= maxUnitLength) [[likely]] {
        units.append(CodeUnit { begin, static_cast<uint16_t>(length), kind });
        return;
    }
    // No emitter recorded these cuts, so on x86-64 one may fall inside an instruction: no piece may be outlined on its own.
    for (; end - begin > maxSplitPieceLength; begin += maxSplitPieceLength)
        units.append(CodeUnit { begin, maxSplitPieceLength, CodeUnit::Kind::Pinned });
    units.append(CodeUnit { begin, static_cast<uint16_t>(end - begin), kind == CodeUnit::Kind::End ? kind : CodeUnit::Kind::Pinned });
}

static const OffsetRange* findRangeCovering(std::span<const OffsetRange> ranges, size_t& cursor, uint32_t offset)
{
    while (cursor < ranges.size() && ranges[cursor].end <= offset)
        ++cursor;
    return cursor < ranges.size() && ranges[cursor].begin <= offset ? &ranges[cursor] : nullptr;
}

static Vector<CodeUnit> tileCode(std::span<const uint32_t> cuts, std::span<const CodeUnit> exactUnits, const LinkedRanges& ranges)
{
    Vector<CodeUnit> units;
    units.reserveInitialCapacity(cuts.size());
    size_t nextExactUnit = 0;
    size_t rangeCursor = 0;
    size_t pinCursor = 0;
    size_t hotBlockCursor = 0;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        uint32_t begin = cuts[i];
        uint32_t end = cuts[i + 1];
        const OffsetRange* range = findRangeCovering(ranges.recorded.span(), rangeCursor, begin);
        bool isHot = !!findRangeCovering(ranges.hotBlocks.span(), hotBlockCursor, begin);
        bool isPinnedByClient = !!findRangeCovering(ranges.pins.span(), pinCursor, begin);

        CodeUnit::Kind kind = CodeUnit::Kind::Pinned;
        if (nextExactUnit < exactUnits.size() && exactUnits[nextExactUnit].offset == begin) {
            const CodeUnit& unit = exactUnits[nextExactUnit++];
            RELEASE_ASSERT(unit.offset + unit.length == end);
            RELEASE_ASSERT(!range || unit.kind != CodeUnit::Kind::Call);
            kind = unit.kind;
        } else if (range && range->kind == CodeUnit::Kind::End && range->end == end)
            kind = CodeUnit::Kind::End;
        else if (range && range->kind == CodeUnit::Kind::Plain)
            kind = CodeUnit::Kind::Plain;
        appendPieces(units, begin, end, isHot || isPinnedByClient ? CodeUnit::Kind::Pinned : kind);
    }
    RELEASE_ASSERT(nextExactUnit == exactUnits.size());
    return units;
}

Vector<CodeUnit> CodeUnitRecorder::link(ImageLinkBuffer& buffer, std::span<const Relocation> relocations)
{
    LinkBuffer& linkBuffer = buffer.linkBuffer();
    auto appendLinkedRange = [&](Vector<OffsetRange>& result, MacroAssembler::Label begin, MacroAssembler::Label end, CodeUnit::Kind kind) {
        OffsetRange range { linkBuffer.offsetOf(begin), linkBuffer.offsetOf(end), kind };
        RELEASE_ASSERT(range.begin <= range.end);
        if (range.begin != range.end)
            result.append(range);
    };

    LinkedRanges ranges;
    ranges.recorded.reserveInitialCapacity(m_ranges.size());
    for (auto& range : m_ranges)
        appendLinkedRange(ranges.recorded, range.begin, range.end, range.kind);
    std::ranges::sort(ranges.recorded, { }, &OffsetRange::begin);
    for (size_t i = 1; i < ranges.recorded.size(); ++i)
        RELEASE_ASSERT(ranges.recorded[i - 1].end <= ranges.recorded[i].begin);

    for (auto& [begin, end] : m_pins)
        appendLinkedRange(ranges.pins, begin, end, CodeUnit::Kind::Pinned);
    std::ranges::sort(ranges.pins, { }, &OffsetRange::begin);

    ranges.hotBlocks.reserveInitialCapacity(m_hotBlockRanges.size());
    for (auto& [begin, end] : m_hotBlockRanges)
        appendLinkedRange(ranges.hotBlocks, begin, end, CodeUnit::Kind::Pinned);
    for (size_t i = 1; i < ranges.hotBlocks.size(); ++i)
        RELEASE_ASSERT(ranges.hotBlocks[i - 1].end <= ranges.hotBlocks[i].begin);

    // Not linkBuffer.size(): LinkBuffer fills the storage that jump shortening and branch compaction leave over with nops.
    uint32_t size = safeCast<uint32_t>(buffer.bytes().size());
    const Vector<LinkedSite>& linkedSites = linkBuffer.linkedSites();
    Vector<CodeUnit> exactUnits = collectExactUnits(relocations, linkedSites.span(), size);

    Vector<uint32_t> cuts;
    cuts.reserveInitialCapacity(2 + m_boundaries.size() + 2 * (ranges.recorded.size() + ranges.pins.size() + exactUnits.size()) + linkedSites.size());
    cuts.append(0);
    cuts.append(size);
    for (auto& boundary : m_boundaries)
        cuts.append(linkBuffer.offsetOf(boundary));
    for (auto* list : { &ranges.recorded, &ranges.pins }) {
        for (auto& range : *list) {
            cuts.append(range.begin);
            cuts.append(range.end);
        }
    }
    for (const CodeUnit& unit : exactUnits) {
        cuts.append(unit.offset);
        cuts.append(unit.offset + unit.length);
    }
    for (const LinkedSite& site : linkedSites)
        cuts.append(site.target);
    std::ranges::sort(cuts);
    removeRepeatedElements(cuts);
    RELEASE_ASSERT(cuts.last() == size);

    return tileCode(cuts.span(), exactUnits.span(), ranges);
}

static bool unitsTileCode(std::span<const CodeUnit> units, size_t size)
{
    uint64_t end = 0;
    for (const CodeUnit& unit : units) {
        if (unit.offset != end || !unit.length)
            return false;
        end += unit.length;
    }
    return end == size;
}

static const CodeUnit* findUnitStartingAt(std::span<const CodeUnit> units, uint32_t offset)
{
    auto found = std::ranges::lower_bound(units, offset, { }, &CodeUnit::offset);
    return found != units.end() && found->offset == offset ? &*found : nullptr;
}

static bool isUnitBoundary(std::span<const CodeUnit> units, size_t size, uint32_t offset)
{
    return offset == size || findUnitStartingAt(units, offset);
}

static bool liesInPinnedUnits(std::span<const CodeUnit> units, uint32_t begin, uint32_t end)
{
    if (begin == end)
        return true;
    auto unit = std::ranges::upper_bound(units, begin, { }, &CodeUnit::offset);
    RELEASE_ASSERT(unit != units.begin());
    for (--unit; unit != units.end() && unit->offset < end; ++unit) {
        if (unit->kind != CodeUnit::Kind::Pinned)
            return false;
    }
    return true;
}

static bool ascendStrictly(const auto& records, auto offset)
{
    return std::ranges::adjacent_find(records, std::ranges::greater_equal { }, offset) == records.end();
}

namespace {

class CodeOffsetVerifier {
public:
    CodeOffsetVerifier(std::span<const CodeUnit> units, size_t codeSize)
        : m_units(units)
        , m_codeSize(codeSize)
    {
    }

    void visit(uint32_t offset, CodeOffsetRole role, unsigned size)
    {
        if (m_error == CodeUnitsError::None)
            m_error = verify(offset, role, size);
    }

    CodeUnitsError finish() const
    {
        if (m_error != CodeUnitsError::None)
            return m_error;
        size_t numberOfCallUnits = static_cast<size_t>(std::ranges::count(m_units, CodeUnit::Kind::Call, &CodeUnit::kind));
        return m_numberOfCallUnitsVisited == numberOfCallUnits ? CodeUnitsError::None : CodeUnitsError::CallIsNotAUnit;
    }

private:
    bool splitsUnitInWindow(uint32_t offset) const
    {
        return !isUnitBoundary(m_units, m_codeSize, offset) && !liesInPinnedUnits(m_units, offset, offset + 1);
    }

    CodeUnitsError verifyCall(uint32_t offset, unsigned size)
    {
        const CodeUnit* unit = findUnitStartingAt(m_units, offset);
        if (unit && unit->kind == CodeUnit::Kind::Call && unit->length == size) {
            ++m_numberOfCallUnitsVisited;
            return CodeUnitsError::None;
        }
        return liesInPinnedUnits(m_units, offset, offset + size) ? CodeUnitsError::None : CodeUnitsError::CallIsNotAUnit;
    }

    CodeUnitsError verify(uint32_t offset, CodeOffsetRole role, unsigned size)
    {
        if (static_cast<uint64_t>(offset) + size > m_codeSize)
            return CodeUnitsError::OffsetOutsideCode;
        switch (role) {
        case CodeOffsetRole::Call:
            return verifyCall(offset, size);
        case CodeOffsetRole::Site:
            return liesInPinnedUnits(m_units, offset, offset + size) ? CodeUnitsError::None : CodeUnitsError::SiteIsNotPinned;
        case CodeOffsetRole::Target:
            return splitsUnitInWindow(offset) ? CodeUnitsError::TargetIsNotABoundary : CodeUnitsError::None;
        case CodeOffsetRole::ReturnAddress:
            return splitsUnitInWindow(offset) ? CodeUnitsError::ReturnAddressIsNotABoundary : CodeUnitsError::None;
        case CodeOffsetRole::Boundary:
            return CodeUnitsError::None;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::span<const CodeUnit> m_units;
    size_t m_codeSize;
    size_t m_numberOfCallUnitsVisited { 0 };
    CodeUnitsError m_error { CodeUnitsError::None };
};

} // anonymous namespace

CodeUnitsError verifyCodeUnits(const CompiledCode& code)
{
    const CodeBody& body = code.body;
    std::span<const CodeUnit> units = body.units.span();
    if (units.empty())
        return CodeUnitsError::None;
    if (!unitsTileCode(units, body.bytes.size()))
        return CodeUnitsError::UnitsDoNotTileCode;
    if (!ascendStrictly(body.relocations, &Relocation::offset) || !ascendStrictly(body.localSites, &LinkedSite::offset) || !ascendStrictly(body.returnSites, &ReturnSite::offset))
        return CodeUnitsError::OffsetsDoNotAscend;

    CodeOffsetVerifier verifier(units, body.bytes.size());
    forEachCodeOffset(code, [&](uint32_t offset, CodeOffsetRole role, unsigned size) {
        verifier.visit(offset, role, size);
    });
    return verifier.finish();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
