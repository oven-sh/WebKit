/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsFieldsInternal.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static bool looksAtObjects(const BuiltinOverload& overload)
{
    auto listsKeys = [](const BuiltinParameter& parameter) {
        return parameter.uses != OperandUses::all() && parameter.protocols.containsAny(protocolsListingKeys);
    };
    if ((overload.receiver && listsKeys(*overload.receiver)) || std::ranges::any_of(overload.parameters, listsKeys))
        return true;
    return std::ranges::any_of(overload.flow.propertyActions, [](const BuiltinPropertyAction& action) {
        switch (action.kind) {
        case BuiltinPropertyActionKind::TestsByKey:
        case BuiltinPropertyActionKind::ReadsGetter:
        case BuiltinPropertyActionKind::ReadsSetter:
        case BuiltinPropertyActionKind::AssignsFrom:
        case BuiltinPropertyActionKind::DefinesFrom:
        case BuiltinPropertyActionKind::TakesDescriptorMap:
            return true;
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
            return false;
        }
    });
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
    noteBehaviors();
    noteObjectsOfRuntimeModules();
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

UnionOfSets UsesOfObjects::takeUnion(UnionOfSets Notes::* parts)
{
    UnionOfSets all;
    for (Notes& notes : m_notes)
        all.takeFrom(notes.*parts);
    return all;
}

Vector<PartOfNumber> UsesOfObjects::takeParts(PartsByNumber Notes::* parts)
{
    Vector<PartOfNumber> all;
    for (Notes& notes : m_notes)
        all.appendVector((notes.*parts).take());
    return all;
}

SetsByNumber UsesOfObjects::takeExpanded(ValueSetMakers& makers, Vector<PartOfNumber>&& parts)
{
    PartsOfNumbers partsOfNumbers(m_inputs.loops(), WTF::move(parts));
    Vector<ValueSetID> expanded(partsOfNumbers.size());
    forEachIndexInParallel(m_inputs.loops(), partsOfNumbers.size(), [&](unsigned position, unsigned worker) {
        ValueSetMaker& maker = makers.of(worker);
        expanded[position] = m_heap.expand(maker, partsOfNumbers.unionAt(position, maker));
    });
    SetsByNumber sets;
    sets.reserveInitialCapacity(partsOfNumbers.size());
    for (unsigned position = 0; position < partsOfNumbers.size(); ++position)
        sets.add(partsOfNumbers.numberAt(position), expanded[position]);
    return sets;
}

// A walk over the keys of an object may go on to the keys that it inherits, as for-in and the formatter of console.log do, and so does a test of a name that is not a test of an own property.
void UsesOfObjects::goOnToPrototypes(ValueSetMakers& makers)
{
    runTasksInParallel(m_inputs.loops(), {
        [&] {
            findObjectsObservedUnderKeys(makers);
        },
        [&] {
            BorrowedValueSetMaker maker(m_inputs.makerPool());
            m_enumerated = m_heap.chainOf(maker.get(), takeUnion(&Notes::enumerated).takeExpanded(m_heap, maker.get()));
        },
        [&] {
            BorrowedValueSetMaker maker(m_inputs.makerPool());
            m_observedUnderAnyKey = m_heap.chainOf(maker.get(), takeUnion(&Notes::observedUnderAnyKey).takeExpanded(m_heap, maker.get()));
        },
        [&] {
            ValueSetMakers makersBeside(m_inputs);
            m_calledUnderKey = takeExpanded(makersBeside, takeParts(&Notes::calledUnderKey));
            m_accessedUnderKey = takeExpanded(makersBeside, takeParts(&Notes::accessedUnderKey));
        },
        [&] {
            listReceiversOfReads();
        },
    });
}

void UsesOfObjects::findObjectsObservedUnderKeys(ValueSetMakers& makers)
{
    const ParallelLoops& loops = m_inputs.loops();
    // Tests of many names start at the same objects, whose chain is walked once.
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
        chains[index] = m_heap.chainOf(makers.of(worker), distinctStarts[index]);
    });
    Vector<PartOfNumber> observedUnderKey = takeParts(&Notes::observedUnderKey);
    for (unsigned position = 0; position < startsOfTests.size(); ++position) {
        size_t index = std::ranges::lower_bound(distinctStarts, starts[position]) - distinctStarts.begin();
        if (chains[index] != ValueSets::nothing)
            observedUnderKey.append({ startsOfTests.numberAt(position), chains[index] });
    }

    m_observedUnderKey = takeExpanded(makers, WTF::move(observedUnderKey));
}

