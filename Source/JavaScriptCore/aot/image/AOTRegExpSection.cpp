/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpInProcessLoader.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTBuildContext.h"
#include "AOTRegExpRecords.h"
#include "AOTSectionLinker.h"
#include "AOTSectionWriter.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "CPUFeatureSet.h"
#include "ExecutableAllocator.h"
#include "MacroAssembler.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <utility>
#include "AOTBitVector.h"
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringHasherInlines.h>

namespace JSC::AOT {

static constexpr uint8_t numberOfCharSizes = 2;
static constexpr size_t bodyAlignment = isX86_64() ? 1 : 4;
static constexpr size_t sectionDataAlignment = 8;
static constexpr size_t routineAlignment = 16;
static constexpr size_t sharedTableAlignment = 16;

static constexpr size_t estimatedPadding = 16;
static constexpr int64_t nearGroupMargin = 64 * KB;
static constexpr size_t maxGroupSize = MacroAssembler::relocationReach(RelocationKind::AddressNear) - nearGroupMargin;
// Where a near reference reaches as far as a far one, a table needs no copy next to its users.
static constexpr UnitSharing sharingOfTables = MacroAssembler::relocationReach(RelocationKind::AddressNear) == MacroAssembler::relocationReach(RelocationKind::AddressFar) ? UnitSharing::Unique : UnitSharing::Copyable;

static_assert(std::tuple_size_v<decltype(CompiledRegExp::code)> == numberOfCharSizes);
static_assert(!(sharedTableAlignment % Yarr::sharedTableAlignment));
static_assert(defaultLimitValue(LimitID::RegExpSectionSize, hostArchitecture()) <= std::numeric_limits<uint32_t>::max());
static_assert(defaultLimitValue(LimitID::RegExpClassTableClusterSize, hostArchitecture()) <= Yarr::ClassTablePacker::defaultMaxClusterSize);

namespace {

// Equal values get equal dense numbers, in order of first use. A hash only finds candidates. The values must outlive the numbering.
template<typename T>
class ValueNumbering {
public:
    uint32_t numberOf(const T& value)
    {
        Candidates& candidates = m_candidates.add(keyOf(value), Candidates()).iterator->value;
        if (std::optional<uint32_t> number = findAmong(candidates, value))
            return *number;
        uint32_t number = size();
        candidates.append(number);
        m_values.append(&value);
        return number;
    }

    std::optional<uint32_t> find(const T& value) const
    {
        auto candidates = m_candidates.find(keyOf(value));
        if (candidates == m_candidates.end())
            return std::nullopt;
        return findAmong(candidates->value, value);
    }

    const T& at(uint32_t number) const { return *m_values[number]; }
    uint32_t size() const { return static_cast<uint32_t>(m_values.size()); }

private:
    using Candidates = Vector<uint32_t, 1>;

    static uint32_t keyOf(const T& value) { return computeHash(value) >> 1; }

    std::optional<uint32_t> findAmong(const Candidates& candidates, const T& value) const
    {
        for (uint32_t candidate : candidates) {
            if (at(candidate) == value)
                return candidate;
        }
        return std::nullopt;
    }

    Vector<const T*> m_values;
    UncheckedKeyHashMap<uint32_t, Candidates, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_candidates;
};

struct Unit {
    const RegExpCode* code;
    Vector<uint32_t> symbols;

    friend bool operator==(const Unit& a, const Unit& b)
    {
        return a.code->reach == b.code->reach
            && a.code->body.bytes == b.code->body.bytes
            && std::ranges::equal(a.code->body.relocations, b.code->body.relocations, [&](const Relocation& left, const Relocation& right) {
                return left.offset == right.offset && left.kind == right.kind && a.symbols[left.symbol] == b.symbols[right.symbol];
            });
    }

