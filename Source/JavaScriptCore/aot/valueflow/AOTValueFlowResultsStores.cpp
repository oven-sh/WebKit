/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsFieldsInternal.h"

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include "AOTThreshold.h"

namespace JSC { namespace AOT { namespace Typed {

LayoutID LayoutsOfObjects::namedLayoutOf(ValueFlowObjectID object) const
{
    ValueFlowObjectKind kind = m_inputs.universe().kindOf(object);
    return kind == ValueFlowObjectKind::Site || kind == ValueFlowObjectKind::Instance ? m_inputs.universe().layoutOf(object) : LayoutID();
}

bool LayoutsOfObjects::selectsLayoutByPrototype(ValueFlowObjectID object) const
{
    const ValueFlowSite* site = m_inputs.universe().siteOf(object);
    if (!site || !site->layout || site->ordinal || !m_inputs.program.prototypeMaySelectOtherClass(site->layout))
        return false;
    const Value& allocation = m_inputs.program.at(m_inputs.universe().functionOf(object)).at(site->value);
    if (site->origin == ValueFlowSiteOrigin::BuiltinResult)
        return OperandPosition::parent(allocation.opcode).has_value();
    return site->origin == ValueFlowSiteOrigin::Operation && allocation.opcode == Opcode::NewObject && allocation.prototypeOperand();
}

LayoutsOfObjects::LayoutsOfObjects(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap)
    : m_inputs(inputs)
{
    struct ObjectOfLayout {
        LayoutID layout;
        ValueFlowObjectID object;
    };

    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    const ParallelLoops& loops = inputs.loops();
    ValueSetMaker& maker = heap.maker();
    uint32_t numberOfLayouts = program.layouts.size();
    Vector<ValueFlowObjectID> objectsThatSelectLayout = collectInParallel<ValueFlowObjectID>(loops, universe.numberOfSiteObjects(), [&](unsigned position, Vector<ValueFlowObjectID>& objects) {
        if (namedLayoutOf(universe.siteObjectAt(position)) && selectsLayoutByPrototype(universe.siteObjectAt(position)))
            objects.append(universe.siteObjectAt(position));
    });
    Vector<ValueFlowObjectID> objectsOfEveryEscapedClass;
    Vector<ValueFlowObjectID> objectsOfEveryLostClass;
    if (!objectsThatSelectLayout.isEmpty()) {
        Vector<ValueFlowObjectID> prototypeObjects;
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            const Layout& layout = program.layouts[index];
            if (!isClassLayout(layout) || !layout.has(LayoutFlags::HasHeritage))
                continue;
            ValueFlowObjectID prototype = universe.prototypeOf(LayoutID(index));
            ValueFlowObjectID classObject = universe.closureOf(layout.constructor);
            prototypeObjects.append(prototype);
            if (sets.contains(heap.escaped(), prototype) || sets.contains(heap.escaped(), classObject))
                m_escapedClasses.append(LayoutID(index));
            if (sets.contains(heap.lost(), prototype) || sets.contains(heap.lost(), classObject))
                m_lostClasses.append(LayoutID(index));
        }
        ValueSetID prototypeObjectsOfClasses = maker.of(prototypeObjects.span());

        ValueSetID all = maker.of(objectsThatSelectLayout.span());
        ValueSetID throughUnknownObject = ValueSets::nothing;
        ValueSetID throughUntrackedObject = ValueSets::nothing;
        SetsByNumber links;
        heap.forEachNodeWrittenUnder(prototypeLinkName, [&](ValueSetID writtenThrough, ValueSetID value) {
            sets.forEachMember(maker.intersect(writtenThrough, all), [&](ValueFlowObjectID object) {
                ValueSetID& link = links.add(object.index(), ValueSets::nothing).iterator->value;
                link = maker.unite(link, value);
            });
            if (sets.contains(writtenThrough, ValueFlowUniverse::unknownObject()))
                throughUnknownObject = maker.unite(throughUnknownObject, value);
            if (sets.contains(writtenThrough, ValueFlowUniverse::untrackedObject()))
                throughUntrackedObject = maker.unite(throughUntrackedObject, value);
        });
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> listOfPrototypes;
        for (ValueFlowObjectID object : objectsThatSelectLayout) {
            ValueSetID prototypes = setAt(links, object.index());
            if (sets.contains(heap.escaped(), object))
                prototypes = maker.unite(prototypes, throughUnknownObject);
            if (sets.contains(heap.lost(), object))
                prototypes = maker.unite(prototypes, throughUntrackedObject);
            SelectedLayouts selected;
            ValueSetID prototypesOfClasses = maker.intersect(prototypes, prototypeObjectsOfClasses);
            selected.list = listOfPrototypes.ensure(prototypesOfClasses.index(), [&] {
                Vector<LayoutID> listed;
                sets.forEachMember(prototypesOfClasses, [&](ValueFlowObjectID prototype) {
                    listed.append(universe.classLayoutOf(prototype));
                });
                AOT::sort(listed);
                removeRepeatedElements(listed);
                m_listsOfSelectedLayouts.append(WTF::move(listed));
                return m_listsOfSelectedLayouts.size() - 1;
            }).iterator->value;
            const Vector<LayoutID>& listed = m_listsOfSelectedLayouts[selected.list];
            bool listsOtherLayout = listed.size() > 1 || (listed.size() == 1 && listed[0] != namedLayoutOf(object));
            selected.everyEscapedClass = !m_escapedClasses.isEmpty() && sets.contains(prototypes, ValueFlowUniverse::unknownObject());
            selected.everyLostClass = !m_lostClasses.isEmpty() && sets.contains(prototypes, ValueFlowUniverse::untrackedObject());
            if (selected.everyEscapedClass)
                objectsOfEveryEscapedClass.append(object);
            if (selected.everyLostClass)
                objectsOfEveryLostClass.append(object);
            if (listsOtherLayout || selected.everyEscapedClass || selected.everyLostClass)
                m_selectedLayouts.add(object.index(), selected);
        }
    }

    Vector<ObjectOfLayout> objectsWithLayouts = collectInParallel<ObjectOfLayout>(loops, universe.numberOfSiteAndLayoutObjects(), [&](unsigned position, Vector<ObjectOfLayout>& found) {
        ValueFlowObjectID object = universe.siteOrLayoutObjectAt(position);
        LayoutID named = namedLayoutOf(object);
        if (!named)
            return;
        found.append({ named, object });
        auto selected = m_selectedLayouts.find(object.index());
        if (selected == m_selectedLayouts.end())
            return;
        for (LayoutID layout : m_listsOfSelectedLayouts[selected->value.list]) {
            if (layout != named)
                found.append({ layout, object });
        }
    });
    Vector<uint32_t> firstObjectOfLayout;
    firstObjectOfLayout.fill(0u, numberOfLayouts + 1);
    for (const ObjectOfLayout& entry : objectsWithLayouts)
        ++firstObjectOfLayout[entry.layout.index() + 1];
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        firstObjectOfLayout[index + 1] += firstObjectOfLayout[index];
    Vector<ValueFlowObjectID> objectsOfLayouts(objectsWithLayouts.size());
    Vector<uint32_t> nextObjectOfLayout(firstObjectOfLayout.span().first(numberOfLayouts));
    for (const ObjectOfLayout& entry : objectsWithLayouts)
        objectsOfLayouts[nextObjectOfLayout[entry.layout.index()]++] = entry.object;

