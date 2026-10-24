/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsFieldsInternal.h"

#if ENABLE(AOT)

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
    // What a base class that is no class of the program makes for `super(..)` has the prototype that new.target says, like the allocation in the constructor of a base class.
    if (site->origin == ValueFlowSiteOrigin::BuiltinResult)
        return OperandPosition::parent(allocation.opcode).has_value();
    return site->origin == ValueFlowSiteOrigin::Operation && allocation.opcode == Opcode::NewObject && allocation.prototypeOperand();
}

// The classes whose layout the prototype that an allocation of another class is given may select.
ValueSetID LayoutsOfObjects::collectClassLayouts(ValueFlowHeapView& heap)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueSets& sets = m_inputs.sets();
    Vector<ValueFlowObjectID> prototypeObjects;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
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
    return heap.maker().of(prototypeObjects.span());
}

SetsByNumber LayoutsOfObjects::collectWrittenPrototypeLinks(std::span<const ValueFlowObjectID> objects, ValueFlowHeapView& heap) const
{
    const ValueSets& sets = m_inputs.sets();
    ValueSetMaker& maker = heap.maker();
    ValueSetID all = maker.of(objects);
    ValueSetID throughUnknownObject = ValueSets::nothing;
    ValueSetID throughUntrackedObject = ValueSets::nothing;
    SetsByNumber links;
    auto add = [&](ValueFlowObjectID object, ValueSetID value) {
        ValueSetID& link = links.add(object.index(), ValueSets::nothing).iterator->value;
        link = maker.unite(link, value);
    };
    heap.forEachNodeWrittenUnder(prototypeLinkName, [&](ValueSetID writtenThrough, ValueSetID value) {
        sets.forEachMember(maker.intersect(writtenThrough, all), [&](ValueFlowObjectID object) {
            add(object, value);
        });
        if (sets.contains(writtenThrough, ValueFlowUniverse::unknownObject()))
            throughUnknownObject = maker.unite(throughUnknownObject, value);
        if (sets.contains(writtenThrough, ValueFlowUniverse::untrackedObject()))
            throughUntrackedObject = maker.unite(throughUntrackedObject, value);
    });
    for (ValueFlowObjectID object : objects) {
        if (sets.contains(heap.escaped(), object))
            add(object, throughUnknownObject);
        if (sets.contains(heap.lost(), object))
            add(object, throughUntrackedObject);
    }
    return links;
}

auto LayoutsOfObjects::layoutsSelectedBy(ValueSetID prototypes, LayoutID named, ValueSetID prototypeObjectsOfClasses, ValueFlowHeapView& heap) const -> SelectedLayouts
{
    const ValueSets& sets = m_inputs.sets();
    SelectedLayouts selected;
    sets.forEachMember(heap.maker().intersect(prototypes, prototypeObjectsOfClasses), [&](ValueFlowObjectID prototype) {
        selected.listed.append(m_inputs.universe().classLayoutOf(prototype));
    });
    AOT::sort(selected.listed);
    removeRepeatedElements(selected.listed);
    selected.listed.removeFirst(named);
    selected.everyEscapedClass = !m_escapedClasses.isEmpty() && sets.contains(prototypes, ValueFlowUniverse::unknownObject());
    selected.everyLostClass = !m_lostClasses.isEmpty() && sets.contains(prototypes, ValueFlowUniverse::untrackedObject());
    return selected;
}