    friend void add(Hasher& hasher, const Unit& unit)
    {
        add(hasher, StringHasher::computeHashAndMaskTop8Bits(unit.code->body.bytes.span()));
        for (const Relocation& relocation : unit.code->body.relocations)
            add(hasher, relocation.offset, relocation.kind, unit.symbols[relocation.symbol]);
    }
};

class SectionBuilder {
    WTF_MAKE_NONCOPYABLE(SectionBuilder);
public:
    SectionBuilder(std::span<const CompiledRegExp> regExps, const AssemblerPolicy& policy, const CodeTemplates& templates, BuildContext* context)
        : m_regExps(regExps)
        , m_policy(policy)
        , m_templates(templates)
        , m_context(context)
        , m_maxSize(context ? context->limit(LimitID::RegExpSectionSize) : defaultLimitValue(LimitID::RegExpSectionSize, hostArchitecture()))
        , m_maxClusterSize(context ? context->limit(LimitID::RegExpClassTableClusterSize) : Yarr::ClassTablePacker::defaultMaxClusterSize)
    {
    }

    std::optional<RegExpSection> build()
    {
        for (uint8_t charSizeIndex = 0; charSizeIndex < numberOfCharSizes; ++charSizeIndex) {
            for (uint32_t regExp = 0; regExp < m_regExps.size(); ++regExp) {
                if (m_regExps[regExp].form == RegExpRecordForm::Code && m_regExps[regExp].code[charSizeIndex])
                    m_bodies.append({ numberUnit(*m_regExps[regExp].code[charSizeIndex], regExp), regExp, charSizeIndex });
            }
        }
        if (!compileRoutines())
            return std::nullopt;

        ValueNumbering<Unit> numbering;
        for (Body& body : m_bodies)
            body.number = numbering.numberOf(body.unit);
        m_unitOfBody.grow(numbering.size());
        m_groupedBodies.ensureSize(numbering.size());

        addUngroupedUnits();
        for (size_t index = 0; index < m_bodies.size();)
            index = formGroup(index);
        if (!addFailureRoutine())
            return std::nullopt;

        auto findTarget = [&](uint32_t unit, const Relocation& relocation) -> std::optional<UnitTarget> {
            const Vector<UnitTarget>& targets = m_unitInfos[unit].targets;
            if (relocation.symbol >= targets.size())
                return std::nullopt;
            return targets[relocation.symbol];
        };
        UnitResolver resolve = findTarget;
        auto linked = linkSection({ m_units.span(), resolve, m_templates, m_maxSize, RegExpSection::alignment });
        if (!linked) {
            reportFailure(linked.error());
            return std::nullopt;
        }
        return createSection(WTF::move(*linked));
    }

private:
    using Kind = RegExpSectionItem::Kind;

    struct Body {
        Unit unit;
        uint32_t regExp;
        uint8_t charSizeIndex;
        uint32_t number { 0 };
    };

    struct Routine {
        std::unique_ptr<RegExpCode> code;
        Unit unit;
    };

    // target names a unit. hasCopy says that a group has counted a copy of it: the units of addUngroupedUnits() exist before any group.
    struct SymbolState {
        size_t maxSize;
        uint32_t firstUser;
        std::optional<UnitTarget> target { };
        std::optional<uint32_t> routine { };
        bool hasCopy { false };
    };

    struct UnitInfo {
        Kind kind;
        uint32_t regExp;
        Vector<UnitTarget> targets;
    };

    Unit numberUnit(const RegExpCode& code, uint32_t regExp)
    {
        return { &code, code.symbols.map([&](const RegExpSymbol& symbol) {
            uint32_t number = m_symbols.numberOf(symbol);
            if (number == m_symbolStates.size())
                m_symbolStates.append({ maxPrivateCopySize(symbol), regExp });
            return number;
        }) };
    }

    std::unique_ptr<RegExpCode> takeRoutine(RegExpCodeOrFailureReason&& code)
    {
        if (code)
            return WTF::move(*code);
        if (m_context)
            m_context->diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode });
        return nullptr;
    }

    [[nodiscard]] bool compileRoutines()
    {
        for (uint32_t symbol = 0; symbol < m_symbols.size(); ++symbol) {
            auto* routine = std::get_if<Yarr::SharedRoutine>(&m_symbols.at(symbol));
            if (!routine)
                continue;
            std::unique_ptr<RegExpCode> code = takeRoutine(compileSharedRoutine(*routine, m_policy, m_context));
            if (!code)
                return false;
            Unit unit = numberUnit(*code, m_symbolStates[symbol].firstUser);
            m_symbolStates[symbol].routine = static_cast<uint32_t>(m_routines.size());
            m_routines.append({ WTF::move(code), WTF::move(unit) });
        }
        m_visitedSymbols.ensureSize(m_symbols.size());
        return true;
    }