    m_objectsOfLayout.fill(ValueSets::nothing, numberOfLayouts);
    {
        ValueSetMakers makers(inputs);
        forEachIndexInParallel(loops, numberOfLayouts, [&](unsigned index, unsigned worker) {
            if (firstObjectOfLayout[index] != firstObjectOfLayout[index + 1])
                m_objectsOfLayout[index] = makers.of(worker).of(objectsOfLayouts.subspan(firstObjectOfLayout[index], firstObjectOfLayout[index + 1] - firstObjectOfLayout[index]));
        });
    }
    auto addToEach = [&](const Vector<LayoutID>& layouts, const Vector<ValueFlowObjectID>& objects) {
        if (objects.isEmpty())
            return;
        ValueSetID set = maker.of(objects.span());
        for (LayoutID layout : layouts)
            m_objectsOfLayout[layout.index()] = maker.unite(m_objectsOfLayout[layout.index()], set);
    };
    addToEach(m_escapedClasses, objectsOfEveryEscapedClass);
    addToEach(m_lostClasses, objectsOfEveryLostClass);
}

static Vector<PropertyKeyID> sortedKeysOf(const StoresIntoObjects& stores)
{
    Vector<PropertyKeyID> keys;
    keys.reserveInitialCapacity(stores.underKey.size());
    for (const auto& [key, stored] : stores.underKey)
        keys.append(PropertyKeyID(key));
    AOT::sort(keys);
    return keys;
}

static Vector<PropertyKeyID> sortedKeysNamedIn(const StoresIntoObjects& stores)
{
    Vector<PropertyKeyID> keys;
    for (const auto& [key, stored] : stores.underKey) {
        if (stored.isNamedByOperation)
            keys.append(PropertyKeyID(key));
    }
    AOT::sort(keys);
    return keys;
}

StoresByLayout::StoresByLayout(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, ValueFlowHeapView& heap, const LayoutsOfObjects& layoutsOfObjects)
    : m_inputs(inputs)
    , m_heap(heap)
    , m_layoutsOfObjects(layoutsOfObjects)
    , m_storesOfLayout(inputs.program.layouts.size())
    , m_hasEscapedObject(inputs.program.layouts.size())
    , m_hasLostObject(inputs.program.layouts.size())
    , m_isEscapedClass(inputs.program.layouts.size())
    , m_isLostClass(inputs.program.layouts.size())
    , m_hasObjectOfCodeWithoutSolve(inputs.program.layouts.size())
    , m_unknownValue(heap.maker().unite(inputs.universe().anyPrimitive(), inputs.universe().unknownObjectAlone()))
{
    const Program& program = inputs.program;
    for (LayoutID layout : layoutsOfObjects.escapedClasses())
        m_isEscapedClass.quickSet(layout.index());
    for (LayoutID layout : layoutsOfObjects.lostClasses())
        m_isLostClass.quickSet(layout.index());
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    findReceiversBehindChecks();
    times.didFinish("stores by layout: receivers behind checks"_s);
    listWrites();
    times.didFinish("stores by layout: writes listed"_s);
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (isCompiledWithoutSolve(inputs, tables, FunctionID(index)))
            noteCodeWithoutSolve(program.functions[index]);
    }
    auto noteLayoutsOfObjectsIn = [&](ValueSetID objects, BitVector& layouts) {
        auto note = [&](LayoutID layout) {
            layouts.quickSet(layout.index());
        };
        if (!ValueSets::isComplement(objects)) {
            layoutsOfObjects.forEachLayoutOfMembers(objects, note);
            return;
        }
        for (uint32_t index = 0; index < inputs.universe().numberOfObjects(); ++index) {
            if (inputs.sets().contains(objects, ValueFlowObjectID(index)))
                layoutsOfObjects.forEachLayoutOf(ValueFlowObjectID(index), note);
        }
    };
    noteLayoutsOfObjectsIn(heap.escaped(), m_hasEscapedObject);
    noteLayoutsOfObjectsIn(heap.lost(), m_hasLostObject);
    m_hasEscapedObject.merge(m_hasObjectOfCodeWithoutSolve);
    times.didFinish("stores by layout: code without solve, layouts of what escaped"_s);
}

void StoresByLayout::noteListedWrites()
{
    PhaseTimes times(m_inputs.build, PassID::AnalyzeFlow);
    noteWrites();
    times.didFinish("stores by layout: writes noted"_s);
    noteNamesStoredThroughDeclaredReceivers();
    times.didFinish("stores by layout: names through declared receivers"_s);
    m_keysThroughUnknownObject = sortedKeysOf(m_storesThroughUnknownObject);
    m_keysThroughUntrackedObject = sortedKeysOf(m_storesThroughUntrackedObject);
    m_keysIntoEveryObject = sortedKeysOf(m_storesIntoEveryObject);
    m_keysIntoEveryEscapedClass = sortedKeysOf(m_storesIntoEveryEscapedClass);
    m_keysIntoEveryLostClass = sortedKeysOf(m_storesIntoEveryLostClass);
    m_keysThroughPlaceholders.appendVector(m_keysThroughUnknownObject);
    m_keysThroughPlaceholders.appendVector(m_keysThroughUntrackedObject);
    m_keysThroughPlaceholders.appendVector(m_keysIntoEveryObject);
    m_keysThroughPlaceholders.appendVector(m_keysIntoEveryEscapedClass);
    m_keysThroughPlaceholders.appendVector(m_keysIntoEveryLostClass);
    AOT::sort(m_keysThroughPlaceholders);
    removeRepeatedElements(m_keysThroughPlaceholders);
    times.didFinish("stores by layout: keys sorted"_s);
    followCopiedProperties();
    followReadsOfFields();
    times.didFinish("stores by layout: copied properties"_s);
}

// The objects that are copied onto those of a layout, with what is copied onto them in turn, where the analysis follows each of them: whatever such an object holds
// was stored through a set that lists it, whoever else has its layout.
auto StoresByLayout::objectsCopiedOnto(ValueSetMaker& maker, LayoutID target) const -> std::optional<CopiedObjects>
{
    const ValueSets& sets = m_inputs.sets();
    ValueSetID objectsNotFollowed = maker.unite(m_inputs.universe().placeholdersAlone(), maker.unite(m_heap.escaped(), m_heap.lost()));
    ValueSetID sources = ValueSets::nothing;
    ValueSetID copiedOnto = m_layoutsOfObjects.objectsWith(target);
    while (true) {
        ValueSetID found = m_heap.writtenUnder(maker, copiedOnto, ValueFlowName::of(ValueFlowNameKind::CopiedFrom));
        if (ValueSets::isComplement(found) || sets.meet(found, objectsNotFollowed))
            return std::nullopt;
        if (sets.isSubset(found, sources))
            break;
        sources = maker.unite(sources, found);
        copiedOnto = found;
    }
    return withLayoutsIfFollowed(maker, sources);
}

auto StoresByLayout::withLayoutsIfFollowed(ValueSetMaker& maker, ValueSetID sources) const -> std::optional<CopiedObjects>
{
    const ValueSets& sets = m_inputs.sets();
    Vector<LayoutID, 4> layouts;
    bool holdOnlyWhatIsWritten = true;
    sets.forEachMember(sources, [&](ValueFlowObjectID source) {
        size_t numberOfOtherLayouts = layouts.size();
        m_layoutsOfObjects.forEachLayoutOf(source, [&](LayoutID layout) {
            layouts.append(layout);
        });
        holdOnlyWhatIsWritten &= layouts.size() != numberOfOtherLayouts && !m_inputs.classOf(source);
    });
    if (!holdOnlyWhatIsWritten)
        return std::nullopt;
    // The constructor at the top makes what is constructed below other classes, and a built-in class lays properties on it that no statement wrote. A function that a store hangs above an object has not made it.
    const ValueFlowHeapTables& tables = m_inputs.universe().builtinTables().heap();
    if (ValueSetID others = maker.intersect(sources, ValueSets::complement(tables.objectsWithoutPropertiesOfNativeCode())); others != ValueSets::nothing) {
        ValueSetID above = m_heap.chainOf(maker, maker.intersect(m_heap.prototypesOf(maker, others), ValueSets::complement(m_inputs.universe().closures())));
        if (ValueSets::isComplement(above) || sets.meet(above, maker.unite(m_inputs.universe().placeholdersAlone(), tables.prototypeObjectsOfClassesThatLayPropertiesOnInstances())))
            return std::nullopt;
    }
    AOT::sort(layouts);
    removeRepeatedElements(layouts);

    for (LayoutID layout : layouts) {
        LayoutKind kind = m_inputs.program.at(layout).kind;
        if (kind != LayoutKind::Record && kind != LayoutKind::Instance)
            return std::nullopt;
        if (m_hasObjectOfCodeWithoutSolve.quickGet(layout.index()))
            return std::nullopt;
    }
    auto isStoredUnder = [&](ValueFlowName name) {
        return m_heap.writtenUnder(maker, sources, name) != ValueSets::nothing;
    };
    constexpr std::array namesNobodyLists { ValueFlowNameKind::UnknownKey, ValueFlowNameKind::Elements, ValueFlowNameKind::GetterUnderUnknownKey, ValueFlowNameKind::SetterUnderUnknownKey };
    if (std::ranges::any_of(namesNobodyLists, [&](ValueFlowNameKind kind) { return isStoredUnder(ValueFlowName::of(kind)); }))
        return std::nullopt;
    for (LayoutID layout : layouts) {
        for (PropertyKeyID key : keysStoredInto(layout, ObjectsOfLayout::Followed)) {
            if (isStoredUnder(ValueFlowName::getter(m_inputs.program, key)) || isStoredUnder(ValueFlowName::setter(m_inputs.program, key)))
                return std::nullopt;
        }
    }
    return CopiedObjects { sources, WTF::move(layouts) };
}

void StoresByLayout::followCopiedProperties()
{
    Vector<LayoutID> targets;
    for (uint32_t index = 0; index < m_storesOfLayout.size(); ++index) {
        if (m_storesOfLayout[index].getsCopiedProperties)
            targets.append(LayoutID(index));
    }
    ValueSetMakers makers(m_inputs);
    Vector<std::optional<CopiedObjects>> sourcesOfTargets(targets.size());
    forEachIndexInParallel(m_inputs.loops(), targets.size(), [&](unsigned position, unsigned worker) {
        sourcesOfTargets[position] = objectsCopiedOnto(makers.of(worker), targets[position]);
    });
    for (size_t position = 0; position < targets.size(); ++position) {
        if (!sourcesOfTargets[position])
            continue;
        const CopiedObjects& sources = *sourcesOfTargets[position];
        StoresIntoObjects& stores = m_storesOfLayout[targets[position].index()];
        stores.getsCopiedProperties = false;
        for (LayoutID source : sources.layouts) {
            for (PropertyKeyID key : keysStoredInto(source, ObjectsOfLayout::Followed)) {
                if (stores.keysLeftOutOfEveryCopy && stores.keysLeftOutOfEveryCopy->contains(key))
                    continue;
                ValueSetID value = m_heap.writtenUnder(sources.objects, ValueFlowName::property(m_inputs.program, key));
                if (value == ValueSets::nothing)
                    continue;
                ValueFlowNumbers numbers = numbersStoredUnder(source, key, ObjectsOfLayout::Followed);
                // A copy reads the field, and a read of an int32 slot says no more than that: what it delivers goes into the target without a test.
                constexpr Range integersOfSlots { minInlineInt32, INT32_MAX };
                if (!numbers.integers.isEmpty() && numbers.integers.isSubsetOf(integersOfSlots))
                    numbers.integers = integersOfSlots;
                StoredUnderKey& stored = stores.underKey.add(key.index(), StoredUnderKey { }).iterator->value;
                unite(stored.value, value);
                stored.uniteNumbers(numbers);
                stored.isStoredAfterCreation = true;
                stored.isNamedByOperation |= !stores.getsCopyWithoutComputedKey;
                stored.isGivenValueByOperation = true;
                stored.isGivenValueByDefinition = true;
                stored.writers.add(Actor::KeysNobodyKnows);
            }
        }
    }
}

static ValueID valueStoredUnder(const Program&, const Value& operation, ValueFlowName);

// The read that produces what the statement stores, if it is a read by name from objects only.
std::optional<ReadOfFields> StoresByLayout::readOfFieldsStoredBy(ValueSetMaker& maker, const Writer& writer, ValueFlowName name) const
{
    const ValueFlowStatement& statement = writer.statement;
    if (statement.kind != ValueFlowStatementKind::Operation || name.kind != ValueFlowNameKind::Property)
        return std::nullopt;
    const Function& function = m_inputs.program.at(statement.function);
    ValueID stored = valueStoredUnder(m_inputs.program, function.at(statement.site), name);
    if (!stored || function.at(stored).opcode != Opcode::PropGet)
        return std::nullopt;
    std::span<const ValueFlowStatement> statements = m_inputs.bodies().statementsOf(statement.function.index());
    if (&statement < statements.data() || &statement >= statements.data() + statements.size())
        return std::nullopt;
    for (const ValueFlowStatement& read : statements.first(&statement - statements.data()) | std::views::reverse) {
        if (read.site != stored || read.kind != ValueFlowStatementKind::Operation || read.opcode != Opcode::PropGet)
            continue;
        ValueSetID receivers = m_heap.possibleValuesOf(maker, m_inputs.bodies().operandsOf(read)[OperandIndex::PropGet::object]);
        if (ValueSets::isComplement(receivers))
            return std::nullopt;
        // A read from undefined or null produces nothing.
        ValueSetID objects = maker.intersect(receivers, m_inputs.universe().objects());
        ValueSetID objectsAndNullish = maker.unite(objects, maker.of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null) }));
        if (objects == ValueSets::nothing || !m_inputs.sets().isSubset(receivers, objectsAndNullish))
            return std::nullopt;
        return ReadOfFields { name.payload, function.at(stored).key().index(), objects };
    }
    return std::nullopt;
}

// A read finds what was stored into the object, or what an object on its prototype chain holds.
auto StoresByLayout::objectsReadUnder(ValueSetMaker& maker, ValueSetID objects, PropertyKeyID key) const -> std::optional<CopiedObjects>
{
    const ValueSets& sets = m_inputs.sets();
    ValueSetID objectsNotFollowed = maker.unite(m_inputs.universe().placeholdersAlone(), maker.unite(m_heap.escaped(), m_heap.lost()));
    if (sets.meet(objects, objectsNotFollowed))
        return std::nullopt;
    std::optional<CopiedObjects> followed = withLayoutsIfFollowed(maker, objects);
    if (!followed)
        return std::nullopt;
    for (LayoutID layout : followed->layouts) {
        bool getsCopiedProperties = false;
        forEachSourceOf(layout, ObjectsOfLayout::Followed, [&](const StoresIntoObjects& stores) {
            getsCopiedProperties |= stores.getsCopiedProperties;
        });
        if (getsCopiedProperties)
            return std::nullopt;
    }
    ValueSetID above = m_heap.chainOf(maker, m_heap.prototypesOf(maker, objects));
    if (ValueSets::isComplement(above))
        return std::nullopt;
    // A locked object holds what the table lists, whatever reaches it.
    if (sets.meet(maker.intersect(above, ValueSets::complement(m_inputs.universe().lockedObjects())), objectsNotFollowed) || sets.meet(above, m_inputs.universe().builtinTables().heap().holdersThatAnswerUnder(key)))
        return std::nullopt;
    for (ValueFlowName name : { ValueFlowName::property(m_inputs.program, key), ValueFlowName::getter(m_inputs.program, key), ValueFlowName::of(ValueFlowNameKind::UnknownKey), ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey) }) {
        if (m_heap.writtenUnder(maker, above, name) != ValueSets::nothing)
            return std::nullopt;
    }
    return followed;
}

void StoresByLayout::followReadsOfFields()
{
    struct FollowedRead {
        StoredUnderKey* stored;
        PropertyKeyID key;
        Vector<LayoutID, 4> layouts;
    };
    Vector<FollowedRead> reads;
    Vector<StoredUnderKey*> waiting;
    ValueSetMaker& maker = m_heap.maker();
    auto take = [&](StoresIntoObjects& stores) {
        for (const ReadOfFields& read : stores.readsOfFields) {
            StoredUnderKey& stored = stores.underKey.add(read.storedUnder, StoredUnderKey { }).iterator->value;
            waiting.append(&stored);
            if (std::optional<CopiedObjects> followed = objectsReadUnder(maker, read.objects, PropertyKeyID(read.key)))
                reads.append({ &stored, PropertyKeyID(read.key), WTF::move(followed->layouts) });
            else
                stored.uniteNumbers(ValueFlowNumbers::any());
        }
    };
    for (StoresIntoObjects& stores : m_storesOfLayout)
        take(stores);
    for (StoresIntoObjects& stores : m_storesSharedByLayouts)
        take(stores);

    // A field may hold what a second field holds, which holds what a third does. No round adds a number that no store passes on, so the rounds terminate.
    for (bool didChange = !reads.isEmpty(); didChange;) {
        didChange = false;
        for (FollowedRead& read : reads) {
            ValueFlowNumbers numbers;
            for (LayoutID layout : read.layouts) {
                forEachSourceOf(layout, ObjectsOfLayout::Followed, [&](const StoresIntoObjects& stores) {
                    if (auto found = stores.underKey.find(read.key.index()); found != stores.underKey.end())
                        numbers.unite(found->value.numbersAddedSoFar());
                    numbers.unite(stores.numbersUnderUnknownKey);
                });
            }
            // As for a spread: a read of an int32 slot says no more than that.
            constexpr Range integersOfSlots { minInlineInt32, INT32_MAX };
            if (!numbers.integers.isEmpty() && numbers.integers.isSubsetOf(integersOfSlots))
                numbers.integers = integersOfSlots;
            ValueFlowNumbers before = read.stored->numbersAddedSoFar();
            read.stored->uniteNumbers(numbers);
            ValueFlowNumbers after = read.stored->numbersAddedSoFar();
            didChange |= before.mayBeOtherThanIntegers != after.mayBeOtherThanIntegers || before.integers != after.integers;
        }
    }
    for (StoredUnderKey* stored : waiting)
        stored->waitsForNumbersOfFields = false;
}

void StoresByLayout::noteKeyBroughtByCopies(LayoutID target, PropertyKeyID key, ValueSetID value)
{
    StoresIntoObjects& stores = m_storesOfLayout[target.index()];
    if (stores.keysLeftOutOfEveryCopy && stores.keysLeftOutOfEveryCopy->contains(key))
        return;
    StoredUnderKey& stored = stores.underKey.add(key.index(), StoredUnderKey { }).iterator->value;
    unite(stored.value, value);
    stored.uniteNumbers(ValueFlowNumbers::any());
    stored.isStoredAfterCreation = true;
    stored.isNamedByOperation |= !stores.getsCopyWithoutComputedKey;
    stored.isGivenValueByOperation = true;
    stored.isGivenValueByDefinition = true;
    stored.writers.add(Actor::KeysNobodyKnows);
}

void StoresByLayout::findReceiversBehindChecks()
{
    const Program& program = m_inputs.program;
    m_valuesBehindChecks.grow(program.functions.size());
    m_hasChecksNotLookedAt.fill(0, program.functions.size());
    forEachIndexInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!m_inputs.isReached(FunctionID(index)))
            return;
        bool hasCheck = false;
        bool hasTypedReceiver = false;
        for (const Value& value : function.values) {
            hasCheck |= value.opcode == Opcode::Check;
            if (hasTypedReceiver || accessOf(value.opcode) == Access::Read)
                continue;
            if (std::optional<uint8_t> receiver = receiverOf(value.opcode); receiver && value.operand(*receiver))
                hasTypedReceiver = program.typeOf(function, value.operand(*receiver)).hasOnlyLayouts();
            if (receiverOf(value.opcode) || value.opcode == Opcode::NewObject) {
                value.forEachOperand([&](ValueID operand, uint32_t) {
                    hasTypedReceiver |= !program.typeOf(function, operand).integers.isEmpty();
                });
            }
        }
        if (hasCheck && hasTypedReceiver) {
            const BitVector* known = m_inputs.bodies().valuesBehindChecks(FunctionID(index));
            m_valuesBehindChecks[index] = known ? *known : valuesTypedBehindChecks(program, function, CFG(function));
        }
        m_hasChecksNotLookedAt[index] = hasCheck && !hasTypedReceiver;
    });
}

