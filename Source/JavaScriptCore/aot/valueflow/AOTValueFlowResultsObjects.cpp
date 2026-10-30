/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTClaimsAboutClassesAndClosures.h"
#include "AOTFlowRecords.h"
#include "AOTLedgerOfClassesAndClosures.h"
#include "AOTPhaseTimes.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTValueFlowResultsFieldsInternal.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueIdentity.h"
#include "AOTWrittenFunction.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<ObjectMutation> mutationsWithoutMark { ObjectMutation::StoreOfOtherType };

namespace {

using PrototypeLinks = LedgerOfClassesAndClosures;

struct ObjectsOfLayouts {
    void add(LayoutID layout, OptionSet<ObjectMutation> mutations, bool escapes)
    {
        isAllocated.quickSet(layout.index());
        possibleMutations[layout.index()].add(mutations);
        if (escapes)
            mayEscape.quickSet(layout.index());
    }

    BitVector isAllocated;
    BitVector mayEscape;
    Vector<OptionSet<ObjectMutation>> possibleMutations;
};

struct Allocation {
    FunctionID function;
    ValueID value;
};

struct MutationCause {
    ObjectMutation mutation;
    UnprovenCause cause;
};

struct ObjectCreation {
    bool mayHaveOtherPrototype { false };
    ValueSetID layoutSelectors { ValueSets::nothing };
};

} // anonymous namespace

static constexpr MutationCause mutationCauses[] {
    { ObjectMutation::ReachUnknownJavaScript, UnprovenCause::ReachesUnknownJavaScript },
    { ObjectMutation::DeleteField, UnprovenCause::FieldDeleted },
    { ObjectMutation::RedefineProperty, UnprovenCause::PropertyRedefined },
    { ObjectMutation::AddProperty, UnprovenCause::PropertyAdded },
    { ObjectMutation::ChangePrototype, UnprovenCause::PrototypeChanged },
    { ObjectMutation::ChangeIntegrity, UnprovenCause::IntegrityChanged },
};

static BitVector findLayoutsWithInstances(const ValueFlowResultInputs& inputs)
{
    struct FoundByWorker {
        ValueSetsMet seenSets;
        Vector<ValueSetID> instancesInSets;
    };
    constexpr uint32_t nodesPerTask = 4096;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowGraph& graph = inputs.solver().graph();
    uint32_t numberOfNodes = graph.numberOfNodes();
    ValueSetMakers makers(inputs);
    Vector<ValueFlowObjectID> everyInstance(inputs.program.layouts.size(), [&](size_t layout) {
        return universe.instanceOf(LayoutID(static_cast<uint32_t>(layout)));
    });
    ValueSetID instances = makers.of(0).of(everyInstance.span());

    Vector<FoundByWorker> foundByWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), (numberOfNodes + nodesPerTask - 1) / nodesPerTask, [&](unsigned task, unsigned worker) {
        FoundByWorker& found = foundByWorker[worker];
        for (uint32_t index = task * nodesPerTask; index < std::min(numberOfNodes, (task + 1) * nodesPerTask); ++index) {
            ValueSetID set = graph.valueOf(ValueFlowNodeID(index));
            if (found.seenSets.add(set) && inputs.sets().meet(set, instances))
                found.instancesInSets.append(makers.of(worker).intersect(set, instances));
        }
    });
    Vector<ValueSetID> instancesInSets;
    for (FoundByWorker& found : foundByWorker)
        instancesInSets.appendVector(WTF::move(found.instancesInSets));
    sortAndRemoveDuplicates(inputs.loops(), instancesInSets);

    BitVector layouts(inputs.program.layouts.size());
    inputs.sets().forEachMember(makers.of(0).uniteAll(instancesInSets.span()), [&](ValueFlowObjectID instance) {
        layouts.quickSet(universe.layoutOf(instance).index());
    });
    return layouts;
}

static OptionSet<ObjectMutation> escapedObjectMutations(const ValueFlowResultInputs& inputs)
{
    if (unseenCodeCanRun(inputs.solution))
        return allObjectMutations;
    return (inputs.solver().marksOf(ValueFlowUniverse::unknownObject()) & allObjectMutations) | ObjectMutation::ReachNativeCode;
}

OptionSet<ObjectMutation> markedMutationsOf(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    OptionSet<ObjectMutation> marked = inputs.solver().marksOf(object) & allObjectMutations;
    return inputs.hasEscaped(object) ? marked | escapedObjectMutations(inputs) : marked;
}

bool neverChangesOnceMade(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    return !markedMutationsOf(inputs, object).containsAny({ ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty,
        ObjectMutation::AddProperty, ObjectMutation::ChangePrototype, ObjectMutation::StoreAfterCreation });
}

// Of a list that never changes once made.
bool isNeverToldApart(const ValueFlowResultInputs& inputs, ValueFlowObjectID list, ValueSetID listsToldApart)
{
    return !markedMutationsOf(inputs, list).containsAny({ ObjectMutation::ChangeIntegrity, ObjectMutation::BecomePrototype }) && !inputs.isLost(list) && !inputs.sets().contains(listsToldApart, list);
}