    uint32_t addUnit(Kind kind, UnitSharing sharing, uint32_t alignment, std::span<const uint8_t> bytes, uint32_t regExp, std::span<const Relocation> relocations = { })
    {
        m_units.append(LinkUnit { bytes, relocations, alignment, sharing, kind == Kind::Table });
        m_unitInfos.append(UnitInfo { kind, regExp, { } });
        return static_cast<uint32_t>(m_units.size() - 1);
    }

    uint32_t addUnit(Kind kind, UnitSharing sharing, uint32_t alignment, const RelocatableCode& code, uint32_t regExp)
    {
        return addUnit(kind, sharing, alignment, code.bytes.span(), regExp, code.relocations.span());
    }

    // The buffer of the inner vector does not move when the outer vector grows.
    std::span<uint8_t> ownBytes(Vector<uint8_t>&& bytes)
    {
        m_ownedBytes.append(WTF::move(bytes));
        return m_ownedBytes.last().mutableSpan();
    }

    Vector<UnitTarget> targetsOf(const Unit& unit) const
    {
        return unit.symbols.map([&](uint32_t symbol) {
            RELEASE_ASSERT(m_symbolStates[symbol].target);
            return *m_symbolStates[symbol].target;
        });
    }

    const Unit& routineUnit(uint32_t symbol) const { return m_routines[*m_symbolStates[symbol].routine].unit; }

    void addUngroupedUnits()
    {
        for (Yarr::SharedTableID id : Yarr::allSharedTableIDs) {
            std::optional<uint32_t> symbol = m_symbols.find(RegExpSymbol { id });
            if (!symbol)
                continue;
            SymbolState& state = m_symbolStates[*symbol];
            state.target = UnitTarget { addUnit(Kind::Table, UnitSharing::Unique, sharedTableAlignment, Yarr::sharedTable(id), state.firstUser) };
        }
        for (uint32_t symbol = 0; symbol < m_symbols.size(); ++symbol) {
            SymbolState& state = m_symbolStates[symbol];
            auto addTable = [&](std::span<const uint8_t> bytes) {
                state.target = UnitTarget { addUnit(Kind::Table, sharingOfTables, sectionDataAlignment, bytes, state.firstUser) };
            };
            WTF::switchOn(m_symbols.at(symbol),
                [&](Yarr::SharedTableID) { },
                [&](const PlainData& data) { addTable(data.bytes.span()); },
                [&](const Latin1BitPlane&) { },
                [&](const Yarr::ClassTable&) { },
                [&](const Yarr::SharedRoutine&) {
                    state.target = UnitTarget { addUnit(Kind::Routine, UnitSharing::Copyable, routineAlignment, routineUnit(symbol).code->body, state.firstUser) };
                },
                [&](const Literal64& literal) { addTable(ownBytes(Vector<uint8_t>(asByteSpan(literal.value)))); });
        }
    }

    void forEachSymbolReachedFrom(const Unit& unit, const Invocable<void(uint32_t, PCRelativeReach)> auto& functor)
    {
        Vector<uint32_t, 16> visited;
        Vector<const Unit*, 4> worklist { &unit };
        while (!worklist.isEmpty()) {
            const Unit& user = *worklist.takeLast();
            for (uint32_t symbol : user.symbols) {
                if (m_visitedSymbols.quickSet(symbol))
                    continue;
                visited.append(symbol);
                functor(symbol, user.code->reach);
                if (m_symbolStates[symbol].routine)
                    worklist.append(&routineUnit(symbol));
            }
        }
        for (uint32_t symbol : visited)
            m_visitedSymbols.quickClear(symbol);
    }

