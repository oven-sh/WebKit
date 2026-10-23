/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpSection.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTBuildContext.h"
#include "AOTRegExpCodeAddressing.h"
#include "AOTSectionLinker.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include "MacroAssembler.h"
#include "YarrClassTable.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
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

// A body or a routine. symbols[i] is the number of code->symbols[i].
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
    SectionBuilder(std::span<const CompiledRegExp>, const AssemblerPolicy&, const CodeTemplates&, BuildContext*);

    std::optional<RegExpSection> build();

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

    // targets[i] is where symbol i of the code of the unit lives.
    struct UnitInfo {
        Kind kind;
        uint32_t regExp;
        Vector<UnitTarget> targets;
    };

    struct Group {
        Vector<size_t> bodies;
        Vector<uint32_t> classTables;
        Vector<uint32_t> bitPlanes;
        Vector<uint32_t> routines;
    };

    void numberSymbols();
    [[nodiscard]] bool compileRoutines();
    void numberBodies();
    void addUngroupedUnits();
    size_t formGroup(size_t firstBody);
    [[nodiscard]] bool addFailureRoutine();
    RegExpSection createSection(LinkedSection&&) const;
    void reportFailure(const SectionLinkFailure&);

    Unit numberUnit(const RegExpCode&, uint32_t regExp);
    size_t estimateSize(uint32_t symbol) const;
    template<typename Functor> void forEachSymbolReachedFrom(const Unit&, const Functor&);
    void addToGroup(Group&, uint32_t symbol);
    void addClassTables(std::span<const uint32_t> symbols, uint32_t regExp);
    void addBitPlanes(std::span<const uint32_t> symbols, uint32_t regExp);
    void addBody(const Body&);

    uint32_t addUnit(Kind, UnitSharing, uint32_t alignment, std::span<const uint8_t>, uint32_t regExp);
    uint32_t addUnit(Kind, UnitSharing, uint32_t alignment, const RelocatableCode&, uint32_t regExp);
    std::span<uint8_t> ownBytes(Vector<uint8_t>&&);
    Vector<UnitTarget> targetsOf(const Unit&) const;

    const Unit& routineUnit(uint32_t symbol) const { return m_routines[*m_symbolStates[symbol].routine].unit; }

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

SectionBuilder::SectionBuilder(std::span<const CompiledRegExp> regExps, const AssemblerPolicy& policy, const CodeTemplates& templates, BuildContext* context)
    : m_regExps(regExps)
    , m_policy(policy)
    , m_templates(templates)
    , m_context(context)
    , m_maxSize(context ? context->limit(LimitID::RegExpSectionSize) : defaultLimitValue(LimitID::RegExpSectionSize, hostArchitecture()))
    , m_maxClusterSize(context ? context->limit(LimitID::RegExpClassTableClusterSize) : Yarr::ClassTablePacker::defaultMaxClusterSize)
{
}

std::optional<RegExpSection> SectionBuilder::build()
{
    numberSymbols();
    if (!compileRoutines())
        return std::nullopt;
    numberBodies();
    addUngroupedUnits();
    for (size_t index = 0; index < m_bodies.size();)
        index = formGroup(index);
    if (!addFailureRoutine())
        return std::nullopt;

    auto findTarget =[&](uint32_t unit, const Relocation& relocation) -> std::optional<UnitTarget> {
        const Vector<UnitTarget>& targets = m_unitInfos[unit].targets;
        if (relocation.symbol >= targets.size())
            return std::nullopt;
        return targets[relocation.symbol];
    };
    UnitResolver resolve = findTarget;
    auto linked = linkSection({ m_units.span(), resolve, m_templates, m_maxSize });
    if (!linked) {
        reportFailure(linked.error());
        return std::nullopt;
    }
    return createSection(WTF::move(*linked));
}

void SectionBuilder::numberSymbols()
{
    for (uint8_t charSizeIndex = 0; charSizeIndex < numberOfCharSizes; ++charSizeIndex) {
        for (uint32_t regExp = 0; regExp < m_regExps.size(); ++regExp) {
            if (m_regExps[regExp].form == RegExpRecordForm::Code && m_regExps[regExp].code[charSizeIndex])
                m_bodies.append({ numberUnit(*m_regExps[regExp].code[charSizeIndex], regExp), regExp, charSizeIndex });
        }
    }
}

Unit SectionBuilder::numberUnit(const RegExpCode& code, uint32_t regExp)
{
    return { &code, code.symbols.map([&](const RegExpSymbol& symbol) {
        uint32_t number = m_symbols.numberOf(symbol);
        if (number == m_symbolStates.size())
            m_symbolStates.append({ maxPrivateCopySize(symbol), regExp });
        return number;
    }) };
}

