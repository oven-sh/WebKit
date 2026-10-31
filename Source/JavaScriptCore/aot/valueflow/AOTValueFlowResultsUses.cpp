/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsFieldsInternal.h"

#if ENABLE(AOT)

#include "AOTThreshold.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class LookOfPropertyAction : uint8_t {
    None,
    TestsKey,
    ListsKeysOfValue,
};

struct ValuesWithOneUse {
    BitVector onlyCalled;
    BitVector onlyAccessed;
};

} // anonymous namespace

static LookOfPropertyAction lookOfAction(BuiltinPropertyActionKind kind)
{
    switch (kind) {
    case BuiltinPropertyActionKind::TestsByKey:
    case BuiltinPropertyActionKind::ReadsGetter:
    case BuiltinPropertyActionKind::ReadsSetter:
        return LookOfPropertyAction::TestsKey;
    case BuiltinPropertyActionKind::AssignsFrom:
    case BuiltinPropertyActionKind::DefinesFrom:
    case BuiltinPropertyActionKind::TakesDescriptorMap:
        return LookOfPropertyAction::ListsKeysOfValue;
    case BuiltinPropertyActionKind::ReadsByKey:
    case BuiltinPropertyActionKind::StoresByKey:
    case BuiltinPropertyActionKind::DeletesByKey:
    case BuiltinPropertyActionKind::DefinesName:
    case BuiltinPropertyActionKind::TakesDescriptor:
    case BuiltinPropertyActionKind::ChangesIntegrity:
    case BuiltinPropertyActionKind::ChangesPrototype:
    case BuiltinPropertyActionKind::BecomesPrototype:
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
    case BuiltinPropertyActionKind::DescribesOwnProperties:
    case BuiltinPropertyActionKind::DescribesResult:
        return LookOfPropertyAction::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool looksAtObjects(const BuiltinOverload& overload)
{
    auto listsKeys = [](const BuiltinParameter& parameter) {
        return parameter.uses != OperandUses::all() && parameter.protocols.containsAny(protocolsListingKeys);
    };
    if ((overload.receiver && listsKeys(*overload.receiver)) || std::ranges::any_of(overload.parameters, listsKeys))
        return true;
    return std::ranges::any_of(overload.flow.propertyActions, [](const BuiltinPropertyAction& action) {
        return lookOfAction(action.kind) != LookOfPropertyAction::None;
    });
}

static void uniteTakenAtOperand(const ValueFlowBuiltinTables& tables, ValueSetMaker& maker, const BuiltinParameter& parameter, auto& takenAtOperands)
{
    ValueSetID taken = parameter.accepted ? tables.membersSurelyOf(parameter.accepted) : ValueSets::anything;
    if (parameter.converted)
        taken = maker.unite(taken, tables.membersSurelyOf(parameter.converted));
    size_t position = takenAtOperands.findIf([&](const auto& entry) {
        return entry.operand == parameter.operand;
    });
    if (position == notFound)
        takenAtOperands.append({ parameter.operand, taken });
    else
        takenAtOperands[position].taken = maker.unite(takenAtOperands[position].taken, taken);
}

static ShownKeys keysShownBy(const BuiltinTable& builtins, BuiltinID function, const BuiltinParameter& parameter)
{
    if (!parameter.protocols.contains(Protocol::OwnKeys))
        return ShownKeys::None;
    function = builtins.canonicalID(function);
    bool skipsWhatIsNotEnumerable = function == Builtins::ObjectKeys || function == Builtins::ObjectValues || function == Builtins::ObjectEntries || function == Builtins::ObjectAssign || function == Builtins::JSONStringify;
    return skipsWhatIsNotEnumerable ? ShownKeys::Enumerable : ShownKeys::All;
}

static void forEachLookAtOperands(const ValueFlowResultInputs& inputs, BuiltinID builtin, BuiltinCallMode mode, NOESCAPE const Invocable<void(std::optional<BuiltinOperand>, ShownKeys)> auto& listsKeys, NOESCAPE const Invocable<void(BuiltinOperand)> auto& looksUnderAnyKey, NOESCAPE const Invocable<void(BuiltinOperand object, PropertyKeyID, bool looksAtOwnPropertyAlone)> auto& testsName, NOESCAPE const Invocable<void(BuiltinOperand object, BuiltinOperand key, bool looksAtOwnPropertyAlone)> auto& testsKeyInOperand)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
        if (overload.mode != mode)
            continue;
        auto lookAtParameter = [&](const BuiltinParameter& parameter) {
            if (parameter.uses == OperandUses::all() || !parameter.protocols.containsAny(protocolsListingKeys))
                return;
            if (parameter.roles.contains(BuiltinOperandRole::KeysAreNotListed)) {
                bool hasAction = std::ranges::any_of(overload.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                    return action.object == parameter.operand;
                });
                if (!hasAction)
                    looksUnderAnyKey(parameter.operand);
                return;
            }
            bool goesOnToContents = parameter.uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents });
            listsKeys(goesOnToContents && !inputs.universe().builtinTables().behaviorOf(parameter) ? std::nullopt : std::optional(parameter.operand), keysShownBy(builtins, builtin, parameter));
        };
        auto lookAtTest = [&](const BuiltinPropertyAction& action) {
            const BuiltinParameter* parameter = builtins.parameterFor(overload, action.object);
            bool looksAtOwnPropertyAlone = parameter && parameter->roles.contains(BuiltinOperandRole::KeysAreNotListed) && !parameter->protocols.contains(Protocol::PrototypeChain);
            switch (action.key.kind) {
            case BuiltinKeyKind::None:
                return;
            case BuiltinKeyKind::Names:
                for (BuiltinName name : action.key.names)
                    testsName(action.object, inputs.program.keyOf(name), looksAtOwnPropertyAlone);
                return;
            case BuiltinKeyKind::Operand:
                testsKeyInOperand(action.object, *action.key.operand, looksAtOwnPropertyAlone);
                return;
            case BuiltinKeyKind::Any:
            case BuiltinKeyKind::Indexes:
            case BuiltinKeyKind::ElementsOf:
                looksUnderAnyKey(action.object);
                return;
            }
            RELEASE_ASSERT_NOT_REACHED();
        };
        if (overload.receiver)
            lookAtParameter(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            lookAtParameter(parameter);
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            switch (lookOfAction(action.kind)) {
            case LookOfPropertyAction::TestsKey:
                lookAtTest(action);
                break;
            case LookOfPropertyAction::ListsKeysOfValue:
                listsKeys(action.value.operand, ShownKeys::All);
                break;
            case LookOfPropertyAction::None:
                break;
            }
        }
    }
}