LayoutsOfObjects::LayoutsOfObjects(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap)
    : m_inputs(inputs)
{
    struct ObjectOfLayout {
        LayoutID layout;
        ValueFlowObjectID object;
    };

    const ValueFlowUniverse& universe = inputs.universe();
    const ParallelLoops& loops = inputs.loops();
    uint32_t numberOfLayouts = inputs.program.layouts.size();
    Vector<ValueFlowObjectID> objectsThatSelectLayout = collectInParallel<ValueFlowObjectID>(loops, universe.numberOfObjects(), [&](unsigned index, Vector<ValueFlowObjectID>& objects) {
        if (namedLayoutOf(ValueFlowObjectID(index)) && selectsLayoutByPrototype(ValueFlowObjectID(index)))
            objects.append(ValueFlowObjectID(index));
    });
    Vector<ValueFlowObjectID> objectsOfEveryEscapedClass;
    Vector<ValueFlowObjectID> objectsOfEveryLostClass;
    if (!objectsThatSelectLayout.isEmpty()) {
        ValueSetID prototypeObjectsOfClasses = collectClassLayouts(heap);
        SetsByNumber links = collectWrittenPrototypeLinks(objectsThatSelectLayout.span(), heap);
        for (ValueFlowObjectID object : objectsThatSelectLayout) {
            SelectedLayouts selected = layoutsSelectedBy(setAt(links, object.index()), namedLayoutOf(object), prototypeObjectsOfClasses, heap);
            if (selected.everyEscapedClass)
                objectsOfEveryEscapedClass.append(object);
            if (selected.everyLostClass)
                objectsOfEveryLostClass.append(object);
            if (!selected.listed.isEmpty() || selected.everyEscapedClass || selected.everyLostClass)
                m_selectedLayouts.add(object.index(), WTF::move(selected));
        }
    }

    // The objects of a layout side by side.
    Vector<ObjectOfLayout> objectsWithLayouts = collectInParallel<ObjectOfLayout>(loops, universe.numberOfObjects(), [&](unsigned index, Vector<ObjectOfLayout>& found) {
        ValueFlowObjectID object(index);
        LayoutID named = namedLayoutOf(object);
        if (!named)
            return;
        found.append({ named, object });
        auto selected = m_selectedLayouts.find(index);
        if (selected == m_selectedLayouts.end())
            return;
        for (LayoutID layout : selected->value.listed)
            found.append({ layout, object });
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
        ValueSetID set = heap.maker().of(objects.span());
        for (LayoutID layout : layouts)
            m_objectsOfLayout[layout.index()] = heap.maker().unite(m_objectsOfLayout[layout.index()], set);
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
    times.didFinish("stores by layout: copied properties"_s);
}

// The layouts of the objects whose properties are copied onto an object of the layout. None where one of those objects may hold a property that nobody lists, or one that answers by running code.
std::optional<Vector<LayoutID, 4>> StoresByLayout::layoutsCopiedOnto(LayoutID target)
{
    const ValueSets& sets = m_inputs.sets();
    ValueSetID sources = m_heap.writtenUnder(m_layoutsOfObjects.objectsWith(target), ValueFlowName::of(ValueFlowNameKind::CopiedFrom));
    if (ValueSets::isComplement(sources) || sets.meet(sources, m_inputs.universe().placeholdersAlone()))
        return std::nullopt;

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
    AOT::sort(layouts);
    removeRepeatedElements(layouts);

    for (LayoutID layout : layouts) {
        LayoutKind kind = m_inputs.program.at(layout).kind;
        if (kind != LayoutKind::Record && kind != LayoutKind::Instance)
            return std::nullopt;
        if (m_hasEscapedObject.quickGet(layout.index()) || m_hasLostObject.quickGet(layout.index()) || getsNameNobodyLists(layout))
            return std::nullopt;
        for (PropertyKeyID key : keysStoredInto(layout)) {
            if (mayGetAccessorUnder(layout, key))
                return std::nullopt;
        }
    }
    return layouts;
}

// The rules let the target of a copy find what the source holds as it finds what it inherits.
void StoresByLayout::followCopiedProperties()
{
    Vector<LayoutID> targets;
    for (uint32_t index = 0; index < m_storesOfLayout.size(); ++index) {
        if (m_storesOfLayout[index].getsCopiedProperties)
            targets.append(LayoutID(index));
    }
    for (bool hasChanged = !targets.isEmpty(); hasChanged;) {
        hasChanged = false;
        for (LayoutID target : targets) {
            StoresIntoObjects& stores = m_storesOfLayout[target.index()];
            if (!stores.getsCopiedProperties)
                continue;
            // An object of the layout may be the source as well.
            stores.getsCopiedProperties = false;
            std::optional<Vector<LayoutID, 4>> sources = layoutsCopiedOnto(target);
            if (!sources) {
                stores.getsCopiedProperties = true;
                continue;
            }
            for (LayoutID source : *sources) {
                for (PropertyKeyID key : keysStoredInto(source)) {
                    ValueSetID value = storedUnder(source, key);
                    ValueFlowNumbers numbers = numbersStoredUnder(source, key);
                    StoredUnderKey& stored = stores.underKey.add(key.index(), StoredUnderKey { }).iterator->value;
                    unite(stored.value, value);
                    stored.uniteNumbers(numbers);
                    stored.isStoredAfterCreation = true;
                    stored.isNamedByOperation |= !stores.getsCopyWithoutComputedKey;
                }
            }
            hasChanged = true;
        }
    }
}

// Only a function with a Check and with a store through a receiver whose type lists layouts is looked at.
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
                hasTypedReceiver = program.typeOf(function, value.operand(*receiver)).hasOnlyLayouts(program);
            // The type of a stored value is asked for its integers.
            if (receiverOf(value.opcode) || value.opcode == Opcode::NewObject) {
                value.forEachOperand([&](ValueID operand, uint32_t) {
                    hasTypedReceiver |= !program.typeOf(function, operand).integers.isEmpty();
                });
            }
        }
        if (hasCheck && hasTypedReceiver) {
            const BitVector* known = m_inputs.bodies().valuesBehindChecks(FunctionID(index));
            m_valuesBehindChecks[index] = known ? *known : findValuesBehindChecks(program, function, CFG(function));
        }
        m_hasChecksNotLookedAt[index] = hasCheck && !hasTypedReceiver;
    });
}

// The layouts that the type of the receiver admits. None where the type says nothing.
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
    if (!type.hasOnlyLayouts(program) || m_valuesBehindChecks[statement.function.index()].get(object.index()))
        return { };
    return type.layouts;
}

// The solver is asked once, statement by statement, on all threads. Behind each statement stands the family of its receiver, one more than its number, or zero.
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
            // A statement that keeps what all its runs have asked for names the same nodes again and again. In a long list a node stands once.
            HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> nodesListed;
            m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID, ValueFlowNodeID node) {
                if (solver.valueOf(node) == ValueSets::nothing)
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

auto StoresByLayout::findObjectsOf(ValueSetID set) const -> ObjectsOfSet
{
    ObjectsOfSet objects;
    if (ValueSets::isComplement(set)) {
        objects.holdsAlmostEverything = true;
        return objects;
    }
    objects.holdsUnknownObject = m_inputs.sets().contains(set, ValueFlowUniverse::unknownObject());
    objects.holdsUntrackedObject = m_inputs.sets().contains(set, ValueFlowUniverse::untrackedObject());
    LayoutsOfObjects::SharedLists lists = m_layoutsOfObjects.forEachLayoutListedForMembers(set, [&](LayoutID layout) {
        objects.layouts.append(layout);
    });
    objects.holdsObjectOfEveryEscapedClass = lists.everyEscapedClass;
    objects.holdsObjectOfEveryLostClass = lists.everyLostClass;
    AOT::sort(objects.layouts);
    removeRepeatedElements(objects.layouts);
    return objects;
}

auto StoresByLayout::objectsOf(ValueSetID set) -> const ObjectsOfSet&
{
    if (std::optional<uint32_t> position = positionOfSetLookedAt(set))
        return objectsOfSetAt(*position);
    noteObjectsOf(set, findObjectsOf(set));
    return m_objectsOfOtherSets.last();
}

auto StoresByLayout::objectsOfSetAt(uint32_t position) const -> const ObjectsOfSet&
{
    return position < m_objectsOfSetsOfWrites.size() ? m_objectsOfSetsOfWrites[position] : m_objectsOfOtherSets[position - m_objectsOfSetsOfWrites.size()];
}

std::optional<uint32_t> StoresByLayout::positionOfSetLookedAt(ValueSetID set) const
{
    return m_positionOfSetLookedAt.find(IntHash<uint32_t>::hash(set.index()), [&](uint32_t position) {
        return m_setsLookedAt[position] == set.index();
    });
}

void StoresByLayout::noteObjectsOf(ValueSetID set, ObjectsOfSet&& objects)
{
    m_positionOfSetLookedAt.add(IntHash<uint32_t>::hash(set.index()), m_setsLookedAt.size());
    m_setsLookedAt.append(set.index());
    m_objectsOfOtherSets.append(WTF::move(objects));
}

// noteWrites() has looked at the set of every write.
auto StoresByLayout::objectsOfSetOfWrite(ValueSetID set) const -> const ObjectsOfSet&
{
    std::optional<uint32_t> position = positionOfSetLookedAt(set);
    RELEASE_ASSERT(position);
    return objectsOfSetAt(*position);
}

// The type of a receiver is proven, so the object has one of the layouts that it lists, or one that is expected to lie below one of them: an allocation is typed with the layout that it names.
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
        Vector<LayoutID, 16> worklist(program.at(familyOfReceiver).members.span());
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
    for (size_t position = 0; position < unseen.size(); ++position)
        noteObjectsOf(unseen[position], WTF::move(objects[position]));
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
    // A computed key that can be one key alone gets that key as its type, and the store becomes a store by name: `holder[kState] = value`.
    // `const { kState } = require(..)` may find nothing as far as anybody knows. SplitUnions tests such a key, and with the symbol the store is a store by name all the same.
    if (statement.opcode == Opcode::ElemSet || statement.opcode == Opcode::ElemDefine) {
        static_assert(OperandIndex::ElemSet::property == OperandIndex::ElemDefine::property);
        ValueSetID keys = std::as_const(m_heap).valueOf(maker, m_inputs.bodies().operandsOf(statement)[OperandIndex::ElemSet::property]);
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
        return;
    }
}