LayoutSetID StoresByLayout::familyOfReceiverOf(const ValueFlowStatement& statement) const
{
    if (statement.kind != ValueFlowStatementKind::Operation)
        return { };
    std::optional<uint8_t> receiver = receiverOf(statement.opcode);
    if (!receiver)
        return { };
    const Program& program = m_inputs.program;
    const Function& function = program.at(statement.function);
    ValueID object = function.at(statement.site).operand(*receiver);
    const Type& type = program.typeOf(function, object);
    if (!type.hasOnlyLayouts() || m_valuesBehindChecks[statement.function.index()].get(object.index()))
        return { };
    return type.layouts;
}

void StoresByLayout::listWrites()
{
    constexpr uint32_t madeStatementsPerIndex = 512;
    AOT_THRESHOLD(size_t, mostWritesNotToldApart, 64);
    const ValueFlowSolver& solver = m_inputs.solver();
    std::span<const ValueFlowStatement> statements = m_inputs.bodies().statements();
    std::span<const ValueFlowBodyRange> ranges = m_inputs.bodies().ranges();
    uint32_t numberOfBodies = m_inputs.bodies().numberOfBodies();
    uint32_t numberOfStatements = solver.numberOfStatements();
    uint32_t numberOfMadeStatements = numberOfStatements - static_cast<uint32_t>(statements.size());
    uint32_t numberOfRunsOfMadeStatements = (numberOfMadeStatements + madeStatementsPerIndex - 1) / madeStatementsPerIndex;
    m_writes = collectInParallel<uint32_t>(m_inputs.loops(), numberOfBodies + numberOfRunsOfMadeStatements, [&](unsigned position, Vector<uint32_t>& writes) {
        auto listWritesOf = [&](ValueFlowStatementID id) {
            bool isListed = false;
            size_t firstWrite = 0;
            HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> nodesListed;
            m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID, ValueFlowNodeID node) {
                if (solver.possibleValuesOf(node) == ValueSets::nothing)
                    return;
                uint32_t index = solver.indexOfMadeNodes(key);
                RELEASE_ASSERT(!((index | id.index()) & isStatementInListOfWrites));
                if (!std::exchange(isListed, true)) {
                    writes.append(id.index() | isStatementInListOfWrites);
                    LayoutSetID family = familyOfReceiverOf(id.index() < statements.size() ? statements[id.index()] : solver.statementAt(id));
                    writes.append(family ? family.index() + 1 : 0);
                    RELEASE_ASSERT(!(writes.last() & isStatementInListOfWrites));
                    firstWrite = writes.size();
                }
                if (writes.size() - firstWrite >= mostWritesNotToldApart) {
                    if (nodesListed.isEmpty()) {
                        for (uint32_t earlier : writes.subspan(firstWrite))
                            nodesListed.add(earlier);
                    }
                    if (!nodesListed.add(index).isNewEntry)
                        return;
                }
                writes.append(index);
            });
        };
        if (position < numberOfBodies) {
            if (position != m_inputs.bodies().hostBody() && !m_inputs.isReached(FunctionID(position)))
                return;
            for (uint32_t index = ranges[position].statement; index < ranges[position + 1].statement; ++index) {
                m_inputs.prefetchMadeNodesAskedForBy(index + 8, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false);
                listWritesOf(ValueFlowStatementID(index));
            }
            return;
        }
        uint32_t first = static_cast<uint32_t>(statements.size()) + (position - numberOfBodies) * madeStatementsPerIndex;
        for (uint32_t index = first; index < std::min(first + madeStatementsPerIndex, numberOfStatements); ++index) {
            if (!solver.isAtRest(ValueFlowStatementID(index)))
                listWritesOf(ValueFlowStatementID(index));
        }
    });
}

auto StoresByLayout::findObjectsOf(ValueSetID set, PartsOfLayouts* parts) const -> ObjectsOfSet
{
    ObjectsOfSet objects;
    if (ValueSets::isComplement(set)) {
        objects.holdsAlmostEverything = true;
        return objects;
    }
    objects.holdsUnknownObject = m_inputs.sets().contains(set, ValueFlowUniverse::unknownObject());
    objects.holdsUntrackedObject = m_inputs.sets().contains(set, ValueFlowUniverse::untrackedObject());
    objects.holdsObjectWithoutLayout = m_inputs.sets().meet(set, m_inputs.universe().constructedObjects()) || m_inputs.sets().meet(set, m_inputs.universe().closures());
    Vector<uint32_t, 4> lists;
    LayoutsOfObjects::SharedLists shared = m_layoutsOfObjects.forEachLayoutNamedByMembers(set, lists, [&](LayoutID layout) {
        objects.layouts.append(layout);
    });
    objects.holdsObjectOfEveryEscapedClass = shared.everyEscapedClass;
    objects.holdsObjectOfEveryLostClass = shared.everyLostClass;
    AOT::sort(objects.layouts);
    removeRepeatedElements(objects.layouts);
    if (lists.isEmpty())
        return objects;
    if (parts) {
        for (LayoutID layout : objects.layouts) {
            bool isListed = std::ranges::any_of(lists, [&](uint32_t list) {
                return std::ranges::binary_search(m_layoutsOfObjects.listOfSelectedLayouts(list), layout);
            });
            if (!isListed)
                parts->named.append(layout);
        }
    }
    for (uint32_t list : lists) {
        std::span<const LayoutID> listed = m_layoutsOfObjects.listOfSelectedLayouts(list);
        Vector<LayoutID, 4> united;
        united.reserveInitialCapacity(objects.layouts.size() + listed.size());
        size_t index = 0;
        for (LayoutID layout : listed) {
            while (index < objects.layouts.size() && objects.layouts[index] < layout)
                united.append(objects.layouts[index++]);
            if (index < objects.layouts.size() && objects.layouts[index] == layout)
                ++index;
            united.append(layout);
        }
        united.append(objects.layouts.subspan(index));
        objects.layouts = WTF::move(united);
    }
    if (parts)
        parts->lists = WTF::move(lists);
    return objects;
}

std::optional<uint32_t> StoresByLayout::positionOfSetLookedAt(ValueSetID set) const
{
    return m_positionOfSetLookedAt.find(IntHash<uint32_t>::hash(set.index()), [&](uint32_t position) {
        return m_setsLookedAt[position] == set.index();
    });
}

auto StoresByLayout::objectsOfSetOfWrite(ValueSetID set) const -> const ObjectsOfSet&
{
    std::optional<uint32_t> position = positionOfSetLookedAt(set);
    RELEASE_ASSERT(position);
    return *position < m_objectsOfSetsOfWrites.size() ? m_objectsOfSetsOfWrites[*position] : m_objectsOfOtherSets[*position - m_objectsOfSetsOfWrites.size()];
}

bool StoresByLayout::admits(std::span<const LayoutID> layoutsOfReceiver, LayoutID layout) const
{
    if (layoutsOfReceiver.empty())
        return true;
    const Program& program = m_inputs.program;
    LayoutID current = layout;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        if (std::ranges::binary_search(layoutsOfReceiver, current))
            return true;
        current = layoutExpectedAbove(program, current);
    }
    return false;
}

const Vector<LayoutID>& StoresByLayout::layoutsAdmittedBy(LayoutSetID familyOfReceiver)
{
    return m_layoutsAdmittedBy.ensure(familyOfReceiver.index(), [&] {
        const Program& program = m_inputs.program;
        if (m_layoutsExpectedDirectlyBelow.isEmpty()) {
            m_layoutsExpectedDirectlyBelow.grow(program.layouts.size());
            for (uint32_t index = 0; index < program.layouts.size(); ++index) {
                if (LayoutID above = layoutExpectedAbove(program, LayoutID(index)))
                    m_layoutsExpectedDirectlyBelow[above.index()].append(LayoutID(index));
            }
        }
        Vector<LayoutID> admitted;
        BitVector isAdmitted(program.layouts.size());
        Vector<LayoutID, 16> worklist(program.at(familyOfReceiver).completeMembers().value());
        while (!worklist.isEmpty()) {
            LayoutID layout = worklist.takeLast();
            if (isAdmitted.quickSet(layout.index()))
                continue;
            admitted.append(layout);
            worklist.appendVector(m_layoutsExpectedDirectlyBelow[layout.index()]);
        }
        AOT::sort(admitted);
        return admitted;
    }).iterator->value;
}

void StoresByLayout::lookAt(std::span<const ValueSetID> sets)
{
    Vector<ValueSetID> unseen;
    for (ValueSetID set : sets) {
        if (!positionOfSetLookedAt(set))
            unseen.append(set);
    }
    Vector<ObjectsOfSet> objects(unseen.size());
    forEachIndexInParallel(m_inputs.loops(), unseen.size(), [&](unsigned position) {
        objects[position] = findObjectsOf(unseen[position]);
    });
    for (ValueSetID set : unseen) {
        m_positionOfSetLookedAt.add(IntHash<uint32_t>::hash(set.index()), m_setsLookedAt.size());
        m_setsLookedAt.append(set.index());
    }
    m_objectsOfOtherSets.appendVector(WTF::move(objects));
}

auto StoresByLayout::placeholdersInSetOfWrite(ValueSetID set) const -> PlaceholdersInSet
{
    const ObjectsOfSet& objects = objectsOfSetOfWrite(set);
    return { objects.holdsUnknownObject, objects.holdsUntrackedObject, objects.holdsAlmostEverything };
}

bool StoresByLayout::spellsKey(ValueSetMaker& maker, const Writer& writer, ValueFlowName name) const
{
    const ValueFlowStatement& statement = writer.statement;
    if (name.kind != ValueFlowNameKind::Property || statement.kind != ValueFlowStatementKind::Operation)
        return false;
    if (statement.opcode == Opcode::ElemSet || statement.opcode == Opcode::ElemDefine) {
        static_assert(OperandIndex::ElemSet::property == OperandIndex::ElemDefine::property);
        ValueSetID keys = std::as_const(m_heap).possibleValuesOf(maker, m_inputs.bodies().operandsOf(statement)[OperandIndex::ElemSet::property]);
        if (ValueSets::isComplement(keys))
            return false;
        ValueFlowObjectID key = m_inputs.sets().onlyMember(maker.intersect(keys, ValueSets::complement(maker.of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Absent) }))));
        return key && m_inputs.universe().kindOf(key) == ValueFlowObjectKind::Key && m_inputs.universe().keyOf(key).index() == name.payload;
    }
    if (!Value::opcodesWithKey.contains(statement.opcode))
        return false;
    PropertyKeyID key = m_inputs.program.at(statement.function).at(statement.site).key();
    return key && key.index() == name.payload;
}

void StoresByLayout::note(ValueSetMaker& maker, StoresIntoObjects& stores, ValueFlowName name, ValueSetID value, bool isAllocation, bool isNamedByOperation)
{
    switch (name.kind) {
    case ValueFlowNameKind::Property: {
        StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
        stored.value = maker.unite(stored.value, value);
        stored.isStoredAfterCreation |= !isAllocation;
        stored.isNamedByOperation |= isNamedByOperation;
        return;
    }
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter: {
        StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
        stored.isStoredAfterCreation = true;
        stored.mayBeAccessor = true;
        return;
    }
    case ValueFlowNameKind::UnknownKey:
        stores.underUnknownKey = maker.unite(stores.underUnknownKey, value);
        return;
    case ValueFlowNameKind::Elements:
        stores.isStoredUnderIndex = true;
        return;
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
        stores.getsAccessorUnderUnknownKey = true;
        return;
    case ValueFlowNameKind::CopiedFrom:
        stores.getsCopiedProperties = true;
        return;
    case ValueFlowNameKind::EveryProperty:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::RedefinedMember:
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::HiddenSlot:
    case ValueFlowNameKind::InternalFields:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::AllocatedAs:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    case ValueFlowNameKind::FoundThroughEscapedInstances:
    case ValueFlowNameKind::NamesOfMethods:
        return;
    }
}

static ValueID valueStoredUnder(const Program& program, const Value& operation, ValueFlowName name)
{
    if (std::optional<uint8_t> stored = storedOf(operation.opcode))
        return operation.operand(*stored);
    switch (operation.opcode) {
    case Opcode::ElemSet:
        return operation.operand(OperandIndex::ElemSet::value);
    case Opcode::ElemDefine:
        return operation.operand(OperandIndex::ElemDefine::value);
    case Opcode::NewObject: {
        if (name.kind != ValueFlowNameKind::Property)
            return { };
        const Layout& layout = program.at(operation.layout());
        auto fields = operation.fields();
        for (uint32_t position = 0; position < fields.size(); ++position) {
            PropertyKeyID key = layout.fields[fields[position]].key;
            if (key && key.index() == name.payload)
                return operation.operand(OperandIndex::NewObject::values + position);
        }
        return { };
    }
    default:
        return { };
    }
}

ValueFlowNumbers StoresByLayout::numbersStoredBy(const Writer& writer, ValueFlowName name) const
{
    const ValueFlowStatement& statement = writer.statement;
    if (statement.kind != ValueFlowStatementKind::Operation)
        return ValueFlowNumbers::any();
    const Program& program = m_inputs.program;
    const Function& function = program.at(statement.function);
    ValueID stored = valueStoredUnder(program, function.at(statement.site), name);
    if (!stored)
        return ValueFlowNumbers::any();
    ValueFlowNumbers found = m_inputs.numbersOfValues ? m_inputs.numbersOfValues->numbersOf(statement.function, stored) : ValueFlowNumbers::any();
    if (m_hasChecksNotLookedAt[statement.function.index()] || m_valuesBehindChecks[statement.function.index()].get(stored.index()))
        return found;
    ValueFlowNumbers numbers;
    numbers.add(program.typeOf(function, stored));
    return numbers.meetWith(found);
}

ValueFlowNumbers StoresByLayout::numbersInData(LayoutID layout, ValueFlowName name) const
{
    if (name.kind != ValueFlowNameKind::Property)
        return ValueFlowNumbers::any();
    auto found = m_numbersInData->find(static_cast<uint64_t>(layout.index()) << 32 | name.payload);
    return found == m_numbersInData->end() ? ValueFlowNumbers { } : found->value;
}