static BitVector findBuiltinsThatLookAtObjects(const BuiltinTable& builtins)
{
    BitVector builtinsThatLook(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.at(BuiltinID(index)).origin == BuiltinOrigin::Host || std::ranges::any_of(builtins.overloadsOf(BuiltinID(index)), looksAtObjects))
            builtinsThatLook.quickSet(index);
    }
    return builtinsThatLook;
}

UsesOfObjects::UsesOfObjects(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap)
    : m_inputs(inputs)
    , m_heap(heap)
    , m_notes(inputs.loops().width())
    , m_builtinsThatLookAtObjects(findBuiltinsThatLookAtObjects(inputs.program.builtins()))
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    ValueSetMakers makers(inputs);
    noteReads(makers);
    times.didFinish("uses of the heap: reads"_s);
    noteOperations(makers);
    noteOperationsWithoutStatement();
    const ValueFlowBuiltinTables& tables = inputs.universe().builtinTables();
    for (uint32_t index = 0; index < tables.numberOfBehaviors(); ++index) {
        const ValueFlowBehavior& behavior = tables.at(ValueFlowBehaviorID(index));
        bool readsEveryMember = (behavior.protocols.contains(Protocol::Named) && behavior.namedMembers.empty()) || (behavior.protocols.contains(Protocol::CallsNamed) && behavior.calledMembers.empty());
        if (behavior.protocols.containsAny(protocolsListingKeys) || readsEveryMember)
            m_notes[0].enumerated.add(inputs.solver().possibleValuesOf(inputs.bodies().nodes().hub(ValueFlowBehaviorID(index))));
    }
    m_notes[0].enumerated.add(inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::TargetsOfProxiesThatAreListed)));
    UnionOfSets exportedByCommonJSModules;
    for (const Module& module : inputs.program.modules) {
        if (module.kind == ModuleKind::CommonJs && module.exportsVariable && module.body && inputs.isReached(module.body))
            exportedByCommonJSModules.add(inputs.solver().possibleValuesOf(inputs.bodies().nodes().variable(module.exportsVariable)));
        if (module.kind != ModuleKind::RuntimeSource || module.exportRule == SyntheticExportRule::None || !module.body || !inputs.isReached(module.body))
            continue;
        if (!inputs.program.at(module.body).results.isEmpty())
            m_notes[0].enumerated.add(inputs.solver().possibleValuesOf(inputs.bodies().nodes().result(module.body, 0)));
    }
    m_exportedByCommonJSModules = exportedByCommonJSModules.takeExpanded(heap, makers.of(0));
    findHostFunctionsThatProgramHasStored(makers);
    times.didFinish("uses of the heap: operations, behaviors, modules"_s);
    forEachBuiltinApplicationInParallel(inputs, heap, makers, m_builtinsThatLookAtObjects, ValueFlowBuiltinApplicationOnWorkerFunctor([&](unsigned worker, const ValueFlowBuiltinApplication& application) {
        noteApplication(noterOf(makers, worker), application);
    }), ValueFlowBuiltinApplicationsOnWorkerFunctor([&](unsigned worker, const ValueFlowBuiltinApplication& application, ValueSetID builtinCallees) {
        noteApplicationOfAll(noterOf(makers, worker), application, builtinCallees);
    }));
    times.didFinish("uses of the heap: applications of built-ins"_s);
    goOnToPrototypes(makers);
    m_notes.clear();
    times.didFinish("uses of the heap: unions made, prototypes"_s);
}

bool UsesOfObjects::mayBeObservedUnder(ValueSetID objects, PropertyKeyID key) const
{
    const ValueSets& sets = m_inputs.sets();
    return sets.meet(objects, m_enumerated) || sets.meet(objects, m_observedUnderAnyKey) || sets.meet(objects, setAt(m_observedUnderKey, key.index()));
}

// A proxy whose handler lacks the trap hands a test on to its target, which answers from its own chain. A walk links a proxy to its target only where it passes the proxy from below.
ValueSetID UsesOfObjects::withTargetsOfProxies(ValueSetMaker& maker, ValueSetID objects) const
{
    ValueFlowName target { ValueFlowNameKind::Slot, BuiltinSlots::ProxyTarget.index() };
    for (ValueSetID frontier = objects; frontier != ValueSets::nothing;) {
        ValueSetID all = maker.unite(objects, m_heap.expand(maker, m_heap.writtenUnder(maker, frontier, target)));
        frontier = maker.intersect(all, ValueSets::complement(objects));
        objects = all;
    }
    return objects;
}