// None where no operand of the operation is the value.
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

// The node holds what every statement that has asked for it has written, and each of them is looked at. For a node that holds a number.
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
    // What was found about the positions that the value is computed from bounds it too: the parameter of a constructor has no type of its own in untyped code.
    ValueFlowNumbers found = m_inputs.numbersOfValues ? m_inputs.numbersOfValues->numbersOf(statement.function, stored) : ValueFlowNumbers::any();
    // Where a check has failed, the generic version stores what the same operations make of whatever the check has turned away.
    if (m_hasChecksNotLookedAt[statement.function.index()] || m_valuesBehindChecks[statement.function.index()].get(stored.index()))
        return found;
    ValueFlowNumbers numbers;
    numbers.add(program.typeOf(function, stored));
    return numbers.meetWith(found);
}

void StoresByLayout::indexNumbersInData()
{
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

// One object stands for what the constructor of a base class allocates for the class and for every class below it, so a store to a field of one of them reaches the layouts of all.
bool StoresByLayout::someReceiverDeclares(PropertyKeyID key, const ObjectsOfSet& objects) const
{
    if (objects.holdsObjectOfEveryEscapedClass && m_keysDeclaredByEscapedClasses.get(key.index()))
        return true;
    if (objects.holdsObjectOfEveryLostClass && m_keysDeclaredByLostClasses.get(key.index()))
        return true;
    return std::ranges::any_of(objects.layouts, [&](LayoutID layout) {
        return findField(m_inputs.program, layout, key, BaseChain::Predicted).has_value();
    });
}

// The names under which findField() finds a field from one of the layouts along the predicted chain.
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

// The call has one callee, a built-in with a typed result, which stores into nothing that it is handed. So the write is what the compiled parser or validator does that takes the place of the call.
static bool fillsTypedResult(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, const Writer& writer, ValueSetID objects)
{
    const ValueFlowStatement& statement = writer.statement;
    if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Call || ValueSets::isComplement(objects))
        return false;
    ValueFlowObjectID object = inputs.sets().onlyMember(objects);
    const ValueFlowSite* site = object ? inputs.universe().siteOf(object) : nullptr;
    if (!site || site->origin != ValueFlowSiteOrigin::TypedResult || !inputs.universe().isSiteObjectOf(statement, object))
        return false;
    ValueSetID callees = heap.valueOf(maker, inputs.bodies().operandsOf(statement)[0]);
    return !ValueSets::isComplement(callees) && inputs.sets().onlyMember(callees);
}

static bool storesIntoThisOfConstructor(const Program&, const Writer&);

// The type that the receiver of a store has or is tested for, where it lists layouts and nothing else.
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
    if (type.hasOnlyLayouts(program))
        return type.layouts.index();
    // `this[kState] = value` in a constructor function: the receiver of a computed key is converted to any object, and the function says what its `this` is expected to be.
    if (!storesIntoThisOfConstructor(program, writer))
        return UINT32_MAX;
    const Type& typeOfThis = program.at(function.thisType.type);
    return typeOfThis.hasOnlyLayouts(program) ? typeOfThis.layouts.index() : UINT32_MAX;
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
    return definitionThroughSameValues(function, function.at(writer.statement.site).operand(*receiver)) == function.at(entryBlock).parameters[*thisParameter];
}

// `Object.defineProperty(object, key, { .. })`: the literals that the call may get. Nothing for any other call, and where the descriptor may be an object that the program has not made.
std::optional<ValueSetID> StoresByLayout::descriptorsWrittenOutIn(ValueSetMaker& maker, const ValueFlowStatement& statement) const
{
    constexpr size_t positionOfDescriptor = OperandIndex::Call::arguments + 2;
    std::span<const ValueFlowOperand> operands = m_inputs.bodies().operandsOf(statement);
    if (statement.opcode != Opcode::Call || operands.size() != positionOfDescriptor + 1)
        return std::nullopt;
    ValueSetID descriptors = std::as_const(m_heap).valueOf(maker, operands[positionOfDescriptor]);
    const ValueSets& sets = m_inputs.sets();
    if (sets.isEmpty(descriptors) || !sets.isSubset(descriptors, maker.intersect(m_inputs.universe().knownObjects(), ValueSets::complement(m_inputs.universe().builtinObjects()))))
        return std::nullopt;
    return descriptors;
}