static bool mayFallBackToObjectPrototype(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, FunctionID id, const Value& read)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    std::optional<unsigned> newTargetIndex = function.newTargetParameterIndex();
    if (!newTargetIndex || sameObjectThroughCasts(function, read.newTargetOperand()) != function.at(entryBlock).parameters[*newTargetIndex])
        return true;
    ValueSetID newTargets = inputs.solver().possibleValuesOf(inputs.bodies().nodes().entryParameter(id, *newTargetIndex));
    return ValueSets::isComplement(newTargets) || inputs.sets().anyMember(newTargets, [&](ValueFlowObjectID newTarget) {
        if (!inputs.universe().isObject(newTarget))
            return false;
        FunctionID constructor = inputs.universe().closureFunction(newTarget);
        if (!constructor)
            return true;
        LayoutID constructed = program.at(constructor).constructedClass;
        return (!constructed || !isClassLayout(program.at(constructed))) && links.covers(links.functionsWithNonObjectPrototype(), newTarget);
    });
}

static ObjectCreation describeCreation(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, ValueSetMaker& maker, ValueFlowObjectID object, LayoutID layout)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    ValueFlowObjectID objectPrototype = universe.objectOf(Builtins::ObjectPrototype);

    ValueSetID prototypes = links.linkedPrototypes(maker, object);
    if (prototypes == ValueSets::nothing)
        return { };
    LayoutKind kind = program.at(layout).kind;
    if (kind != LayoutKind::Instance && kind != LayoutKind::Record && kind != LayoutKind::Container)
        return { .mayHaveOtherPrototype = true };

    ObjectCreation creation;
    ValueSetID allowed = maker.of(kind == LayoutKind::Instance ? universe.prototypeOf(declaredLayoutOf(program, layout)) : objectPrototype);
    const ValueFlowSite* site = universe.siteOf(object);
    bool isMadeByOperation = site && !site->ordinal && (site->origin == ValueFlowSiteOrigin::Operation || site->origin == ValueFlowSiteOrigin::BuiltinResult);
    const Function* function = isMadeByOperation ? &program.at(universe.functionOf(object)) : nullptr;
    const Value* operation = isMadeByOperation ? &function->at(site->value) : nullptr;
    if (universe.kindOf(object) == ValueFlowObjectKind::Instance) {
        FunctionID constructor = program.at(layout).constructor;
        if (constructor && !links.covers(links.functionsWithNonObjectPrototype(), universe.closureOf(constructor)))
            allowed = maker.unite(allowed, universe.undefinedValue());
    } else if (operation && site->origin == ValueFlowSiteOrigin::Operation && operation->opcode == Opcode::NewObject && operation->prototypeOperand()) {
        if (mayAllocateOtherLayout(program, *operation)) {
            creation.layoutSelectors = prototypes;
            allowed = maker.unite(allowed, links.classPrototypes());
        }
        const Value& prototype = function->at(sameObjectThroughCasts(*function, operation->prototypeOperand()));
        if (prototype.opcode == Opcode::GetConstructPrototype && !mayFallBackToObjectPrototype(inputs, links, universe.functionOf(object), prototype))
            allowed = maker.unite(allowed, maker.of(objectPrototype));
    } else if (operation && site->origin == ValueFlowSiteOrigin::BuiltinResult && program.prototypeMaySelectOtherClass(layout) && OperandPosition::parent(operation->opcode)) {
        creation.layoutSelectors = prototypes;
        allowed = maker.unite(allowed, links.classPrototypes());
    }
    creation.mayHaveOtherPrototype = !inputs.sets().isSubset(prototypes, allowed);
    return creation;
}

// A mark is the report of somebody who was seen. Whoever has not accounted for the object may do anything to it.
static OptionSet<ObjectMutation> possibleMutationsOf(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, ValueFlowObjectID object, ClassesAbove classesAbove = ClassesAbove::MayListNoOwnNames)
{
    if (!actorsOpenOn(inputs, object, classesAbove).isEmpty())
        return allObjectMutations;
    OptionSet<ObjectMutation> possible = markedMutationsOf(inputs, object) | mutationsWithoutMark;
    return links.mayBeLinkedAbove(object) ? possible | ObjectMutation::BecomePrototype : possible;
}

static bool isAllocatedObject(const ValueFlowResultInputs& inputs, const BitVector& layoutsWithInstances, ValueFlowObjectID object)
{
    const ValueFlowUniverse& universe = inputs.universe();
    ValueFlowObjectKind kind = universe.kindOf(object);
    if (kind != ValueFlowObjectKind::Site && kind != ValueFlowObjectKind::Instance)
        return false;
    LayoutID layout = universe.layoutOf(object);
    if (!layout || isSiteOfTypedResultThatNothingAllocates(inputs, object))
        return false;
    if (kind == ValueFlowObjectKind::Instance)
        return inputs.program.at(layout).constructor && layoutsWithInstances.quickGet(layout.index());
    return universe.siteOf(object)->origin == ValueFlowSiteOrigin::TypedResult || inputs.isReached(universe.functionOf(object));
}