// The walk may meet a proxy at its start and at any step above it.
ValueSetID UsesOfObjects::chainThroughProxies(ValueSetMaker& maker, ValueSetID objects) const
{
    for (;;) {
        ValueSetID chain = m_heap.chainOf(maker, withTargetsOfProxies(maker, objects));
        if (chain == objects)
            return chain;
        objects = chain;
    }
}

void UsesOfObjects::goOnToPrototypes(ValueSetMakers& makers)
{
    const ParallelLoops& loops = m_inputs.loops();
    auto takeUnion = [&](UnionOfSets Notes::* parts) {
        UnionOfSets all;
        for (Notes& notes : m_notes)
            all.takeFrom(notes.*parts);
        return all;
    };
    auto takeParts = [&](PartsByNumber Notes::* parts) {
        Vector<PartOfNumber> all;
        for (Notes& notes : m_notes)
            all.appendVector((notes.*parts).take());
        return all;
    };
    auto takeExpanded = [&](ValueSetMakers& makersOfTask, Vector<PartOfNumber>&& parts) {
        PartsOfNumbers partsOfNumbers(loops, WTF::move(parts));
        Vector<ValueSetID> expanded(partsOfNumbers.size());
        forEachIndexInParallel(loops, partsOfNumbers.size(), [&](unsigned position, unsigned worker) {
            ValueSetMaker& maker = makersOfTask.of(worker);
            expanded[position] = m_heap.expand(maker, partsOfNumbers.unionAt(position, maker));
        });
        SetsByNumber sets;
        sets.reserveInitialCapacity(partsOfNumbers.size());
        for (unsigned position = 0; position < partsOfNumbers.size(); ++position)
            sets.add(partsOfNumbers.numberAt(position), expanded[position]);
        return sets;
    };
    runTasksInParallel(loops, {
        [&] {
            PartsOfNumbers startsOfTests(loops, takeParts(&Notes::startsOfTestsUnderKey));
            Vector<ValueSetID> starts(startsOfTests.size());
            forEachIndexInParallel(loops, startsOfTests.size(), [&](unsigned position, unsigned worker) {
                starts[position] = m_heap.expand(makers.of(worker), startsOfTests.unionAt(position, makers.of(worker)));
            });
            Vector<ValueSetID> distinctStarts = starts;
            AOT::sort(distinctStarts);
            removeRepeatedElements(distinctStarts);
            Vector<ValueSetID> chains(distinctStarts.size());
            forEachIndexInParallel(loops, distinctStarts.size(), [&](unsigned index, unsigned worker) {
                chains[index] = chainThroughProxies(makers.of(worker), distinctStarts[index]);
            });
            Vector<PartOfNumber> observedUnderKey = takeParts(&Notes::observedUnderKey);
            for (unsigned position = 0; position < startsOfTests.size(); ++position) {
                size_t index = std::ranges::lower_bound(distinctStarts, starts[position]) - distinctStarts.begin();
                if (chains[index] != ValueSets::nothing)
                    observedUnderKey.append({ startsOfTests.numberAt(position), chains[index] });
            }
            m_observedUnderKey = takeExpanded(makers, WTF::move(observedUnderKey));
        },
        [&] {
            BorrowedValueSetMaker maker(m_inputs.makerPool());
            m_enumerated = chainThroughProxies(maker.get(), takeUnion(&Notes::enumerated).takeExpanded(m_heap, maker.get()));
            m_withAllKeysShown = takeUnion(&Notes::withAllKeysShown).take(maker.get());
            m_withEnumerableKeysShown = maker.get().unite(takeUnion(&Notes::withEnumerableKeysShown).take(maker.get()), followPrototypeLinks(maker.get(), takeUnion(&Notes::walkedForEnumerableKeys).take(maker.get())));
        },
        [&] {
            BorrowedValueSetMaker maker(m_inputs.makerPool());
            m_observedUnderAnyKey = chainThroughProxies(maker.get(), takeUnion(&Notes::observedUnderAnyKey).takeExpanded(m_heap, maker.get()));
        },
        [&] {
            ValueSetMakers makersBeside(m_inputs);
            m_calledUnderKey = takeExpanded(makersBeside, takeParts(&Notes::calledUnderKey));
            m_accessedUnderKey = takeExpanded(makersBeside, takeParts(&Notes::accessedUnderKey));
        },
        [&] {
            PartsOfNumbers receiversOfReads(loops, takeParts(&Notes::receiversOfReads));
            m_receiversOfReads.reserveInitialCapacity(receiversOfReads.size());
            for (unsigned position = 0; position < receiversOfReads.size(); ++position) {
                Vector<ValueSetID> receivers;
                for (const PartOfNumber& part : receiversOfReads.partsAt(position)) {
                    if (!ValueSets::isComplement(part.set))
                        receivers.append(part.set);
                }
                m_receiversOfReads.add(receiversOfReads.numberAt(position), WTF::move(receivers));
            }
        },
    });
}