bool SectionBuilder::compileRoutines()
{
    for (uint32_t symbol = 0; symbol < m_symbols.size(); ++symbol) {
        auto* routine = std::get_if<Yarr::SharedRoutine>(&m_symbols.at(symbol));
        if (!routine)
            continue;
        auto code = compileSharedRoutine(*routine, m_policy, m_context);
        if (!code) {
            if (m_context)
                m_context->diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode });
            return false;
        }
        Unit unit = numberUnit(**code, m_symbolStates[symbol].firstUser);
        m_symbolStates[symbol].routine = static_cast<uint32_t>(m_routines.size());
        m_routines.append({ WTF::move(*code), WTF::move(unit) });
    }
    m_visitedSymbols.ensureSize(m_symbols.size());
    return true;
}

void SectionBuilder::numberBodies()
{
    ValueNumbering<Unit> numbering;
    for (Body& body : m_bodies)
        body.number = numbering.numberOf(body.unit);
    m_unitOfBody.grow(numbering.size());
    m_groupedBodies.ensureSize(numbering.size());
}

uint32_t SectionBuilder::addUnit(Kind kind, UnitSharing sharing, uint32_t alignment, std::span<const uint8_t> bytes, uint32_t regExp)
{
    uint32_t unit = static_cast<uint32_t>(m_units.size());
    m_units.append(LinkUnit { bytes, { }, alignment, sharing });
    m_unitInfos.append(UnitInfo { kind, regExp, { } });
    return unit;
}

uint32_t SectionBuilder::addUnit(Kind kind, UnitSharing sharing, uint32_t alignment, const RelocatableCode& code, uint32_t regExp)
{
    uint32_t unit = addUnit(kind, sharing, alignment, code.bytes.span(), regExp);
    m_units[unit].relocations = code.relocations.span();
    return unit;
}

// The buffer of the inner vector does not move when the outer vector grows.
std::span<uint8_t> SectionBuilder::ownBytes(Vector<uint8_t>&& bytes)
{
    m_ownedBytes.append(WTF::move(bytes));
    return m_ownedBytes.last().mutableSpan();
}

Vector<UnitTarget> SectionBuilder::targetsOf(const Unit& unit) const
{
    return unit.symbols.map([&](uint32_t symbol) {
        RELEASE_ASSERT(m_symbolStates[symbol].target);
        return *m_symbolStates[symbol].target;
    });
}

void SectionBuilder::addUngroupedUnits()
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
            state.target = UnitTarget { addUnit(Kind::Table, UnitSharing::Copyable, sectionDataAlignment, bytes, state.firstUser) };
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

// The functor gets each symbol once, with the reach of the code that names it: the reach of the unit if the unit names it.
template<typename Functor>
void SectionBuilder::forEachSymbolReachedFrom(const Unit& unit, const Functor& functor)
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

size_t SectionBuilder::estimateSize(uint32_t symbol) const
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

void SectionBuilder::addToGroup(Group& group, uint32_t symbol)
{
    bool isFirstCopy = !std::exchange(m_symbolStates[symbol].hasCopy, true);
    WTF::switchOn(m_symbols.at(symbol),
        [&](Yarr::SharedTableID) { },
        [&](const PlainData&) { },
        [&](const Latin1BitPlane&) { group.bitPlanes.append(symbol); },
        [&](const Yarr::ClassTable&) { group.classTables.append(symbol); },
        [&](const Yarr::SharedRoutine&) {
            if (isFirstCopy)
                group.routines.append(symbol);
        },
        [&](const Literal64&) { });
}

// Every size is an upper bound, so a group of several bodies spans at most maxGroupSize and its near references reach its tables.
size_t SectionBuilder::formGroup(size_t firstBody)
{
    Group group;
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
        group.bodies.append(index);
        forEachSymbolReachedFrom(body.unit, [&](uint32_t symbol, PCRelativeReach reach) {
            if (!needsCopy(symbol, reach))
                return;
            seenSymbols.set(symbol);
            addToGroup(group, symbol);
        });
    }

    uint32_t firstRegExp = m_bodies[firstBody].regExp;
    addClassTables(group.classTables.span(), firstRegExp);
    addBitPlanes(group.bitPlanes.span(), firstRegExp);

    // Targets are taken now: a later group gives the same table another target.
    for (uint32_t symbol : group.routines)
        m_unitInfos[m_symbolStates[symbol].target->unit].targets = targetsOf(routineUnit(symbol));
    for (size_t bodyIndex : group.bodies)
        addBody(m_bodies[bodyIndex]);
    return index;
}