void StoresByLayout::noteNumbers(StoresIntoObjects& stores, ValueFlowName name, const ValueFlowNumbers& numbers)
{
    if (numbers.isEmpty())
        return;
    if (name.kind == ValueFlowNameKind::Property)
        stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value.uniteNumbers(numbers);
    else if (name.kind == ValueFlowNameKind::UnknownKey)
        stores.numbersUnderUnknownKey.unite(numbers);
}

static BitVector collectKeysDeclaredBy(const Program& program, std::span<const LayoutID> layouts)
{
    BitVector keys(program.types.numberOfPropertyKeys());
    BitVector isVisited(program.layouts.size());
    for (LayoutID layout : layouts) {
        for (LayoutID current = layout; current && !isVisited.quickSet(current.index());) {
            const Layout& visited = program.at(current);
            for (const Field& field : visited.fields) {
                if (field.key)
                    keys.quickSet(field.key.index());
            }
            switch (visited.flow.baseState) {
            case BaseState::Layout:
                current = visited.flow.base;
                break;
            case BaseState::Unknown:
                current = predictedHeritageOf(visited);
                break;
            case BaseState::None:
            case BaseState::BuiltinClass:
                current = { };
                break;
            }
        }
    }
    return keys;
}

// Whoever else the call may enter, only the parser makes these objects.
static bool fillsTypedResult(const ValueFlowResultInputs& inputs, const Writer& writer, ValueSetID objects)
{
    const ValueFlowStatement& statement = writer.statement;
    if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Call || ValueSets::isComplement(objects))
        return false;
    ValueFlowObjectID object = inputs.sets().onlyMember(objects);
    const ValueFlowSite* site = object ? inputs.universe().siteOf(object) : nullptr;
    return site && site->origin == ValueFlowSiteOrigin::TypedResult && inputs.universe().isSiteObjectOf(statement, object);
}

static bool storesIntoThisOfConstructor(const Program& program, const Writer& writer)
{
    std::optional<uint8_t> receiver = receiverOf(writer.statement.opcode);
    if (writer.statement.kind != ValueFlowStatementKind::Operation || !receiver)
        return false;
    const Function& function = program.at(writer.statement.function);
    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    if (!function.constructedClass || !thisParameter)
        return false;
    return sameObjectThroughConversionsToObject(function, function.at(writer.statement.site).operand(*receiver)) == function.at(entryBlock).parameters[*thisParameter];
}

uint32_t StoresByLayout::familyDeclaredForReceiverOf(const Writer& writer) const
{
    if (!writer.layoutsOfReceiver.empty())
        return writer.familyOfReceiver.index();
    std::optional<uint8_t> receiver = receiverOf(writer.statement.opcode);
    if (!receiver)
        return UINT32_MAX;
    const Program& program = m_inputs.program;
    const Function& function = program.at(writer.statement.function);
    const Type& type = program.typeOf(function, function.at(writer.statement.site).operand(*receiver));
    if (type.hasOnlyLayouts())
        return type.layouts.index();
    if (!storesIntoThisOfConstructor(program, writer))
        return UINT32_MAX;
    const Type& typeOfThis = program.at(function.thisType.expected().predicted());
    return typeOfThis.hasOnlyLayouts() ? typeOfThis.layouts.index() : UINT32_MAX;
}

std::optional<ValueSetID> StoresByLayout::descriptorsWrittenOutIn(ValueSetMaker& maker, const ValueFlowStatement& statement) const
{
    constexpr size_t positionOfDescriptor = OperandIndex::Call::arguments + 2;
    std::span<const ValueFlowOperand> operands = m_inputs.bodies().operandsOf(statement);
    if (statement.opcode != Opcode::Call || operands.size() != positionOfDescriptor + 1)
        return std::nullopt;
    ValueSetID descriptors = std::as_const(m_heap).possibleValuesOf(maker, operands[positionOfDescriptor]);
    const ValueSets& sets = m_inputs.sets();
    if (sets.isEmpty(descriptors) || !sets.isSubset(descriptors, maker.intersect(m_inputs.universe().knownObjects(), ValueSets::complement(m_inputs.universe().builtinObjects()))))
        return std::nullopt;
    return descriptors;
}

uint8_t StoresByLayout::formsOfAccessorDefinedBy(ValueSetMaker& maker, const Writer& writer, ValueFlowName name) const
{
    if (ValueFlowNameShape shape = shapeOf(name.kind); !shape.isAccessor() || shape.reach == ValueFlowNameReach::EveryKey)
        return 0;
    const ValueFlowStatement& statement = writer.statement;
    if (statement.kind != ValueFlowStatementKind::Operation)
        return 0;
    if (statement.opcode == Opcode::DefineGetter || statement.opcode == Opcode::DefineSetter)
        return AccessorForms::of(true, true);
    std::optional<ValueSetID> descriptors = descriptorsWrittenOutIn(maker, statement);
    if (!descriptors)
        return AccessorForms::unknown;
    const ValueSets& sets = m_inputs.sets();
    auto attribute = [&](BuiltinName member) -> std::optional<bool> {
        ValueSetID written = std::as_const(m_heap).writtenUnder(maker, *descriptors, ValueFlowName::property(m_inputs.program, m_inputs.program.keyOf(member)));
        if (sets.isEmpty(written))
            return false;
        ValueFlowObjectID only = sets.onlyMember(written);
        if (only == ValueFlowUniverse::primitive(PrimitiveSet::True))
            return true;
        if (only == ValueFlowUniverse::primitive(PrimitiveSet::False))
            return false;
        return std::nullopt;
    };
    std::optional<bool> isEnumerable = attribute(BuiltinNames::enumerable);
    std::optional<bool> isConfigurable = attribute(BuiltinNames::configurable);
    if (!isEnumerable || !isConfigurable)
        return AccessorForms::unknown;
    return AccessorForms::of(*isEnumerable, *isConfigurable);
}

uint8_t StoresByLayout::formsOfValueDefinedByCall(ValueSetMaker& maker, const ValueFlowStatement& statement) const
{
    // A store by name consults the access plan, which falls back to a lookup by name if the field has another form. Anything else that gives an object a property may fill the slot directly.
    if (statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::PropSet)
        return ValueForms::of(true, true, true);
    if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Call)
        return ValueForms::unknown;
    const Program& program = m_inputs.program;
    // `Reflect.set(object, key, value)` also has three arguments, and its third is not a descriptor.
    const Function& function = program.at(statement.function);
    const Value& callee = function.at(definitionThroughUpcasts(function, function.at(statement.site).calleeOperand()));
    BuiltinID builtin = callee.opcode == Opcode::Builtin ? program.builtins().canonicalID(callee.builtin()) : BuiltinID();
    if (builtin != Builtins::ObjectDefineProperty && builtin != Builtins::ReflectDefineProperty)
        return ValueForms::unknown;
    std::optional<ValueSetID> descriptors = descriptorsWrittenOutIn(maker, statement);
    if (!descriptors)
        return ValueForms::unknown;
    const ValueSets& sets = m_inputs.sets();
    // The form is what the layout plans for, and nothing is proven from it: an object that gets the property in another form keeps it outside the slot.
    auto writtenUnder = [&](ValueSetID objects, BuiltinName member) {
        return std::as_const(m_heap).writtenUnder(maker, objects, ValueFlowName::property(program, program.keyOf(member)));
    };
    if (!sets.isEmpty(writtenUnder(*descriptors, BuiltinNames::get)) || !sets.isEmpty(writtenUnder(*descriptors, BuiltinNames::set)))
        return ValueForms::unknown;
    auto attribute = [&](BuiltinName member) -> std::optional<bool> {
        ValueSetID written = writtenUnder(*descriptors, member);
        if (sets.isEmpty(written))
            return false;
        ValueFlowObjectID only = sets.onlyMember(written);
        if (only == ValueFlowUniverse::primitive(PrimitiveSet::True))
            return true;
        if (only == ValueFlowUniverse::primitive(PrimitiveSet::False))
            return false;
        return std::nullopt;
    };
    std::optional<bool> isWritable = attribute(BuiltinNames::writable);
    std::optional<bool> isEnumerable = attribute(BuiltinNames::enumerable);
    std::optional<bool> isConfigurable = attribute(BuiltinNames::configurable);
    if (!isWritable || !isEnumerable || !isConfigurable)
        return ValueForms::unknown;
    return ValueForms::of(*isWritable, *isEnumerable, *isConfigurable);
}

bool StoresByLayout::definesValue(ValueSetMaker& maker, const Writer& writer, ValueFlowName name) const
{
    const ValueFlowStatement& statement = writer.statement;
    if (name.kind != ValueFlowNameKind::Property || statement.kind != ValueFlowStatementKind::Operation || statement.opcode == Opcode::PropSet)
        return false;
    if (statement.opcode != Opcode::Call)
        return true;
    std::optional<ValueSetID> descriptors = descriptorsWrittenOutIn(maker, statement);
    return !descriptors || !m_inputs.sets().isEmpty(std::as_const(m_heap).writtenUnder(maker, *descriptors, ValueFlowName::property(m_inputs.program.keyOf(BuiltinNames::value))));
}

auto StoresByLayout::describeWrite(ValueSetMaker& maker, StoreUnderName& store, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID value) const -> WritesThroughSet
{
    const ValueFlowStatement& statement = writer.statement;
    bool isOperation = statement.kind == ValueFlowStatementKind::Operation;
    ValueFlowName name = key.name();
    if (store.statement != &statement || store.name != name) {
        bool spellsItsKey = spellsKey(maker, writer, name);
        store = {
            .statement = &statement,
            .name = name,
            .spellsKey = spellsItsKey,
            .isIntoThisOfConstructor = spellsItsKey && storesIntoThisOfConstructor(m_inputs.program, writer),
            .definesValue = definesValue(maker, writer, name),
            .accessorForms = formsOfAccessorDefinedBy(maker, writer, name),
            .formsOfDefinedValues = name.kind == ValueFlowNameKind::Property ? formsOfValueDefinedByCall(maker, statement) : static_cast<uint8_t>(0),
            .familyDeclaredForReceiver = spellsItsKey ? familyDeclaredForReceiverOf(writer) : UINT32_MAX,
            .numbers = std::nullopt,
            .objectsRead = ValueSets::nothing,
            .keyRead = 0,
        };
    }
    bool holdsNumber = std::ranges::any_of(std::array { PrimitiveSet::Integer, PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber }, [&](PrimitiveSet kind) {
        return m_inputs.sets().contains(value, ValueFlowUniverse::primitive(kind));
    });
    if (holdsNumber && !store.numbers) {
        store.numbers = numbersStoredBy(writer, name);
        constexpr Range integersOfSlots { minInlineInt32, INT32_MAX };
        bool fitsEverySlot = !store.numbers->mayBeOtherThanIntegers && store.numbers->integers.isSubsetOf(integersOfSlots);
        if (std::optional<ReadOfFields> read = fitsEverySlot ? std::nullopt : readOfFieldsStoredBy(maker, writer, name)) {
            store.numbers = ValueFlowNumbers { };
            store.objectsRead = read->objects;
            store.keyRead = read->key;
        }
    }
    ValueFlowNumbers numbers = holdsNumber ? *store.numbers : ValueFlowNumbers { };
    bool followsRead = holdsNumber && store.objectsRead != ValueSets::nothing;
    bool isCopyIntoLiteral = name.kind == ValueFlowNameKind::CopiedFrom && isOperation && statement.opcode == Opcode::CopyDataProperties;
    bool leavesOutKeys = isCopyIntoLiteral && !m_inputs.program.at(statement.function).at(statement.site).excludedKeys().empty();
    bool fillsResult = fillsTypedResult(m_inputs, writer, key.set);
    bool isNamedByOperation = !fillsResult && store.spellsKey;
    return {
        .set = key.set.index(),
        .payload = key.payload,
        .familyOfReceiver = writer.layoutsOfReceiver.empty() ? UINT32_MAX : writer.familyOfReceiver.index(),
        .familyDeclaredForReceiver = isNamedByOperation ? store.familyDeclaredForReceiver : UINT32_MAX,
        .detail = key.detail,
        .fillsTypedResult = fillsResult,
        .createsData = !fillsResult && isOperation && statement.opcode == Opcode::Data && !numbers.isEmpty(),
        .isStoredAfterCreation = !writer.isAllocation,
        .isNamedByOperation = isNamedByOperation,
        .isStoredByConstructor = isNamedByOperation && store.isIntoThisOfConstructor,
        .isWrittenByOperation = isOperation,
        .givesValueItself = isOperation && statement.opcode != Opcode::Call,
        .isWrittenByCallOfBuiltin = isOperation && statement.opcode == Opcode::Call,
        .definesValue = store.definesValue,
        .isAssignment = isOperation && statement.opcode == Opcode::PropSet,
        .isOtherThanAssignment = !isOperation || statement.opcode != Opcode::PropSet,
        .leavesOutComputedKey = isCopyIntoLiteral && !m_inputs.program.at(statement.function).at(statement.site).excludedOperands().empty(),
        .copyLeavingOutKeys = leavesOutKeys ? static_cast<uint64_t>(statement.function.index()) << 32 | statement.site.index() : noCopyLeavingOutKeys,
        .accessorForms = store.accessorForms,
        .formsOfDefinedValues = store.formsOfDefinedValues,
        .writers = actorOf(m_inputs, statement, key.set, name),
        .value = value,
        .numbers = numbers,
        .objectsRead = followsRead ? store.objectsRead : ValueSets::nothing,
        .keyRead = followsRead ? store.keyRead : 0,
    };
}