static ValuesWithOneUse findValuesWithOneUse(const Function& function)
{
    struct Cast {
        uint32_t result;
        uint32_t operand;
    };

    BitVector isCalled(function.values.size());
    BitVector isNotOnlyCalled(function.values.size());
    BitVector isAccessed(function.values.size());
    BitVector isNotOnlyAccessed(function.values.size());
    Vector<Cast, 8> casts;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        const Value& value = function.values[index];
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            OperandUses uses = operandUsesOf(value.opcode, operandIndex);
            if (isCast(value.opcode) && uses.contains(OperandUse::Returns)) {
                casts.append({ index, operand.index() });
                return;
            }
            bool calls = uses.contains(OperandUse::Calls) && !uses.containsAny({ OperandUse::Retains, OperandUse::Returns });
            (calls ? isCalled : isNotOnlyCalled).quickSet(operand.index());
            bool accesses = (value.opcode == Opcode::PropGet && operandIndex == OperandIndex::PropGet::object) || (value.opcode == Opcode::PropSet && operandIndex == OperandIndex::PropSet::object);
            (accesses ? isAccessed : isNotOnlyAccessed).quickSet(operand.index());
        });
    }
    for (const Block& block : function.blocks) {
        block.forEachChildOutsideValues([&](ValueID child) {
            isNotOnlyCalled.quickSet(child.index());
            isNotOnlyAccessed.quickSet(child.index());
        });
    }

    auto followCasts = [&](BitVector& isUsed, BitVector& isUsedOtherwise) {
        for (bool hasChanged = !casts.isEmpty(); hasChanged;) {
            hasChanged = false;
            for (const Cast& cast : casts)
                hasChanged |= isUsed.quickGet(cast.result) && !isUsed.quickSet(cast.operand);
        }
        for (bool hasChanged = !casts.isEmpty(); hasChanged;) {
            hasChanged = false;
            for (const Cast& cast : casts)
                hasChanged |= (isUsedOtherwise.quickGet(cast.result) || !isUsed.quickGet(cast.result)) && !isUsedOtherwise.quickSet(cast.operand);
        }
        isUsed.exclude(isUsedOtherwise);
    };
    followCasts(isCalled, isNotOnlyCalled);
    followCasts(isAccessed, isNotOnlyAccessed);
    return { WTF::move(isCalled), WTF::move(isAccessed) };
}

void UsesOfObjects::noteRead(Noter noter, const ValueFlowNodeKey& key, Reader reader, ValueSetID startOfLookup)
{
    switch (key.name().kind) {
    case ValueFlowNameKind::Property:
        if (reader != Reader::TakesValue) {
            (reader == Reader::OnlyCalls ? noter.notes.calledUnderKey : noter.notes.accessedUnderKey).add(key.payload, noter.maker.intersect(key.set, startOfLookup));
            noter.notes.observedUnderKey.add(key.payload, noter.maker.intersect(key.set, ValueSets::complement(startOfLookup)));
        } else
            noter.notes.observedUnderKey.add(key.payload, key.set);
        break;
    case ValueFlowNameKind::EveryProperty:
        noter.notes.observedUnderAnyKey.add(key.set);
        break;
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
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
    case ValueFlowNameKind::NamesOfMethods:
    case ValueFlowNameKind::ReceiversOfCalls:
    case ValueFlowNameKind::NamespaceExports:
        break;
    }
}

