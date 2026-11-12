/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumbering.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTInstanceLayout.h"
#include "AOTLimits.h"
#include "AOTNameClass.h"
#include "AOTOperationData.h"
#include "AOTPlan.h"
#include "AOTRuntimeWords.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <compare>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

template<typename... Records>
static constexpr bool requestRecordsAreWholeWords = (!(sizeof(Records) % sizeof(uint32_t)) && ...);

static_assert(requestRecordsAreWholeWords<NewClassRequest, TemplateObjectRequest, TemplateObjectRequest::Part, DirectEvalRequest, EvalBindingData, DataRequest, CollectionDataRequest>);

static constexpr uint32_t offsetOfFirstNumberedRequest = sizeof(uint32_t);

namespace {

struct NumberedBuiltinCells {
    Vector<BuiltinCellKey> cells;
    Vector<uint32_t> eagerCells;
    Vector<BuiltinID> membersAskedByByteAlone;
};

} // anonymous namespace

static Vector<TypeID> collectTypesWithRecord(const Typed::Program& program, const GatheredMentions& mentions, const NamesOfTables& tables)
{
    Typed::TypeReader reader(program);
    Vector<TypeID> types;
    BitVector isListed;
    auto add = [&](TypeID type) {
        if (!type || Typed::liesInThread(type))
            return;
        if (Typed::liesInProgram(type)) {
            size_t position = Typed::positionOf(type);
            if (position >= isListed.size())
                isListed.ensureSize(std::max(position + 1, 2 * isListed.size()));
            if (isListed.quickSet(position))
                return;
        }
        types.append(type);
    };

    add(Typed::TypeTable::anything);
    add(Typed::TypeTable::number);
    const NumberedKeys& mentioned = mentions.keys(NumberSpace::Type);
    for (uint32_t index = 0; index < mentioned.size(); ++index) {
        std::span<const uint32_t> key = mentioned.keyAt(index);
        RELEASE_ASSERT(key.size() == 1);
        add(TypeID::fromBits(key[0]));
    }
    for (TypeID type : tables.types)
        add(type);

    for (size_t index = 0; index < types.size(); ++index) {
        if (Typed::holdsOnlyPrimitives(types[index]))
            continue;
        Typed::Type type = reader.at(types[index]);
        for (const Typed::ClassPart& part : Typed::classPartsOf(reader, type)) {
            for (TypeID contents : part.writtenContents)
                add(contents);
        }
    }
    return types;
}

static void sortBuiltinCellsAndRemoveKeysWithoutCell(const BuiltinTable& builtins, Vector<BuiltinCellKey>& cells)
{
    AOT::sortInLine(cells, { }, &BuiltinCellKey::encoded);
    removeRepeatedElements(cells);
    cells.removeAllMatching([&](BuiltinCellKey key) {
        return !key.hasCell(builtins);
    });
}

static void appendCellsNamedByTypeRecords(const Typed::Program& program, std::span<const TypeID> typesWithRecord, Vector<BuiltinCellKey>& cells)
{
    const BuiltinTable& builtins = program.builtins();
    Typed::TypeReader reader(program);
    for (TypeID id : typesWithRecord) {
        if (Typed::holdsOnlyPrimitives(id))
            continue;
        Typed::Type type = reader.at(id);
        for (const Typed::ClassPart& part : Typed::classPartsOf(reader, type)) {
            const BuiltinClass& builtinClass = builtins.at(part.builtinClass);
            if (builtinClass.constructor)
                cells.append(BuiltinCellKey::object(builtinClass.constructor));
            if (builtinClass.prototype)
                cells.append(BuiltinCellKey::object(builtinClass.prototype));
        }
        Typed::forEachBuiltinOf(reader, type, [&](BuiltinID builtin) {
            cells.append(BuiltinCellKey::object(builtin));
        });
    }
}

static bool isLockedLocationOfBuiltinCell(const BuiltinTable& builtins, BuiltinID member)
{
    return builtins.at(member).mutability == BuiltinMutability::AlwaysLocked || builtins.isLocationOfLockedObject(member);
}