auto StoresByLayout::collectWrites(ValueSetMakers& makers) const -> ParallelArray<WritesThroughSet>
{
    const ParallelLoops& loops = m_inputs.loops();
    auto isInEarlierGroup = [](const WritesThroughSet& a, const WritesThroughSet& b) {
        return a.group() < b.group();
    };
    auto fold = [](WritesThroughSet& merged, const WritesThroughSet& write) {
        merged.isStoredAfterCreation |= write.isStoredAfterCreation;
        merged.isNamedByOperation |= write.isNamedByOperation;
        merged.isStoredByConstructor |= write.isStoredByConstructor;
        merged.isWrittenByOperation |= write.isWrittenByOperation;
        merged.givesValueItself |= write.givesValueItself;
        merged.isWrittenByCallOfBuiltin |= write.isWrittenByCallOfBuiltin;
        merged.definesValue |= write.definesValue;
        merged.isAssignment |= write.isAssignment;
        merged.isOtherThanAssignment |= write.isOtherThanAssignment;
        merged.leavesOutComputedKey |= write.leavesOutComputedKey;
        merged.accessorForms |= write.accessorForms;
        merged.formsOfDefinedValues |= write.formsOfDefinedValues;
        merged.writers.add(write.writers);
        merged.numbers.unite(write.numbers);
    };
    struct GroupsOfWorker {
        Vector<WritesThroughSet> writes;
        HashedIndex positionOfGroup;
        StoreUnderName lastStore;
    };
    Vector<GroupsOfWorker> groupsOfWorker(loops.width());
    forEachWriteInParallel([&](unsigned worker, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID value) {
        GroupsOfWorker& groups = groupsOfWorker[worker];
        WritesThroughSet write = describeWrite(makers.of(worker), groups.lastStore, writer, key, value);
        uint32_t hash = write.hashOfGroup();
        std::optional<uint32_t> position = groups.positionOfGroup.find(hash, [&](uint32_t candidate) {
            return groups.writes[candidate].group() == write.group();
        });
        if (position) {
            fold(groups.writes[*position], write);
            return;
        }
        groups.positionOfGroup.add(hash, groups.writes.size());
        groups.writes.append(write);
    });
    Vector<Vector<WritesThroughSet>> writesOfWorkers(groupsOfWorker.size(), [&](size_t worker) {
        return WTF::move(groupsOfWorker[worker].writes);
    });
    ParallelArray<WritesThroughSet> writes = ParallelArray<WritesThroughSet>::concatenationOf(loops, WTF::move(writesOfWorkers));
    forEachIndexInParallel(loops, groupsOfWorker.size(), [&](unsigned worker) {
        groupsOfWorker[worker] = { };
    });
    parallelSort(loops, writes.mutableSpan(), isInEarlierGroup);

    constexpr size_t writesPerShare = 1 << 13;
    size_t numberOfWrites = writes.size();
    unsigned numberOfShares = static_cast<unsigned>((numberOfWrites + writesPerShare - 1) / writesPerShare);
    Vector<size_t> firstWriteOfShare(numberOfShares + 1, [&](size_t share) {
        size_t first = std::min(share * writesPerShare, numberOfWrites);
        while (first && first < numberOfWrites && writes[first - 1].group() == writes[first].group())
            ++first;
        return first;
    });
    Vector<Vector<WritesThroughSet>> groupsOfShares(numberOfShares);
    forEachIndexInParallel(loops, numberOfShares, [&](unsigned share) {
        Vector<WritesThroughSet>& groups = groupsOfShares[share];
        if (firstWriteOfShare[share] >= firstWriteOfShare[share + 1])
            return;
        for (const WritesThroughSet& write : writes.span().subspan(firstWriteOfShare[share], firstWriteOfShare[share + 1] - firstWriteOfShare[share])) {
            if (groups.isEmpty() || groups.last().group() != write.group())
                groups.append(write);
            else
                fold(groups.last(), write);
        }
    });
    m_inputs.build.destroyLater(WTF::move(writes));
    return ParallelArray<WritesThroughSet>::concatenationOf(loops, WTF::move(groupsOfShares));
}

void StoresByLayout::noteWriters(StoresIntoObjects& stores, const WritesThroughSet& writes)
{
    ValueFlowName name = writes.name();
    switch (shapeOf(name.kind).sort) {
    case ValueFlowNameSort::Value:
    case ValueFlowNameSort::Getter:
    case ValueFlowNameSort::Setter:
    case ValueFlowNameSort::Copy:
        break;
    case ValueFlowNameSort::Deletion:
    case ValueFlowNameSort::Redefinition:
    case ValueFlowNameSort::PrototypeLink:
    case ValueFlowNameSort::Other:
        return;
    }
    if (name.kind == ValueFlowNameKind::Property || name.kind == ValueFlowNameKind::Getter || name.kind == ValueFlowNameKind::Setter)
        stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value.writers.add(writes.writers);
    else if (name.kind != ValueFlowNameKind::Elements)
        stores.writersUnderNamesNobodyLists.add(writes.writers);
}

void StoresByLayout::noteWritesInto(ValueSetMaker& maker, StoresIntoObjects& stores, LayoutID layout, const WritesThroughSet& writes)
{
    ValueFlowName name = writes.name();
    if (writes.fillsTypedResult) {
        if (name.kind == ValueFlowNameKind::UnknownKey) {
            stores.isStoredUnderIndex = true;
            return;
        }
        std::optional<FieldReference> field = name.kind == ValueFlowNameKind::Property ? findField(m_inputs.program, layout, PropertyKeyID(name.payload), BaseChain::Predicted) : std::nullopt;
        bool isFilledByAllocation = field && fieldAt(m_inputs.program, *field).flags.contains(FieldFlags::IsRequired);
        note(maker, stores, name, writes.value, isFilledByAllocation, false);
        noteWriters(stores, writes);
        noteNumbers(stores, name, writes.numbersWithoutFollowedReads());
        if (name.kind == ValueFlowNameKind::Property) {
            StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
            stored.isGivenValueByOperation = true;
            stored.isGivenValueByDefinition = true;
        }
        return;
    }
    if (writes.familyOfReceiver != UINT32_MAX && !admits(m_inputs.program.at(LayoutSetID(writes.familyOfReceiver)).completeMembers(), layout))
        return;
    note(maker, stores, name, writes.value, !writes.isStoredAfterCreation, writes.isNamedByOperation);
    noteWriters(stores, writes);
    if (name.kind == ValueFlowNameKind::Property || name.kind == ValueFlowNameKind::Getter || name.kind == ValueFlowNameKind::Setter) {
        StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
        stored.isAssigned |= writes.isAssignment;
        stored.isWrittenOtherThanByAssignment |= writes.isOtherThanAssignment;
    }
    stores.getsCopyWithoutComputedKey |= writes.leavesOutComputedKey;
    if (name.kind == ValueFlowNameKind::CopiedFrom) {
        std::span<const PropertyKeyID> leftOut;
        if (writes.copyLeavingOutKeys != noCopyLeavingOutKeys)
            leftOut = m_inputs.program.at(FunctionID(static_cast<uint32_t>(writes.copyLeavingOutKeys >> 32))).at(ValueID(static_cast<uint32_t>(writes.copyLeavingOutKeys))).excludedKeys();
        if (!stores.keysLeftOutOfEveryCopy)
            stores.keysLeftOutOfEveryCopy.emplace(leftOut);
        else {
            stores.keysLeftOutOfEveryCopy->removeAllMatching([&](PropertyKeyID key) {
                return !std::ranges::contains(leftOut, key);
            });
        }
    }
    noteNumbers(stores, name, writes.createsData ? numbersInData(layout, name) : writes.numbers);
    if (writes.objectsRead != ValueSets::nothing) {
        stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value.waitsForNumbersOfFields = true;
        stores.readsOfFields.append({ name.payload, writes.keyRead, writes.objectsRead });
    }
    if (writes.isStoredByConstructor && name.kind == ValueFlowNameKind::Property)
        stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value.isStoredByConstructor = true;
    if (!writes.isWrittenByOperation)
        return;
    if (name.kind == ValueFlowNameKind::Property) {
        StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
        stored.valueFromOperations = maker.unite(stored.valueFromOperations, writes.value);
        stored.isGivenValueByOperation |= writes.givesValueItself;
        stored.isGivenValueByDefinition |= writes.definesValue;
        stored.formsOfDefinedValues |= writes.formsOfDefinedValues;
    }
    if (name.kind == ValueFlowNameKind::UnknownKey) {
        stores.valueFromOperationsUnderUnknownKey = maker.unite(stores.valueFromOperationsUnderUnknownKey, writes.value);
        stores.isGivenValueByOperationUnderUnknownKey |= writes.givesValueItself;
    }
    if (!writes.accessorForms)
        return;
    if (name.kind == ValueFlowNameKind::GetterUnderUnknownKey || name.kind == ValueFlowNameKind::SetterUnderUnknownKey) {
        stores.formsOfAccessorsUnderUnknownKey |= writes.accessorForms;
        if (name.kind == ValueFlowNameKind::GetterUnderUnknownKey)
            stores.gettersUnderUnknownKey = maker.unite(stores.gettersUnderUnknownKey, writes.value);
        return;
    }
    StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
    stored.accessorForms |= writes.accessorForms;
    if (name.kind == ValueFlowNameKind::Getter)
        stored.getters = maker.unite(stored.getters, writes.value);
    else
        stored.setters = maker.unite(stored.setters, writes.value);
}