void UsesOfObjects::noteReads(ValueSetMakers& makers)
{
    struct Visit {
        ValueFlowStatementID statement;
        Reader reader;
    };

    struct NodesLookedAt {
        BitVector isLookedAt;
        Vector<uint32_t> numbers;
    };

    constexpr uint32_t madeStatementsPerIndex = 512;
    AOT_THRESHOLD(uint32_t, mostNodesNotToldApart, 64);
    const Program& program = m_inputs.program;
    const ParallelLoops& loops = m_inputs.loops();
    const ValueFlowSolver& solver = m_inputs.solver();
    const ValueFlowBodies& bodies = m_inputs.bodies();
    std::span<const ValueFlowStatement> statements = bodies.statements();
    std::span<const ValueFlowBodyRange> ranges = bodies.ranges();
    ValueSetID placeholders = m_inputs.universe().placeholdersAlone();
    uint32_t numberOfStatements = solver.numberOfStatements();
    uint32_t numberOfMadeStatements = numberOfStatements - static_cast<uint32_t>(statements.size());

    Vector<uint8_t> readersOfMadeStatement;
    readersOfMadeStatement.fill(0, numberOfMadeStatements);
    auto readersOf = [&](ValueFlowStatementID made) {
        return AtomicRef(readersOfMadeStatement[made.index() - statements.size()]);
    };
    auto addReader = [&](ValueFlowStatementID made, Reader reader) {
        uint8_t bit = static_cast<uint8_t>(reader);
        return !(readersOf(made).load(std::memory_order_relaxed) & bit) && !(readersOf(made).fetch_or(bit, std::memory_order_relaxed) & bit);
    };
    Vector<Vector<Visit>> visitsListedBy(loops.width());
    Vector<NodesLookedAt> nodesLookedAtBy(loops.width());
    uint32_t numberOfMadeNodes = solver.graph().numberOfNodes() - solver.input().numberOfNodes;
    auto follow = [&](unsigned worker, ValueFlowStatementID statement, Reader reader, ValueSetID startOfLookup) {
        Noter noter = noterOf(makers, worker);
        NodesLookedAt& nodesLookedAt = nodesLookedAtBy[worker];
        uint32_t numberOfNodesAskedFor = 0;
        m_inputs.forEachMadeNodeAskedForBy(statement, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup }), true, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            if (numberOfNodesAskedFor++ >= mostNodesNotToldApart) {
                nodesLookedAt.isLookedAt.ensureSize(numberOfMadeNodes);
                uint32_t number = solver.indexOfMadeNodes(key);
                if (nodesLookedAt.isLookedAt.set(number))
                    return;
                nodesLookedAt.numbers.append(number);
            }
            bool readsByName = (key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::Lookup) && key.name().kind == ValueFlowNameKind::Property;
            bool onlyCalls = (reader == Reader::OnlyCalls || isLookupOfCalleeByCall(m_inputs, solver.statementAt(statement), key)) && readsByName && !m_inputs.sets().meet(key.set, placeholders);
            bool onlyAccesses = reader == Reader::OnlyAccesses && readsByName && !m_inputs.sets().meet(key.set, placeholders);
            Reader readerThatLeavesNoValue = onlyCalls ? Reader::OnlyCalls : onlyAccesses ? Reader::OnlyAccesses : Reader::TakesValue;
            if (key.kind == ValueFlowMadeNodeKind::Read)
                noteRead(noter, key, startOfLookup ? readerThatLeavesNoValue : Reader::TakesValue, startOfLookup);
            if (key.kind == ValueFlowMadeNodeKind::Lookup && readsByName)
                noter.notes.receiversOfReads.add(key.payload, key.set);
            if (!made)
                return;
            Reader readerOfMade = key.kind == ValueFlowMadeNodeKind::Lookup ? readerThatLeavesNoValue : Reader::TakesValue;
            if (key.kind == ValueFlowMadeNodeKind::Lookup && key.name().kind == ValueFlowNameKind::ReceiversOfCalls)
                readerOfMade = Reader::OnlyCalls;
            if (addReader(made, readerOfMade))
                visitsListedBy[worker].append({ made, readerOfMade });
        });
        for (uint32_t number : nodesLookedAt.numbers)
            nodesLookedAt.isLookedAt.quickClear(number);
        nodesLookedAt.numbers.shrink(0);
    };
    auto followMadeStatements = [&] {
        Vector<Visit> visits;
        for (;;) {
            visits.shrink(0);
            for (Vector<Visit>& listed : visitsListedBy)
                visits.appendVector(std::exchange(listed, { }));
            if (visits.isEmpty())
                return;
            forEachIndexInParallel(loops, visits.size(), [&](unsigned index, unsigned worker) {
                Visit visit = visits[index];
                follow(worker, visit.statement, visit.reader, visit.reader != Reader::TakesValue ? solver.keyOf(visit.statement).set : ValueSetID());
            });
        }
    };

    forEachIndexInParallel(loops, bodies.numberOfBodies(), [&](unsigned body, unsigned worker) {
        if (body != bodies.hostBody() && !m_inputs.isReached(FunctionID(body)))
            return;
        bool isEmitted = body != bodies.hostBody() && program.functions[body].role != FunctionRole::Model;
        std::optional<ValuesWithOneUse> values;
        for (uint32_t index = ranges[body].statement; index < ranges[body + 1].statement; ++index) {
            m_inputs.prefetchMadeNodesAskedForBy(index + 8, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup }), true);
            const ValueFlowStatement& statement = statements[index];
            Reader reader = Reader::TakesValue;
            if (isEmitted && statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::PropGet && statement.function == FunctionID(body)) {
                if (!values)
                    values = findValuesWithOneUse(program.functions[body]);
                if (values->onlyCalled.quickGet(statement.site.index()))
                    reader = Reader::OnlyCalls;
                else if (values->onlyAccessed.quickGet(statement.site.index()))
                    reader = Reader::OnlyAccesses;
            }
            follow(worker, ValueFlowStatementID(index), reader, { });
        }
    });
    followMadeStatements();
    forEachIndexInParallel(loops, (numberOfMadeStatements + madeStatementsPerIndex - 1) / madeStatementsPerIndex, [&](unsigned run, unsigned worker) {
        uint32_t first = static_cast<uint32_t>(statements.size()) + run * madeStatementsPerIndex;
        for (uint32_t index = first; index < std::min(first + madeStatementsPerIndex, numberOfStatements); ++index) {
            ValueFlowStatementID statement(index);
            RELEASE_ASSERT(solver.statementAt(statement).kind != ValueFlowStatementKind::Operation);
            if (readersOf(statement).load(std::memory_order_relaxed) || solver.isAtRest(statement))
                continue;
            if (addReader(statement, Reader::TakesValue))
                follow(worker, statement, Reader::TakesValue, { });
        }
    });
    followMadeStatements();
}

bool UsesOfObjects::isFullyKnown(ValueSetID objects) const
{
    return !ValueSets::isComplement(objects) && !m_inputs.sets().meet(objects, m_inputs.universe().placeholdersAlone());
}

// A value that may be almost anything may be any built-in object, and so may what stands above an object that nobody knows. Which built-in objects such an object may be itself is for
// findHoldersWithListedKeys() to say: it stays in the set.
void UsesOfObjects::noteShownKeys(Noter noter, ValueSetID objects, ShownKeys shown, bool goesOnToPrototypes) const
{
    if (shown == ShownKeys::None)
        return;
    const ValueFlowUniverse& universe = m_inputs.universe();
    if (goesOnToPrototypes) {
        noter.notes.walkedForEnumerableKeys.add(isFullyKnown(objects) ? objects : universe.builtinObjects());
        return;
    }
    UnionOfSets& shownObjects = shown == ShownKeys::All ? noter.notes.withAllKeysShown : noter.notes.withEnumerableKeysShown;
    if (ValueSets::isComplement(objects)) {
        shownObjects.add(universe.builtinObjects());
        return;
    }
    shownObjects.add(noter.maker.intersect(objects, universe.builtinObjects()));
    shownObjects.add(noter.maker.intersect(objects, universe.placeholdersAlone()));
}

void UsesOfObjects::noteShownKeys(Noter noter, const ValueFlowBuiltinApplication& application, BuiltinOperand operand, ShownKeys shown) const
{
    noteShownKeys(noter, application.valueAt(operand, noter.maker), shown);
}