    size_t estimateSize(uint32_t symbol) const
    {
        size_t maxSize = m_symbolStates[symbol].maxSize;
        return WTF::switchOn(m_symbols.at(symbol),
            [&](Yarr::SharedTableID) -> size_t { return 0; },
            [&](const PlainData&) -> size_t { return maxSize; },
            [&](const Latin1BitPlane&) -> size_t { return maxSize; },
            [&](const Yarr::ClassTable&) -> size_t { return maxSize; },
            [&](const Yarr::SharedRoutine&) -> size_t { return routineUnit(symbol).code->body.bytes.size() + estimatedPadding; },
            [&](const Literal64&) -> size_t { return maxSize; });
    }

    // Every size is an upper bound, so a group of several bodies spans at most maxGroupSize and its near references reach its tables.
    size_t formGroup(size_t firstBody)
    {
        Vector<size_t> bodies;
        Vector<uint32_t> classTables;
        Vector<uint32_t> bitPlanes;
        Vector<uint32_t> routines;
        BitVector seenSymbols(m_symbols.size());
        auto needsCopy = [&](uint32_t symbol, PCRelativeReach reach) {
            if (reach == PCRelativeReach::Far && m_symbolStates[symbol].hasCopy)
                return false;
            return !seenSymbols.get(symbol);
        };

        size_t groupSize = 0;
        size_t index = firstBody;
        for (; index < m_bodies.size(); ++index) {
            const Body& body = m_bodies[index];
            if (m_groupedBodies.get(body.number))
                continue;
            size_t bodySize = body.unit.code->body.bytes.size() + estimatedPadding;
            forEachSymbolReachedFrom(body.unit, [&](uint32_t symbol, PCRelativeReach reach) {
                if (needsCopy(symbol, reach))
                    bodySize += estimateSize(symbol);
            });
            if (groupSize && groupSize + bodySize > maxGroupSize)
                break;
            groupSize += bodySize;
            m_groupedBodies.set(body.number);
            bodies.append(index);
            forEachSymbolReachedFrom(body.unit, [&](uint32_t symbol, PCRelativeReach reach) {
                if (!needsCopy(symbol, reach))
                    return;
                seenSymbols.set(symbol);
                bool isFirstCopy = !std::exchange(m_symbolStates[symbol].hasCopy, true);
                WTF::switchOn(m_symbols.at(symbol),
                    [&](Yarr::SharedTableID) { },
                    [&](const PlainData&) { },
                    [&](const Latin1BitPlane&) { bitPlanes.append(symbol); },
                    [&](const Yarr::ClassTable&) { classTables.append(symbol); },
                    [&](const Yarr::SharedRoutine&) {
                        if (isFirstCopy)
                            routines.append(symbol);
                    },
                    [&](const Literal64&) { });
            });
        }

        uint32_t firstRegExp = m_bodies[firstBody].regExp;
        addClassTables(classTables.span(), firstRegExp);
        addBitPlanes(bitPlanes.span(), firstRegExp);

        // Targets are taken now: a later group gives the same table another target.
        for (uint32_t symbol : routines)
            m_unitInfos[m_symbolStates[symbol].target->unit].targets = targetsOf(routineUnit(symbol));
        for (size_t bodyIndex : bodies) {
            const Body& body = m_bodies[bodyIndex];
            uint32_t unit = addUnit(Kind::Body, UnitSharing::Unique, bodyAlignment, body.unit.code->body, body.regExp);
            m_unitInfos[unit].targets = targetsOf(body.unit);
            m_unitOfBody[body.number] = unit;
        }
        return index;
    }

    // A thunk is bound to a literal before anybody knows whether the literal gets code, and it takes JITCodeFailure to the generic operation.
    [[nodiscard]] bool addFailureRoutine()
    {
        auto withoutCode = std::ranges::find_if(m_regExps, [](const CompiledRegExp& regExp) {
            return regExp.form != RegExpRecordForm::Code || !regExp.code[0] || !regExp.code[1];
        });
        if (withoutCode == m_regExps.end())
            return true;
        m_failureRoutine = takeRoutine(compileFailureRoutine(m_policy, m_context));
        if (!m_failureRoutine)
            return false;
        m_failureRoutineUnit = addUnit(Kind::Routine, UnitSharing::Unique, routineAlignment, m_failureRoutine->body, static_cast<uint32_t>(withoutCode - m_regExps.begin()));
        return true;
    }