void UsesOfObjects::listReceiversOfReads()
{
    // The parts that list their members, each once. Nobody pays for a union that is asked for a handful of objects.
    PartsOfNumbers receiversOfReads(m_inputs.loops(), takeParts(&Notes::receiversOfReads));
    m_receiversOfReads.reserveInitialCapacity(receiversOfReads.size());
    for (unsigned position = 0; position < receiversOfReads.size(); ++position) {
        Vector<ValueSetID> receivers;
        for (const PartOfNumber& part : receiversOfReads.partsAt(position)) {
            if (!ValueSets::isComplement(part.set))
                receivers.append(part.set);
        }
        m_receiversOfReads.add(receiversOfReads.numberAt(position), WTF::move(receivers));
    }
}

namespace {

struct ValuesWithOneUse {
    // The values that go nowhere but into the callee position of calls.
    BitVector onlyCalled;
    // The values that go nowhere but into the receiver position of reads and stores by name.
    BitVector onlyAccessed;
};

} // anonymous namespace

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

    // A cast hands on what it takes.
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

// A location that is only read as a callee loses its value once the operations that read it are gone, so a pass has to find each of them from the location.
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
        // A read of what was stored under a key nobody knows finds nothing that was stored by name. A read under such a key takes every property.
        break;
    }
}

// Every read of a value goes through a node that is keyed by the objects it looks into: those that the lookup starts at and all that lies above them.
void UsesOfObjects::noteReads(ValueSetMakers& makers)
{
    struct Visit {
        ValueFlowStatementID statement;
        Reader reader;
    };

    // The nodes that a thread has looked at for the statement that it follows, by ValueFlowSolver::indexOfMadeNodes().
    struct NodesLookedAt {
        BitVector isLookedAt;
        Vector<uint32_t> numbers;
    };

    constexpr uint32_t madeStatementsPerIndex = 512;
    constexpr uint32_t mostNodesNotToldApart = 64;
    const Program& program = m_inputs.program;
    const ParallelLoops& loops = m_inputs.loops();
    const ValueFlowSolver& solver = m_inputs.solver();
    const ValueFlowBodies& bodies = m_inputs.bodies();
    std::span<const ValueFlowStatement> statements = bodies.statements();
    std::span<const ValueFlowBodyRange> ranges = bodies.ranges();
    ValueSetID placeholders = m_inputs.universe().placeholdersAlone();
    uint32_t numberOfStatements = solver.numberOfStatements();
    uint32_t numberOfMadeStatements = numberOfStatements - static_cast<uint32_t>(statements.size());

    // Bits of Reader. The thread that sets a bit is the one that lists the visit.
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
    // A number stands for one made node or for several that were made together.
    uint32_t numberOfMadeNodes = solver.graph().numberOfNodes() - solver.input().numberOfNodes;
    auto follow = [&](unsigned worker, ValueFlowStatementID statement, Reader reader, ValueSetID startOfLookup) {
        Noter noter = noterOf(makers, worker);
        NodesLookedAt& nodesLookedAt = nodesLookedAtBy[worker];
        uint32_t numberOfNodesAskedFor = 0;
        m_inputs.forEachMadeNodeAskedForBy(statement, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup }), true, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            // A statement that keeps what all its runs have asked for lists the same nodes again and again. In a long list a node is looked at once.
            if (numberOfNodesAskedFor++ >= mostNodesNotToldApart) {
                nodesLookedAt.isLookedAt.ensureSize(numberOfMadeNodes);
                uint32_t number = solver.indexOfMadeNodes(key);
                if (nodesLookedAt.isLookedAt.set(number))
                    return;
                nodesLookedAt.numbers.append(number);
            }
            bool readsByName = (key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::Lookup) && key.name().kind == ValueFlowNameKind::Property;
            // Nobody lists the receivers of a read through a placeholder.
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
        // No code is emitted for a model: native code reads what it reads, and no pass turns its calls into direct calls.
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
    // Whatever is reached from here on is reached by a reader that takes the value: no made statement is an operation, and only an operation brings another reader up.
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

// Where an import links a module of the run time, the loader lists the names of the object that the body of the module answers.
void UsesOfObjects::noteObjectsOfRuntimeModules()
{
    const Program& program = m_inputs.program;
    for (const Module& module : program.modules) {
        if (module.kind != ModuleKind::RuntimeSource || module.exportRule == SyntheticExportRule::None || !module.body || !m_inputs.isReached(module.body))
            continue;
        if (!program.at(module.body).results.isEmpty())
            m_notes[0].enumerated.add(m_inputs.solver().valueOf(m_inputs.bodies().nodes().result(module.body, 0)));
    }
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
                if (protocolsOf(statement.opcode, operand).containsAny(protocolsListingKeys))
                    noter.notes.enumerated.add(m_heap.valueOf(noter.maker, operands[operand]));
            }
            switch (statement.opcode) {
            case Opcode::PropHas:
            case Opcode::ScopeHasBinding:
                noter.notes.startsOfTestsUnderKey.add(function.at(statement.site).key().index(), m_heap.valueOf(noter.maker, operands[0]));
                break;
            case Opcode::ElemHas:
                noter.notes.observedUnderAnyKey.add(m_heap.valueOf(noter.maker, operands[OperandIndex::ElemHas::object]));
                break;
            case Opcode::CopyDataProperties:
                noter.notes.enumerated.add(m_heap.valueOf(noter.maker, operands[OperandIndex::CopyDataProperties::source]));
                break;
            case Opcode::CopyObject:
                noter.notes.enumerated.add(m_heap.valueOf(noter.maker, operands[OperandIndex::CopyObject::source]));
                break;
            default:
                break;
            }
        }
    });
}