static Vector<Allocation> findAllocations(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, Opcode other, ValueFlowNotedOperation otherAsNoted)
{
    return collectInParallel<Allocation>(inputs.loops(), inputs.program.functions.size(), [&](unsigned index, Vector<Allocation>& found) {
        if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::NewObject, otherAsNoted })) || (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, tables, FunctionID(index))))
            return;
        const Function& function = inputs.program.functions[index];
        for (uint32_t value = 0; value < function.values.size(); ++value) {
            if (function.values[value].opcode == Opcode::NewObject || function.values[value].opcode == other)
                found.append({ FunctionID(index), ValueID(value) });
        }
    });
}

static ObjectsOfLayouts collectObjectsOfLayouts(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, const PrototypeLinks& links, const BitVector& layoutsWithInstances)
{
    struct Found {
        LayoutID layout;
        uint32_t placeholder;
        OptionSet<ObjectMutation> possibleMutations;
        bool escapes;
    };
    constexpr ValueFlowObjectID placeholders[] { ValueFlowUniverse::unknownObject(), ValueFlowUniverse::untrackedObject() };

    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    ObjectsOfLayouts objects { BitVector(program.layouts.size()), BitVector(program.layouts.size()), Vector<OptionSet<ObjectMutation>>(program.layouts.size()) };

    for (Allocation allocation : findAllocations(inputs, tables, Opcode::CreateIteratorResult, ValueFlowNotedOperation::CreateIteratorResult)) {
        const Value& value = program.at(allocation.function).at(allocation.value);
        LayoutID layout = value.opcode == Opcode::NewObject ? value.layout() : program.layoutOf(BuiltinLayouts::IteratorResult);
        bool isUnsolved = isUnsolvedCode(inputs, tables, allocation.function);
        objects.add(layout, isUnsolved ? allObjectMutations : OptionSet<ObjectMutation> { }, isUnsolved);
    }
    BitVector layoutsWithLayoutsBelow(program.layouts.size());
    for (const Layout& layout : program.layouts) {
        if (layout.base)
            layoutsWithLayoutsBelow.quickSet(layout.base.index());
    }
    ValueFlowHeapView heapOfCallGroups(inputs);
    // A function that always answers with an object of its own leaves no instance in any set, and is constructed all the same.
    std::optional<ValueSetID> constructed;
    auto isConstructedByProgram = [&](FunctionID function) {
        if (!constructed) {
            UnionOfSets callees;
            for (ValueFlowNodeID first : heapOfCallGroups.index().firstNodesOfCallGroups()) {
                const ValueFlowNodeKey& key = *inputs.solver().keyOfMadeNode(first);
                if (key.callKind() == ValueFlowCallKind::Construct)
                    callees.add(key.set);
            }
            constructed = callees.take(heapOfCallGroups.maker());
        }
        return sets.contains(*constructed, universe.closureOf(function));
    };
    // `function NullObject() {}` that only code out of run-time text constructs: the compiler knows no name of its objects, so a layout would hold none of them in a slot. The run time makes ordinary objects.
    auto hasNothingToKeepInSlots = [&](uint32_t index) {
        const Layout& layout = program.layouts[index];
        return layout.has(LayoutFlags::IsConstructorFunction) && layout.fields.isEmpty() && !layout.base && layout.supertypes.isEmpty() && !layoutsWithLayoutsBelow.quickGet(index)
            && !layoutsWithInstances.quickGet(index) && !program.at(layout.constructor).has(FunctionFlags::HasThis) && !isConstructedByProgram(layout.constructor);
    };
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        FunctionID constructor = program.layouts[index].constructor;
        bool isUnsolved = constructor && isUnsolvedCode(inputs, tables, constructor);
        if (!constructor || (!isUnsolved && !inputs.isReached(constructor)))
            continue;
        if (!isUnsolved && hasNothingToKeepInSlots(index))
            continue;
        if (isUnsolved)
            objects.add(LayoutID(index), allObjectMutations, true);
        else if (program.layouts[index].has(LayoutFlags::IsConstructorFunction) && (!layoutsWithInstances.quickGet(index) || inputs.hasEscaped(universe.closureOf(constructor))))
            objects.add(LayoutID(index), escapedObjectMutations(inputs) | mutationsWithoutMark, true);
        else
            objects.isAllocated.quickSet(index);
    }

    ValueSetMakers makers(inputs);
    ValueFlowHeapView heap(inputs);
    Vector<Found> allFound = collectWithMakers<Found>(inputs, makers, universe.numberOfSiteAndLayoutObjects(), [&](unsigned position, ValueSetMaker& maker, Vector<Found>& found) {
        ValueFlowObjectID object = universe.siteOrLayoutObjectAt(position);
        if (!isAllocatedObject(inputs, layoutsWithInstances, object))
            return;
        LayoutID layout = universe.layoutOf(object);
        OptionSet<ObjectMutation> possible = possibleMutationsOf(inputs, links, object, links.classesAbove(heap, maker, object));
        bool escapes = possible == allObjectMutations || inputs.hasEscaped(object) || inputs.isLost(object);
        ObjectCreation creation = describeCreation(inputs, links, maker, object, layout);
        found.append({ layout, 0, creation.mayHaveOtherPrototype ? possible | ObjectMutation::ChangePrototype : possible, escapes });
        if (creation.layoutSelectors == ValueSets::nothing)
            return;
        LayoutID ownClass = declaredLayoutOf(program, layout);
        sets.forEachMember(maker.intersect(creation.layoutSelectors, links.classPrototypes()), [&](ValueFlowObjectID prototype) {
            if (universe.classLayoutOf(prototype) != ownClass)
                found.append({ universe.classLayoutOf(prototype), 0, possible, escapes });
        });
        for (uint32_t placeholder = 0; placeholder < std::size(placeholders); ++placeholder) {
            if (sets.contains(creation.layoutSelectors, placeholders[placeholder]))
                found.append({ { }, placeholder, possible, escapes });
        }
    });
    OptionSet<ObjectMutation> possibleUnderPlaceholder[std::size(placeholders)];
    for (const Found& found : allFound) {
        if (found.layout)
            objects.add(found.layout, found.possibleMutations, found.escapes);
        else
            possibleUnderPlaceholder[found.placeholder].add(found.possibleMutations);
    }
    for (uint32_t placeholder = 0; placeholder < std::size(placeholders); ++placeholder) {
        ValueSetID covered = inputs.solver().possibleValuesOf(placeholder ? ValueFlowProgramNodes::lost() : ValueFlowProgramNodes::escaped());
        for (uint32_t index = 0; index < program.layouts.size() && !possibleUnderPlaceholder[placeholder].isEmpty(); ++index) {
            const Layout& layout = program.layouts[index];
            if (isClassLayout(layout) && (sets.contains(covered, universe.prototypeOf(LayoutID(index))) || sets.contains(covered, universe.closureOf(layout.constructor))))
                objects.add(LayoutID(index), possibleUnderPlaceholder[placeholder], true);
        }
    }
    return objects;
}