    void addClassTables(std::span<const uint32_t> symbols, uint32_t regExp)
    {
        if (symbols.empty())
            return;
        Yarr::ClassTablePacker packer(m_maxClusterSize);
        Vector<size_t> tables(symbols.size(), [&](size_t index) {
            return packer.add(std::get<Yarr::ClassTable>(m_symbols.at(symbols[index])));
        });
        packer.pack();

        uint32_t unit = addUnit(Kind::Table, sharingOfTables, sectionDataAlignment, ownBytes(Vector<uint8_t>(packer.bytes())), regExp);
        for (size_t index = 0; index < symbols.size(); ++index)
            m_symbolStates[symbols[index]].target = UnitTarget { unit, packer.offsetOf(tables[index]) };
    }

    void addBitPlanes(std::span<const uint32_t> symbols, uint32_t regExp)
    {
        struct ByteTable {
            uint32_t unit;
            std::span<uint8_t> bytes;
        };

        std::array<Vector<ByteTable>, Latin1BitPlane::planesPerByteTable> tablesWithFreeBit;
        for (uint32_t symbol : symbols) {
            const Latin1BitPlane& plane = std::get<Latin1BitPlane>(m_symbols.at(symbol));
            RELEASE_ASSERT(plane.bit < Latin1BitPlane::planesPerByteTable);
            if (tablesWithFreeBit[plane.bit].isEmpty()) {
                std::span<uint8_t> bytes = ownBytes(Vector<uint8_t>(FillWith { }, Yarr::CharacterClass::latin1TableSize, static_cast<uint8_t>(0)));
                ByteTable table { addUnit(Kind::Table, sharingOfTables, sectionDataAlignment, bytes, regExp), bytes };
                for (Vector<ByteTable>& tables : tablesWithFreeBit)
                    tables.append(table);
            }
            ByteTable table = tablesWithFreeBit[plane.bit].takeLast();
            plane.members.forEachSetBit([&](size_t character) {
                table.bytes[character] |= 1 << plane.bit;
            });
            m_symbolStates[symbol].target = UnitTarget { table.unit };
        }
    }

    RegExpSection createSection(LinkedSection&& linked) const
    {
        // A veneer clobbers a register that a routine may take an argument in. Only routines are called, and they are copied.
        RELEASE_ASSERT(!linked.numberOfVeneers);

        RegExpSection section;
        auto addItem = [&](Kind kind, uint32_t unit, uint32_t offset, uint32_t regExp) {
            section.items.append({ kind, offset, static_cast<uint32_t>(m_units[unit].bytes.size()), regExp });
        };
        section.bytes = WTF::move(linked.bytes);
        section.dataOffset = linked.dataOffset;
        uint32_t entryWithoutCode = m_failureRoutineUnit ? linked.uniqueUnitOffsets[*m_failureRoutineUnit] : 0;
        section.entries.fill({ entryWithoutCode, entryWithoutCode }, m_regExps.size());
        for (uint32_t unit = 0; unit < m_units.size(); ++unit) {
            if (m_units[unit].sharing == UnitSharing::Unique)
                addItem(m_unitInfos[unit].kind, unit, linked.uniqueUnitOffsets[unit], m_unitInfos[unit].regExp);
        }
        for (const PlacedUnit& copy : linked.copies)
            addItem(m_unitInfos[copy.unit].kind, copy.unit, copy.offset, m_unitInfos[copy.unit].regExp);

        BitVector seenBodies(m_unitOfBody.size());
        for (const Body& body : m_bodies) {
            uint32_t unit = *m_unitOfBody[body.number];
            section.entries[body.regExp][body.charSizeIndex] = linked.uniqueUnitOffsets[unit];
            if (seenBodies.set(body.number))
                addItem(Kind::DuplicateBody, unit, linked.uniqueUnitOffsets[unit], body.regExp);
        }
        AOT::stableSort(section.items, { }, &RegExpSectionItem::offset);
        return section;
    }