// `{ get name() { .. } }` makes a property that is enumerable and configurable. `Object.defineProperty(object, key, { get, enumerable: true })` makes what the literal says: an attribute that it leaves out is false.
uint8_t StoresByLayout::formsOfAccessorDefinedBy(ValueSetMaker& maker, const Writer& writer, ValueFlowName name) const
{
    switch (name.kind) {
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
        break;
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::EveryProperty:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::CopiedFrom:
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
        return 0;
    }
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
        ValueSetID written = std::as_const(m_heap).writtenUnder(maker, *descriptors, ValueFlowName::property(m_inputs.program.keyOf(member)));
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

// Whatever makes a property with a value, or may: not `object.name = value`, which leaves an accessor as it is, and not a descriptor that is written out without `value`.
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

auto StoresByLayout::describeStore(ValueSetMaker& maker, const Writer& writer, ValueFlowName name) const -> StoreUnderName
{
    bool spellsItsKey = spellsKey(maker, writer, name);
    return {
        .statement = &writer.statement,
        .name = name,
        .spellsKey = spellsItsKey,
        .isIntoThisOfConstructor = spellsItsKey && storesIntoThisOfConstructor(m_inputs.program, writer),
        .definesValue = definesValue(maker, writer, name),
        .accessorForms = formsOfAccessorDefinedBy(maker, writer, name),
        .familyDeclaredForReceiver = spellsItsKey ? familyDeclaredForReceiverOf(writer) : UINT32_MAX,
        .numbers = std::nullopt,
    };
}

static bool leavesOutComputedKey(const Program& program, const ValueFlowStatement& statement, ValueFlowName name)
{
    if (name.kind != ValueFlowNameKind::CopiedFrom || statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::CopyDataProperties)
        return false;
    return !program.at(statement.function).at(statement.site).excludedOperands().empty();
}

auto StoresByLayout::describeWrite(ValueSetMaker& maker, StoreUnderName& store, const Writer& writer, const ValueFlowNodeKey& key, ValueSetID value) const -> WritesThroughSet
{
    ValueFlowName name = key.name();
    if (store.statement != &writer.statement || store.name != name)
        store = describeStore(maker, writer, name);
    bool holdsNumber = std::ranges::any_of(std::array { PrimitiveSet::Integer, PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber }, [&](PrimitiveSet kind) {
        return m_inputs.sets().contains(value, ValueFlowUniverse::primitive(kind));
    });
    if (holdsNumber && !store.numbers)
        store.numbers = numbersStoredBy(writer, name);
    ValueFlowNumbers numbers = holdsNumber ? *store.numbers : ValueFlowNumbers { };
    bool fillsResult = fillsTypedResult(m_inputs, m_heap, maker, writer, key.set);
    bool isNamedByOperation = !fillsResult && store.spellsKey;
    return {
        .set = key.set.index(),
        .payload = key.payload,
        .familyOfReceiver = writer.layoutsOfReceiver.empty() ? UINT32_MAX : writer.familyOfReceiver.index(),
        .familyDeclaredForReceiver = isNamedByOperation ? store.familyDeclaredForReceiver : UINT32_MAX,
        .detail = key.detail,
        .fillsTypedResult = fillsResult,
        .createsData = !fillsResult && writer.statement.kind == ValueFlowStatementKind::Operation && writer.statement.opcode == Opcode::Data && !numbers.isEmpty(),
        .isStoredAfterCreation = !writer.isAllocation,
        .isNamedByOperation = isNamedByOperation,
        .isStoredByConstructor = isNamedByOperation && store.isIntoThisOfConstructor,
        .isWrittenByOperation = writer.statement.kind == ValueFlowStatementKind::Operation,
        .givesValueItself = writer.statement.kind == ValueFlowStatementKind::Operation && writer.statement.opcode != Opcode::Call,
        .definesValue = store.definesValue,
        .leavesOutComputedKey = leavesOutComputedKey(m_inputs.program, writer.statement, name),
        .accessorForms = store.accessorForms,
        .value = value,
        .numbers = numbers,
    };
}

auto StoresByLayout::collectWrites(ValueSetMakers& makers) const -> ParallelArray<WritesThroughSet>
{
    const ParallelLoops& loops = m_inputs.loops();
    auto isInEarlierGroup = [](const WritesThroughSet& a, const WritesThroughSet& b) {
        return a.group() < b.group();
    };
    // In whatever order the writes of a group are folded, the same record comes out: the node of the group gives the value.
    auto fold = [](WritesThroughSet& merged, const WritesThroughSet& write) {
        merged.isStoredAfterCreation |= write.isStoredAfterCreation;
        merged.isNamedByOperation |= write.isNamedByOperation;
        merged.isStoredByConstructor |= write.isStoredByConstructor;
        merged.isWrittenByOperation |= write.isWrittenByOperation;
        merged.givesValueItself |= write.givesValueItself;
        merged.definesValue |= write.definesValue;
        merged.leavesOutComputedKey |= write.leavesOutComputedKey;
        merged.accessorForms |= write.accessorForms;
        merged.numbers.unite(write.numbers);
    };
    // Many statements write through one set under one name. Each thread has one record for each group that it has met, so nobody holds a record per statement.
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

void StoresByLayout::noteWritesInto(ValueSetMaker& maker, LayoutID layout, const WritesThroughSet& writes)
{
    StoresIntoObjects& stores = m_storesOfLayout[layout.index()];
    ValueFlowName name = writes.name();
    // A parser puts a member that the layout declares into its field: a required one as it allocates the object, any other behind the allocation, if the text has it.
    if (writes.fillsTypedResult) {
        if (name.kind == ValueFlowNameKind::UnknownKey) {
            stores.isStoredUnderIndex = true;
            return;
        }
        std::optional<FieldReference> field = name.kind == ValueFlowNameKind::Property ? findField(m_inputs.program, layout, PropertyKeyID(name.payload), BaseChain::Predicted) : std::nullopt;
        bool isFilledByAllocation = field && fieldAt(m_inputs.program, *field).flags.contains(FieldFlags::IsRequired);
        note(maker, stores, name, writes.value, isFilledByAllocation, false);
        noteNumbers(stores, name, writes.numbers);
        if (name.kind == ValueFlowNameKind::Property) {
            StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
            stored.isGivenValueByOperation = true;
            stored.isGivenValueByDefinition = true;
        }
        return;
    }
    if (writes.familyOfReceiver != UINT32_MAX && !admits(m_inputs.program.at(LayoutSetID(writes.familyOfReceiver)).members.span(), layout))
        return;
    note(maker, stores, name, writes.value, !writes.isStoredAfterCreation, writes.isNamedByOperation);
    stores.getsCopyWithoutComputedKey |= writes.leavesOutComputedKey;
    noteNumbers(stores, name, writes.createsData ? numbersInData(layout, name) : writes.numbers);
    if (writes.isStoredByConstructor && name.kind == ValueFlowNameKind::Property)
        stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value.isStoredByConstructor = true;
    if (!writes.isWrittenByOperation)
        return;
    if (name.kind == ValueFlowNameKind::Property) {
        StoredUnderKey& stored = stores.underKey.add(name.payload, StoredUnderKey { }).iterator->value;
        stored.valueFromOperations = maker.unite(stored.valueFromOperations, writes.value);
        stored.isGivenValueByOperation |= writes.givesValueItself;
        stored.isGivenValueByDefinition |= writes.definesValue;
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

// Where the proven type of the receiver lists layouts, the object that a placeholder stands for has one of them: noteWrites() puts the write down for those.
void StoresByLayout::noteWritesThroughPlaceholder(ValueSetMaker& maker, StoresIntoObjects& stores, const WritesThroughSet& writes)
{
    if (writes.fillsTypedResult || writes.familyOfReceiver != UINT32_MAX)
        return;
    note(maker, stores, writes.name(), writes.value, false, writes.isNamedByOperation);
    noteNumbers(stores, writes.name(), writes.numbers);
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

    // A store by name whose receiver may have a layout that declares the name is a store to that field. It gives the name no room in the other layouts.
    Vector<ObjectsOfSet> objectsOfSet(numberOfSets);
    forEachIndexInParallel(loops, numberOfSets, [&](unsigned position) {
        objectsOfSet[position] = findObjectsOf(ValueSetID(writes[firstWriteOfSet[position]].set));
        for (WritesThroughSet& write : writesThrough(position)) {
            if (write.isNamedByOperation && someReceiverDeclares(PropertyKeyID(write.payload), objectsOfSet[position]))
                write.isNamedByOperation = false;
        }
    });
    if (std::ranges::any_of(writes.span(), &WritesThroughSet::createsData))
        indexNumbersInData();

    Vector<uint32_t> firstSetOfLayout;
    firstSetOfLayout.fill(0u, numberOfLayouts + 1);
    for (const ObjectsOfSet& objects : objectsOfSet) {
        for (LayoutID layout : objects.layouts)
            ++firstSetOfLayout[layout.index() + 1];
    }
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        firstSetOfLayout[index + 1] += firstSetOfLayout[index];
    Vector<uint32_t> setsOfLayouts;
    setsOfLayouts.grow(firstSetOfLayout.last());
    Vector<uint32_t> nextSetOfLayout(firstSetOfLayout.span().first(numberOfLayouts));
    for (uint32_t position = 0; position < numberOfSets; ++position) {
        for (LayoutID layout : objectsOfSet[position].layouts)
            setsOfLayouts[nextSetOfLayout[layout.index()]++] = position;
    }

    auto isSameForEveryClass = [](const WritesThroughSet& write) {
        ValueFlowNameKind kind = write.name().kind;
        bool liesUnderKey = kind == ValueFlowNameKind::Property || kind == ValueFlowNameKind::Getter || kind == ValueFlowNameKind::Setter;
        return liesUnderKey && write.familyOfReceiver == UINT32_MAX && !write.fillsTypedResult && !write.createsData;
    };
    struct WritesIntoEveryClass {
        StoresIntoObjects& stores;
        bool ObjectsOfSet::* isHeld;
        const BitVector& classes;
        Vector<uint32_t> notedOnce { };
        Vector<uint32_t> notedForEachClass { };
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
                (isSameForEveryClass(writes[index]) ? list.notedOnce : list.notedForEachClass).append(index);
        }
    }

    Vector<Vector<uint32_t>> writesThroughPlaceholdersInto(numberOfLayouts);
    for (uint32_t position = 0; position < numberOfSets; ++position) {
        const ObjectsOfSet& objects = objectsOfSet[position];
        if (!objects.holdsUnknownObject && !objects.holdsUntrackedObject && !objects.holdsAlmostEverything)
            continue;
        for (uint32_t index = firstWriteOfSet[position]; index < firstWriteOfSet[position + 1]; ++index) {
            if (writes[index].fillsTypedResult || writes[index].familyOfReceiver == UINT32_MAX)
                continue;
            for (LayoutID layout : layoutsAdmittedBy(LayoutSetID(writes[index].familyOfReceiver))) {
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
    forEachIndexInParallel(loops, numberOfSharedTables + numberOfLayouts, [&](unsigned index, unsigned worker) {
        ValueSetMaker& maker = makers.of(worker);
        if (index < std::size(placeholders)) {
            for (uint32_t position = 0; position < numberOfSets; ++position) {
                if (!(objectsOfSet[position].*placeholders[index].isHeld))
                    continue;
                for (const WritesThroughSet& write : writesThrough(position))
                    noteWritesThroughPlaceholder(maker, placeholders[index].stores, write);
            }
            return;
        }
        if (index < numberOfSharedTables) {
            WritesIntoEveryClass& list = writesIntoEveryClass[index - std::size(placeholders)];
            for (uint32_t position : list.notedOnce) {
                const WritesThroughSet& write = writes[position];
                note(maker, list.stores, write.name(), write.value, !write.isStoredAfterCreation, false);
                noteNumbers(list.stores, write.name(), write.numbers);
            }
            return;
        }
        uint32_t layout = index - numberOfSharedTables;
        auto noteWritesThroughSets = [&](std::span<const uint32_t> positions) {
            for (uint32_t position : positions) {
                for (const WritesThroughSet& write : writesThrough(position))
                    noteWritesInto(maker, LayoutID(layout), write);
            }
        };
        noteWritesThroughSets(setsOfLayouts.subspan(firstSetOfLayout[layout], firstSetOfLayout[layout + 1] - firstSetOfLayout[layout]));
        for (const WritesIntoEveryClass& list : writesIntoEveryClass) {
            if (!list.classes.quickGet(layout))
                continue;
            for (uint32_t position : list.notedForEachClass)
                noteWritesInto(maker, LayoutID(layout), writes[position]);
        }
        for (uint32_t position : writesThroughPlaceholdersInto[layout]) {
            note(maker, m_storesOfLayout[layout], writes[position].name(), writes[position].value, false, false);
            noteNumbers(m_storesOfLayout[layout], writes[position].name(), writes[position].numbers);
        }
    });

    for (uint32_t position = 0; position < numberOfSets; ++position) {
        const ObjectsOfSet& objects = objectsOfSet[position];
        uint8_t held = objects.holdsUnknownObject | objects.holdsUntrackedObject << 1 | objects.holdsAlmostEverything << 2;
        if (!held)
            continue;
        for (const WritesThroughSet& write : writesThrough(position)) {
            if (write.isNamedByOperation && write.familyDeclaredForReceiver != UINT32_MAX)
                m_namesStoredThroughDeclaredReceivers.append({ write.familyDeclaredForReceiver, held, write.isStoredByConstructor, PropertyKeyID(write.payload) });
        }
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

// The layouts that the type of a receiver admits and that have an object which the placeholders may stand for.
Vector<LayoutID> StoresByLayout::receiversNamedBy(LayoutSetID family, uint8_t placeholders)
{
    Vector<LayoutID> receivers;
    for (LayoutID layout : layoutsAdmittedBy(family)) {
        bool mayBeReceiver = (placeholders & 1 && m_hasEscapedObject.quickGet(layout.index())) || (placeholders & 2 && m_hasLostObject.quickGet(layout.index()))
            || (placeholders & 4 && !m_inputs.sets().isEmpty(m_layoutsOfObjects.objectsWith(layout)));
        if (mayBeReceiver)
            receivers.append(layout);
    }
    return receivers;
}

// How many operations of reached functions name one of the keys on a receiver whose type admits the layout. A type that admits more layouts than `mostLayouts` says nothing about any of them.
auto StoresByLayout::countAccessesThroughTypesOfFewLayouts(const BitVector& keys, uint64_t mostLayouts) -> AccessesByKeyAndLayout
{
    struct Access {
        PropertyKeyID key;
        LayoutSetID familyOfReceiver;
    };

    const Program& program = m_inputs.program;
    Vector<Access> accesses = collectInParallel<Access>(m_inputs.loops(), program.functions.size(), [&](unsigned index, Vector<Access>& found) {
        if (!m_inputs.isReached(FunctionID(index)))
            return;
        const Function& function = program.functions[index];
        for (const Value& value : function.values) {
            if (!Value::opcodesWithKey.contains(value.opcode) || !value.key() || !keys.quickGet(value.key().index()))
                continue;
            std::optional<uint8_t> receiver = receiverOf(value.opcode);
            if (!receiver)
                continue;
            const Type& type = program.typeOf(function, value.operand(*receiver));
            if (type.hasOnlyLayouts(program))
                found.append({ value.key(), type.layouts });
        }
    });
    AccessesByKeyAndLayout counts;
    for (const Access& access : accesses) {
        const Vector<LayoutID>& admitted = layoutsAdmittedBy(access.familyOfReceiver);
        if (admitted.size() > mostLayouts)
            continue;
        for (LayoutID layout : admitted)
            ++counts.add(static_cast<uint64_t>(access.key.index()) << 32 | layout.index(), 0).iterator->value;
    }
    return counts;
}

// A placeholder names no layout, but the type of the receiver does: the object is expected to have one of the layouts that it lists.
void StoresByLayout::noteNamesStoredThroughDeclaredReceivers()
{
    Vector<NameStoredThroughDeclaredReceiver> names = std::exchange(m_namesStoredThroughDeclaredReceivers, { });
    if (names.isEmpty())
        return;
    AOT::sort(names, { }, &NameStoredThroughDeclaredReceiver::order);
    removeRepeatedElements(names, [](const NameStoredThroughDeclaredReceiver& a, const NameStoredThroughDeclaredReceiver& b) {
        return a.order() == b.order();
    });

    const Program& program = m_inputs.program;
    uint64_t mostLayoutsGivenRoom = m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart);
    auto forEachNameWithReceivers = [&](NOESCAPE const Invocable<void(const NameStoredThroughDeclaredReceiver&, const Vector<LayoutID>&, bool areTooMany)> auto& functor) {
        // Whoever constructs with a function that has escaped makes objects of its layout that nobody has seen escape: the placeholder stands for them too.
        auto placeholdersOf = [](const NameStoredThroughDeclaredReceiver& name) -> uint8_t {
            constexpr uint8_t everyPlaceholder = 7;
            return name.isStoredByConstructor ? everyPlaceholder : name.placeholders;
        };
        Vector<LayoutID> receivers;
        for (size_t index = 0; index < names.size(); ++index) {
            const NameStoredThroughDeclaredReceiver& name = names[index];
            if (!index || names[index - 1].family != name.family || placeholdersOf(names[index - 1]) != placeholdersOf(name))
                receivers = receiversNamedBy(LayoutSetID(name.family), placeholdersOf(name));
            functor(name, receivers, !name.isStoredByConstructor && receivers.size() > mostLayoutsGivenRoom);
        }
    };
    auto hasNameAnyway = [&](LayoutID layout, PropertyKeyID key) {
        const auto& underKey = m_storesOfLayout[layout.index()].underKey;
        auto found = underKey.find(key.index());
        return (found != underKey.end() && found->value.isNamedByOperation) || findField(program, layout, key, BaseChain::Predicted);
    };
    auto noteName = [&](LayoutID layout, const NameStoredThroughDeclaredReceiver& name) {
        StoredUnderKey& stored = m_storesOfLayout[layout.index()].underKey.add(name.key.index(), StoredUnderKey { }).iterator->value;
        stored.isStoredAfterCreation = true;
        stored.isNamedByOperation = true;
        stored.isStoredByConstructor |= name.isStoredByConstructor;
    };

    BitVector keysWithTooManyReceivers(program.types.numberOfPropertyKeys());
    forEachNameWithReceivers([&](const NameStoredThroughDeclaredReceiver& name, const Vector<LayoutID>&, bool areTooMany) {
        if (areTooMany)
            keysWithTooManyReceivers.quickSet(name.key.index());
    });
    AccessesByKeyAndLayout counts;
    if (!keysWithTooManyReceivers.isEmpty())
        counts = countAccessesThroughTypesOfFewLayouts(keysWithTooManyReceivers, mostLayoutsGivenRoom);

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

// Whatever such code takes has escaped, so what it stores it stores through the unknown object. What it allocates is in nobody's set, and it fills the fields with values that nobody has looked at.
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
                note(m_heap.maker(), m_storesOfLayout[value.layout().index()], key ? ValueFlowName::property(key) : ValueFlowName::of(ValueFlowNameKind::UnknownKey), m_unknownValue, true, false);
                noteNumbers(m_storesOfLayout[value.layout().index()], key ? ValueFlowName::property(key) : ValueFlowName::of(ValueFlowNameKind::UnknownKey), ValueFlowNumbers::any());
            }
            continue;
        }
        bool storesByName = accessOf(value.opcode) == Access::Store || accessOf(value.opcode) == Access::Define;
        bool storesInOtherWays = false;
        for (uint32_t index = 0; index < value.numberOfOperands() && !storesByName; ++index)
            storesInOtherWays |= operandUsesOf(value.opcode, index).containsAny({ OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor });
        if (storesByName && Value::opcodesWithKey.contains(value.opcode) && value.key()) {
            note(m_heap.maker(), m_storesThroughUnknownObject, ValueFlowName::property(value.key()), m_unknownValue, false, true);
            noteNumbers(m_storesThroughUnknownObject, ValueFlowName::property(value.key()), ValueFlowNumbers::any());
        } else if (storesInOtherWays) {
            note(m_heap.maker(), m_storesThroughUnknownObject, ValueFlowName::of(ValueFlowNameKind::UnknownKey), m_unknownValue, false, false);
            noteNumbers(m_storesThroughUnknownObject, ValueFlowName::of(ValueFlowNameKind::UnknownKey), ValueFlowNumbers::any());
            note(m_heap.maker(), m_storesThroughUnknownObject, ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey), m_unknownValue, false, false);
        }
    }
}

ValueFlowNumbers StoresByLayout::numbersStoredUnder(LayoutID layout, PropertyKeyID key) const
{
    ValueFlowNumbers numbers;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
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
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        if (found != stores.underKey.end())
            stored = maker.unite(stored, found->value.value);
        stored = maker.unite(stored, stores.underUnknownKey);
    });
    return stored;
}

bool StoresByLayout::isStoredAfterCreation(LayoutID layout, PropertyKeyID key) const
{
    bool isStored = false;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        isStored |= (found != stores.underKey.end() && found->value.isStoredAfterCreation) || stores.underUnknownKey != ValueSets::nothing || stores.getsAccessorUnderUnknownKey || stores.getsCopiedProperties;
    });
    return isStored;
}