static bool stepToBuiltinCellMayBeReplaced(const BuiltinTable& builtins, const Typed::Facts& facts, BuiltinID member)
{
    const Builtin& entry = builtins.at(member);
    if (!entry.holder || !entry.name || isLockedLocationOfBuiltinCell(builtins, member))
        return false;
    if (entry.flags.contains(BuiltinFlag::IsCompilerHelper))
        return false;
    return facts.builtinLocation(member) != Typed::BuiltinLocationState::Constant;
}

static std::optional<BuiltinCellKey> findFirstReplaceableStepToCell(const BuiltinTable& builtins, const Typed::Facts& facts, BuiltinCellKey key)
{
    BuiltinCellKey first = key.kind() == BuiltinCellKey::Kind::Value ? BuiltinCellKey::object(builtins.at(key.builtin()).holder) : key;
    for (BuiltinCellKey step = first; step.builtin(); step = BuiltinCellKey::object(builtins.at(step.builtin()).holder)) {
        if (stepToBuiltinCellMayBeReplaced(builtins, facts, step.builtin()))
            return step;
        if (isLockedLocationOfBuiltinCell(builtins, step.builtin()))
            break;
    }
    return std::nullopt;
}

static bool firstReadOfCellChangesListedKeys(const BuiltinTable& builtins, const Typed::Facts& facts, BuiltinID member)
{
    return builtins.at(member).isAddedAtFirstRead && facts.listsKeysOf(builtins.canonicalID(builtins.at(member).holder));
}

static NumberedBuiltinCells numberBuiltinCellsOfImage(const NumberingInputs& inputs, const GatheredMentions& mentions, const NamesOfTables& tables)
{
    const Typed::Program& program = inputs.program;
    const Typed::Facts& facts = inputs.facts;
    const BuiltinTable& builtins = program.builtins();

    Vector<BuiltinCellKey> cells;
    for (size_t encoded : mentions.byID(NumberSpace::BuiltinCell).isMentioned) {
        if (std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(static_cast<uint32_t>(encoded), builtins))
            cells.append(*key);
    }
    Vector<BuiltinID> membersAskedByByteAlone;
    for (BuiltinCellKey key : cells) {
        if (key.kind() != BuiltinCellKey::Kind::Getter || key.hasCell(builtins))
            continue;
        if (isMemberAskedByByteAlone(builtins, key.builtin()))
            membersAskedByByteAlone.append(key.builtin());
        else
            RELEASE_ASSERT(program.admitsReplacedBuiltins);
    }
    appendCellsNamedByTypeRecords(program, collectTypesWithRecord(program, mentions, tables).span(), cells);
    sortBuiltinCellsAndRemoveKeysWithoutCell(builtins, cells);

    Vector<BuiltinCellKey> eagerCells;
    for (BuiltinCellKey key : cells) {
        if (std::optional<BuiltinCellKey> step = findFirstReplaceableStepToCell(builtins, facts, key))
            eagerCells.append(*step);
    }
    sortBuiltinCellsAndRemoveKeysWithoutCell(builtins, eagerCells);

    cells.appendVector(eagerCells);
    for (const auto& use : facts.builtinUses())
        cells.append(BuiltinCellKey::object(BuiltinID(use.builtin)));
    for (size_t index = 0, size = cells.size(); index < size; ++index) {
        BuiltinCellKey key = cells[index];
        if (key.kind() == BuiltinCellKey::Kind::Object && firstReadOfCellChangesListedKeys(builtins, facts, key.builtin()))
            cells.append(BuiltinCellKey::value(key.builtin()));
    }
    sortBuiltinCellsAndRemoveKeysWithoutCell(builtins, cells);

    Vector<uint32_t> placesOfEagerCells = eagerCells.map([&](BuiltinCellKey key) {
        return static_cast<uint32_t>(std::ranges::lower_bound(cells, key.encoded(), { }, &BuiltinCellKey::encoded) - cells.begin());
    });
    return { WTF::move(cells), WTF::move(placesOfEagerCells), WTF::move(membersAskedByByteAlone) };
}