    void reportFailure(const SectionLinkFailure& failure)
    {
        if (!m_context)
            return;
        switch (failure.error) {
        case SectionLinkError::SectionTooLarge:
            m_context->reportLimitExceeded(LimitID::RegExpSectionSize, m_maxSize);
            return;
        case SectionLinkError::OutOfPCRelativeReach:
        case SectionLinkError::SymbolWithoutUnit:
            m_context->diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .count = failure.unit });
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::span<const CompiledRegExp> m_regExps;
    const AssemblerPolicy& m_policy;
    const CodeTemplates& m_templates;
    BuildContext* m_context;
    uint64_t m_maxSize;
    size_t m_maxClusterSize;

    ValueNumbering<RegExpSymbol> m_symbols;
    Vector<SymbolState> m_symbolStates;
    BitVector m_visitedSymbols;
    Vector<Routine> m_routines;
    Vector<Body> m_bodies; // In placement order.
    BitVector m_groupedBodies; // By Body::number.
    Vector<std::optional<uint32_t>> m_unitOfBody; // By Body::number.

    Vector<LinkUnit> m_units;
    Vector<UnitInfo> m_unitInfos;
    Vector<Vector<uint8_t>> m_ownedBytes;
    std::unique_ptr<RegExpCode> m_failureRoutine;
    std::optional<uint32_t> m_failureRoutineUnit;
};

} // anonymous namespace

std::optional<RegExpSection> layOutRegExpSection(std::span<const CompiledRegExp> regExps, const AssemblerPolicy& policy, const CodeTemplates& templates, BuildContext* context)
{
    return SectionBuilder(regExps, policy, templates, context).build();
}

static uint32_t encodeNamedGroups(SectionWriter& details, const RegExpRequest& request, const RegExpSummary& summary)
{
    Vector<uint32_t> words;
    for (StringID name : request.captureGroupNames)
        words.append(name ? name.index() + 1 : 0);
    for (const RegExpNamedGroupStrings& group : request.namedGroups) {
        words.append(group.name.index());
        words.append(static_cast<uint32_t>(group.parenIndices.size()));
        words.appendVector(group.parenIndices);
    }

    ImageRegExpNamedGroups header {
        .numDuplicateNamedCaptureGroups = summary.numDuplicateNamedCaptureGroups,
        .numberOfCaptureGroupNames = static_cast<uint32_t>(request.captureGroupNames.size()),
        .numberOfNamedGroups = static_cast<uint32_t>(request.namedGroups.size()),
        .numberOfWords = static_cast<uint32_t>(words.size()),
    };
    uint32_t offset = details.append(asByteSpan(header), ImageRegExpNamedGroups::alignment);
    details.append(asByteSpan(words.span()), alignof(uint32_t));
    return offset;
}

static void encodeMetadata(ImageRegExp& record, const RegExpRequest& request, const RegExpSummary& summary, SectionWriter& details)
{
    record.numSubpatterns = summary.numSubpatterns;
    record.minimumSize = summary.minimumSize;
    record.rawSpecificPattern = static_cast<uint8_t>(summary.specificPattern);
    record.sourceIsPattern = summary.sourceIsPattern;
    record.hasNamedGroups = summary.hasNamedGroups();
    RELEASE_ASSERT(!record.hasNamedGroups || !record.hasCharacterSet());
    if (record.hasNamedGroups)
        record.details = encodeNamedGroups(details, request, summary);
    if (record.hasCharacterSet()) {
        RELEASE_ASSERT(summary.characterSet);
        const Yarr::CharacterSetWithRanges& set = *summary.characterSet;
        ASSERT(!set.bits.firstWideRange);
        RELEASE_ASSERT(set.bits.wideRangeCount == set.wideRanges.size());
        ImageRegExpCharacterSet header { set.bits };
        record.details = details.append(asByteSpan(header), ImageRegExpCharacterSet::alignment);
        details.append(asByteSpan(set.wideRanges.span()), alignof(Yarr::CharacterRange));
    }
}