static Vector<AllocationClassID> createAllocationClasses(const ValueFlowResultInputs& inputs, const ObjectsOfLayouts& objects, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    Vector<AllocationClassID> classOfLayout(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutFlow& flow = tables.application->layouts[index].flow;
        LayoutKind kind = program.layouts[index].kind;
        if (!objects.isAllocated.quickGet(index)) {
            bool isAllocatedByOperations = kind != LayoutKind::Closure && kind != LayoutKind::CoroutineFrame && kind != LayoutKind::ModuleNamespace;
            if (isAllocatedByOperations && inputs.build.isEnabled(FlowResultKind::ProvenLayoutSets, index))
                flow.allocation = AllocationState::NeverAllocated;
            continue;
        }
        if (!hasObjectsThatCodeAlters(kind))
            continue;
        if (!inputs.build.isEnabled(FlowResultKind::HolderMutations, index)) {
            flow.cause = UnprovenCause::ResultDisabled;
            continue;
        }

        AllocationClass allocationClass;
        allocationClass.layout = LayoutID(index);
        allocationClass.impossibleMutations = allObjectMutations - objects.possibleMutations[index];
        const MutationCause* cause = std::ranges::find_if(mutationCauses, [&](const MutationCause& entry) {
            return objects.possibleMutations[index].contains(entry.mutation);
        });
        if (cause != std::end(mutationCauses))
            allocationClass.cause = cause->cause;
        classOfLayout[index] = AllocationClassID(tables.sites.allocationClasses.size());
        flow.allocationClasses = { classOfLayout[index] };
        flow.impossibleMutations = allocationClass.impossibleMutations;
        flow.neverEscapes = !objects.mayEscape.quickGet(index);
        flow.cause = allocationClass.cause;
        tables.sites.allocationClasses.append(WTF::move(allocationClass));
    }
    return classOfLayout;
}