bool StoresByLayout::getsCopiedProperties(LayoutID layout) const
{
    bool getsThem = false;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        getsThem |= stores.getsCopiedProperties;
    });
    return getsThem;
}

bool StoresByLayout::getsNameNobodyLists(LayoutID layout) const
{
    bool getsOne = false;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        getsOne |= stores.getsNameNobodyLists();
    });
    return getsOne;
}

Vector<PropertyKeyID> StoresByLayout::keysStoredInto(LayoutID layout) const
{
    Vector<PropertyKeyID> keys = sortedKeysOf(m_storesOfLayout[layout.index()]);
    uniteSortedKeys(keys, m_keysIntoEveryObject);
    if (m_isEscapedClass.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysIntoEveryEscapedClass);
    if (m_isLostClass.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysIntoEveryLostClass);
    if (m_hasEscapedObject.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysThroughUnknownObject);
    if (m_hasLostObject.quickGet(layout.index()))
        uniteSortedKeys(keys, m_keysThroughUntrackedObject);
    return keys;
}

Vector<PropertyKeyID> StoresByLayout::keysStoredIntoKnownObjects(LayoutID layout) const
{
    return sortedKeysOf(m_storesOfLayout[layout.index()]);
}

Vector<PropertyKeyID> StoresByLayout::keysNamedByStoresIntoKnownObjects(LayoutID layout) const
{
    Vector<PropertyKeyID> keys;
    for (const auto& [key, stored] : m_storesOfLayout[layout.index()].underKey) {
        if (stored.isNamedByOperation)
            keys.append(PropertyKeyID(key));
    }
    AOT::sort(keys);
    return keys;
}