static Vector<PropertyKeyID> listKeyCellsOfImage(const Typed::Program& program, const GatheredMentions& mentions)
{
    const Typed::TypeTable& types = program.types;
    unsigned numberOfKeys = types.numberOfPropertyKeys();
    BitVector hasCell = mentions.byID(NumberSpace::KeyCell).isMentioned;
    hasCell.ensureSize(numberOfKeys);
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        if (Typed::isSymbolKind(types.at(PropertyKeyID(index)).kind))
            hasCell.quickSet(index);
    }

    Vector<PropertyKeyID> keys;
    keys.reserveInitialCapacity(hasCell.bitCount());
    for (size_t index : hasCell)
        keys.append(PropertyKeyID(index));

    size_t numberOfStableKeys = std::ranges::partition_point(keys, [&](PropertyKeyID key) {
        return types.hasStableNumber(key);
    }) - keys.begin();
    AOT::sort(keys.mutableSpan().subspan(numberOfStableKeys), [&](PropertyKeyID first, PropertyKeyID second) {
        return std::is_lt(types.compareByContent(first, second));
    });
    AOT::sort(keys, { }, [&](PropertyKeyID key) {
        return mentions.firstFunctionMentioning(NumberSpace::KeyCell, key.index());
    });
    return keys;
}