static void recordWitnesses(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, const BitVector& layoutsWithInstances, FlowResultTables& tables)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowSolver& solver = inputs.solver();
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    auto witnessAt = [&](FunctionID function, ValueID value) {
        return Witness { function.index(), value.index(), inputs.program.at(function).locationOf(value) };
    };
    auto flowOf = [&](ValueFlowObjectID object) -> LayoutFlow& {
        return tables.application->layouts[universe.layoutOf(object).index()].flow;
    };

    Vector<ValueFlowObjectID> objectsAskedAbout[std::size(mutationCauses)];
    for (uint32_t position = 0; position < universe.numberOfSiteAndLayoutObjects(); ++position) {
        ValueFlowObjectID object = universe.siteOrLayoutObjectAt(position);
        if (!isAllocatedObject(inputs, layoutsWithInstances, object) || flowOf(object).allocationClasses.isEmpty() || hasFunction(flowOf(object).witness))
            continue;
        if (const MutationCause* cause = std::ranges::find(mutationCauses, flowOf(object).cause, &MutationCause::cause); cause != std::end(mutationCauses))
            objectsAskedAbout[cause - mutationCauses].append(object);
    }

    for (size_t position = 0; position < std::size(mutationCauses); ++position) {
        HashMap<uint32_t, Witness, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> witnessOfMark;
        ValueFlowNodeID markNode = links.nodeOfMark(mutationCauses[position].mutation);
        if (markNode && !objectsAskedAbout[position].isEmpty()) {
            ValueSetID remaining = maker.intersect(maker.unite(maker.of(objectsAskedAbout[position].span()), universe.placeholdersAlone()), solver.possibleValuesOf(markNode));
            Vector<std::pair<Witness, ValueFlowNodeID>> marks;
            solver.graph().forEachPredecessor(markNode, [&](ValueFlowNodeID predecessor) {
                ValueFlowStatementID id = solver.statementThatLogged({ predecessor, markNode });
                if (!id)
                    return;
                const ValueFlowStatement& statement = solver.statementAt(id);
                if (statement.function && statement.function.index() < inputs.program.functions.size() && statement.site)
                    marks.append({ witnessAt(statement.function, statement.site), predecessor });
            });
            AOT::sort(marks, [](const auto& first, const auto& second) {
                return std::tie(first.first.function, first.first.value) < std::tie(second.first.function, second.first.value);
            });
            for (const auto& [witness, predecessor] : marks) {
                ValueSetID brought = maker.intersect(solver.possibleValuesOf(predecessor), remaining);
                inputs.sets().forEachMember(brought, [&, witness = witness](ValueFlowObjectID object) {
                    witnessOfMark.add(object.index(), witness);
                });
                remaining = maker.intersect(remaining, ValueSets::complement(brought));
            }
        }
        for (ValueFlowObjectID object : objectsAskedAbout[position]) {
            LayoutFlow& flow = flowOf(object);
            if (hasFunction(flow.witness))
                continue;
            for (ValueFlowObjectID standIn : links.standInsOf(object)) {
                if (auto found = standIn ? witnessOfMark.find(standIn.index()) : witnessOfMark.end(); found != witnessOfMark.end() && !hasFunction(flow.witness))
                    flow.witness = found->value;
            }
            const ValueFlowSite* site = universe.siteOf(object);
            if (!hasFunction(flow.witness) && mutationCauses[position].mutation == ObjectMutation::ChangePrototype && site && describeCreation(inputs, links, maker, object, site->layout).mayHaveOtherPrototype)
                flow.witness = witnessAt(universe.functionOf(object), site->value);
            for (AllocationClassID allocationClass : flow.allocationClasses) {
                if (hasFunction(flow.witness))
                    tables.sites.allocationClasses[allocationClass.index()].witness = flow.witness;
            }
        }
    }
}

// One text that statements give to the one function object of a function as it is written names the function itself. So does the key under which a function stands where its object is made: who reads the key's type decides that.
static BitVector findWrittenFunctionsNamedApartFromTheirFunctionObjects(const ValueFlowResultInputs& inputs, const BitVector& writtenFunctionsWithOneFunctionObject)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    BitVector functions(program.functions.size());
    ValueSetID namedByStatements = inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ClosuresThatStatementsName));
    uint32_t nameKey = program.keyOf(BuiltinNames::name).index();
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> textOfFunction;
    for (const ValueFlowHeapIndex::Stretch& stretch : inputs.heapIndex->stretchesOfNodesWrittenUnder([&](ValueFlowName name) { return name.kind == ValueFlowNameKind::Property && name.payload == nameKey; })) {
        for (const auto& node : stretch.nodes) {
            if (ValueSets::isComplement(node.objects))
                continue;
            ValueFlowObjectID text = inputs.sets().onlyMember(node.value);
            bool isOneText = text && universe.kindOf(text) == ValueFlowObjectKind::Key && program.at(universe.keyOf(text)).kind == PropertyKeyKind::Text;
            inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID object) {
                if (universe.kindOf(object) != ValueFlowObjectKind::Closure)
                    return;
                FunctionID function = universe.closureFunction(object);
                bool isNamedByStatement = inputs.sets().contains(namedByStatements, object);
                if (!program.at(function).constructedClass && !isNamedByStatement)
                    return;
                uint32_t written = writtenFunctionOf(program, function).index();
                // A statement names the function object that it is handed, and leaves the others as they are.
                bool leavesOthersUnnamed = isNamedByStatement && !writtenFunctionsWithOneFunctionObject.get(written);
                // The tables of names leave async functions out.
                if (!isOneText || leavesOthersUnnamed || program.at(function).has(FunctionFlags::IsAsync) || textOfFunction.add(written, text.index()).iterator->value != text.index())
                    functions.quickSet(written);
            });
        }
    }
    return functions;
}

// A stack frame shows the own property `name` or `displayName` of the function object. What code that nobody has seen may do to a closure is written down as stores under unknown keys.
struct FunctionsWhoseClosuresMayBeRenamed {
    BitVector all;
    // Of those: no statement is seen to rename a closure of the function. A displayName that is written through an object nobody follows may.
    BitVector onlyWhereNobodySees;
    // In neither: no statement is seen to rename a closure of the function, and its closures go where nobody follows them.
    BitVector outOfSight;
    // Of those: a statement of the program defines `name` on an object nobody follows.
    BitVector namedOutOfSight;
};