bool StoresByLayout::isStoredByConstructor(LayoutID layout, PropertyKeyID key) const
{
    const auto& underKey = m_storesOfLayout[layout.index()].underKey;
    auto found = underKey.find(key.index());
    return found != underKey.end() && found->value.isStoredByConstructor;
}

bool StoresByLayout::mayGetAccessorUnder(LayoutID layout, PropertyKeyID key) const
{
    bool mayGetOne = false;
    forEachSourceOf(layout, [&](const StoresIntoObjects& stores) {
        auto found = stores.underKey.find(key.index());
        mayGetOne |= found != stores.underKey.end() && found->value.mayBeAccessor;
    });
    return mayGetOne;
}

// `for (const key of names) Object.defineProperty(object, key, { get: getterFor(key) })`: whatever the names are, each of them is an accessor of that form.
FlowAccessorField StoresByLayout::accessorDefinedUnder(ValueSetMaker& maker, LayoutID layout, PropertyKeyID key) const
{
    const StoresIntoObjects& stores = m_storesOfLayout[layout.index()];
    const ValueSets& sets = m_inputs.sets();
    // What is defined under a key nobody knows speaks for the key only in an object that is born empty: see getsAccessorsUnderKeysNobodyKnows(). It speaks against the key in any.
    ValueSetID getters = m_inputs.program.at(layout).fields.isEmpty() ? stores.gettersUnderUnknownKey : ValueSets::nothing;
    ValueSetID setters = ValueSets::nothing;
    ValueSetID values = stores.valueFromOperationsUnderUnknownKey;
    uint8_t forms = stores.formsOfAccessorsUnderUnknownKey;
    bool isGivenValue = stores.isGivenValueByOperationUnderUnknownKey;
    if (auto found = stores.underKey.find(key.index()); found != stores.underKey.end()) {
        getters = maker.unite(getters, found->value.getters);
        setters = found->value.setters;
        forms |= found->value.accessorForms;
        // `object.name = value` runs the setter where there is one: what it stores is no value of the property.
        if (setters == ValueSets::nothing) {
            values = maker.unite(values, found->value.valueFromOperations);
            isGivenValue |= found->value.isGivenValueByOperation;
        } else
            isGivenValue |= found->value.isGivenValueByDefinition;
    }
    // A descriptor without `value` defines none.
    if (isGivenValue || getters == ValueSets::nothing || !sets.isSubset(values, m_inputs.universe().undefinedValue()))
        return { };
    std::optional<OptionSet<FieldForm>> form = AccessorForms::onlyFormIn(forms);
    if (!form)
        return { };
    // From an heir the run time finds an accessor faster among the extra properties of the object above than in a slot of it.
    if (sets.meet(m_layoutsOfObjects.objectsWith(layout), m_objectsPutAboveOthers))
        return { };

    // A helper that defines the getters of every caller under one roof has hundreds: there the slot takes any function.
    FlowAccessorField accessor { *form, { }, { } };
    // The closures among them are what to expect.
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
    struct KeyReadFromLayout {
        LayoutID layout;
        PropertyKeyID key;
    };

    m_receiversOfReads = WTF::move(receivers);
    // The objects that a link lists. What code that nobody sees may hang above an object that has escaped is a placeholder there.
    UnionOfSets above;
    m_heap.forEachNodeWrittenUnder(prototypeLinkName, [&](ValueSetID, ValueSetID value) {
        above.add(value);
    });
    m_objectsPutAboveOthers = above.takeAsListed(m_heap.maker());

    // Each list of receivers is walked once, for the layouts that ask: never the names once for each layout.
    const ValueSets& sets = m_inputs.sets();
    ValueSetMaker& maker = m_heap.maker();
    uint32_t numberOfLayouts = m_inputs.program.layouts.size();
    m_keysReadFrom = Vector<Vector<PropertyKeyID>>(numberOfLayouts);
    BitVector asks(numberOfLayouts);
    Vector<ValueSetID> objectsOfLayoutsThatAsk;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!getsAccessorsUnderKeysNobodyKnows(LayoutID(index)))
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
    Vector<Vector<KeyReadFromLayout>> keysFoundBy(loops.width());
    forEachIndexInParallel(loops, receiversUnderKeys.size(), [&](unsigned index, unsigned worker) {
        for (ValueSetID listed : receiversUnderKeys[index]->value) {
            if (!sets.meet(listed, objects))
                continue;
            m_layoutsOfObjects.forEachLayoutOfMembers(makers.of(worker).intersect(listed, objects), [&](LayoutID layout) {
                if (asks.quickGet(layout.index()))
                    keysFoundBy[worker].append({ layout, PropertyKeyID(receiversUnderKeys[index]->key) });
            });
        }
    });
    for (const Vector<KeyReadFromLayout>& keysFound : keysFoundBy) {
        for (const KeyReadFromLayout& found : keysFound)
            m_keysReadFrom[found.layout.index()].append(found.key);
    }
    for (Vector<PropertyKeyID>& keys : m_keysReadFrom) {
        AOT::sort(keys);
        removeRepeatedElements(keys);
    }
}