// A thunk is bound to a literal before anybody knows whether the literal gets code, and it takes JITCodeFailure to the generic operation.
bool SectionBuilder::addFailureRoutine()
{
    auto withoutCode = std::ranges::find_if(m_regExps, [](const CompiledRegExp& regExp) {
        return regExp.form != RegExpRecordForm::Code || !regExp.code[0] || !regExp.code[1];
    });
    if (withoutCode == m_regExps.end())
        return true;

    auto code = compileFailureRoutine(m_policy, m_context);
    if (!code) {
        if (m_context)
            m_context->diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode });
        return false;
    }
    m_failureRoutine = WTF::move(*code);
    m_failureRoutineUnit = addUnit(Kind::Routine, UnitSharing::Unique, routineAlignment, m_failureRoutine->body, static_cast<uint32_t>(withoutCode - m_regExps.begin()));
    return true;
}

void SectionBuilder::addClassTables(std::span<const uint32_t> symbols, uint32_t regExp)
{
    if (symbols.empty())
        return;

    Yarr::ClassTablePacker packer(m_maxClusterSize);
    Vector<size_t> tables(symbols.size(), [&](size_t index) {
        return packer.add(std::get<Yarr::ClassTable>(m_symbols.at(symbols[index])));
    });
    packer.pack();

    uint32_t unit = addUnit(Kind::Table, UnitSharing::Copyable, sectionDataAlignment, ownBytes(Vector<uint8_t>(packer.bytes())), regExp);
    for (size_t index = 0; index < symbols.size(); ++index)
        m_symbolStates[symbols[index]].target = UnitTarget { unit, packer.offsetOf(tables[index]) };
}

void SectionBuilder::addBitPlanes(std::span<const uint32_t> symbols, uint32_t regExp)
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
            ByteTable table { addUnit(Kind::Table, UnitSharing::Copyable, sectionDataAlignment, bytes, regExp), bytes };
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

void SectionBuilder::addBody(const Body& body)
{
    uint32_t unit = addUnit(Kind::Body, UnitSharing::Unique, bodyAlignment, body.unit.code->body, body.regExp);
    m_unitInfos[unit].targets = targetsOf(body.unit);
    m_unitOfBody[body.number] = unit;
}

RegExpSection SectionBuilder::createSection(LinkedSection&& linked) const
{
    // A veneer clobbers a register that a routine may take an argument in. Only routines are called, and they are copied.
    RELEASE_ASSERT(!linked.numberOfVeneers);

    auto sizeOf = [&](uint32_t unit) {
        return static_cast<uint32_t>(m_units[unit].bytes.size());
    };

    RegExpSection section;
    section.bytes = WTF::move(linked.bytes);
    uint32_t entryWithoutCode = m_failureRoutineUnit ? linked.uniqueUnitOffsets[*m_failureRoutineUnit] : 0;
    section.entries.fill({ entryWithoutCode, entryWithoutCode }, m_regExps.size());
    for (uint32_t unit = 0; unit < m_units.size(); ++unit) {
        if (m_units[unit].sharing == UnitSharing::Unique)
            section.items.append({ m_unitInfos[unit].kind, linked.uniqueUnitOffsets[unit], sizeOf(unit), m_unitInfos[unit].regExp });
    }
    for (const PlacedUnit& copy : linked.copies)
        section.items.append({ m_unitInfos[copy.unit].kind, copy.offset, sizeOf(copy.unit), m_unitInfos[copy.unit].regExp });

    BitVector seenBodies(m_unitOfBody.size());
    for (const Body& body : m_bodies) {
        uint32_t unit = *m_unitOfBody[body.number];
        uint32_t offset = linked.uniqueUnitOffsets[unit];
        section.entries[body.regExp][body.charSizeIndex] = offset;
        if (seenBodies.set(body.number))
            section.items.append({ Kind::DuplicateBody, offset, sizeOf(unit), body.regExp });
    }
    AOT::stableSort(section.items, { }, &RegExpSectionItem::offset);
    return section;
}

void SectionBuilder::reportFailure(const SectionLinkFailure& failure)
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

std::optional<RegExpSection> layOutRegExpSection(std::span<const CompiledRegExp> regExps, const AssemblerPolicy& policy, const CodeTemplates& templates, BuildContext* context)
{
    return SectionBuilder(regExps, policy, templates, context).build();
}

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