void StoresByLayout::noteWrites()
{
    const ParallelLoops& loops = m_inputs.loops();
    uint32_t numberOfLayouts = m_inputs.program.layouts.size();
    ValueSetMakers makers(m_inputs);
    ParallelArray<WritesThroughSet> writes = collectWrites(makers);

    Vector<uint32_t> firstWriteOfSet;
    for (uint32_t index = 0; index < writes.size(); ++index) {
        if (!index || writes[index - 1].set != writes[index].set)
            firstWriteOfSet.append(index);
    }
    uint32_t numberOfSets = firstWriteOfSet.size();
    firstWriteOfSet.append(writes.size());
    auto writesThrough = [&](uint32_t position) {
        return writes.mutableSpan().subspan(firstWriteOfSet[position], firstWriteOfSet[position + 1] - firstWriteOfSet[position]);
    };

    m_keysDeclaredByEscapedClasses = collectKeysDeclaredBy(m_inputs.program, m_layoutsOfObjects.escapedClasses());
    m_keysDeclaredByLostClasses = collectKeysDeclaredBy(m_inputs.program, m_layoutsOfObjects.lostClasses());

    Vector<ObjectsOfSet> objectsOfSet(numberOfSets);
    Vector<PartsOfLayouts> partsOfSets = collectInParallel<PartsOfLayouts>(loops, numberOfSets, [&](unsigned position, Vector<PartsOfLayouts>& found) {
        PartsOfLayouts parts;
        objectsOfSet[position] = findObjectsOf(ValueSetID(writes[firstWriteOfSet[position]].set), &parts);
        if (parts.lists.isEmpty())
            return;
        parts.position = position;
        found.append(WTF::move(parts));
    });

    auto liesUnderKey = [](const WritesThroughSet& write) {
        ValueFlowNameKind kind = write.name().kind;
        return kind == ValueFlowNameKind::Property || kind == ValueFlowNameKind::Getter || kind == ValueFlowNameKind::Setter;
    };
    auto asksNothingOfLayout = [](const WritesThroughSet& write) {
        return write.familyOfReceiver == UINT32_MAX && !write.fillsTypedResult && !write.createsData;
    };
    auto isSameForEveryLayout = [&](const WritesThroughSet& write) {
        return liesUnderKey(write) && asksNothingOfLayout(write);
    };
    // noteWritesInto() puts the forms of an accessor down under a key unless the name says that nobody knows the key.
    auto touchesNoKey = [&](const WritesThroughSet& write) {
        ValueFlowNameKind kind = write.name().kind;
        return !liesUnderKey(write) && (!write.accessorForms || kind == ValueFlowNameKind::GetterUnderUnknownKey || kind == ValueFlowNameKind::SetterUnderUnknownKey);
    };

    auto isNotedInTables = [](ValueFlowNameKind kind) {
        ValueFlowNameShape shape = shapeOf(kind);
        return shape.sort == ValueFlowNameSort::Copy || (shape.isValueOrAccessor() && shape.reach != ValueFlowNameReach::EveryKey);
    };

    // Many sets may reach the same many layouts: the allocations of all the base classes below one expression, the classes that any of them may allocate for. What such
    // sets store under keys is noted once for the layouts together, not once for each of them.
    AOT_THRESHOLD(size_t, fewestLayoutsThatShareTable, 16);
    AOT_THRESHOLD(size_t, fewestEntriesThatSharedTableSaves, 4096);
    struct GroupOfLayouts {
        std::span<const LayoutID> layouts;
        Vector<uint32_t> writes { };
        uint32_t table { UINT32_MAX };
        BitVector keysDeclared { };
    };
    struct SetThatShares {
        uint32_t position;
        bool hasOtherWrites { false };
        Vector<uint32_t, 2> groups { };
        Vector<LayoutID> otherLayouts { };
    };
    Vector<GroupOfLayouts> groups;
    Vector<SetThatShares> setsThatShare;
    {
        HashedIndex groupOfLayouts;
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> groupOfList;
        auto addGroup = [&](std::span<const LayoutID> layouts) {
            groups.append(GroupOfLayouts { layouts });
            return static_cast<uint32_t>(groups.size() - 1);
        };
        auto groupOf = [&](std::span<const LayoutID> layouts) {
            uint32_t hash = 0;
            for (LayoutID layout : layouts)
                hash = pairIntHash(hash, layout.index());
            if (std::optional<uint32_t> found = groupOfLayouts.find(hash, [&](uint32_t candidate) { return std::ranges::equal(groups[candidate].layouts, layouts); }))
                return *found;
            groupOfLayouts.add(hash, groups.size());
            return addGroup(layouts);
        };
        size_t nextParts = 0;
        for (uint32_t position = 0; position < numberOfSets; ++position) {
            const PartsOfLayouts* parts = nextParts < partsOfSets.size() && partsOfSets[nextParts].position == position ? &partsOfSets[nextParts++] : nullptr;
            std::span<const LayoutID> named = parts ? parts->named.span() : objectsOfSet[position].layouts.span();
            if (!parts && named.size() < fewestLayoutsThatShareTable)
                continue;
            SetThatShares set { position };
            if (named.size() >= fewestLayoutsThatShareTable)
                set.groups.append(groupOf(named));
            else
                set.otherLayouts.append(named);
            for (uint32_t list : parts ? parts->lists.span() : std::span<const uint32_t>()) {
                std::span<const LayoutID> listed = m_layoutsOfObjects.listOfSelectedLayouts(list);
                if (listed.size() >= fewestLayoutsThatShareTable)
                    set.groups.append(groupOfList.ensure(list, [&] { return addGroup(listed); }).iterator->value);
                else
                    set.otherLayouts.append(listed);
            }
            if (set.groups.isEmpty())
                continue;
            for (uint32_t index = firstWriteOfSet[position]; index < firstWriteOfSet[position + 1]; ++index) {
                if (!isSameForEveryLayout(writes[index])) {
                    set.hasOtherWrites |= isNotedInTables(writes[index].name().kind);
                    continue;
                }
                for (uint32_t group : set.groups)
                    groups[group].writes.append(index);
            }
            setsThatShare.append(WTF::move(set));
        }
        for (GroupOfLayouts& group : groups) {
            if (group.layouts.size() * group.writes.size() < fewestEntriesThatSharedTableSaves)
                continue;
            group.table = m_storesSharedByLayouts.size();
            m_storesSharedByLayouts.append(StoresIntoObjects { });
        }
        for (SetThatShares& set : setsThatShare) {
            set.groups.removeAllMatching([&](uint32_t group) {
                if (groups[group].table != UINT32_MAX)
                    return false;
                set.otherLayouts.append(groups[group].layouts);
                return true;
            });
            AOT::sort(set.otherLayouts);
            removeRepeatedElements(set.otherLayouts);
        }
        setsThatShare.removeAllMatching([](const SetThatShares& set) {
            return set.groups.isEmpty();
        });
    }
    forEachIndexInParallel(loops, groups.size(), [&](unsigned index) {
        if (groups[index].table != UINT32_MAX)
            groups[index].keysDeclared = collectKeysDeclaredBy(m_inputs.program, groups[index].layouts);
    });
    auto sharingOf = [&](uint32_t position) -> const SetThatShares* {
        auto found = std::ranges::lower_bound(setsThatShare, position, { }, &SetThatShares::position);
        return found != setsThatShare.end() && found->position == position ? &*found : nullptr;
    };

    forEachIndexInParallel(loops, numberOfSets, [&](unsigned position) {
        const ObjectsOfSet& objects = objectsOfSet[position];
        const SetThatShares* sharing = sharingOf(position);
        std::span<const LayoutID> layoutsAskedOneByOne = sharing ? sharing->otherLayouts.span() : objects.layouts.span();
        for (WritesThroughSet& write : writesThrough(position)) {
            if (!write.isNamedByOperation)
                continue;
            bool someReceiverDeclares = (objects.holdsObjectOfEveryEscapedClass && m_keysDeclaredByEscapedClasses.get(write.payload)) || (objects.holdsObjectOfEveryLostClass && m_keysDeclaredByLostClasses.get(write.payload))
                || (sharing && std::ranges::any_of(sharing->groups, [&](uint32_t group) { return groups[group].keysDeclared.get(write.payload); }))
                || std::ranges::any_of(layoutsAskedOneByOne, [&](LayoutID layout) {
                    return findField(m_inputs.program, layout, PropertyKeyID(write.payload), BaseChain::Predicted).has_value();
                });
            write.isNamedByOperation = !someReceiverDeclares;
        }
    });
    if (std::ranges::any_of(writes.span(), &WritesThroughSet::createsData)) {
        m_numbersInData.emplace();
        for (const Data& data : m_inputs.program.data) {
            forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
                const auto* object = node.tryAs<DataNode::Object>();
                if (!object)
                    return;
                for (const DataMember& member : object->members) {
                    const auto* number = member.value.tryAs<DataNode::Number>();
                    if (!number)
                        continue;
                    ValueFlowNumbers& numbers = m_numbersInData->add(static_cast<uint64_t>(object->layout.index()) << 32 | member.key.index(), ValueFlowNumbers { }).iterator->value;
                    if (isTrackedInteger(number->value))
                        numbers.integers = numbers.integers.unionWith(Range::one(static_cast<int64_t>(number->value)));
                    else
                        numbers.mayBeOtherThanIntegers = true;
                }
            });
        }
    }

    Vector<PropertyKeyID> keysOfAccessorsAndMethods;
    for (uint32_t position = 0; position < numberOfSets; ++position) {
        const ObjectsOfSet& objects = objectsOfSet[position];
        bool holdsPlaceholder = objects.holdsUnknownObject || objects.holdsUntrackedObject || objects.holdsAlmostEverything;
        if (!objects.holdsObjectWithoutLayout && !holdsPlaceholder)
            continue;
        for (const WritesThroughSet& write : writesThrough(position)) {
            if (!write.isWrittenByCallOfBuiltin)
                continue;
            ValueFlowNameKind kind = write.name().kind;
            bool mayDefineMethod = kind == ValueFlowNameKind::Property && (ValueSets::isComplement(write.value) || m_inputs.sets().meet(write.value, m_inputs.universe().callables()));
            if (kind == ValueFlowNameKind::Getter || kind == ValueFlowNameKind::Setter || mayDefineMethod)
                keysOfAccessorsAndMethods.append(PropertyKeyID(write.payload));
            else if (kind == ValueFlowNameKind::Property)
                m_keysDefinedOnObjectsWithoutKnownLayout.append(PropertyKeyID(write.payload));
        }
    }
    AOT::sort(keysOfAccessorsAndMethods);
    AOT::sort(m_keysDefinedOnObjectsWithoutKnownLayout);
    removeRepeatedElements(m_keysDefinedOnObjectsWithoutKnownLayout);
    m_keysDefinedOnObjectsWithoutKnownLayout.removeAllMatching([&](PropertyKeyID key) {
        return std::ranges::binary_search(keysOfAccessorsAndMethods, key);
    });

    constexpr uint32_t sharedTableHasTheRest = 1;
    auto forEachLayoutWithTableOfItsOwn = [&](NOESCAPE const Invocable<void(LayoutID, uint32_t entry)> auto& functor) {
        size_t nextSetThatShares = 0;
        for (uint32_t position = 0; position < numberOfSets; ++position) {
            const SetThatShares* sharing = nextSetThatShares < setsThatShare.size() && setsThatShare[nextSetThatShares].position == position ? &setsThatShare[nextSetThatShares++] : nullptr;
            for (LayoutID layout : sharing ? sharing->otherLayouts.span() : objectsOfSet[position].layouts.span())
                functor(layout, position << 1);
            if (!sharing || !sharing->hasOtherWrites)
                continue;
            for (uint32_t group : sharing->groups) {
                for (LayoutID layout : groups[group].layouts)
                    functor(layout, position << 1 | sharedTableHasTheRest);
            }
        }
    };
    RELEASE_ASSERT(numberOfSets < 1u << 31);
    Vector<uint32_t> firstSetOfLayout;
    firstSetOfLayout.fill(0u, numberOfLayouts + 1);
    forEachLayoutWithTableOfItsOwn([&](LayoutID layout, uint32_t) {
        ++firstSetOfLayout[layout.index() + 1];
    });
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        firstSetOfLayout[index + 1] += firstSetOfLayout[index];
    Vector<uint32_t> setsOfLayouts;
    setsOfLayouts.grow(firstSetOfLayout.last());
    Vector<uint32_t> nextSetOfLayout(firstSetOfLayout.span().first(numberOfLayouts));
    forEachLayoutWithTableOfItsOwn([&](LayoutID layout, uint32_t entry) {
        setsOfLayouts[nextSetOfLayout[layout.index()]++] = entry;
    });
    struct WritesIntoEveryClass {
        StoresIntoObjects& stores;
        bool ObjectsOfSet::* isHeld;
        const BitVector& classes;
        Vector<uint32_t> notedOnce { };
        Vector<uint32_t> notedForEachClass { };
        // What the table of each class gets outside keys, which is the same for all of them.
        Vector<uint32_t> gatheredForEachClass { };
        StoresIntoObjects gathered { };
    };
    WritesIntoEveryClass writesIntoEveryClass[] {
        { m_storesIntoEveryEscapedClass, &ObjectsOfSet::holdsObjectOfEveryEscapedClass, m_isEscapedClass },
        { m_storesIntoEveryLostClass, &ObjectsOfSet::holdsObjectOfEveryLostClass, m_isLostClass },
    };
    for (uint32_t position = 0; position < numberOfSets; ++position) {
        for (WritesIntoEveryClass& list : writesIntoEveryClass) {
            if (!(objectsOfSet[position].*list.isHeld))
                continue;
            for (uint32_t index = firstWriteOfSet[position]; index < firstWriteOfSet[position + 1]; ++index)
                (isSameForEveryLayout(writes[index]) ? list.notedOnce : asksNothingOfLayout(writes[index]) && touchesNoKey(writes[index]) ? list.gatheredForEachClass : list.notedForEachClass).append(index);
        }
    }
    for (WritesIntoEveryClass& list : writesIntoEveryClass) {
        for (uint32_t position : list.gatheredForEachClass)
            noteWritesInto(makers.of(0), list.gathered, LayoutID(), writes[position]);
        RELEASE_ASSERT(list.gathered.underKey.isEmpty());
    }

    Vector<Vector<uint32_t>> writesThroughPlaceholdersInto(numberOfLayouts);
    for (uint32_t position = 0; position < numberOfSets; ++position) {
        const ObjectsOfSet& objects = objectsOfSet[position];
        uint8_t held = objects.holdsUnknownObject | objects.holdsUntrackedObject << 1 | objects.holdsAlmostEverything << 2;
        if (!held)
            continue;
        for (uint32_t index = firstWriteOfSet[position]; index < firstWriteOfSet[position + 1]; ++index) {
            const WritesThroughSet& write = writes[index];
            if (write.isNamedByOperation && write.familyDeclaredForReceiver != UINT32_MAX)
                m_namesStoredThroughDeclaredReceivers.append({ write.familyDeclaredForReceiver, held, write.isStoredByConstructor, PropertyKeyID(write.payload) });
            if (write.fillsTypedResult || write.familyOfReceiver == UINT32_MAX)
                continue;
            for (LayoutID layout : layoutsAdmittedBy(LayoutSetID(write.familyOfReceiver))) {
                bool isReached = objects.holdsAlmostEverything || (objects.holdsUnknownObject && m_hasEscapedObject.quickGet(layout.index())) || (objects.holdsUntrackedObject && m_hasLostObject.quickGet(layout.index()));
                if (isReached)
                    writesThroughPlaceholdersInto[layout.index()].append(index);
            }
        }
    }

    struct Placeholder {
        StoresIntoObjects& stores;
        bool ObjectsOfSet::* isHeld;
    };
    Placeholder placeholders[] {
        { m_storesThroughUnknownObject, &ObjectsOfSet::holdsUnknownObject },
        { m_storesThroughUntrackedObject, &ObjectsOfSet::holdsUntrackedObject },
        { m_storesIntoEveryObject, &ObjectsOfSet::holdsAlmostEverything },
    };
    constexpr size_t numberOfSharedTables = std::size(placeholders) + std::size(writesIntoEveryClass);
    forEachIndexInParallel(loops, groups.size(), [&](unsigned index, unsigned worker) {
        const GroupOfLayouts& group = groups[index];
        if (group.table == UINT32_MAX)
            return;
        for (uint32_t position : group.writes)
            noteWritesInto(makers.of(worker), m_storesSharedByLayouts[group.table], LayoutID(), writes[position]);
    });
    forEachIndexInParallel(loops, numberOfSharedTables + numberOfLayouts, [&](unsigned index, unsigned worker) {
        ValueSetMaker& maker = makers.of(worker);
        if (index < std::size(placeholders)) {
            for (uint32_t position = 0; position < numberOfSets; ++position) {
                if (!(objectsOfSet[position].*placeholders[index].isHeld))
                    continue;
                for (const WritesThroughSet& write : writesThrough(position)) {
                    if (write.fillsTypedResult || write.familyOfReceiver != UINT32_MAX)
                        continue;
                    note(maker, placeholders[index].stores, write.name(), write.value, false, write.isNamedByOperation);
                    noteWriters(placeholders[index].stores, write);
                    noteNumbers(placeholders[index].stores, write.name(), write.numbersWithoutFollowedReads());
                }
            }
            return;
        }
        if (index < numberOfSharedTables) {
            WritesIntoEveryClass& list = writesIntoEveryClass[index - std::size(placeholders)];
            for (uint32_t position : list.notedOnce) {
                const WritesThroughSet& write = writes[position];
                note(maker, list.stores, write.name(), write.value, !write.isStoredAfterCreation, false);
                noteWriters(list.stores, write);
                noteNumbers(list.stores, write.name(), write.numbersWithoutFollowedReads());
            }
            return;
        }
        uint32_t layout = index - numberOfSharedTables;
        auto noteWritesThroughSets = [&](std::span<const uint32_t> entries) {
            for (uint32_t entry : entries) {
                for (const WritesThroughSet& write : writesThrough(entry >> 1)) {
                    if (!(entry & sharedTableHasTheRest) || !isSameForEveryLayout(write))
                        noteWritesInto(maker, m_storesOfLayout[layout], LayoutID(layout), write);
                }
            }
        };
        noteWritesThroughSets(setsOfLayouts.subspan(firstSetOfLayout[layout], firstSetOfLayout[layout + 1] - firstSetOfLayout[layout]));
        for (const WritesIntoEveryClass& list : writesIntoEveryClass) {
            if (!list.classes.quickGet(layout))
                continue;
            for (uint32_t position : list.notedForEachClass)
                noteWritesInto(maker, m_storesOfLayout[layout], LayoutID(layout), writes[position]);
            m_storesOfLayout[layout].uniteWhatLiesOutsideKeys(maker, list.gathered);
        }
        for (uint32_t position : writesThroughPlaceholdersInto[layout]) {
            note(maker, m_storesOfLayout[layout], writes[position].name(), writes[position].value, false, false);
            noteNumbers(m_storesOfLayout[layout], writes[position].name(), writes[position].numbersWithoutFollowedReads());
        }
    });

    if (!m_storesSharedByLayouts.isEmpty()) {
        m_firstSharedStoresOfLayout.fill(0u, numberOfLayouts + 1);
        auto forEachLayoutThatShares = [&](NOESCAPE const Invocable<void(LayoutID, uint32_t table)> auto& functor) {
            for (const GroupOfLayouts& group : groups) {
                for (LayoutID layout : group.table != UINT32_MAX ? group.layouts : std::span<const LayoutID>())
                    functor(layout, group.table);
            }
        };
        forEachLayoutThatShares([&](LayoutID layout, uint32_t) {
            ++m_firstSharedStoresOfLayout[layout.index() + 1];
        });
        for (uint32_t index = 0; index < numberOfLayouts; ++index)
            m_firstSharedStoresOfLayout[index + 1] += m_firstSharedStoresOfLayout[index];
        m_sharedStoresOfLayouts.grow(m_firstSharedStoresOfLayout.last());
        Vector<uint32_t> nextSharedStoresOfLayout(m_firstSharedStoresOfLayout.span().first(numberOfLayouts));
        forEachLayoutThatShares([&](LayoutID layout, uint32_t table) {
            m_sharedStoresOfLayouts[nextSharedStoresOfLayout[layout.index()]++] = table;
        });
        m_keysOfSharedStores.grow(m_storesSharedByLayouts.size());
        m_keysNamedInSharedStores.grow(m_storesSharedByLayouts.size());
        forEachIndexInParallel(loops, m_storesSharedByLayouts.size(), [&](unsigned table) {
            m_keysOfSharedStores[table] = sortedKeysOf(m_storesSharedByLayouts[table]);
            m_keysNamedInSharedStores[table] = sortedKeysNamedIn(m_storesSharedByLayouts[table]);
        });
    }

    RELEASE_ASSERT(m_setsLookedAt.isEmpty());
    m_objectsOfSetsOfWrites = WTF::move(objectsOfSet);
    m_setsLookedAt.grow(numberOfSets);
    m_positionOfSetLookedAt.reserve(loops, numberOfSets);
    forEachIndexInParallel(loops, numberOfSets, [&](unsigned position) {
        uint32_t set = writes[firstWriteOfSet[position]].set;
        m_setsLookedAt[position] = set;
        m_positionOfSetLookedAt.addConcurrently(IntHash<uint32_t>::hash(set), position);
    });
    m_positionOfSetLookedAt.didAddConcurrently(numberOfSets);
    m_inputs.build.destroyLater(WTF::move(writes));
    m_inputs.build.destroyLater(WTF::move(writesThroughPlaceholdersInto));
}