static uint64_t sizeInBytesOfRequestKey(const Typed::Program& program, std::span<const uint32_t> key)
{
    RELEASE_ASSERT(!key.empty());
    std::span<const uint32_t> words = key.subspan(1);
    switch (static_cast<RequestKind>(key[0])) {
    case RequestKind::NewClass:
        RELEASE_ASSERT(words.size() == 1);
        return sizeof(NewClassRequest);
    case RequestKind::TemplateObject:
        RELEASE_ASSERT(!(words.size() % 2));
        return sizeof(TemplateObjectRequest) + sizeof(TemplateObjectRequest::Part) * (words.size() / 2);
    case RequestKind::DirectEval:
        RELEASE_ASSERT(words.size() == 2);
        return sizeof(DirectEvalRequest) + (sizeof(EvalBindingData) + sizeof(uint32_t)) * program.at(Typed::EvalScopeID(words[0])).bindings.size();
    case RequestKind::Data:
        RELEASE_ASSERT(words.size() == 2);
        return sizeof(DataRequest);
    case RequestKind::CollectionData:
        RELEASE_ASSERT(words.size() == 2);
        return sizeof(CollectionDataRequest);
    case RequestKind::FormatPattern:
        return words.size_bytes();
    case RequestKind::ImportChecks:
        RELEASE_ASSERT(!(words.size() % 2));
        return sizeof(ImportChecksRequest) + sizeof(ImportChecksRequest::Check) * (words.size() / 2);
    case RequestKind::Prefetch:
        RELEASE_ASSERT(!(words.size() % 2));
        return sizeof(PrefetchRequest) + sizeof(PrefetchRequest::Step) * (words.size() / 2);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static NumberedKeys uniteNumberedKeys(const ParallelLoops& loops, NumberedKeys&& ascending, std::span<const uint32_t> listed)
{
    if (listed.empty())
        return WTF::move(ascending);

    auto comesFirst = [](std::span<const uint32_t> first, std::span<const uint32_t> second) {
        return std::ranges::lexicographical_compare(first, second);
    };
    Vector<std::span<const uint32_t>> listedKeys;
    while (!listed.empty()) {
        size_t numberOfWords = listed[0];
        listedKeys.append(listed.subspan(1, numberOfWords));
        listed = listed.subspan(1 + numberOfWords);
    }
    sortAndRemoveDuplicates(loops, listedKeys, comesFirst, [](std::span<const uint32_t> first, std::span<const uint32_t> second) {
        return std::ranges::equal(first, second);
    });

    NumberedKeys united;
    united.words.reserveInitialCapacity(ascending.words.size());
    united.firstWord.reserveInitialCapacity(ascending.size() + listedKeys.size() + 1);
    united.firstWord.append(0);
    auto append = [&](std::span<const uint32_t> key) {
        united.words.append(key);
        united.firstWord.append(static_cast<uint32_t>(united.words.size()));
    };
    size_t nextListed = 0;
    for (uint32_t index = 0; index < ascending.size(); ++index) {
        std::span<const uint32_t> key = ascending.keyAt(index);
        for (; nextListed < listedKeys.size() && comesFirst(listedKeys[nextListed], key); ++nextListed)
            append(listedKeys[nextListed]);
        if (nextListed < listedKeys.size() && std::ranges::equal(listedKeys[nextListed], key))
            ++nextListed;
        append(key);
    }
    for (; nextListed < listedKeys.size(); ++nextListed)
        append(listedKeys[nextListed]);
    united.buildHashIndex(loops);
    return united;
}

static bool isAllowedInPropertyRun(const Typed::Program& program, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case Typed::PropertyKeyKind::EnginePrivateName:
    case Typed::PropertyKeyKind::ModulePrivateName:
    case Typed::PropertyKeyKind::PrivateName:
        return false;
    case Typed::PropertyKeyKind::Text:
    case Typed::PropertyKeyKind::WellKnownSymbol:
    case Typed::PropertyKeyKind::CreatedSymbol:
    case Typed::PropertyKeyKind::RegisteredSymbol:
        break;
    }
    switch (program.nameClassOf(key)) {
    case Typed::NameClass::Index:
    case Typed::NameClass::CanonicalNumericString:
    case Typed::NameClass::Proto:
        return false;
    case Typed::NameClass::Ordinary:
    case Typed::NameClass::Constructor:
    case Typed::NameClass::ObjectPrototypeMember:
    case Typed::NameClass::Length:
    case Typed::NameClass::Prototype:
    case Typed::NameClass::CallOrApply:
    case Typed::NameClass::Bind:
    case Typed::NameClass::LastIndex:
    case Typed::NameClass::Then:
    case Typed::NameClass::ToJSON:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool canBePropertyRun(const Typed::Program& program, std::span<const uint32_t> keys)
{
    if (keys.empty() || keys.size() > defaultLimitValue(LimitID::MaxPropertyListNames, hostArchitecture()))
        return false;
    Vector<uint32_t, 8> sortedKeys(keys);
    AOT::sort(sortedKeys);
    if (std::ranges::adjacent_find(sortedKeys) != sortedKeys.end())
        return false;
    return std::ranges::all_of(keys, [&](uint32_t key) {
        return isAllowedInPropertyRun(program, PropertyKeyID(key));
    });
}

void ImageNumbersBuilder::numberConstants()
{
    const Typed::Program& program = inputs.program;

    NumberedBuiltinCells builtinCells = numberBuiltinCellsOfImage(inputs, mentions, tables);
    if (!InstanceLayout::holdsBuiltinCells(builtinCells.cells.size())) {
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .count = static_cast<uint32_t>(builtinCells.cells.size()),
            .detail = "the program names more built-in cells than the instance has words for"_s,
        });
    }
    numbers.m_builtinCells = WTF::move(builtinCells.cells);
    numbers.m_eagerBuiltinCells = WTF::move(builtinCells.eagerCells);
    numbers.m_membersAskedByByteAlone = WTF::move(builtinCells.membersAskedByByteAlone);

    numbers.m_keyCells = listKeyCellsOfImage(program, mentions);
    numbers.m_keyCellNumbersPlusOne.fill(0, program.types.numberOfPropertyKeys());
    for (uint32_t number = 0; number < numbers.m_keyCells.size(); ++number)
        numbers.m_keyCellNumbersPlusOne[numbers.m_keyCells[number].index()] = number + 1;

    BitVector digitsOfBigInts = mentions.byID(NumberSpace::BigInt).isMentioned;
    digitsOfBigInts.merge(tables.bigInts);
    Vector<StringID> bigIntConstants;
    bigIntConstants.reserveInitialCapacity(digitsOfBigInts.bitCount());
    for (size_t index : digitsOfBigInts)
        bigIntConstants.append(StringID(index));
    numbers.m_bigIntConstants = WTF::move(bigIntConstants);

    const NumberedKeys& bindings = mentions.keys(NumberSpace::GlobalBinding);
    numbers.m_globalBindings = Vector<PlacedGlobalBinding>(bindings.size(), [&](size_t index) {
        std::span<const uint32_t> key = bindings.keyAt(static_cast<uint32_t>(index));
        RELEASE_ASSERT(key.size() == 2);
        PropertyKeyID name(key[0]);
        auto flags = OptionSet<GlobalBindingFlags>::fromRaw(static_cast<uint8_t>(key[1]));
        Typed::GlobalNameUse use = inputs.plan.globalName(name);
        bool readLooksAtLocationAlone = !flags.containsAny({ GlobalBindingFlags::IsStore, GlobalBindingFlags::IsForTypeOf }) && use.isStoredThroughBinding;
        return PlacedGlobalBinding { name, flags, use.isStoredByName, readLooksAtLocationAlone };
    });

    const NumberedKeys& edges = mentions.keys(NumberSpace::RequireEdge);
    numbers.m_requireEdges = Vector<RequireEdge>(edges.size(), [&](size_t index) {
        std::span<const uint32_t> key = edges.keyAt(static_cast<uint32_t>(index));
        RELEASE_ASSERT(key.size() == 3);
        return RequireEdge { ModuleID(key[0]), ModuleID(key[1]), StringID(key[2]) };
    });
}

void ImageNumbersBuilder::numberRows()
{
    const Typed::Program& program = inputs.program;

    numbers.m_allocations = uniteNumberedKeys(context.parallelLoops(), mentions.takeKeys(NumberSpace::Allocation), tables.allocations.span());
    if (context.exceeds(LimitID::ClosureAllocationSites, numbers.m_allocations.size()))
        context.reportLimitExceeded(LimitID::ClosureAllocationSites, numbers.m_allocations.size());

    numbers.m_propertyRuns = mentions.takeKeys(NumberSpace::PropertyRun);
    for (uint32_t index = 0; index < numbers.m_propertyRuns.size(); ++index) {
        if (canBePropertyRun(program, numbers.m_propertyRuns.keyAt(index)))
            continue;
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .count = index,
            .detail = "code names a run of properties that is empty or too long, repeats a key or holds a key that is no ordinary name"_s,
        });
    }

    numbers.m_requests = mentions.takeKeys(NumberSpace::Request);
    uint32_t numberOfRequests = numbers.m_requests.size();
    uint64_t end = offsetOfFirstNumberedRequest;
    Vector<uint32_t> offsetsOfRequests;
    offsetsOfRequests.reserveInitialCapacity(numberOfRequests);
    for (uint32_t index = 0; index < numberOfRequests; ++index) {
        offsetsOfRequests.append(static_cast<uint32_t>(end));
        end += sizeInBytesOfRequestKey(program, numbers.m_requests.keyAt(index));
        if (end > UINT32_MAX) {
            context.reportLimitExceeded(LimitID::TableBytes, end);
            return;
        }
    }
    numbers.m_offsetsOfRequests = WTF::move(offsetsOfRequests);
    numbers.m_sizeOfRequests = static_cast<uint32_t>(end);
}