static ImageRegExp createRecord(const RegExpRequest& request, RegExpRecordForm form, const RegExpSummary& summary, const std::array<uint32_t, 2>& entries, SectionWriter& details)
{
    ASSERT(request.flags == ImageRegExp::significantFlags(request.flags));
    RELEASE_ASSERT((form == RegExpRecordForm::NoMetadata) == (summary.metadata == RegExpMetadata::Unknown));
    ImageRegExp record { };
    record.pattern = request.pattern.index();
    record.rawFlags = request.flags.toRaw();
    record.rawForm = static_cast<uint8_t>(form);
    record.isLiteral = request.origin == RegExpRequestOrigin::Literal;

    switch (summary.metadata) {
    case RegExpMetadata::Unknown:
        break;
    case RegExpMetadata::Known:
        encodeMetadata(record, request, summary, details);
        break;
    }

    switch (form) {
    case RegExpRecordForm::Code:
        record.codeFor8Bit = entries[0];
        record.codeFor16Bit = entries[1];
        break;
    case RegExpRecordForm::Atom:
        RELEASE_ASSERT(request.atom);
        record.atom = request.atom.index();
        break;
    case RegExpRecordForm::NoCode:
    case RegExpRecordForm::NoMetadata:
        break;
    }
    return record;
}