void StoresByLayout::noteNamesStoredThroughDeclaredReceivers()
{
    struct Access {
        PropertyKeyID key;
        LayoutSetID familyOfReceiver;
    };

    Vector<NameStoredThroughDeclaredReceiver> names = std::exchange(m_namesStoredThroughDeclaredReceivers, { });
    if (names.isEmpty())
        return;
    AOT::sortInLine(names, { }, &NameStoredThroughDeclaredReceiver::order);
    removeRepeatedElements(names, [](const NameStoredThroughDeclaredReceiver& a, const NameStoredThroughDeclaredReceiver& b) {
        return a.order() == b.order();
    });

    const Program& program = m_inputs.program;
    uint64_t mostLayoutsGivenRoom = m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart);
    auto forEachNameWithReceivers = [&](NOESCAPE const Invocable<void(const NameStoredThroughDeclaredReceiver&, const Vector<LayoutID>&, bool areTooMany)> auto& functor) {
        auto placeholdersOf = [](const NameStoredThroughDeclaredReceiver& name) -> uint8_t {
            constexpr uint8_t everyPlaceholder = 7;
            return name.isStoredByConstructor ? everyPlaceholder : name.placeholders;
        };
        Vector<LayoutID> receivers;
        for (size_t index = 0; index < names.size(); ++index) {
            const NameStoredThroughDeclaredReceiver& name = names[index];
            uint8_t placeholders = placeholdersOf(name);
            if (!index || names[index - 1].family != name.family || placeholdersOf(names[index - 1]) != placeholders) {
                receivers.shrink(0);
                for (LayoutID layout : layoutsAdmittedBy(LayoutSetID(name.family))) {
                    bool mayBeReceiver = (placeholders & 1 && m_hasEscapedObject.quickGet(layout.index())) || (placeholders & 2 && m_hasLostObject.quickGet(layout.index()))
                        || (placeholders & 4 && !m_inputs.sets().isEmpty(m_layoutsOfObjects.objectsWith(layout)));
                    if (mayBeReceiver)
                        receivers.append(layout);
                }
            }
            functor(name, receivers, !name.isStoredByConstructor && receivers.size() > mostLayoutsGivenRoom);
        }
    };
    auto hasNameAnyway = [&](LayoutID layout, PropertyKeyID key) {
        bool isNamed = false;
        forEachTableOfKnownObjects(layout, [&](const StoresIntoObjects& stores) {
            auto found = stores.underKey.find(key.index());
            isNamed |= found != stores.underKey.end() && found->value.isNamedByOperation;
        });
        return isNamed || findField(program, layout, key, BaseChain::Predicted);
    };
    auto noteName = [&](LayoutID layout, const NameStoredThroughDeclaredReceiver& name) {
        StoredUnderKey& stored = m_storesOfLayout[layout.index()].underKey.add(name.key.index(), StoredUnderKey { }).iterator->value;
        stored.isStoredAfterCreation = true;
        stored.isNamedByOperation = true;
        stored.isStoredByConstructor |= name.isStoredByConstructor;
        stored.writers.add(Actor::ReceiversNobodyKnows);
    };

    BitVector keysWithTooManyReceivers(program.types.numberOfPropertyKeys());
    forEachNameWithReceivers([&](const NameStoredThroughDeclaredReceiver& name, const Vector<LayoutID>&, bool areTooMany) {
        if (areTooMany)
            keysWithTooManyReceivers.quickSet(name.key.index());
    });
    AccessesByKeyAndLayout counts;
    if (!keysWithTooManyReceivers.isEmpty()) {
        Vector<Access> accesses = collectInParallel<Access>(m_inputs.loops(), program.functions.size(), [&](unsigned index, Vector<Access>& found) {
            if (!m_inputs.isReached(FunctionID(index)))
                return;
            const Function& function = program.functions[index];
            for (const Value& value : function.values) {
                if (!Value::opcodesWithKey.contains(value.opcode) || !value.key() || !keysWithTooManyReceivers.quickGet(value.key().index()))
                    continue;
                std::optional<uint8_t> receiver = receiverOf(value.opcode);
                if (!receiver)
                    continue;
                const Type& type = program.typeOf(function, value.operand(*receiver));
                if (type.hasOnlyLayouts())
                    found.append({ value.key(), type.layouts });
            }
        });
        for (const Access& access : accesses) {
            const Vector<LayoutID>& admitted = layoutsAdmittedBy(access.familyOfReceiver);
            if (admitted.size() > mostLayoutsGivenRoom)
                continue;
            for (LayoutID layout : admitted)
                ++counts.add(static_cast<uint64_t>(access.key.index()) << 32 | layout.index(), 0).iterator->value;
        }
    }

    forEachNameWithReceivers([&](const NameStoredThroughDeclaredReceiver& name, const Vector<LayoutID>& receivers, bool areTooMany) {
        PropertyKeyID key = name.key;
        if (!areTooMany) {
            for (LayoutID layout : receivers)
                noteName(layout, name);
            return;
        }
        auto countOf = [&](LayoutID layout) {
            return counts.get(static_cast<uint64_t>(key.index()) << 32 | layout.index());
        };
        Vector<LayoutID> layoutsGivenRoom;
        LayoutID firstLayoutWithoutRoom;
        for (LayoutID layout : receivers) {
            if (hasNameAnyway(layout, key))
                noteName(layout, name);
            else if (countOf(layout))
                layoutsGivenRoom.append(layout);
            else if (!firstLayoutWithoutRoom)
                firstLayoutWithoutRoom = layout;
        }
        if (layoutsGivenRoom.size() > mostLayoutsGivenRoom) {
            AOT::sort(layoutsGivenRoom, [&](LayoutID a, LayoutID b) {
                return std::pair { countOf(b), a } < std::pair { countOf(a), b };
            });
            LayoutID firstLayoutPastLimit = *std::ranges::min_element(layoutsGivenRoom.span().subspan(mostLayoutsGivenRoom));
            if (!firstLayoutWithoutRoom || firstLayoutPastLimit < firstLayoutWithoutRoom)
                firstLayoutWithoutRoom = firstLayoutPastLimit;
            layoutsGivenRoom.shrink(mostLayoutsGivenRoom);
        }
        if (firstLayoutWithoutRoom)
            m_inputs.build.gaveUp(LimitID::MaxValueFlowMembersToldApart, Subject { .layout = firstLayoutWithoutRoom });
        for (LayoutID layout : layoutsGivenRoom)
            noteName(layout, name);
    });
}

void StoresByLayout::noteCodeWithoutSolve(const Function& function)
{
    const Program& program = m_inputs.program;
    for (const Value& value : function.values) {
        if (value.opcode == Opcode::NewObject) {
            const Layout& layout = program.at(value.layout());
            if (layout.isEnvironment())
                continue;
            m_hasObjectOfCodeWithoutSolve.quickSet(value.layout().index());
            for (uint32_t field : value.fields()) {
                PropertyKeyID key = layout.fields[field].key;
                note(m_heap.maker(), m_storesOfLayout[value.layout().index()], key ? ValueFlowName::property(program, key) : ValueFlowName::of(ValueFlowNameKind::UnknownKey), m_unknownValue, true, false);
                noteNumbers(m_storesOfLayout[value.layout().index()], key ? ValueFlowName::property(program, key) : ValueFlowName::of(ValueFlowNameKind::UnknownKey), ValueFlowNumbers::any());
            }
            continue;
        }
        bool storesByName = accessOf(value.opcode) == Access::Store || accessOf(value.opcode) == Access::Define;
        bool storesInOtherWays = false;
        for (uint32_t index = 0; index < value.numberOfOperands() && !storesByName; ++index)
            storesInOtherWays |= operandUsesOf(value.opcode, index).containsAny({ OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor });
        if (storesByName && Value::opcodesWithKey.contains(value.opcode) && value.key()) {
            note(m_heap.maker(), m_storesThroughUnknownObject, ValueFlowName::property(program, value.key()), m_unknownValue, false, true);
            noteNumbers(m_storesThroughUnknownObject, ValueFlowName::property(program, value.key()), ValueFlowNumbers::any());
        } else if (storesInOtherWays) {
            note(m_heap.maker(), m_storesThroughUnknownObject, ValueFlowName::of(ValueFlowNameKind::UnknownKey), m_unknownValue, false, false);
            noteNumbers(m_storesThroughUnknownObject, ValueFlowName::of(ValueFlowNameKind::UnknownKey), ValueFlowNumbers::any());
            note(m_heap.maker(), m_storesThroughUnknownObject, ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey), m_unknownValue, false, false);
        }
    }
}

ValueFlowNumbers StoresByLayout::numbersStoredUnder(LayoutID layout, PropertyKeyID key, ObjectsOfLayout objects) const
{
    ValueFlowNumbers numbers;
    forEachSourceOf(layout, objects, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        if (found != stores.underKey.end())
            numbers.unite(found->value.numbers());
        numbers.unite(stores.numbersUnderUnknownKey);
    });
    return numbers;
}

ValueSetID StoresByLayout::storedUnder(ValueSetMaker& maker, LayoutID layout, PropertyKeyID key) const
{
    ValueSetID stored = ValueSets::nothing;
    forEachSourceOf(layout, ObjectsOfLayout::All, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        if (found != stores.underKey.end())
            stored = maker.unite(stored, found->value.value);
        stored = maker.unite(stored, stores.underUnknownKey);
    });
    return stored;
}

ValueSetID StoresByLayout::storedUnderExactName(ValueSetMaker& maker, LayoutID layout, PropertyKeyID key) const
{
    ValueSetID stored = ValueSets::nothing;
    forEachSourceOf(layout, ObjectsOfLayout::All, [&](const StoresIntoObjects& stores) {
        if (auto found = stores.underKey.find(key.index()); found != stores.underKey.end())
            stored = maker.unite(stored, found->value.value);
    });
    return stored;
}

bool StoresByLayout::isStoredAfterCreation(LayoutID layout, PropertyKeyID key) const
{
    bool isStored = false;
    forEachSourceOf(layout, ObjectsOfLayout::All, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        isStored |= (found != stores.underKey.end() && found->value.isStoredAfterCreation) || stores.underUnknownKey != ValueSets::nothing || stores.getsAccessorUnderUnknownKey || stores.getsCopiedProperties;
    });
    return isStored;
}

OptionSet<Actor> StoresByLayout::writersSeenUnder(LayoutID layout, PropertyKeyID key) const
{
    OptionSet<Actor> writers;
    forEachSourceOf(layout, ObjectsOfLayout::All, [&](const StoresIntoObjects& stores) {
        if (auto found = stores.underKey.find(key.index()); found != stores.underKey.end())
            writers.add(found->value.writers);
        writers.add(stores.writersUnderNamesNobodyLists);
    });
    return writers;
}

bool StoresByLayout::getsCopiedProperties(LayoutID layout, ObjectsOfLayout objects) const
{
    bool getsThem = false;
    forEachSourceOf(layout, objects, [&](const StoresIntoObjects& stores) {
        getsThem |= stores.getsCopiedProperties;
    });
    return getsThem;
}

