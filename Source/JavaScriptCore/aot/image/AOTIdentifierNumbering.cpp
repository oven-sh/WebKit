/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIdentifierNumbering.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTPropertyCachesFront.h"
#include "AOTThreads.h"
#include <algorithm>
#include <span>

namespace JSC { namespace AOT {

static_assert(!RowDisplacementTable::rowMarkerColumn);
static_assert(defaultLimitValue(LimitID::LookupTableNames, hostArchitecture()) == RowDisplacementTable::maxColumn);
static_assert(StringID(0u) < StringID(), "a symbol without a description sorts last");
static_assert(PropertyKeyID(0u) < PropertyKeyID(), "a name without a key sorts last");

namespace {

struct NameCandidate {
    NumberedIdentifier identifier;
    bool hasPromisedColumn { false };
    bool isEngineSymbol { false };
    uint32_t uses { 0 };
    uint32_t tableLookupUses { 0 };
    FunctionID firstTableLookupFunction { };
};

} // anonymous namespace

std::optional<uint32_t> IdentifierNumbering::numberOf(PropertyKeyID key) const
{
    if (!key || key.index() >= m_numbersByKey.size() || !m_numbersByKey[key.index()])
        return std::nullopt;
    return m_numbersByKey[key.index()];
}

std::optional<uint32_t> IdentifierNumbering::numberOfText(StringID text) const
{
    if (!text || text.index() >= m_numbersByText.size() || !m_numbersByText[text.index()])
        return std::nullopt;
    return m_numbersByText[text.index()];
}

const NumberedIdentifier& IdentifierNumbering::at(uint32_t number) const
{
    RELEASE_ASSERT(number < count());
    return m_identifiers[number];
}

static bool isFoundByText(const Typed::Program& program, const NumberedIdentifier& identifier)
{
    return !identifier.key || program.at(identifier.key).kind == PropertyKeyKind::Text;
}

// The run time creates the identifiers of all of these in one step, before it looks up the first uid.
static bool isEngineSymbol(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::EnginePrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<NameCandidate> collectNumberedNames(const Typed::Program& program, const KeyUses& uses, const BitVector& keysWithColumn)
{
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    RELEASE_ASSERT(uses.byKey.size() == numberOfKeys);

    Vector<NameCandidate> names;
    BitVector isTextOfNumberedKey(program.strings.size());
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        PropertyKeyID key(index);
        const KeyUse& use = uses.byKey[index];
        if (!use.uses && !uses.keysNamedByTables.get(index) && !program.builtinNameOf(key))
            continue;
        NumberedIdentifier identifier { key, program.at(key).text };
        if (isFoundByText(program, identifier))
            isTextOfNumberedKey.quickSet(identifier.text.index());
        names.append(NameCandidate {
            .identifier = identifier,
            .hasPromisedColumn = keysWithColumn.get(index),
            .isEngineSymbol = isEngineSymbol(program.at(key).kind),
            .uses = use.uses,
            .tableLookupUses = use.tableLookupUses,
            .firstTableLookupFunction = use.firstTableLookupFunction,
        });
    }
    for (size_t index : uses.stringsNamedAsKeysByTables) {
        if (!isTextOfNumberedKey.get(index))
            names.append(NameCandidate { .identifier = { PropertyKeyID(), StringID(index) } });
    }
    return names;
}

static void sortByUse(const ParallelLoops& loops, std::span<NameCandidate> names)
{
    parallelSort(loops, names, [](const NameCandidate& a, const NameCandidate& b) {
        if (a.hasPromisedColumn != b.hasPromisedColumn)
            return a.hasPromisedColumn;
        if (a.tableLookupUses != b.tableLookupUses)
            return a.tableLookupUses > b.tableLookupUses;
        if (a.uses != b.uses)
            return a.uses > b.uses;
        if (a.identifier.key != b.identifier.key)
            return a.identifier.key < b.identifier.key;
        return a.identifier.text < b.identifier.text;
    });
}

// numberForRank scatters the hot names, so they stay sorted by use. The others are grouped by the first function to look them up, so the cells it reads of one structure share cache lines.
static void groupColumnsByFunction(std::span<NameCandidate> namesWithColumn)
{
    constexpr size_t hotNameCount = RowDisplacementTable::numberOfHotNames - 1;
    if (namesWithColumn.size() <= hotNameCount)
        return;
    static_assert(FunctionID(0u) < FunctionID(), "a name that no function looks up in the table sorts last");
    auto remainingNamesWithColumn = namesWithColumn.subspan(hotNameCount);
    std::stable_sort(remainingNamesWithColumn.begin(), remainingNamesWithColumn.end(), [](const NameCandidate& a, const NameCandidate& b) {
        return a.firstTableLookupFunction < b.firstTableLookupFunction;
    });
}

static void reportNamesWithoutColumn(std::span<const NameCandidate> namesWithoutColumn, BuildContext& context)
{
    auto isLookedUp = [](const NameCandidate& name) {
        return name.tableLookupUses > 0;
    };
    auto mostLookedUp = std::ranges::find_if(namesWithoutColumn, isLookedUp);
    if (mostLookedUp == namesWithoutColumn.end())
        return;
    auto lookedUpCount = static_cast<uint64_t>(std::ranges::count_if(namesWithoutColumn, isLookedUp));
    context.gaveUp(LimitID::LookupTableNames, { .function = mostLookedUp->firstTableLookupFunction });
    context.statistics().count(hitsOf(LimitID::LookupTableNames), lookedUpCount - 1);
}

// The front end numbers the strings in the order in which the program's text first mentions them, file by file, and the engine adds its own behind them.
static void sortByFirstMention(const ParallelLoops& loops, std::span<NameCandidate> namesWithoutColumn)
{
    parallelSort(loops, namesWithoutColumn, [](const NameCandidate& a, const NameCandidate& b) {
        if (a.isEngineSymbol != b.isEngineSymbol)
            return a.isEngineSymbol;
        if (a.identifier.text != b.identifier.text)
            return a.identifier.text < b.identifier.text;
        return a.identifier.key < b.identifier.key;
    });
}

IdentifierNumbering numberIdentifiers(const Typed::Program& program, const KeyUses& uses, const BitVector& keysWithColumn, BuildContext& context)
{
    Vector<NameCandidate> names = collectNumberedNames(program, uses, keysWithColumn);
    sortByUse(context.parallelLoops(), names.mutableSpan());
    auto columnCount = static_cast<uint32_t>(std::min<uint64_t>(names.size(), context.limit(LimitID::LookupTableNames)));
    groupColumnsByFunction(names.mutableSpan().first(columnCount));
    reportNamesWithoutColumn(names.span().subspan(columnCount), context);
    sortByFirstMention(context.parallelLoops(), names.mutableSpan().subspan(columnCount));

    IdentifierNumbering numbering;
    numbering.m_numbersByKey.fill(0, program.types.numberOfPropertyKeys());
    numbering.m_numbersByText.fill(0, program.strings.size());
    numbering.m_identifiers.grow(names.size() + 1);
    for (uint32_t index = 0; index < names.size(); ++index) {
        const NumberedIdentifier& identifier = names[index].identifier;
        uint32_t number = RowDisplacementTable::numberForRank(index + 1, columnCount);
        numbering.m_identifiers[number] = identifier;
        if (identifier.key)
            numbering.m_numbersByKey[identifier.key.index()] = number;
        if (isFoundByText(program, identifier))
            numbering.m_numbersByText[identifier.text.index()] = number;
    }
    numbering.m_countWithLookupTableColumn = columnCount + 1;

    context.statistics().count(StatisticID::IdentifiersNumbered, names.size());
    if (context.exceeds(LimitID::MaxIdentifiers, numbering.count()))
        context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyIdentifiers, .limit = LimitID::MaxIdentifiers, .count = numbering.count() });
    return numbering;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