static bool hasRecord(RegExpRequestOrigin origin, RegExpRecordForm form)
{
    switch (form) {
    case RegExpRecordForm::Code:
    case RegExpRecordForm::Atom:
        return true;
    // With the record the run time need not parse the text to create the object.
    case RegExpRecordForm::NoCode:
        return origin != RegExpRequestOrigin::Candidate;
    case RegExpRecordForm::NoMetadata:
        return origin == RegExpRequestOrigin::Literal;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RegExpSections encodeRegExpRecords(const RegExpRecordInputs& inputs, BuildContext& context)
{
    std::span<const RegExpRequest> requests = inputs.requests.all();
    RELEASE_ASSERT(requests.size() == inputs.compiled.results.size() && requests.size() == inputs.section.entries.size());

    Vector<ImageRegExp> records;
    Vector<ImageRegExpHash> hashes;
    SectionWriter details(context);
    uint32_t maxOutputSize = regExpOutputSizeWithoutSubpatterns;
    for (size_t index = 0; index < requests.size(); ++index) {
        const RegExpRequest& request = requests[index];
        RegExpRecordForm form = inputs.compiled.results[index].form;
        if (!hasRecord(request.origin, form))
            continue;

        uint32_t recordNumber = static_cast<uint32_t>(records.size());
        RELEASE_ASSERT(request.origin != RegExpRequestOrigin::Literal || recordNumber == index);
        const RegExpSummary* summary = context.regExpSummaries().summaryIfComputed(request.pattern, request.flags);
        RELEASE_ASSERT(summary);

        records.append(createRecord(request, form, *summary, inputs.section.entries[index], details));
        hashes.append(ImageRegExpHash { ImageRegExp::hashOf(summary->patternHash, request.flags), recordNumber });
        if (form == RegExpRecordForm::Code)
            maxOutputSize = std::max(maxOutputSize, (summary->numSubpatterns + 1) * 2 + summary->numDuplicateNamedCaptureGroups);
    }
    if (details.didOverflow())
        return { };

    AOT::sort(hashes, { }, [](const ImageRegExpHash& entry) {
        return std::pair { entry.hash, entry.record };
    });
    return { bytesOf(records), bytesOf(hashes), details.takeBytes(), maxOutputSize };
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if ENABLE(JIT_SCAN_ASSEMBLER_BUFFER_FOR_ZEROES)

// Tables hold runs of zero bytes, which performJITMemcpy rejects in this configuration.
const uint8_t* loadRegExpSection(const RegExpSection&)
{
    return nullptr;
}

#else

AOT_THRESHOLD(size_t, maxLoadedRegExpBytes, 64 * MB);
static std::atomic<size_t> loadedRegExpBytes { 0 };

const uint8_t* loadRegExpSection(const RegExpSection& section)
{
    auto bytes = section.bytes.span();
    size_t size = roundUpToMultipleOf<RegExpSection::alignment>(bytes.size()) + RegExpSection::alignment;
    bool isWithinLimit = loadedRegExpBytes.fetch_add(size, std::memory_order_relaxed) + size <= maxLoadedRegExpBytes;
    auto handle = isWithinLimit ? ExecutableAllocator::singleton().allocate(size, JITCompilationCanFail) : nullptr;
    if (!handle) {
        loadedRegExpBytes.fetch_sub(size, std::memory_order_relaxed);
        return nullptr;
    }

    auto* start = std::bit_cast<uint8_t*>(roundUpToMultipleOf<RegExpSection::alignment>(handle->startAsInteger()));
    performJITMemcpy<jitMemcpyRepatch>(start, bytes.data(), bytes.size());
    MacroAssembler::cacheFlush(start, bytes.size());
    UNUSED_VARIABLE(handle.leakRef());
    return start;
}

#endif // ENABLE(JIT_SCAN_ASSEMBLER_BUFFER_FOR_ZEROES)

static Yarr::JITFailureReason failureReasonFor(RegExpDeclineReason reason)
{
    switch (reason) {
    case RegExpDeclineReason::NestedTooDeep:
        return Yarr::JITFailureReason::ParenthesisNestedTooDeep;
    case RegExpDeclineReason::CodeTooLarge:
    case RegExpDeclineReason::PatternTooLong:
    case RegExpDeclineReason::CandidateBudgetExhausted:
    case RegExpDeclineReason::SectionFull:
        return Yarr::JITFailureReason::GeneratedCodeSizeTooLarge;
    case RegExpDeclineReason::NotSupportedByCodeGenerator:
    case RegExpDeclineReason::TextKnownAtRunTime:
        return Yarr::JITFailureReason::UnlistedSharedTable;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Yarr::JITFailureReason> failureReasonFor(const CompiledRegExp& compiled)
{
    switch (compiled.form) {
    case RegExpRecordForm::Code:
        return std::nullopt;
    case RegExpRecordForm::Atom:
        return Yarr::JITFailureReason::UnlistedSharedTable;
    case RegExpRecordForm::NoCode:
    case RegExpRecordForm::NoMetadata:
        return failureReasonFor(*compiled.reason);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::expected<RegExpEntrypoints, Yarr::JITFailureReason> compileThroughImage(StringView pattern, OptionSet<Yarr::Flags> flags)
{
    if (!hasStackForRegExpCompiler())
        return makeUnexpected(Yarr::JITFailureReason::ParenthesisNestedTooDeep);

    AssemblerPolicy policy = AssemblerPolicy::forImage(CPUFeatureSet::buildTarget());
    RegExpSummary summary = summarizeRegExp(pattern, flags);
    std::array<CompiledRegExp, 1> compiled { compileRegExp({ pattern, flags, summary, policy, nullptr }) };
    if (auto reason = failureReasonFor(compiled[0]))
        return makeUnexpected(*reason);

    auto templates = CodeTemplates::create(policy, nullptr);
    if (!templates)
        return makeUnexpected(Yarr::JITFailureReason::ExecutableMemoryAllocationFailure);

    auto section = layOutRegExpSection(compiled, policy, *templates, nullptr);
    if (!section)
        return makeUnexpected(Yarr::JITFailureReason::GeneratedCodeSizeTooLarge);

    auto* base = loadRegExpSection(*section);
    if (!base)
        return makeUnexpected(Yarr::JITFailureReason::ExecutableMemoryAllocationFailure);

    auto [offsetFor8Bit, offsetFor16Bit] = section->entries[0];
    return RegExpEntrypoints { base + offsetFor8Bit, base + offsetFor16Bit };
}

void jitCompileThroughImage(StringView pattern, OptionSet<Yarr::Flags> flags, Yarr::YarrCodeBlock& codeBlock)
{
    auto entrypoints = compileThroughImage(pattern, ImageRegExp::significantFlags(flags));
    if (entrypoints)
        codeBlock.setCodeFromImage(entrypoints->for8Bit, entrypoints->for16Bit);
    else
        codeBlock.setFallBackWithFailureReason(entrypoints.error());
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
