/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLedgerOfClassesAndClosures.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTFlowRecords.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTValueFlowRulesHeap.h"
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<ObjectMutation> mutationsOfProperties { ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation };
static constexpr OptionSet<ObjectMutation> storesBehindCreation { ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation };
static constexpr OptionSet<ObjectMutation> mutationsThatAreReaches { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::BecomePrototype };

namespace {

using WrittenNode = ValueFlowHeapIndex::WrittenNode;

enum class Group : uint8_t { Names, WholeObject, Elements, Link };

struct FiledWrite {
    auto order() const { return std::tuple { holder, group, write.under.payload, write.under.kind, sequence }; }

    uint32_t holder { 0 };
    Group group { Group::Names };
    uint32_t sequence { 0 };
    Write write;
};

} // anonymous namespace

static std::optional<Group> groupOf(ValueFlowName name)
{
    ValueFlowNameShape shape = shapeOf(name.kind);
    if (name == prototypeLinkName)
        return Group::Link;
    if (shape.mayBeAnyMember())
        return Group::WholeObject;
    if (!shape.isValueOrAccessor())
        return std::nullopt;
    switch (shape.reach) {
    case ValueFlowNameReach::OneKey:
        return Group::Names;
    case ValueFlowNameReach::Elements:
        return Group::Elements;
    case ValueFlowNameReach::UnknownKey:
    case ValueFlowNameReach::EveryKey:
    case ValueFlowNameReach::NoKey:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WriteForm formOf(ValueFlowName name, OptionSet<ObjectMutation> marked)
{
    switch (shapeOf(name.kind).sort) {
    case ValueFlowNameSort::Getter:
    case ValueFlowNameSort::Setter:
        return marked.contains(ObjectMutation::RedefineProperty) ? WriteForm::DefineAccessor : WriteForm::AtCreation;
    case ValueFlowNameSort::PrototypeLink:
        return marked.contains(ObjectMutation::ChangePrototype) ? WriteForm::Link : WriteForm::AtCreation;
    case ValueFlowNameSort::Deletion:
        return WriteForm::Delete;
    case ValueFlowNameSort::Redefinition:
        return WriteForm::Define;
    case ValueFlowNameSort::Copy:
        return WriteForm::Store;
    case ValueFlowNameSort::Value:
    case ValueFlowNameSort::Other:
        return marked.containsAny(storesBehindCreation) ? WriteForm::Store : WriteForm::AtCreation;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueSetID seenValuesOf(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, ValueSetMaker& maker)
{
    ValueSetID valuesOfNode = operand.node ? inputs.solver().seenValuesOf(operand.node) : ValueSets::nothing;
    return ValueFlowRuleContext::throughMaskOf(operand, operand.constant ? maker.unite(operand.constant, valuesOfNode) : valuesOfNode, inputs.solver(), maker);
}

static LayoutID findClassOfConstructorFunction(const ValueFlowResultInputs& inputs, ValueFlowObjectID closure)
{
    FunctionID function = inputs.universe().closureFunction(closure);
    LayoutID constructed = function ? inputs.program.at(function).constructedClass : LayoutID();
    return constructed && inputs.program.at(constructed).has(LayoutFlags::IsConstructorFunction) ? constructed : LayoutID();
}

LedgerOfClassesAndClosures::LedgerOfClassesAndClosures(const ValueFlowResultInputs& inputs)
    : m_inputs(inputs)
{
    for (unsigned forms = 0; forms < m_reachesOfForms.size(); ++forms) {
        for (unsigned form = 0; form < numberOfReachForms; ++form) {
            if (forms >> form & 1)
                m_reachesOfForms[forms].append(Reach { Actor::Statements, { }, static_cast<ReachForm>(form) });
        }
    }
    BorrowedValueSetMaker makerOfWrites(inputs.makerPool());
    BorrowedValueSetMaker makerOfLinks(inputs.makerPool());
    BorrowedValueSetMaker makerOfReads(inputs.makerPool());
    runTasksInParallel(inputs.loops(), {
        [&] {
            findPrototypeObjectsThatProgramStores(makerOfWrites.get());
            fileWrites(makerOfWrites.get());
        },
        [&] {
            findLinks(makerOfLinks.get());
        },
        [&] {
            findCallablesLookedInto(makerOfReads.get());
        },
    });
}

void LedgerOfClassesAndClosures::findPrototypeObjectsThatProgramStores(ValueSetMaker& maker)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueSets& sets = m_inputs.sets();

    for (uint32_t index = program.numberOfLayoutsAsRead(); index < program.layouts.size(); ++index) {
        if (!program.layouts[index].has(LayoutFlags::IsOfCreatedObject))
            continue;
        ValueSetID handed = ValueSets::nothing;
        bool isKnown = false;
        sets.forEachMember(universe.objectsWith(LayoutID(index)), [&](ValueFlowObjectID created) {
            const ValueFlowSite* site = universe.kindOf(created) == ValueFlowObjectKind::Site ? universe.siteOf(created) : nullptr;
            FunctionID id = site ? universe.functionOf(created) : FunctionID();
            if (!id || !m_inputs.isReached(id))
                return;
            const Function& function = program.at(id);
            for (const ValueFlowStatement& statement : m_inputs.bodies().statementsOf(id.index())) {
                if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::SetLiteralPrototype || function.at(statement.site).objectOperand() != site->value)
                    continue;
                handed = maker.unite(handed, seenValuesOf(m_inputs, m_inputs.bodies().operandsOf(statement)[OperandIndex::SetLiteralPrototype::prototype], maker));
                isKnown = true;
            }
        });
        if (isKnown && !ValueSets::isComplement(handed))
            m_handedToObjectCreate.append({ LayoutID(index), handed });
    }

    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> fewestHandedWith;
    auto noteObjectsHanded = [&](LayoutID classLayout, ValueSetID handed) {
        unsigned numberHanded = 0;
        sets.forEachMember(handed, [&](ValueFlowObjectID stored) {
            numberHanded += universe.kindOf(stored) == ValueFlowObjectKind::Site;
        });
        sets.forEachMember(handed, [&](ValueFlowObjectID stored) {
            if (universe.kindOf(stored) != ValueFlowObjectKind::Site)
                return;
            auto fewest = fewestHandedWith.add(stored.index(), numberHanded);
            if (fewest.isNewEntry || numberHanded < fewest.iterator->value) {
                fewest.iterator->value = numberHanded;
                m_classesOfStoredPrototypeObjects.set(stored.index(), classLayout);
            } else if (numberHanded == fewest.iterator->value && m_classesOfStoredPrototypeObjects.get(stored.index()) != classLayout)
                m_classesOfStoredPrototypeObjects.set(stored.index(), LayoutID());
        });
    };
    for (const auto& [layout, handed] : m_handedToObjectCreate)
        noteObjectsHanded(layout, handed);
    const ValueFlowHeapIndex::NodesWrittenUnderName* written = m_inputs.heapIndex->findNodesWrittenUnder(ValueFlowName::property(program.keyOf(BuiltinNames::prototype)));
    for (const WrittenNode& node : written ? written->nodes.span() : std::span<const WrittenNode>()) {
        if (ValueSets::isComplement(node.objects) || ValueSets::isComplement(node.seenValue))
            continue;
        sets.forEachMember(node.objects, [&](ValueFlowObjectID holder) {
            noteObjectsHanded(findClassOfConstructorFunction(m_inputs, holder), node.seenValue);
        });
    }

    Vector<ValueFlowObjectID> holders;
    for (const auto& [stored, classLayout] : m_classesOfStoredPrototypeObjects) {
        if (!classLayout)
            continue;
        holders.append(ValueFlowObjectID(stored));
        m_storedPrototypeObjectsByClass.append({ classLayout, ValueFlowObjectID(stored) });
    }
    AOT::sort(holders);
    AOT::sort(m_storedPrototypeObjectsByClass);
    m_storedPrototypeObjects = Vector<ValueFlowObjectID>(m_storedPrototypeObjectsByClass.size(), [&](size_t index) {
        return m_storedPrototypeObjectsByClass[index].second;
    });
    m_otherHolders = maker.of(holders.span());
}

std::span<const ValueFlowObjectID> LedgerOfClassesAndClosures::prototypeObjectsStoredFor(LayoutID classLayout) const
{
    auto found = std::ranges::equal_range(m_storedPrototypeObjectsByClass, classLayout, { }, &std::pair<LayoutID, ValueFlowObjectID>::first);
    return m_storedPrototypeObjects.span().subspan(found.begin() - m_storedPrototypeObjectsByClass.begin(), found.size());
}

bool LedgerOfClassesAndClosures::isHolder(ValueFlowObjectID object) const
{
    switch (m_inputs.universe().kindOf(object)) {
    case ValueFlowObjectKind::Closure:
    case ValueFlowObjectKind::Prototype:
        return true;
    case ValueFlowObjectKind::Site:
        return m_inputs.sets().contains(m_otherHolders, object);
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::Key:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void LedgerOfClassesAndClosures::fileWrites(ValueSetMaker& maker)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueSets& sets = m_inputs.sets();
    const ParallelLoops& loops = m_inputs.loops();

    Vector<ValueFlowObjectID> prototypeObjects(program.layouts.size(), [&](size_t index) {
        return universe.prototypeOf(LayoutID(static_cast<uint32_t>(index)));
    });
    ValueSetID holders = maker.unite(maker.unite(universe.closures(), maker.of(prototypeObjects.span())), m_otherHolders);

    Vector<ValueFlowHeapIndex::Stretch> stretches = m_inputs.heapIndex->stretchesOfNodesWrittenUnder([](ValueFlowName name) {
        return !!groupOf(name);
    });
    Vector<SetsMeetingOneSet> setsWithHolders(loops.width(), [&](size_t) {
        return SetsMeetingOneSet(sets, holders);
    });
    Vector<Vector<FiledWrite>> filedOfStretch(stretches.size());
    forEachIndexInParallel(loops, stretches.size(), [&](unsigned position, unsigned worker) {
        ValueFlowName name = stretches[position].name;
        Group group = *groupOf(name);
        for (const WrittenNode& node : stretches[position].nodes) {
            Write write { .actor = group == Group::WholeObject ? Actor::KeysNobodyKnows : Actor::Statements, .arrives = node.value, .under = name, .through = node.objects, .seenToArrive = node.seenValue };
            if (ValueSets::isComplement(node.objects)) {
                write.actor = Actor::ReceiversNobodyKnows;
                filedOfStretch[position].append({ UINT32_MAX, group, 0, write });
                continue;
            }
            if (!setsWithHolders[worker].contains(node.objects))
                continue;
            sets.forEachMember(node.objects, [&](ValueFlowObjectID object) {
                if (isHolder(object))
                    filedOfStretch[position].append({ object.index(), group, 0, write });
            });
        }
    });

    Vector<FiledWrite> filed;
    for (Vector<FiledWrite>& ofStretch : filedOfStretch)
        filed.appendVector(WTF::move(ofStretch));
    auto fileWhatIsMarked = [&](ValueFlowObjectID holder) {
        OptionSet<ObjectMutation> marked = markedMutationsOf(m_inputs, holder);
        auto file = [&](ObjectMutation mutation, WriteForm form, ValueFlowName under) {
            if (marked.contains(mutation))
                filed.append({ holder.index(), Group::WholeObject, 0, Write { .actor = Actor::Statements, .form = form, .arrives = ValueSets::nothing, .under = under, .through = ValueSets::nothing, .seenToArrive = ValueSets::nothing } });
        };
        file(ObjectMutation::DeleteField, WriteForm::Delete, ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey));
        file(ObjectMutation::RedefineProperty, WriteForm::Define, ValueFlowName::of(ValueFlowNameKind::RedefinedMemberUnderUnknownKey));
        file(ObjectMutation::ChangeIntegrity, WriteForm::Freeze, everyOwnPropertyName);
    };
    sets.forEachMember(holders, fileWhatIsMarked);
    for (uint32_t index = 0; index < filed.size(); ++index)
        filed[index].sequence = index;
    parallelSort(loops, filed.mutableSpan(), [](const FiledWrite& a, const FiledWrite& b) {
        return a.order() < b.order();
    });

    m_writes.reserveInitialCapacity(filed.size());
    for (size_t first = 0, end = 0; first < filed.size(); first = end) {
        uint32_t holder = filed[first].holder;
        while (end < filed.size() && filed[end].holder == holder)
            ++end;
        if (holder == UINT32_MAX) {
            for (size_t index = first; index < end; ++index) {
                m_namesAreWrittenThroughEveryObject |= filed[index].group == Group::Names || filed[index].group == Group::WholeObject;
                m_elementsAreWrittenThroughEveryObject |= filed[index].group == Group::Elements;
                m_linksAreWrittenThroughEveryObject |= filed[index].group == Group::Link;
                filed[index].write.form = formOf(filed[index].write.under, allObjectMutations);
                m_writesThroughEveryObject.append(filed[index].write);
            }
            continue;
        }
        OptionSet<ObjectMutation> marked = markedMutationsOf(m_inputs, ValueFlowObjectID(holder));
        WritesOfHolder writes { .holder = holder, .first = static_cast<uint32_t>(m_writes.size()) };
        std::span<const FiledWrite> filedOnHolder = filed.span().subspan(first, end - first);
        auto endOf = [&](Group group) {
            return writes.first + static_cast<uint32_t>(std::ranges::upper_bound(filedOnHolder, group, { }, &FiledWrite::group) - filedOnHolder.begin());
        };
        writes.endOfNames = endOf(Group::Names);
        writes.endOfWholeObject = endOf(Group::WholeObject);
        writes.endOfElements = endOf(Group::Elements);
        writes.end = endOf(Group::Link);
        for (size_t index = first; index < end; ++index) {
            Write& write = filed[index].write;
            if (write.through != ValueSets::nothing) {
                write.form = formOf(write.under, marked);
                if (write.form == WriteForm::AtCreation)
                    write.actor = Actor::Creation;
            }
            if (write.form != WriteForm::Freeze && write.form != WriteForm::AtCreation) {
                writes.isStoredToUnderKeysNobodyKnows |= filed[index].group == Group::WholeObject && write.form != WriteForm::Delete;
                writes.isDeletedFromOrStoredToUnderIndexes |= filed[index].group == Group::Elements || (filed[index].group == Group::WholeObject && write.form == WriteForm::Delete);
            }
            m_writes.append(write);
        }
        m_writesOfHolders.append(writes);
    }
}

void LedgerOfClassesAndClosures::findLinks(ValueSetMaker& maker)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueFlowHeapIndex& heapIndex = *m_inputs.heapIndex;
    const ValueSets& sets = m_inputs.sets();
    auto unite = [&](ValueSetID& into, ValueSetID value) {
        into = maker.unite(into, value);
    };

    Vector<ValueFlowObjectID> prototypesOfClasses;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (isClassLayout(program.layouts[index]))
            prototypesOfClasses.append(universe.prototypeOf(LayoutID(index)));
    }
    m_classPrototypes = maker.of(prototypesOfClasses.span());
    for (ValueFlowNodeID node : heapIndex.nodesOfMarks()) {
        if (!m_inputs.solver().keyOfMadeNode(node)->detail)
            m_nodesOfMarks.add(m_inputs.solver().keyOfMadeNode(node)->payload, node);
    }

    Vector<WrittenNode> storesToPrototypePropertyOfOtherObjects;
    Vector<WrittenNode> copies;
    ValueFlowName prototypeProperty = ValueFlowName::property(program.keyOf(BuiltinNames::prototype));
    for (const ValueFlowHeapIndex::Stretch& stretch : heapIndex.stretchesOfNodesWrittenUnder([&](ValueFlowName name) {
        return name.kind == ValueFlowNameKind::PrototypeLink || name.kind == ValueFlowNameKind::UnknownKey || name.kind == ValueFlowNameKind::CopiedFrom || name == prototypeProperty;
    })) {
        for (const WrittenNode& node : stretch.nodes) {
            if (stretch.name.kind == ValueFlowNameKind::PrototypeLink) {
                unite(m_objectsLinkedAbove, node.value);
                if (ValueSets::isComplement(node.objects)) {
                    unite(m_linksOfAllObjects, node.value);
                    continue;
                }
                sets.forEachMember(node.objects, [&](ValueFlowObjectID object) {
                    unite(m_linksByObject.add(object.index(), ValueSets::nothing).iterator->value, node.value);
                });
                continue;
            }
            bool mayWriteIntoFunction = sets.meet(node.objects, universe.callables());
            if (stretch.name.kind == ValueFlowNameKind::CopiedFrom) {
                copies.append({ node.objects, maker.intersect(node.value, universe.objects()) });
                if (mayWriteIntoFunction)
                    unite(m_functionsWithNonObjectPrototype, node.objects);
            } else if (mayWriteIntoFunction) {
                unite(m_objectsLinkedAbove, node.value);
                if (!sets.isSubset(node.value, universe.objects()))
                    unite(m_functionsWithNonObjectPrototype, node.objects);
            } else
                storesToPrototypePropertyOfOtherObjects.append(node);
        }
    }

    auto standsForMemberOf = [&](ValueSetID holdingPlaceholder, ValueSetID other) {
        return (sets.contains(holdingPlaceholder, ValueFlowUniverse::unknownObject()) && sets.meet(other, m_inputs.solver().possibleValuesOf(ValueFlowProgramNodes::escaped())))
            || (sets.contains(holdingPlaceholder, ValueFlowUniverse::untrackedObject()) && sets.meet(other, m_inputs.solver().possibleValuesOf(ValueFlowProgramNodes::lost())));
    };
    auto mayShareObject = [&](ValueSetID first, ValueSetID second) {
        return sets.meet(first, second) || standsForMemberOf(first, second) || standsForMemberOf(second, first);
    };
    ValueSetID sources = ValueSets::nothing;
    for (bool hasGrown = true; hasGrown;) {
        hasGrown = false;
        for (const WrittenNode& copy : copies) {
            if (sets.isSubset(copy.value, sources) || (!sets.meet(copy.objects, universe.callables()) && !mayShareObject(copy.objects, sources)))
                continue;
            unite(sources, copy.value);
            hasGrown = true;
        }
    }
    for (const WrittenNode& store : storesToPrototypePropertyOfOtherObjects) {
        if (mayShareObject(store.objects, sources))
            unite(m_objectsLinkedAbove, store.value);
    }
}