// A prototype that nobody knows may be any built-in object.
ValueSetID UsesOfObjects::followPrototypeLinks(ValueSetMaker& maker, ValueSetID objects) const
{
    ValueSetID chain = objects;
    ValueSetID frontier = objects;
    while (frontier != ValueSets::nothing) {
        ValueSetID above = ValueSets::nothing;
        m_inputs.sets().forEachMemberInLine(frontier, [&](ValueFlowObjectID object) {
            ValueSetID prototypes = m_heap.prototypesOf(maker, maker.of(object));
            above = maker.unite(above, isFullyKnown(prototypes) ? prototypes : m_inputs.universe().builtinObjects());
        });
        frontier = maker.intersect(above, ValueSets::complement(chain));
        chain = maker.unite(chain, above);
    }
    return chain;
}

void UsesOfObjects::noteOperations(ValueSetMakers& makers)
{
    const Program& program = m_inputs.program;
    forEachIndexInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index, unsigned worker) {
        if (!m_inputs.isReached(FunctionID(index)))
            return;
        Noter noter = noterOf(makers, worker);
        const Function& function = program.functions[index];
        for (const ValueFlowStatement& statement : m_inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            std::span<const ValueFlowOperand> operands = m_inputs.bodies().operandsOf(statement);
            for (uint32_t operand = 0; operand < operands.size(); ++operand) {
                Protocols protocols = protocolsOf(statement.opcode, operand);
                if (!protocols.containsAny(protocolsListingKeys))
                    continue;
                ValueSetID objects = m_heap.possibleValuesOf(noter.maker, operands[operand]);
                noter.notes.enumerated.add(objects);
                noteShownKeys(noter, objects, ShownKeys::Enumerable, protocols.contains(Protocol::PrototypeChain));
            }
            switch (statement.opcode) {
            case Opcode::PropHas:
            case Opcode::ScopeHasBinding:
                noter.notes.startsOfTestsUnderKey.add(function.at(statement.site).key().index(), m_heap.possibleValuesOf(noter.maker, operands[0]));
                break;
            case Opcode::ElemHas:
                noter.notes.observedUnderAnyKey.add(m_heap.possibleValuesOf(noter.maker, operands[OperandIndex::ElemHas::object]));
                break;
            case Opcode::CopyDataProperties:
            case Opcode::CopyObject: {
                uint32_t sourceIndex = statement.opcode == Opcode::CopyObject ? OperandIndex::CopyObject::source : OperandIndex::CopyDataProperties::source;
                ValueSetID source = m_heap.possibleValuesOf(noter.maker, operands[sourceIndex]);
                noter.notes.enumerated.add(source);
                noteShownKeys(noter, source, ShownKeys::Enumerable);
                break;
            }
            default:
                break;
            }
        }
    });
}

void UsesOfObjects::noteOperationsWithoutStatement()
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (hasRuleOfItsOwn(opcode))
            continue;
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
        for (uint32_t operand = 0; operand < numberOfDeclaredOperands; ++operand) {
            if (protocolsOf(opcode, operand).contains(Protocol::OwnKeys))
                opcodes.add(opcode);
        }
    }
    if (opcodes.isEmpty())
        return;
    const Program& program = m_inputs.program;
    std::atomic<bool> listsKeys { false };
    forEachIndexInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index) {
        if (!m_inputs.isReached(FunctionID(index)) || listsKeys.load(std::memory_order_relaxed))
            return;
        bool functionListsKeys = std::ranges::any_of(program.functions[index].values, [&](const Value& value) {
            return opcodes.contains(value.opcode);
        });
        if (functionListsKeys)
            listsKeys.store(true, std::memory_order_relaxed);
    });
    if (listsKeys.load(std::memory_order_relaxed))
        m_notes[0].enumerated.add(ValueSets::anything);
}

void UsesOfObjects::findHostFunctionsThatProgramHasStored(ValueSetMakers& makers)
{
    const Program& program = m_inputs.program;
    const ValueSets& sets = m_inputs.sets();
    const BuiltinTable& builtins = program.builtins();
    Vector<ValueFlowObjectID> functionsThatRefuseReceivers;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.at(BuiltinID(index)).origin != BuiltinOrigin::Host)
            continue;
        bool refusesReceivers = std::ranges::any_of(builtins.overloadsOf(BuiltinID(index)), [&](const BuiltinOverload& overload) {
            return overload.receiver && overload.receiver->accepted && !builtins.at(overload.receiver->accepted).isAnything();
        });
        if (refusesReceivers)
            functionsThatRefuseReceivers.append(m_inputs.universe().objectOf(BuiltinID(index)));
    }
    ValueSetID builtinFunctions = makers.of(0).intersect(makers.of(0).of(functionsThatRefuseReceivers.span()), m_inputs.universe().builtinFunctions());
    std::span<const ValueFlowStatement> statements = m_inputs.bodies().statements();
    Vector<uint32_t> keys;
    for (const ValueFlowHeapIndex::CallOfValue& call : m_heap.index().callsThatMayApplyBuiltins()) {
        const ValueFlowStatement& statement = statements[call.statement];
        if ((statement.opcode != Opcode::Call && statement.opcode != Opcode::CallList) || !sets.meet(call.callees, builtinFunctions))
            continue;
        if (PropertyKeyID key = program.at(statement.function).at(statement.site).calleeKey())
            keys.append(key.index());
    }
    AOT::sort(keys);
    removeRepeatedElements(keys);

    Vector<ValueSetID> stored(FillWith { }, keys.size(), ValueSets::nothing);
    forEachIndexInParallel(m_inputs.loops(), keys.size(), [&](unsigned position, unsigned worker) {
        UnionOfSets values;
        m_heap.forEachNodeWrittenUnder(ValueFlowName::property(program, PropertyKeyID(keys[position])), [&](ValueSetID, ValueSetID value) {
            if (sets.meet(value, builtinFunctions))
                values.add(value);
        });
        stored[position] = makers.of(worker).intersect(values.take(makers.of(worker)), builtinFunctions);
    });
    for (size_t position = 0; position < keys.size(); ++position) {
        if (stored[position] != ValueSets::nothing)
            m_hostFunctionsStoredUnderKey.add(keys[position], stored[position]);
    }
}