void ImageNumbersBuilder::numberTypes()
{
    const ParallelLoops& loops = context.parallelLoops();
    Vector<TypeID> types = collectTypesWithRecord(inputs.program, mentions, tables);
    parallelSort(loops, types.mutableSpan(), [](TypeID first, TypeID second) {
        return first.bits() < second.bits();
    });
    removeRepeatedElements(types);
    Typed::TypeArena arena(inputs.program.types, Typed::ArenaKeeps::UntilReset);
    AOT::sort(types, [&](TypeID first, TypeID second) {
        return arena.compareByContent(first, second) < 0;
    });

    size_t numberOfSlots = std::bit_ceil(2 * types.size() + 2);
    numbers.m_placesOfTypes.fill(ImageNumbers::PlaceOfType { 0, 0 }, numberOfSlots);
    for (size_t place = 0; place < types.size(); ++place) {
        size_t slot = ImageNumbers::firstSlotOfType(types[place].bits(), numberOfSlots);
        while (numbers.m_placesOfTypes[slot].placePlusOne)
            slot = (slot + 1) & (numberOfSlots - 1);
        numbers.m_placesOfTypes[slot] = { types[place].bits(), static_cast<uint32_t>(place) + 1 };
    }
    numbers.m_typesNeededAtRunTime = WTF::move(types);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