// The bodies keep nothing of an operand that only such a protocol gets hold of where the operation has no rule of its own. No operation of a program as read is like that.
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

// The hub of a behavior holds whatever has reached an operand with the behavior, and where the behavior goes on to what an object holds, that too.
void UsesOfObjects::noteBehaviors()
{
    const ValueFlowBuiltinTables& tables = m_inputs.universe().builtinTables();
    for (uint32_t index = 0; index < tables.numberOfBehaviors(); ++index) {
        const ValueFlowBehavior& behavior = tables.at(ValueFlowBehaviorID(index));
        bool readsEveryMember = (behavior.protocols.contains(Protocol::Named) && behavior.namedMembers.empty()) || (behavior.protocols.contains(Protocol::CallsNamed) && behavior.calledMembers.empty());
        if (behavior.protocols.containsAny(protocolsListingKeys) || readsEveryMember)
            m_notes[0].enumerated.add(m_inputs.solver().valueOf(m_inputs.bodies().nodes().hub(ValueFlowBehaviorID(index))));
    }
}

void UsesOfObjects::noteParameter(Noter noter, const ValueFlowBuiltinApplication& application, const BuiltinParameter& parameter)
{
    // Such an operand has escaped.
    if (parameter.uses == OperandUses::all())
        return;
    if (!parameter.protocols.containsAny(protocolsListingKeys) || parameter.roles.contains(BuiltinOperandRole::KeysAreNotListed))
        return;
    // What the operand holds is listed too, and only a hub follows that.
    bool goesOnToContents = parameter.uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents });
    if (goesOnToContents && !m_inputs.universe().builtinTables().behaviorOf(parameter)) {
        noter.notes.enumerated.add(ValueSets::anything);
        return;
    }
    noter.notes.enumerated.add(application.valueAt(parameter.operand, noter.maker));
}