void UsesOfObjects::noteRefusedOperands(Noter noter, const ValueFlowBuiltinApplication& application)
{
    const BuiltinTable& builtins = m_inputs.program.builtins();
    if (builtins.at(application.builtin).origin != BuiltinOrigin::Host)
        return;
    const ValueFlowBuiltinTables& tables = m_inputs.universe().builtinTables();
    Vector<NotesOfBuiltins::RefusedOperand, 8> takenAtOperands;
    bool mayRunOnObjectOfProgram = application.function && application.kind == ValueFlowCallKind::CallByNativeCode;
    if (PropertyKeyID key = application.function && application.kind == ValueFlowCallKind::Call ? m_inputs.program.at(application.function).at(application.site).calleeKey() : PropertyKeyID()) {
        const ValueFlowUniverse& universe = m_inputs.universe();
        ValueFlowObjectID function = universe.objectOf(application.builtin);
        auto stored = m_hostFunctionsStoredUnderKey.find(key.index());
        if (stored != m_hostFunctionsStoredUnderKey.end() && m_inputs.sets().contains(stored->value, function)) {
            ValueSetID found = m_foundAboveReceivers.ensure(static_cast<uint64_t>(application.thisValue.index()) << 32 | key.index(), [&] {
                ValueSetID ofProgram = noter.maker.intersect(application.thisValue, noter.maker.intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects())));
                return ofProgram == ValueSets::nothing ? ValueSets::nothing : m_heap.writtenUnder(noter.maker, m_heap.chainOf(noter.maker, ofProgram), ValueFlowName::property(m_inputs.program, key));
            });
            mayRunOnObjectOfProgram = m_inputs.sets().contains(found, function);
        }
    }
    for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
        if (overload.mode != application.mode())
            continue;
        if (overload.receiver && mayRunOnObjectOfProgram)
            uniteTakenAtOperand(tables, noter.maker, *overload.receiver, takenAtOperands);
        for (const BuiltinParameter& parameter : overload.parameters)
            uniteTakenAtOperand(tables, noter.maker, parameter, takenAtOperands);
    }
    for (const NotesOfBuiltins::RefusedOperand& entry : takenAtOperands) {
        if (entry.taken != ValueSets::anything)
            noter.notes.enumerated.add(noter.maker.intersect(application.valueAt(entry.operand, noter.maker), ValueSets::complement(entry.taken)));
    }
}

void UsesOfObjects::noteApplication(Noter noter, const ValueFlowBuiltinApplication& application)
{
    if (!m_builtinsThatLookAtObjects.quickGet(application.builtin.index()))
        return;
    noteRefusedOperands(noter, application);
    forEachLookAtOperands(m_inputs, application.builtin, application.mode(), [&](std::optional<BuiltinOperand> operand, ShownKeys shown) {
        ValueSetID objects = operand ? application.valueAt(*operand, noter.maker) : ValueSets::anything;
        noter.notes.enumerated.add(objects);
        noteShownKeys(noter, objects, shown);
    }, [&](BuiltinOperand operand) {
        noter.notes.observedUnderAnyKey.add(application.valueAt(operand, noter.maker));
    }, [&](BuiltinOperand object, PropertyKeyID key, bool looksAtOwnPropertyAlone) {
        (looksAtOwnPropertyAlone ? noter.notes.observedUnderKey : noter.notes.startsOfTestsUnderKey).add(key.index(), application.valueAt(object, noter.maker));
    }, [&](BuiltinOperand object, BuiltinOperand key, bool looksAtOwnPropertyAlone) {
        noteTestUnderKeys(noter, application, object, key, looksAtOwnPropertyAlone);
    });
}