// No key stands for a key that nobody knows.
static bool isWrittenUnderAnyKeyThat(std::span<const Write> writes, NOESCAPE const Invocable<bool(PropertyKeyID)> auto& matches)
{
    return std::ranges::any_of(writes, [&](const Write& write) {
        return write.through != ValueSets::nothing && write.under != prototypeLinkName && matches(ClaimsAboutClassesAndClosures::keyOf(write));
    });
}

static FunctionsWhoseClosuresMayBeRenamed findFunctionsWhoseClosuresMayBeRenamed(const ValueFlowResultInputs& inputs, const LedgerOfClassesAndClosures& ledger, std::span<const AllocationClass> classesOfClosures, const BitVector& writtenFunctionsWithOneFunctionObject)
{
    const Program& program = inputs.program;
    BitVector namedApartFromFunctionObjects = findWrittenFunctionsNamedApartFromTheirFunctionObjects(inputs, writtenFunctionsWithOneFunctionObject);
    Vector<PropertyKeyID, 2> keys { program.keyOf(BuiltinNames::name) };
    if (StringID text = program.strings.find(byteCast<uint8_t>("displayName"_span))) {
        if (auto key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text }))
            keys.append(*key);
    }

    // A displayName that is written through an object nobody follows is filed under that object alone, which stands for every closure that has gone the same way. A copy into such an object writes the names that its sources hold.
    ValueSetID givenDisplayName = inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ObjectsNobodyFollowsThatStatementsGiveDisplayName));
    auto isDisplayNameWrittenThrough = [&](ValueFlowObjectID placeholder, ValueFlowHostNode sourcesOfCopies) {
        if (inputs.sets().contains(givenDisplayName, placeholder))
            return true;
        ValueSetID sources = inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(sourcesOfCopies));
        return keys.size() > 1 && std::ranges::any_of(std::array { ValueFlowNameKind::Property, ValueFlowNameKind::Getter, ValueFlowNameKind::Setter }, [&](ValueFlowNameKind kind) {
            const ValueFlowHeapIndex::NodesWrittenUnderName* written = inputs.heapIndex->findNodesWrittenUnder({ kind, keys[1].index() });
            return written && std::ranges::any_of(written->nodes, [&](const ValueFlowHeapIndex::WrittenNode& node) {
                return inputs.sets().meet(node.objects, sources);
            });
        });
    };
    bool escapedClosuresMayBeRenamed = isDisplayNameWrittenThrough(ValueFlowUniverse::unknownObject(), ValueFlowHostNode::SourcesOfCopiesIntoUnknownObject);
    bool lostClosuresMayBeRenamed = isDisplayNameWrittenThrough(ValueFlowUniverse::untrackedObject(), ValueFlowHostNode::SourcesOfCopiesIntoUntrackedObject);

    BitVector functions(program.functions.size());
    BitVector onlyWhereNobodySees(program.functions.size());
    BitVector outOfSight(program.functions.size());
    BitVector namedOutOfSight(program.functions.size());
    ValueSetID namedByDefinitions = inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ObjectsNobodyFollowsThatDefinitionsName));
    bool escapedClosuresMayBeNamed = inputs.sets().contains(namedByDefinitions, ValueFlowUniverse::unknownObject());
    bool lostClosuresMayBeNamed = inputs.sets().contains(namedByDefinitions, ValueFlowUniverse::untrackedObject());
    ValueSetID handedToUnseenCode = inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::OperandsHandedToUnseenCode));
    inputs.sets().forEachMember(inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ClosuresThatStatementsMayRename)), [&](ValueFlowObjectID object) {
        if (FunctionID function = inputs.universe().closureFunction(object); function && function.index() < program.functions.size())
            functions.quickSet(function.index());
    });
    // A function that nothing enters has no class of closures.
    for (uint32_t function = 0; function < program.functions.size(); ++function) {
        if (!inputs.isReached(FunctionID(function)) && isWrittenUnderAnyKeyThat(ledger.writesFiledOn(inputs.universe().closureOf(FunctionID(function))), [&](PropertyKeyID key) { return key && keys.contains(key); }))
            functions.quickSet(function);
    }
    for (const AllocationClass& closures : classesOfClosures) {
        bool isSeenByKnownCodeAlone = closures.impossibleMutations.containsAll({ ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript });
        auto mayNameFrames = [&](PropertyKeyID key) {
            return key ? keys.contains(key) : isSeenByKnownCodeAlone;
        };
        uint32_t index = closures.closureFunction.index();
        LayoutID constructedClass = program.functions[index].constructedClass;
        auto mayNameFramesOfThisFunction = [&](PropertyKeyID key) {
            return key == keys[0] ? namedApartFromFunctionObjects.quickGet(writtenFunctionOf(program, closures.closureFunction).index()) : mayNameFrames(key);
        };
        ValueFlowObjectID closure = inputs.universe().closureOf(closures.closureFunction);
        std::span<const Write> writtenOnThisFunction = ledger.writesFiledOn(closure);
        if (isWrittenUnderAnyKeyThat(writtenOnThisFunction, mayNameFramesOfThisFunction)
            || isWrittenUnderAnyKeyThat(ledger.writesThroughEveryObject(), mayNameFrames)
            || (constructedClass && std::ranges::any_of(program.at(constructedClass).staticFields, mayNameFrames, &StaticField::key)))
            functions.quickSet(index);
        else if ((escapedClosuresMayBeRenamed && inputs.hasEscaped(closure)) || (lostClosuresMayBeRenamed && inputs.isLost(closure)))
            onlyWhereNobodySees.quickSet(index);
        else if (inputs.hasEscaped(closure) || inputs.isLost(closure)) {
            outOfSight.quickSet(index);
            // Unseen code may define any property of what it is handed.
            if (inputs.sets().contains(handedToUnseenCode, closure) || (escapedClosuresMayBeNamed && inputs.hasEscaped(closure)) || (lostClosuresMayBeNamed && inputs.isLost(closure)))
                namedOutOfSight.quickSet(index);
        }
    }
    // Without a class of its closures nothing says what a function object of the function meets.
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (!inputs.build.isEnabled(FlowResultKind::HolderMutations, index))
            functions.quickSet(index);
    }
    // The run time keeps a name for the function as it is written, so what holds of one form holds of all.
    for (BitVector* forms : { &functions, &onlyWhereNobodySees, &outOfSight, &namedOutOfSight }) {
        BitVector written(program.functions.size());
        for (size_t index : *forms)
            written.quickSet(writtenFunctionOf(program, FunctionID(static_cast<uint32_t>(index))).index());
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (written.quickGet(writtenFunctionOf(program, FunctionID(index)).index()))
                forms->quickSet(index);
        }
    }
    onlyWhereNobodySees.exclude(functions);
    functions.merge(onlyWhereNobodySees);
    outOfSight.exclude(functions);
    return { WTF::move(functions), WTF::move(onlyWhereNobodySees), WTF::move(outOfSight), WTF::move(namedOutOfSight) };
}