// The built-in looks at single properties of the operand and shows nobody the order of the keys. Where no action says under which key it looks, as for Reflect.has, the key may be any.
void UsesOfObjects::noteParameterWithoutAction(Noter noter, const ValueFlowBuiltinApplication& application, const BuiltinOverload& overload, const BuiltinParameter& parameter)
{
    if (parameter.uses == OperandUses::all() || !parameter.roles.contains(BuiltinOperandRole::KeysAreNotListed) || !parameter.protocols.containsAny(protocolsListingKeys))
        return;
    bool hasAction = std::ranges::any_of(overload.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
        return action.object == parameter.operand;
    });
    if (!hasAction)
        noter.notes.observedUnderAnyKey.add(application.valueAt(parameter.operand, noter.maker));
}

// The built-in finds out whether something lies under a key, or what kind of property does. An operation on one own property runs the traps of a proxy, and one that goes on to the prototypes declares the walk.
void UsesOfObjects::noteTest(Noter noter, const ValueFlowBuiltinApplication& application, const BuiltinOverload& overload, const BuiltinPropertyAction& action)
{
    const Program& program = m_inputs.program;
    const ValueSets& sets = m_inputs.sets();
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueFlowHeapTables& tables = universe.builtinTables().heap();
    ValueSetID objects = application.valueAt(action.object, noter.maker);
    const BuiltinParameter* parameter = program.builtins().parameterFor(overload, action.object);
    bool looksAtOwnPropertyAlone = parameter && parameter->roles.contains(BuiltinOperandRole::KeysAreNotListed) && !parameter->protocols.contains(Protocol::PrototypeChain);
    PartsByNumber& underKey = looksAtOwnPropertyAlone ? noter.notes.observedUnderKey : noter.notes.startsOfTestsUnderKey;

    switch (action.key.kind) {
    case BuiltinKeyKind::None:
        return;
    case BuiltinKeyKind::Names:
        for (BuiltinName name : action.key.names)
            underKey.add(program.keyOf(name).index(), objects);
        return;
    case BuiltinKeyKind::Operand: {
        ValueSetID keys = application.valueAt(*action.key.operand, noter.maker);
        // An operand that is left out is undefined, which names a property like any other value.
        if (sets.isEmpty(keys) || sets.meet(keys, tables.keysNobodyKnows()) || sets.meet(keys, tables.numberKinds()))
            break;
        sets.forEachMember(keys, [&](ValueFlowObjectID constant) {
            underKey.add(universe.keyOf(constant).index(), objects);
        });
        return;
    }
    case BuiltinKeyKind::Any:
    case BuiltinKeyKind::Indexes:
    case BuiltinKeyKind::ElementsOf:
        break;
    }
    noter.notes.observedUnderAnyKey.add(objects);
}