auto UsesOfObjects::findNotesOf(ValueSetMaker& maker, ValueSetID builtinFunctions, BuiltinCallMode mode) const -> std::unique_ptr<NotesOfBuiltins>
{
    const BuiltinTable& builtins = m_inputs.program.builtins();
    auto notes = makeUnique<NotesOfBuiltins>();
    auto addOnce = [](auto& list, const auto& entry) {
        if (!list.contains(entry))
            list.append(entry);
    };
    m_inputs.sets().forEachMember(builtinFunctions, [&](ValueFlowObjectID callee) {
        BuiltinID builtin = m_inputs.universe().builtinOf(callee);
        if (!m_builtinsThatLookAtObjects.quickGet(builtin.index()))
            return;
        forEachLookAtOperands(m_inputs, builtin, mode, [&](std::optional<BuiltinOperand> operand, ShownKeys shown) {
            if (!operand) {
                notes->enumeratesAnything = true;
                return;
            }
            addOnce(notes->enumerated, *operand);
            if (shown != ShownKeys::None)
                addOnce(shown == ShownKeys::All ? notes->withAllKeysShown : notes->withEnumerableKeysShown, *operand);
        }, [&](BuiltinOperand operand) {
            addOnce(notes->observedUnderAnyKey, operand);
        }, [&](BuiltinOperand object, PropertyKeyID key, bool looksAtOwnPropertyAlone) {
            addOnce(notes->testsUnderNames, NotesOfBuiltins::TestUnderName { object, key.index(), looksAtOwnPropertyAlone });
        }, [&](BuiltinOperand object, BuiltinOperand key, bool looksAtOwnPropertyAlone) {
            addOnce(notes->testsUnderOperands, NotesOfBuiltins::TestUnderOperand { object, key, looksAtOwnPropertyAlone });
        });
        if (builtins.at(builtin).origin != BuiltinOrigin::Host)
            return;
        Vector<NotesOfBuiltins::RefusedOperand, 8> takenAtOperands;
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
            if (overload.mode != mode)
                continue;
            for (const BuiltinParameter& parameter : overload.parameters) {
                notes->areToldApart |= parameter.operand.isReceiver();
                uniteTakenAtOperand(m_inputs.universe().builtinTables(), maker, parameter, takenAtOperands);
            }
        }
        for (const NotesOfBuiltins::RefusedOperand& entry : takenAtOperands) {
            if (entry.taken == ValueSets::anything)
                continue;
            size_t position = notes->refusedOperands.findIf([&](const NotesOfBuiltins::RefusedOperand& known) {
                return known.operand == entry.operand;
            });
            if (position == notFound)
                notes->refusedOperands.append(entry);
            else
                notes->refusedOperands[position].taken = maker.intersect(notes->refusedOperands[position].taken, entry.taken);
        }
    });
    return notes;
}

void UsesOfObjects::noteTestUnderKeys(Noter noter, const ValueFlowBuiltinApplication& application, BuiltinOperand object, BuiltinOperand key, bool looksAtOwnPropertyAlone)
{
    const ValueSets& sets = m_inputs.sets();
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueFlowHeapTables& tables = universe.builtinTables().heap();
    ValueSetID objects = application.valueAt(object, noter.maker);
    ValueSetID keys = application.valueAt(key, noter.maker);
    if (sets.isEmpty(keys) || sets.meet(keys, tables.keysNobodyKnows()) || sets.meet(keys, tables.numberKinds())) {
        noter.notes.observedUnderAnyKey.add(objects);
        return;
    }
    PartsByNumber& underKey = looksAtOwnPropertyAlone ? noter.notes.observedUnderKey : noter.notes.startsOfTestsUnderKey;
    sets.forEachMember(keys, [&](ValueFlowObjectID constant) {
        underKey.add(universe.keyOf(constant).index(), objects);
    });
}

void UsesOfObjects::noteApplicationOfAll(Noter noter, const ValueFlowBuiltinApplication& application, ValueSetID builtinCallees)
{
    const NotesOfBuiltins& notes = m_notesOfSetsOfBuiltins.ensure(static_cast<uint64_t>(builtinCallees.index()) << 8 | static_cast<uint64_t>(application.mode()), [&] {
        return findNotesOf(noter.maker, builtinCallees, application.mode());
    });
    ValueFlowBuiltinApplication applicationOfOne = application;
    if (notes.areToldApart) {
        m_inputs.sets().forEachMember(builtinCallees, [&](ValueFlowObjectID callee) {
            applicationOfOne.builtin = m_inputs.universe().builtinOf(callee);
            noteApplication(noter, applicationOfOne);
        });
        return;
    }
    if (notes.enumeratesAnything) {
        noter.notes.enumerated.add(ValueSets::anything);
        noteShownKeys(noter, ValueSets::anything, ShownKeys::All);
    }
    for (BuiltinOperand operand : notes.enumerated)
        noter.notes.enumerated.add(application.valueAt(operand, noter.maker));
    for (BuiltinOperand operand : notes.withAllKeysShown)
        noteShownKeys(noter, application, operand, ShownKeys::All);
    for (BuiltinOperand operand : notes.withEnumerableKeysShown)
        noteShownKeys(noter, application, operand, ShownKeys::Enumerable);
    for (BuiltinOperand operand : notes.observedUnderAnyKey)
        noter.notes.observedUnderAnyKey.add(application.valueAt(operand, noter.maker));
    for (const NotesOfBuiltins::TestUnderName& test : notes.testsUnderNames)
        (test.looksAtOwnPropertyAlone ? noter.notes.observedUnderKey : noter.notes.startsOfTestsUnderKey).add(test.key, application.valueAt(test.object, noter.maker));
    for (const NotesOfBuiltins::TestUnderOperand& test : notes.testsUnderOperands)
        noteTestUnderKeys(noter, application, test.object, test.key, test.looksAtOwnPropertyAlone);
    for (const NotesOfBuiltins::RefusedOperand& refused : notes.refusedOperands)
        noter.notes.enumerated.add(noter.maker.intersect(application.valueAt(refused.operand, noter.maker), ValueSets::complement(refused.taken)));

    PropertyKeyID key = application.kind == ValueFlowCallKind::Call ? m_inputs.program.at(application.function).at(application.site).calleeKey() : PropertyKeyID();
    auto stored = key ? m_hostFunctionsStoredUnderKey.find(key.index()) : m_hostFunctionsStoredUnderKey.end();
    if (stored == m_hostFunctionsStoredUnderKey.end())
        return;
    m_inputs.sets().forEachMember(noter.maker.intersect(builtinCallees, stored->value), [&](ValueFlowObjectID callee) {
        applicationOfOne.builtin = m_inputs.universe().builtinOf(callee);
        noteRefusedOperands(noter, applicationOfOne);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