static constexpr bool looksIntoObject(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
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
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
        return true;
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
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

void LedgerOfClassesAndClosures::findCallablesLookedInto(ValueSetMaker& maker)
{
    struct FoundByWorker {
        ValueSetsMet seenSets;
        Vector<ValueSetID> callablesInSets;
    };
    constexpr uint32_t nodesPerTask = 4096;

    const ValueFlowSolver& solver = m_inputs.solver();
    ValueSetID callables = m_inputs.universe().callables();
    uint32_t firstMadeNode = solver.input().numberOfNodes;
    uint32_t numberOfMadeNodes = solver.graph().numberOfNodes() - firstMadeNode;
    ValueSetMakers makers(m_inputs);
    Vector<FoundByWorker> foundByWorker(m_inputs.loops().width());
    forEachIndexInParallel(m_inputs.loops(), (numberOfMadeNodes + nodesPerTask - 1) / nodesPerTask, [&](unsigned task, unsigned worker) {
        FoundByWorker& ofWorker = foundByWorker[worker];
        uint32_t end = std::min(numberOfMadeNodes, (task + 1) * nodesPerTask);
        for (uint32_t index = task * nodesPerTask; index < end; ++index) {
            const ValueFlowNodeKey& key = *solver.keyOfMadeNode(ValueFlowNodeID(firstMadeNode + index));
            switch (key.kind) {
            case ValueFlowMadeNodeKind::Read:
            case ValueFlowMadeNodeKind::Lookup:
            case ValueFlowMadeNodeKind::ReadUnderKind:
                break;
            case ValueFlowMadeNodeKind::Constant:
            case ValueFlowMadeNodeKind::Written:
            case ValueFlowMadeNodeKind::CallGroup:
            case ValueFlowMadeNodeKind::Hub:
            case ValueFlowMadeNodeKind::Masked:
            case ValueFlowMadeNodeKind::Marked:
            case ValueFlowMadeNodeKind::WrittenUnderKind:
            case ValueFlowMadeNodeKind::Instance:
                continue;
            }
            if (key.set == m_inputs.universe().knownObjects())
                continue;
            if (looksIntoObject(key.name().kind) && ofWorker.seenSets.add(key.set) && m_inputs.sets().meet(key.set, callables))
                ofWorker.callablesInSets.append(makers.of(worker).intersect(key.set, callables));
        }
    });
    Vector<ValueSetID> callablesInSets;
    for (FoundByWorker& ofWorker : foundByWorker)
        callablesInSets.appendVector(WTF::move(ofWorker.callablesInSets));
    sortAndRemoveDuplicates(m_inputs.loops(), callablesInSets);
    m_callablesLookedInto = maker.uniteAll(callablesInSets.span());
}

std::array<ValueFlowObjectID, 3> LedgerOfClassesAndClosures::standInsOf(ValueFlowObjectID object) const
{
    return { object, m_inputs.hasEscaped(object) ? ValueFlowUniverse::unknownObject() : ValueFlowObjectID(), m_inputs.isLost(object) ? ValueFlowUniverse::untrackedObject() : ValueFlowObjectID() };
}

bool LedgerOfClassesAndClosures::covers(ValueSetID set, ValueFlowObjectID object) const
{
    return std::ranges::any_of(standInsOf(object), [&](ValueFlowObjectID standIn) {
        return standIn && m_inputs.sets().contains(set, standIn);
    });
}

ValueSetID LedgerOfClassesAndClosures::linkedPrototypes(ValueSetMaker& maker, ValueFlowObjectID object) const
{
    ValueSetID links = m_linksOfAllObjects;
    for (ValueFlowObjectID standIn : standInsOf(object)) {
        if (auto found = standIn ? m_linksByObject.find(standIn.index()) : m_linksByObject.end(); found != m_linksByObject.end())
            links = maker.unite(links, found->value);
    }
    return links;
}

const LedgerOfClassesAndClosures::WritesOfHolder* LedgerOfClassesAndClosures::findWritesOf(ValueFlowObjectID holder) const
{
    auto found = std::ranges::lower_bound(m_writesOfHolders, holder.index(), { }, &WritesOfHolder::holder);
    return found != m_writesOfHolders.end() && found->holder == holder.index() ? &*found : nullptr;
}

OptionSet<Actor> LedgerOfClassesAndClosures::openOn(ValueFlowObjectID holder, PlaceKind kind, const WritesOfHolder* writes, Deletions deletions) const
{
    OptionSet<ObjectMutation> marked = markedMutationsOf(m_inputs, holder);
    OptionSet<Actor> open = actorsOpenOn(m_inputs, holder);
    bool isOutOfSight = m_inputs.hasEscaped(holder) || m_inputs.isLost(holder);
    switch (kind) {
    case PlaceKind::PrototypeLink:
        if (m_linksAreWrittenThroughEveryObject || (marked.contains(ObjectMutation::ChangePrototype) && (isOutOfSight || !writes || writes->endOfElements == writes->end)))
            open.add(Actor::ReceiversNobodyKnows);
        break;
    case PlaceKind::Property:
    case PlaceKind::Accessor:
    case PlaceKind::Elements:
        if (writes && (writes->isStoredToUnderKeysNobodyKnows || (writes->isDeletedFromOrStoredToUnderIndexes && deletions == Deletions::MayHitPlace)))
            open.add(Actor::KeysNobodyKnows);
        [[fallthrough]];
    case PlaceKind::EveryOwnProperty:
        if (m_namesAreWrittenThroughEveryObject || (kind == PlaceKind::Elements && m_elementsAreWrittenThroughEveryObject) || (isOutOfSight && marked.containsAny(mutationsOfProperties)))
            open.add(Actor::ReceiversNobodyKnows);
        if (marked.contains(ObjectMutation::ReachNativeCode) && marked.containsAny(mutationsOfProperties))
            open.add(Actor::Table);
        break;
    case PlaceKind::Variable:
    case PlaceKind::EnvironmentField:
    case PlaceKind::Field:
    case PlaceKind::Cell:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return open & actorsThatCanWrite(kind);
}

Writers LedgerOfClassesAndClosures::accountOf(ValueFlowObjectID holder, PlaceKind kind, ValueFlowName name) const
{
    const WritesOfHolder* writes = findWritesOf(holder);
    // The language lets nobody delete the prototype property of a function, and no index names it.
    bool isPrototypeOfFunction = kind == PlaceKind::Property && PropertyKeyID(name.payload) == m_inputs.program.keyOf(BuiltinNames::prototype) && m_inputs.universe().kindOf(holder) == ValueFlowObjectKind::Closure;
    OptionSet<Actor> open = openOn(holder, kind, writes, isPrototypeOfFunction ? Deletions::CannotHitPlace : Deletions::MayHitPlace);
    // A built-in that fills the elements of an operand sets its length and lists no name.
    if (kind == PlaceKind::Property && PropertyKeyID(name.payload) == m_inputs.program.keyOf(BuiltinNames::length) && markedMutationsOf(m_inputs, holder).containsAny(storesBehindCreation))
        open.add(Actor::Table);
    if (!writes)
        return { { }, open };
    auto between = [&](uint32_t first, uint32_t end) {
        return m_writes.span().subspan(first, end - first);
    };
    switch (kind) {
    case PlaceKind::Property:
        return { std::span(std::ranges::equal_range(between(writes->first, writes->endOfNames), name.payload, { }, [](const Write& write) { return write.under.payload; })), open };
    case PlaceKind::Accessor:
        return { std::span(std::ranges::equal_range(between(writes->first, writes->endOfNames), std::tuple { name.payload, name.kind }, { }, [](const Write& write) { return std::tuple { write.under.payload, write.under.kind }; })), open };
    case PlaceKind::EveryOwnProperty:
        return { between(writes->first, writes->endOfElements), open };
    case PlaceKind::Elements:
        return { between(writes->endOfWholeObject, writes->endOfElements), open };
    case PlaceKind::PrototypeLink:
        return { between(writes->endOfElements, writes->end), open };
    case PlaceKind::Variable:
    case PlaceKind::EnvironmentField:
    case PlaceKind::Field:
    case PlaceKind::Cell:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Writers> LedgerOfClassesAndClosures::writersOf(const Place& place) const
{
    if (!place.liesOnObjects() || place.kind() == PlaceKind::Cell || place.search() != Place::Search::Own || ValueSets::isComplement(place.holders()))
        return std::nullopt;
    ValueFlowObjectID holder = m_inputs.sets().onlyMember(place.holders());
    if (!holder || !isHolder(holder))
        return std::nullopt;
    return accountOf(holder, place.kind(), place.name());
}

Writers LedgerOfClassesAndClosures::writersOfProperty(ValueFlowObjectID holder, PropertyKeyID key) const
{
    ASSERT(isHolder(holder));
    ValueFlowName name = ValueFlowName::property(m_inputs.program, key);
    return accountOf(holder, name.kind == ValueFlowNameKind::Elements ? PlaceKind::Elements : PlaceKind::Property, name);
}

Writers LedgerOfClassesAndClosures::writersOfEveryOwnProperty(ValueFlowObjectID holder) const
{
    ASSERT(isHolder(holder));
    return accountOf(holder, PlaceKind::EveryOwnProperty, everyOwnPropertyName);
}

Writers LedgerOfClassesAndClosures::writersOfElements(ValueFlowObjectID holder) const
{
    ASSERT(isHolder(holder));
    return accountOf(holder, PlaceKind::Elements, elementsName);
}

Writers LedgerOfClassesAndClosures::writersOfPrototypeLink(ValueFlowObjectID holder) const
{
    ASSERT(isHolder(holder));
    return accountOf(holder, PlaceKind::PrototypeLink, prototypeLinkName);
}

std::span<const Write> LedgerOfClassesAndClosures::writesFiledOn(ValueFlowObjectID holder) const
{
    const WritesOfHolder* writes = findWritesOf(holder);
    return writes ? m_writes.span().subspan(writes->first, writes->endOfElements - writes->first) : std::span<const Write>();
}

OptionSet<Actor> LedgerOfClassesAndClosures::handsOpenOn(ValueFlowObjectID object) const
{
    OptionSet<ObjectMutation> marked = markedMutationsOf(m_inputs, object);
    OptionSet<Actor> open;
    if (m_inputs.hasEscaped(object) || m_inputs.isLost(object))
        open.add(Actor::ReceiversNobodyKnows);
    if (m_inputs.isHeldByNativeCode(object) || marked.contains(ObjectMutation::ReachNativeCode))
        open.add(Actor::NativeCode);
    if (marked.contains(ObjectMutation::ReachUnknownJavaScript))
        open.add(Actor::UnseenCode);
    return open;
}

std::optional<Reachers> LedgerOfClassesAndClosures::reachersOf(ValueFlowObjectID object) const
{
    return reachersOf(object, ValueSets::nothing);
}

std::optional<Reachers> LedgerOfClassesAndClosures::reachersOf(ValueFlowObjectID object, ValueSetID closuresTakenOutOfMembers) const
{
    const ValueSets& sets = m_inputs.sets();
    if (ValueFlowResultInputs::isPlaceholder(object) || !sets.contains(m_inputs.universe().callables(), object) || sets.contains(m_inputs.universe().builtinObjects(), object))
        return std::nullopt;

    OptionSet<ObjectMutation> marked = markedMutationsOf(m_inputs, object);
    OptionSet<Actor> open = handsOpenOn(object);
    unsigned forms = 0;
    auto add = [&](ReachForm form, bool isSeen) {
        forms |= static_cast<unsigned>(isSeen) << static_cast<unsigned>(form);
    };
    add(ReachForm::LooksInto, sets.contains(m_callablesLookedInto, object));
    add(ReachForm::TakesOutOfMember, sets.contains(closuresTakenOutOfMembers, object));
    add(ReachForm::Changes, !(marked - mutationsThatAreReaches).isEmpty());
    add(ReachForm::LinksAbove, marked.contains(ObjectMutation::BecomePrototype) || mayBeLinkedAbove(object));
    return Reachers { m_reachesOfForms[forms].span(), open };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