// Which overload a call selects depends on its operands. Every overload of the mode counts.
void UsesOfObjects::findHostFunctionsThatProgramHasStored(ValueSetMakers& makers)
{
    const Program& program = m_inputs.program;
    const ValueSets& sets = m_inputs.sets();
    // Most functions of the host take whatever they are called on.
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
        m_heap.forEachNodeWrittenUnder(ValueFlowName::property(PropertyKeyID(keys[position])), [&](ValueSetID, ValueSetID value) {
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

// A function of the host names a value that it refuses in the message of its error: `Received an instance of Point`, `Received {}`, `Received [Object]`.
void UsesOfObjects::noteRefusedOperands(Noter noter, const ValueFlowBuiltinApplication& application)
{
    const BuiltinTable& builtins = m_inputs.program.builtins();
    if (builtins.at(application.builtin).origin != BuiltinOrigin::Host)
        return;
    const ValueFlowBuiltinTables& tables = m_inputs.universe().builtinTables();
    struct TakenAtOperand {
        BuiltinOperand operand;
        ValueSetID taken;
    };
    Vector<TakenAtOperand, 8> takenAtOperands;
    auto note = [&](const BuiltinParameter& parameter) {
        ValueSetID taken = parameter.accepted ? tables.membersSurelyOf(parameter.accepted) : ValueSets::anything;
        if (parameter.converted)
            taken = noter.maker.unite(taken, tables.membersSurelyOf(parameter.converted));
        for (TakenAtOperand& entry : takenAtOperands) {
            if (entry.operand == parameter.operand) {
                entry.taken = noter.maker.unite(entry.taken, taken);
                return;
            }
        }
        takenAtOperands.append({ parameter.operand, taken });
    };
    // A call of a bound function that an operation spells out comes with what was bound as `this`.
    bool mayRunOnObjectOfProgram = application.function && application.kind == ValueFlowCallKind::CallByNativeCode;
    if (PropertyKeyID key = application.function && application.kind == ValueFlowCallKind::Call ? m_inputs.program.at(application.function).at(application.site).calleeKey() : PropertyKeyID()) {
        const ValueFlowUniverse& universe = m_inputs.universe();
        ValueFlowObjectID function = universe.objectOf(application.builtin);
        auto stored = m_hostFunctionsStoredUnderKey.find(key.index());
        if (stored != m_hostFunctionsStoredUnderKey.end() && m_inputs.sets().contains(stored->value, function)) {
            // One call has many such callees where its receiver is little known, and many calls have the same receivers: the chain above them is walked once for a name.
            ValueSetID found = m_foundAboveReceivers.ensure(static_cast<uint64_t>(application.thisValue.index()) << 32 | key.index(), [&] {
                ValueSetID ofProgram = noter.maker.intersect(application.thisValue, noter.maker.intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects())));
                return ofProgram == ValueSets::nothing ? ValueSets::nothing : m_heap.writtenUnder(noter.maker, m_heap.chainOf(noter.maker, ofProgram), ValueFlowName::property(key));
            });
            mayRunOnObjectOfProgram = m_inputs.sets().contains(found, function);
        }
    }
    for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
        if (overload.mode != application.mode())
            continue;
        if (overload.receiver && mayRunOnObjectOfProgram)
            note(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            note(parameter);
    }
    for (const TakenAtOperand& entry : takenAtOperands) {
        if (entry.taken != ValueSets::anything)
            noter.notes.enumerated.add(noter.maker.intersect(application.valueAt(entry.operand, noter.maker), ValueSets::complement(entry.taken)));
    }
}

void UsesOfObjects::noteApplication(Noter noter, const ValueFlowBuiltinApplication& application)
{
    if (!m_builtinsThatLookAtObjects.quickGet(application.builtin.index()))
        return;
    noteRefusedOperands(noter, application);
    for (const BuiltinOverload& overload : m_inputs.program.builtins().overloadsOf(application.builtin)) {
        if (overload.mode != application.mode())
            continue;
        if (overload.receiver) {
            noteParameter(noter, application, *overload.receiver);
            noteParameterWithoutAction(noter, application, overload, *overload.receiver);
        }
        for (const BuiltinParameter& parameter : overload.parameters) {
            noteParameter(noter, application, parameter);
            noteParameterWithoutAction(noter, application, overload, parameter);
        }
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            switch (action.kind) {
            case BuiltinPropertyActionKind::TestsByKey:
            case BuiltinPropertyActionKind::ReadsGetter:
            case BuiltinPropertyActionKind::ReadsSetter:
                noteTest(noter, application, overload, action);
                break;
            case BuiltinPropertyActionKind::AssignsFrom:
            case BuiltinPropertyActionKind::DefinesFrom:
            case BuiltinPropertyActionKind::TakesDescriptorMap:
                if (action.value.operand)
                    noter.notes.enumerated.add(application.valueAt(*action.value.operand, noter.maker));
                else
                    noter.notes.enumerated.add(ValueSets::anything);
                break;
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
                break;
            }
        }
    }
}