bool StoresByLayout::getsAccessorsUnderKeysNobodyKnows(LayoutID layout) const
{
    // The guess is for an object that is born empty and filled by a loop.
    if (!m_inputs.program.at(layout).fields.isEmpty())
        return false;
    // What is known under a key comes on top of this, so what fails here fails under every key.
    const StoresIntoObjects& stores = m_storesOfLayout[layout.index()];
    return stores.gettersUnderUnknownKey != ValueSets::nothing && !stores.isGivenValueByOperationUnderUnknownKey && m_inputs.sets().isSubset(stores.valueFromOperationsUnderUnknownKey, m_inputs.universe().undefinedValue())
        && AccessorForms::onlyFormIn(stores.formsOfAccessorsUnderUnknownKey) && !m_inputs.sets().meet(m_layoutsOfObjects.objectsWith(layout), m_objectsPutAboveOthers);
}

Vector<PropertyKeyID> StoresByLayout::keysOfAccessorsDefinedIn(ValueSetMaker& maker, LayoutID layout) const
{
    Vector<PropertyKeyID> keys;
    for (const auto& [key, stored] : m_storesOfLayout[layout.index()].underKey) {
        if (stored.getters != ValueSets::nothing && accessorDefinedUnder(maker, layout, PropertyKeyID(key)).isAccessor())
            keys.append(PropertyKeyID(key));
    }
    AOT::sort(keys);
    return keys;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