bool StoresByLayout::getsNameNobodyLists(LayoutID layout) const
{
    bool getsOne = false;
    forEachSourceOf(layout, ObjectsOfLayout::All, [&](const StoresIntoObjects& stores) {
        getsOne |= stores.getsNameNobodyLists();
    });
    return getsOne;
}

Vector<PropertyKeyID> StoresByLayout::keysStoredInto(LayoutID layout, ObjectsOfLayout objects) const
{
    Vector<PropertyKeyID> keys = keysStoredIntoKnownObjects(layout);
    uniteSortedKeys(keys, m_keysIntoEveryObject);
    if (m_isEscapedClass.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysIntoEveryEscapedClass);
    if (m_isLostClass.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysIntoEveryLostClass);
    if (objects == ObjectsOfLayout::Followed)
        return keys;
    if (m_hasEscapedObject.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysThroughUnknownObject);
    if (m_hasLostObject.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysThroughUntrackedObject);
    return keys;
}

Vector<PropertyKeyID> StoresByLayout::keysStoredIntoKnownObjects(LayoutID layout) const
{
    Vector<PropertyKeyID> keys = sortedKeysOf(m_storesOfLayout[layout.index()]);
    for (uint32_t table : sharedStoresOf(layout))
        uniteSortedKeys(keys, m_keysOfSharedStores[table]);
    return keys;
}

bool StoresByLayout::isOnlyAssignedByStatements(LayoutID layout, PropertyKeyID key) const
{
    bool isOnlyAssigned = true;
    forEachTableOfKnownObjects(layout, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        if (found == stores.underKey.end())
            return;
        const StoredUnderKey& stored = found->value;
        isOnlyAssigned &= stored.isAssigned && !stored.isWrittenOtherThanByAssignment && stored.writers == OptionSet<Actor> { Actor::Statements } && !stored.isGivenValueByDefinition && !stored.mayBeAccessor;
    });
    return isOnlyAssigned;
}

Vector<PropertyKeyID> StoresByLayout::keysNamedByStoresIntoKnownObjects(LayoutID layout) const
{
    Vector<PropertyKeyID> keys = sortedKeysNamedIn(m_storesOfLayout[layout.index()]);
    for (uint32_t table : sharedStoresOf(layout))
        uniteSortedKeys(keys, m_keysNamedInSharedStores[table]);
    return keys;
}

bool StoresByLayout::isStoredByConstructor(LayoutID layout, PropertyKeyID key) const
{
    bool isStored = false;
    forEachTableOfKnownObjects(layout, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        isStored |= found != stores.underKey.end() && found->value.isStoredByConstructor;
    });
    return isStored;
}

bool StoresByLayout::mayGetAccessorUnder(LayoutID layout, PropertyKeyID key, ObjectsOfLayout objects) const
{
    bool mayGetOne = false;
    forEachSourceOf(layout, objects, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        mayGetOne |= found != stores.underKey.end() && found->value.mayBeAccessor;
    });
    return mayGetOne;
}

// An allocation fills the slots that it lists, whatever flow holds for the values.
bool StoresByLayout::hasAllocationListing(LayoutID layout, PropertyKeyID key) const
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    bool isListed = false;
    m_inputs.sets().forEachMember(m_layoutsOfObjects.objectsWith(layout), [&](ValueFlowObjectID object) {
        const ValueFlowSite* site = universe.siteOf(object);
        if (isListed || !site || site->origin != ValueFlowSiteOrigin::Operation)
            return;
        const Value& allocation = program.at(universe.functionOf(object)).at(site->value);
        if (allocation.opcode != Opcode::NewObject)
            return;
        const Vector<Field>& fields = program.at(allocation.layout()).fields;
        isListed = std::ranges::any_of(allocation.fields(), [&](uint32_t field) { return fields[field].key == key; });
    });
    return isListed;
}

FlowAccessorField StoresByLayout::accessorDefinedUnder(ValueSetMaker& maker, LayoutID layout, PropertyKeyID key) const
{
    const StoresIntoObjects& stores = m_storesOfLayout[layout.index()];
    const ValueSets& sets = m_inputs.sets();
    const Layout& declaring = m_inputs.program.at(layout);
    std::optional<uint32_t> field = declaring.findField(key);
    bool mayBeDefinedUnderUnknownKey = declaring.fields.isEmpty() || (field && !declaring.fields[*field].flags.containsAny({ FieldFlags::IsRequired, FieldFlags::IsRequiredUntilDeleted }));
    ValueSetID getters = mayBeDefinedUnderUnknownKey ? stores.gettersUnderUnknownKey : ValueSets::nothing;
    ValueSetID setters = ValueSets::nothing;
    ValueSetID values = stores.valueFromOperationsUnderUnknownKey;
    uint8_t forms = stores.formsOfAccessorsUnderUnknownKey;
    bool isGivenValue = stores.isGivenValueByOperationUnderUnknownKey;
    ValueSetID valuesUnderKey = ValueSets::nothing;
    bool isGivenValueByOperation = false;
    bool isGivenValueByDefinition = false;
    forEachTableOfKnownObjects(layout, [&](const StoresIntoObjects& table) {
        // A copy lays down a value under every name that it brings, and one whose sources nobody lists may bring any name.
        isGivenValue |= table.getsCopiedProperties && !(table.keysLeftOutOfEveryCopy && table.keysLeftOutOfEveryCopy->contains(key));
        auto found = table.underKey.find(key.index());
        if (found == table.underKey.end())
            return;
        getters = maker.unite(getters, found->value.getters);
        setters = maker.unite(setters, found->value.setters);
        forms |= found->value.accessorForms;
        valuesUnderKey = maker.unite(valuesUnderKey, found->value.valueFromOperations);
        isGivenValueByOperation |= found->value.isGivenValueByOperation;
        isGivenValueByDefinition |= found->value.isGivenValueByDefinition;
    });
    if (setters == ValueSets::nothing) {
        values = maker.unite(values, valuesUnderKey);
        isGivenValue |= isGivenValueByOperation;
    } else
        isGivenValue |= isGivenValueByDefinition;
    if (isGivenValue || (getters == ValueSets::nothing && setters == ValueSets::nothing) || !sets.isSubset(values, m_inputs.universe().undefinedValue()))
        return { };
    std::optional<OptionSet<FieldForm>> form = AccessorForms::onlyFormIn(forms);
    if (!form)
        return { };
    if (sets.meet(m_layoutsOfObjects.objectsWith(layout), m_objectsPutAboveOthers) || hasAllocationListing(layout, key))
        return { };

    FlowAccessorField accessor { *form, { }, { } };
    auto listClosures = [&](ValueSetID functions, Vector<FunctionID, 1>& list) {
        if (ValueSets::isComplement(functions) || sets.size(functions) > m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
            return;
        sets.forEachMember(functions, [&](ValueFlowObjectID member) {
            if (FunctionID function = m_inputs.universe().closureFunction(member))
                list.append(function);
        });
    };
    listClosures(getters, accessor.getters);
    listClosures(setters, accessor.setters);
    return accessor;
}

void StoresByLayout::setReceiversOfReads(ListsOfSetsByNumber&& receivers)
{
    m_receiversOfReads = WTF::move(receivers);
    UnionOfSets above;
    m_heap.forEachSeenValueWrittenUnder(prototypeLinkName, [&](ValueSetID seenValue) {
        above.add(seenValue);
    });
    m_objectsPutAboveOthers = above.takeAsListed(m_heap.maker());

    const ValueSets& sets = m_inputs.sets();
    ValueSetMaker& maker = m_heap.maker();
    uint32_t numberOfLayouts = m_inputs.program.layouts.size();
    m_groupsOfKeysReadFrom = Vector<Vector<uint32_t>>(numberOfLayouts);
    BitVector asks(numberOfLayouts);
    Vector<ValueSetID> objectsOfLayoutsThatAsk;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        bool isLiteralFilledByCopies = m_inputs.program.layouts[index].fields.isEmpty() && getsCopiedProperties(LayoutID(index));
        if (!getsAccessorsUnderKeysNobodyKnows(LayoutID(index)) && !isLiteralFilledByCopies)
            continue;
        asks.quickSet(index);
        objectsOfLayoutsThatAsk.append(m_layoutsOfObjects.objectsWith(LayoutID(index)));
    }
    if (objectsOfLayoutsThatAsk.isEmpty())
        return;
    ValueSetID objects = maker.uniteAll(objectsOfLayoutsThatAsk.span());
    const ParallelLoops& loops = m_inputs.loops();
    Vector<const ListsOfSetsByNumber::KeyValuePairType*> receiversUnderKeys;
    receiversUnderKeys.reserveInitialCapacity(m_receiversOfReads.size());
    for (const auto& receiversUnderKey : m_receiversOfReads)
        receiversUnderKeys.append(&receiversUnderKey);
    ValueSetMakers makers(m_inputs);
    struct GroupsOfWorker {
        HashMap<Vector<uint32_t>, uint32_t> groupOfLayouts;
        Vector<Vector<PropertyKeyID>> keys;
        Vector<uint32_t> layoutsOfKey;
    };
    Vector<GroupsOfWorker> groupsFoundBy(loops.width());
    forEachIndexInParallel(loops, receiversUnderKeys.size(), [&](unsigned index, unsigned worker) {
        GroupsOfWorker& groups = groupsFoundBy[worker];
        groups.layoutsOfKey.shrink(0);
        for (ValueSetID listed : receiversUnderKeys[index]->value) {
            if (!sets.meet(listed, objects))
                continue;
            m_layoutsOfObjects.forEachLayoutOfMembers(makers.of(worker).intersect(listed, objects), [&](LayoutID layout) {
                if (asks.quickGet(layout.index()))
                    groups.layoutsOfKey.append(layout.index());
            });
        }
        if (groups.layoutsOfKey.isEmpty())
            return;
        // A key is read from a layout at many places: most of what was found is a repeat.
        AOT::sort(groups.layoutsOfKey);
        removeRepeatedElements(groups.layoutsOfKey);
        uint32_t group = groups.groupOfLayouts.ensure(groups.layoutsOfKey, [&] {
            groups.keys.append(Vector<PropertyKeyID>());
            return static_cast<uint32_t>(groups.keys.size() - 1);
        }).iterator->value;
        groups.keys[group].append(PropertyKeyID(receiversUnderKeys[index]->key));
    });
    HashMap<Vector<uint32_t>, uint32_t> groupOfLayouts;
    for (GroupsOfWorker& groups : groupsFoundBy) {
        for (auto& entry : groups.groupOfLayouts) {
            uint32_t group = groupOfLayouts.ensure(entry.key, [&] {
                m_groupsOfKeys.append(Vector<PropertyKeyID>());
                for (uint32_t layout : entry.key)
                    m_groupsOfKeysReadFrom[layout].append(static_cast<uint32_t>(m_groupsOfKeys.size() - 1));
                return static_cast<uint32_t>(m_groupsOfKeys.size() - 1);
            }).iterator->value;
            m_groupsOfKeys[group].appendVector(groups.keys[entry.value]);
        }
    }
}

Vector<PropertyKeyID> StoresByLayout::keysReadFrom(LayoutID layout) const
{
    Vector<PropertyKeyID> keys;
    for (uint32_t group : m_groupsOfKeysReadFrom[layout.index()])
        keys.appendVector(m_groupsOfKeys[group]);
    AOT::sort(keys);
    return keys;
}

bool StoresByLayout::getsAccessorsUnderKeysNobodyKnows(LayoutID layout) const
{
    if (!m_inputs.program.at(layout).fields.isEmpty())
        return false;
    const StoresIntoObjects& stores = m_storesOfLayout[layout.index()];
    return stores.gettersUnderUnknownKey != ValueSets::nothing && !stores.isGivenValueByOperationUnderUnknownKey && m_inputs.sets().isSubset(stores.valueFromOperationsUnderUnknownKey, m_inputs.universe().undefinedValue())
        && AccessorForms::onlyFormIn(stores.formsOfAccessorsUnderUnknownKey) && !m_inputs.sets().meet(m_layoutsOfObjects.objectsWith(layout), m_objectsPutAboveOthers);
}

uint8_t StoresByLayout::formsOfValuesGivenUnder(LayoutID layout, PropertyKeyID key) const
{
    uint8_t forms = 0;
    forEachTableOfKnownObjects(layout, [&](const StoresIntoObjects& table) {
        auto found = table.underKey.find(key.index());
        if (found == table.underKey.end())
            return;
        forms |= found->value.formsOfDefinedValues;
        if (!found->value.formsOfDefinedValues || !found->value.isStoredAfterCreation || found->value.mayBeAccessor)
            forms = ValueForms::unknown;
    });
    if (hasAllocationListing(layout, key))
        forms = ValueForms::unknown;
    return forms;
}

Vector<PropertyKeyID> StoresByLayout::keysOfValuesDefinedIn(LayoutID layout) const
{
    Vector<PropertyKeyID> keys;
    forEachTableOfKnownObjects(layout, [&](const StoresIntoObjects& stores) {
        for (const auto& [key, stored] : stores.underKey) {
            if ((stored.formsOfDefinedValues & ~ValueForms::of(true, true, true)) && ValueForms::formToPlanFor(formsOfValuesGivenUnder(layout, PropertyKeyID(key))))
                keys.append(PropertyKeyID(key));
        }
    });
    AOT::sort(keys);
    removeRepeatedElements(keys);
    return keys;
}

Vector<PropertyKeyID> StoresByLayout::keysOfAccessorsDefinedIn(ValueSetMaker& maker, LayoutID layout) const
{
    Vector<PropertyKeyID> keys;
    forEachTableOfKnownObjects(layout, [&](const StoresIntoObjects& stores) {
        for (const auto& [key, stored] : stores.underKey) {
            if ((stored.getters != ValueSets::nothing || stored.setters != ValueSets::nothing) && accessorDefinedUnder(maker, layout, PropertyKeyID(key)).isAccessor())
                keys.append(PropertyKeyID(key));
        }
    });
    AOT::sort(keys);
    removeRepeatedElements(keys);
    return keys;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