static BitVector findFunctionsWhoseClosuresAreGivenDisplayName(const ValueFlowResultInputs& inputs)
{
    BitVector functions(inputs.program.functions.size());
    inputs.sets().forEachMember(inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ClosuresThatStatementsGiveDisplayName)), [&](ValueFlowObjectID object) {
        if (FunctionID function = inputs.universe().closureFunction(object); function && function.index() < inputs.program.functions.size())
            functions.quickSet(function.index());
    });
    return functions;
}

static void noteClosuresBelowFunctions(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, std::span<AllocationClass> classes)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    auto listsItsNames = [&](const AllocationClass& closures) {
        return closures.addedKeysAreComplete && closures.impossibleMutations.contains(ObjectMutation::ReachUnknownJavaScript)
            && !program.at(closures.closureFunction).hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
    };
    auto findClassOf = [&](FunctionID function) -> const AllocationClass* {
        auto found = std::ranges::lower_bound(classes, function, { }, &AllocationClass::closureFunction);
        return found != classes.end() && found->closureFunction == function ? &*found : nullptr;
    };

    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueFlowObjectID functionPrototype = universe.objectOf(Builtins::FunctionPrototype);
    Vector<std::pair<uint32_t, Vector<PropertyKeyID>>> namesOnTheWay;
    for (uint32_t index = 0; index < classes.size(); ++index) {
        if (!listsItsNames(classes[index]))
            continue;
        if (classes[index].impossibleMutations.contains(ObjectMutation::ChangePrototype)) {
            classes[index].inheritsFromListedFunctions = true;
            continue;
        }
        Vector<PropertyKeyID> names = classes[index].addedKeys;
        Vector<FunctionID, 4> passed { classes[index].closureFunction };
        bool isKnown = true;
        for (size_t next = 0; next < passed.size() && isKnown; ++next) {
            ValueSetID parents = links.linkedPrototypes(borrowedMaker.get(), universe.closureOf(passed[next]));
            if (ValueSets::isComplement(parents)) {
                isKnown = false;
                break;
            }
            inputs.sets().forEachMember(parents, [&](ValueFlowObjectID parent) {
                if (parent == functionPrototype)
                    return;
                FunctionID function = universe.kindOf(parent) == ValueFlowObjectKind::Closure ? universe.closureFunction(parent) : FunctionID();
                const AllocationClass* above = function ? findClassOf(function) : nullptr;
                if (!above || !listsItsNames(*above)) {
                    isKnown = false;
                    return;
                }
                if (passed.contains(function))
                    return;
                passed.append(function);
                names.appendVector(above->addedKeys);
            });
        }
        if (isKnown)
            namesOnTheWay.append({ index, WTF::move(names) });
    }
    for (auto& [index, names] : namesOnTheWay) {
        AOT::sort(names);
        names.shrink(std::ranges::unique(names).begin() - names.begin());
        classes[index].addedKeys = WTF::move(names);
        classes[index].inheritsFromListedFunctions = true;
    }
}