auto UsesOfObjects::findNotesOf(ValueSetMaker& maker, ValueSetID builtinFunctions, BuiltinCallMode mode) const -> std::unique_ptr<NotesOfBuiltins>
{
    const Program& program = m_inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowBuiltinTables& tables = m_inputs.universe().builtinTables();
    auto notes = makeUnique<NotesOfBuiltins>();
    auto addOnce = [](auto& list, const auto& entry) {
        if (!list.contains(entry))
            list.append(entry);
    };
    auto noteParameter = [&](const BuiltinOverload& overload, const BuiltinParameter& parameter) {
        if (parameter.uses == OperandUses::all() || !parameter.protocols.containsAny(protocolsListingKeys))
            return;
        if (parameter.roles.contains(BuiltinOperandRole::KeysAreNotListed)) {
            bool hasAction = std::ranges::any_of(overload.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                return action.object == parameter.operand;
            });
            if (!hasAction)
                addOnce(notes->observedUnderAnyKey, parameter.operand);
            return;
        }
        bool goesOnToContents = parameter.uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents });
        if (goesOnToContents && !tables.behaviorOf(parameter))
            notes->enumeratesAnything = true;
        else
            addOnce(notes->enumerated, parameter.operand);
    };
    auto noteTest = [&](const BuiltinOverload& overload, const BuiltinPropertyAction& action) {
        const BuiltinParameter* parameter = builtins.parameterFor(overload, action.object);
        bool looksAtOwnPropertyAlone = parameter && parameter->roles.contains(BuiltinOperandRole::KeysAreNotListed) && !parameter->protocols.contains(Protocol::PrototypeChain);
        switch (action.key.kind) {
        case BuiltinKeyKind::None:
            return;
        case BuiltinKeyKind::Names:
            for (BuiltinName name : action.key.names)
                addOnce(notes->testsUnderNames, NotesOfBuiltins::TestUnderName { action.object, program.keyOf(name).index(), looksAtOwnPropertyAlone });
            return;
        case BuiltinKeyKind::Operand:
            addOnce(notes->testsUnderOperands, NotesOfBuiltins::TestUnderOperand { action.object, *action.key.operand, looksAtOwnPropertyAlone });
            return;
        case BuiltinKeyKind::Any:
        case BuiltinKeyKind::Indexes:
        case BuiltinKeyKind::ElementsOf:
            addOnce(notes->observedUnderAnyKey, action.object);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    m_inputs.sets().forEachMember(builtinFunctions, [&](ValueFlowObjectID callee) {
        BuiltinID builtin = m_inputs.universe().builtinOf(callee);
        if (!m_builtinsThatLookAtObjects.quickGet(builtin.index()))
            return;
        Vector<NotesOfBuiltins::RefusedOperand, 8> takenAtOperands;
        bool isOfHost = builtins.at(builtin).origin == BuiltinOrigin::Host;
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
            if (overload.mode != mode)
                continue;
            if (overload.receiver)
                noteParameter(overload, *overload.receiver);
            for (const BuiltinParameter& parameter : overload.parameters) {
                noteParameter(overload, parameter);
                if (!isOfHost)
                    continue;
                notes->areToldApart |= parameter.operand.isReceiver();
                ValueSetID taken = parameter.accepted ? tables.membersSurelyOf(parameter.accepted) : ValueSets::anything;
                if (parameter.converted)
                    taken = maker.unite(taken, tables.membersSurelyOf(parameter.converted));
                size_t position = takenAtOperands.findIf([&](const NotesOfBuiltins::RefusedOperand& entry) {
                    return entry.operand == parameter.operand;
                });
                if (position == notFound)
                    takenAtOperands.append({ parameter.operand, taken });
                else
                    takenAtOperands[position].taken = maker.unite(takenAtOperands[position].taken, taken);
            }
            for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                switch (action.kind) {
                case BuiltinPropertyActionKind::TestsByKey:
                case BuiltinPropertyActionKind::ReadsGetter:
                case BuiltinPropertyActionKind::ReadsSetter:
                    noteTest(overload, action);
                    break;
                case BuiltinPropertyActionKind::AssignsFrom:
                case BuiltinPropertyActionKind::DefinesFrom:
                case BuiltinPropertyActionKind::TakesDescriptorMap:
                    if (action.value.operand)
                        addOnce(notes->enumerated, *action.value.operand);
                    else
                        notes->enumeratesAnything = true;
                    break;
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
                    break;
                }
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
    if (notes.enumeratesAnything)
        noter.notes.enumerated.add(ValueSets::anything);
    for (BuiltinOperand operand : notes.enumerated)
        noter.notes.enumerated.add(application.valueAt(operand, noter.maker));
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