static void createClassesOfClosures(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, const LedgerOfClassesAndClosures& links, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    Vector<AllocationClass> classesOfClosures = collectInParallel<AllocationClass>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<AllocationClass>& classes) {
        // A function that nobody calls may still have a closure that the program reads.
        if (calls.functionsNamedByUnsolvedCode.get(index) || !inputs.build.isEnabled(FlowResultKind::HolderMutations, index))
            return;
        ValueFlowObjectID closure = inputs.universe().closureOf(FunctionID(index));
        OptionSet<ObjectMutation> possible = possibleMutationsOf(inputs, links, closure);
        AllocationClass closures;
        closures.closureFunction = FunctionID(index);
        closures.impossibleMutations = allObjectMutations - possible;
        LayoutID constructedClass = program.functions[index].constructedClass;
        bool isDefinedByClass = constructedClass && !program.at(constructedClass).has(LayoutFlags::IsConstructorFunction);
        bool namesEveryStaticField = !constructedClass || std::ranges::all_of(program.at(constructedClass).staticFields, [](const StaticField& field) {
            return !!field.key;
        });
        std::optional<Vector<PropertyKeyID>> addedKeys;
        if (!isDefinedByClass && namesEveryStaticField && inputs.build.isEnabled(FlowResultKind::OwnNames, index))
            addedKeys = ClaimsAboutClassesAndClosures::keysAddedTo(links.writersOfEveryOwnProperty(closure), links.writersOfElements(closure), links.reachersOf(closure).value_or(Reachers { { }, everyActor }));
        closures.addedKeysAreComplete = !!addedKeys;
        if (addedKeys) {
            closures.addedKeys = WTF::move(*addedKeys);
            if (constructedClass) {
                for (const StaticField& field : program.at(constructedClass).staticFields)
                    closures.addedKeys.append(field.key);
                AOT::sort(closures.addedKeys);
                closures.addedKeys.shrink(std::ranges::unique(closures.addedKeys).begin() - closures.addedKeys.begin());
            }
        }
        classes.append(WTF::move(closures));
    });
    noteClosuresBelowFunctions(inputs, links, classesOfClosures.mutableSpan());
    FunctionsWhoseClosuresMayBeRenamed renamed = findFunctionsWhoseClosuresMayBeRenamed(inputs, links, classesOfClosures.span(), tables.functions.writtenFunctionsWithOneFunctionObject);
    tables.functions.functionsWhoseClosuresMayBeRenamed = WTF::move(renamed.all);
    tables.functions.functionsWhoseClosuresMayBeRenamedOnlyWhereNobodySees = WTF::move(renamed.onlyWhereNobodySees);
    tables.functions.functionsWhoseClosuresGoOutOfSightUnnamed = WTF::move(renamed.outOfSight);
    tables.functions.functionsWhoseClosuresMayBeNamedOutOfSight = WTF::move(renamed.namedOutOfSight);
    tables.functions.functionsWhoseClosuresAreGivenDisplayName = findFunctionsWhoseClosuresAreGivenDisplayName(inputs);
    tables.sites.allocationClasses.appendVector(WTF::move(classesOfClosures));
}

void computeObjectResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    const LedgerOfClassesAndClosures* links = inputs.classesAndClosures;
    BitVector layoutsWithInstances = findLayoutsWithInstances(inputs);
    times.didFinish("objects: layouts with instances"_s);
    ObjectsOfLayouts objects = collectObjectsOfLayouts(inputs, tables, *links, layoutsWithInstances);
    times.didFinish("objects: objects of layouts"_s);
    Vector<AllocationClassID> classOfLayout = createAllocationClasses(inputs, objects, tables);
    if (inputs.solver().input().remembersWhoLoggedEachEdge)
        recordWitnesses(inputs, *links, layoutsWithInstances, tables);
    times.didFinish("objects: allocation classes, witnesses"_s);

    for (Allocation allocation : findAllocations(inputs, tables, Opcode::NewClosure, ValueFlowNotedOperation::NewClosure)) {
        const Function& function = inputs.program.at(allocation.function);
        const Value& value = function.at(allocation.value);
        LayoutID layout = value.opcode == Opcode::NewObject ? value.layout() : LayoutID();
        AllocationClassID allocationClass = layout ? classOfLayout[layout.index()] : AllocationClassID();
        AllocationSiteID site(tables.sites.allocationSites.size());
        tables.sites.allocationSites.append(AllocationSite { allocation.function, layout, { }, allocationClass, function.locationOf(allocation.value) });
        tables.application->bodies[allocation.function.index()].allocations.append(FlowAllocationStamp { .value = allocation.value, .site = site, .allocationClass = allocationClass });
    }
    times.didFinish("objects: allocations stamped"_s);
    createClassesOfClosures(inputs, calls, *links, tables);
    times.didFinish("objects: classes of closures"_s);
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size());
    inputs.build.statistics().count(StatisticID::FlowAllocationClasses, tables.sites.allocationClasses.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
