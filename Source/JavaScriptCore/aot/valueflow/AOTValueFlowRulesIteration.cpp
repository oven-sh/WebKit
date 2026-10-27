/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesProtocolsInternal.h"

#if ENABLE(AOT)

#include "AOTValueFlowBuiltins.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BuiltinIteration {
    BuiltinClassID iteratorClass;
    IterationKind kind { IterationKind::Values };
    ValueSetID containers { ValueSets::nothing };
};

// The unknown value stands for everything else that the unknown object holds.
static ValueFlowNodeID bunStoredInUnknownObject(ValueFlowRuleContext& context, ValueFlowName name)
{
    return context.masked(context.readWhatWasWritten(ClimbingSet(context.universe().unknownObjectAlone()), name), context.maker().of(context.universe().objectOf(Builtins::Bun)));
}

struct StepTarget {
    void take(ValueFlowNodeID values) const
    {
        if (kind == IteratorKind::Async)
            connectAwaitedValue(context, { values, ValueSets::nothing, ValueSets::anything }, node);
        else
            context.connect(values, node);
    }

    void take(ClimbingSet values) const
    {
        if (kind == IteratorKind::Async)
            context.connect(context.lookup(values, awaitedValueName), node);
        else
            context.add(node, values);
    }

    void takeContentsOfSlot(ClimbingSet objects, ValueFlowName name) const
    {
        const ValueFlowUniverse& universe = context.universe();
        ClimbingSet unknownObject(universe.unknownObjectAlone());
        ClimbingSet untrackedObject(universe.untrackedObjectAlone());
        if (mayBeUntrackedObject(context, objects)) {
            take(context.readWhatWasWritten(ClimbingSet(universe.knownObjects()), name));
            take(context.readWhatWasWritten(untrackedObject, name));
            take(context.readWhatWasWritten(unknownObject, name));
        } else {
            ClimbingSet followed = known(context, objects);
            take(context.readWhatWasWritten(followed, name));
            bool followsSomething = holdsSomething(context, followed);
            if (followsSomething && context.holdsAnyOf(followed, ValueFlowProgramNodes::escaped()))
                take(context.readWhatWasWritten(unknownObject, name));
            if (followsSomething && context.holdsAnyOf(followed, ValueFlowProgramNodes::lost()))
                take(context.readWhatWasWritten(untrackedObject, name));
        }
        if (mayBeUnknownObject(context, objects)) {
            context.add(node, unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject));
            take(bunStoredInUnknownObject(context, name));
        }
    }

    ValueFlowRuleContext& context;
    IteratorKind kind;
    ValueFlowNodeID node;
};

struct IteratorStep {
    IteratorKind kind { IteratorKind::Sync };
    ResumeMode mode { ResumeMode::Next };
    bool takesReturnedValue { false };
    ValueFlowObjectID pair;
    std::optional<uint32_t> indexInPattern { };
};

struct StepOfBuiltinIterator {
    BuiltinClassID iteratorClass;
    ResumeMode mode { ResumeMode::Next };
};

} // anonymous namespace

void connectContentsOfSlot(ValueFlowRuleContext& context, ClimbingSet objects, ValueFlowName name, ValueFlowNodeID target) { StepTarget { context, IteratorKind::Sync, target }.takeContentsOfSlot(objects, name); }

static constexpr ValueFlowCellName openedByName { ValueFlowNameKind::OpenedBy };

static constexpr ValueFlowCellName openedIteratorsName(IteratorKind kind) { return ValueFlowCellName { ValueFlowNameKind::OpenedIterators, static_cast<uint32_t>(kind) }; }

enum class Stepper : uint8_t {
    NamedBySeenCode,
    HolderOfEscapedIterator,
};

static constexpr uint32_t numberOfResumeModes = 3;

static constexpr ValueFlowCellName iteratorResultsName(ResumeMode mode, Stepper stepper = Stepper::NamedBySeenCode) { return ValueFlowCellName { ValueFlowNameKind::IteratorResults, static_cast<uint32_t>(mode) + numberOfResumeModes * static_cast<uint32_t>(stepper) }; }

static BuiltinName methodNameOf(ResumeMode mode)
{
    switch (mode) {
    case ResumeMode::Next:
        return BuiltinNames::next;
    case ResumeMode::Throw:
        return BuiltinNames::throwKeyword;
    case ResumeMode::Return:
        return BuiltinNames::returnKeyword;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ClimbingSet kinds(ValueFlowRuleContext& context, std::initializer_list<PrimitiveSet> kinds)
{
    Vector<ValueFlowObjectID, 4> members;
    for (PrimitiveSet kind : kinds)
        members.append(ValueFlowUniverse::primitive(kind));
    return ClimbingSet(context.maker().of(members.span()));
}

static ValueFlowNodeID nodeThatNobodyReads(ValueFlowRuleContext& context) { return context.written(ClimbingSet(), steppedValuesName); }

static BuiltinClassID classOfResult(const BuiltinTable& builtins, const BuiltinOverload& overload)
{
    for (const BuiltinTypePart& part : builtins.at(overload.result.type).parts) {
        if (part.kind == BuiltinTypePartKind::Instance)
            return part.builtinClass;
    }
    return { };
}

static bool goesOverContainer(BuiltinClassID iteratorClass) { return iteratorClass == BuiltinClasses::ArrayIterator || iteratorClass == BuiltinClasses::MapIterator || iteratorClass == BuiltinClasses::SetIterator || iteratorClass == BuiltinClasses::StringIterator; }

static std::optional<BuiltinIteration> iterationOverReceiverBy(ValueFlowRuleContext& context, BuiltinID builtin)
{
    const BuiltinTable& builtins = context.program().builtins();
    const ValueFlowBuiltinTables& tables = context.universe().builtinTables();
    std::optional<BuiltinIteration> iteration;
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
        if (overload.mode != BuiltinCallMode::Call || !overload.result.iteratedOperand || !overload.result.iteratedOperand->isReceiver() || !goesOverContainer(classOfResult(builtins, overload)))
            continue;
        if (!iteration)
            iteration = BuiltinIteration { classOfResult(builtins, overload), overload.result.iterationKind, ValueSets::nothing };
        RELEASE_ASSERT(iteration->iteratorClass == classOfResult(builtins, overload) && iteration->kind == overload.result.iterationKind);
        bool takesAnything = !overload.receiver || !overload.receiver->accepted;
        iteration->containers = takesAnything ? ValueSets::anything : context.maker().unite(iteration->containers, tables.maskOf(overload.receiver->accepted));
    }
    return iteration;
}

static void takeKeys(ValueFlowRuleContext& context, ClimbingSet containers, BuiltinClassID iteratorClass, const StepTarget& target)
{
    if (iteratorClass == BuiltinClasses::ArrayIterator)
        target.take(kinds(context, { PrimitiveSet::Integer, PrimitiveSet::WholeNumber }));
    else
        target.takeContentsOfSlot(containers, nameOf(iteratorClass == BuiltinClasses::MapIterator ? BuiltinSlots::MapKey : BuiltinSlots::SetElement));
}

static bool readsKeyOrValueOfPair(ValueFlowRuleContext& context, ClimbingSet containers, std::optional<uint32_t> indexInPattern)
{
    if (!indexInPattern || *indexInPattern >= 2)
        return false;
    const ValueFlowProgramNodes& nodes = context.nodes();
    ClimbingSet others = context.membersMissingFrom(nodes.ofHost(ValueFlowHostNode::PairsOfSteps), context.membersMissingFrom(nodes.ofHost(ValueFlowHostNode::PairsOfEntries), containers));
    return !holdsSomething(context, others) && !context.holdsAnyOf(containers, ValueFlowProgramNodes::escaped()) && !context.holdsAnyOf(containers, ValueFlowProgramNodes::lost())
        && !holdsSomething(context, context.markedAmong(containers, OptionSet<ObjectMutation>::all()));
}

static void takeValues(ValueFlowRuleContext& context, ClimbingSet containers, BuiltinClassID iteratorClass, const StepTarget& target, std::optional<uint32_t> indexInPattern = std::nullopt)
{
    const ValueFlowUniverse& universe = context.universe();
    if (iteratorClass == BuiltinClasses::ArrayIterator) {
        if (readsKeyOrValueOfPair(context, containers, indexInPattern)) {
            target.take(context.read(containers, *indexInPattern ? valueOfEntriesPair : keyOfEntriesPair));
            return;
        }
        ClimbingSet objects = context.restrict(containers, universe.objects());
        target.take(context.lookup(objects, elementsName));
        context.lookup(context.restrict(objects, ValueSets::complement(universe.sureInstancesOf(BuiltinClasses::Array))), nameOf(context, BuiltinNames::length));
        if (context.meet(containers, universe.strings()))
            target.take(kinds(context, { PrimitiveSet::String }));
    } else
        target.takeContentsOfSlot(containers, nameOf(iteratorClass == BuiltinClasses::MapIterator ? BuiltinSlots::MapValue : BuiltinSlots::SetElement));
}

static void takeContents(ValueFlowRuleContext& context, ClimbingSet containers, const BuiltinIteration& iteration, ValueFlowObjectID pair, const StepTarget& target, std::optional<uint32_t> indexInPattern = std::nullopt)
{
    const ValueFlowUniverse& universe = context.universe();
    BuiltinClassID iteratorClass = iteration.iteratorClass;
    if (iteratorClass == BuiltinClasses::StringIterator) {
        target.take(kinds(context, { PrimitiveSet::String }));
        return;
    }
    switch (iteration.kind) {
    case IterationKind::Keys:
        takeKeys(context, containers, iteratorClass, target);
        return;
    case IterationKind::Values:
        takeValues(context, containers, iteratorClass, target, indexInPattern);
        return;
    case IterationKind::Entries: {
        ClimbingSet pairs = only(context, pair);
        if (!universe.classOf(pair))
            context.add(context.written(pairs, prototypeLinkName), only(context, universe.objectOf(Builtins::ArrayPrototype)));
        for (ValueFlowName name : std::initializer_list<ValueFlowName> { elementsName, keyOfEntriesPair })
            takeKeys(context, containers, iteratorClass, StepTarget { context, IteratorKind::Sync, context.written(pairs, name) });
        for (ValueFlowName name : std::initializer_list<ValueFlowName> { elementsName, valueOfEntriesPair })
            takeValues(context, containers, iteratorClass, StepTarget { context, IteratorKind::Sync, context.written(pairs, name) });
        context.add(context.nodes().ofHost(ValueFlowHostNode::PairsOfSteps), pairs);
        target.take(pairs);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void useStepOf(ValueFlowRuleContext& context, BuiltinClassID iteratorClass)
{
    const BuiltinTable& builtins = context.program().builtins();
    BuiltinID prototype = iteratorClass ? builtins.at(iteratorClass).prototype : BuiltinID();
    if (BuiltinID next = prototype ? builtins.findOwnMember(builtins.canonicalID(prototype), BuiltinNames::next) : BuiltinID())
        context.use(builtins.canonicalID(next), BuiltinUse::Called);
}

static void openIterators(ValueFlowRuleContext& context, ClimbingSet iterables, IteratorKind kind, ValueFlowNodeID state)
{
    const ValueFlowUniverse& universe = context.universe();
    forEachPlaceholderAndOthers(context, iterables, [&](ClimbingSet receivers) {
        if (kind == IteratorKind::Async)
            callInProtocol(context, { .callees = context.valueOf(context.lookup(receivers, nameOf(context, BuiltinNames::asyncIteratorSymbol))), .thisValue = receivers, .result = state });

        ClimbingSet methods = context.valueOf(context.lookup(receivers, nameOf(context, BuiltinNames::iteratorSymbol)));
        ClimbingSet calledMethods = methods;
        context.forEachMember(context.restrict(methods, universe.builtinFunctions()), [&](ValueFlowObjectID method) {
            BuiltinID builtin = universe.builtinOf(method);
            std::optional<BuiltinIteration> iteration = iterationOverReceiverBy(context, builtin);
            if (!iteration)
                return;
            calledMethods = without(context, calledMethods, method);

            // The containers of one method may hold objects that find another method.
            ClimbingSet otherReceivers = context.restrict(known(context, receivers), ValueSets::complement(iteration->containers));
            if (holdsSomething(context, otherReceivers) && context.contains(context.valueOf(context.lookup(otherReceivers, nameOf(context, BuiltinNames::iteratorSymbol))), method))
                callInProtocol(context, { .callees = only(context, method), .thisValue = otherReceivers, .result = state });

            ClimbingSet containers = context.restrict(receivers, iteration->containers);
            if (!holdsSomething(context, containers))
                return;
            context.add(state, containers);
            context.add(context.written(containers, openedByName), only(context, method));
            context.use(builtin, BuiltinUse::Called);
            useStepOf(context, iteration->iteratorClass);
        });
        callInProtocol(context, { .callees = calledMethods, .thisValue = receivers, .result = state });
    });
}

static void stepIterators(ValueFlowRuleContext& context, ClimbingSet state, const IteratorStep& step, ValueFlowNodeID values)
{
    const ValueFlowUniverse& universe = context.universe();
    StepTarget target { context, step.kind, values };
    StepTarget unawaited { context, IteratorKind::Sync, values };
    if (mayBeUnknownObject(context, state)) {
        context.add(values, unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee));
        context.lookup(ClimbingSet(context.universe().unknownObjectAlone()), iteratorResultsName(step.mode));
        target.take(bunStoredInUnknownObject(context, elementsName));
    }

    ClimbingSet holders = context.restrict(without(context, state, ValueFlowUniverse::unknownObject()), universe.objects());
    target.takeContentsOfSlot(holders, steppedValuesName);
    if (step.takesReturnedValue)
        target.takeContentsOfSlot(holders, returnedValueName);
    if (step.kind == IteratorKind::Async) {
        ClimbingSet holdersNotSurelyWrapped = context.restrict(holders, ValueSets::complement(universe.sureInstancesOf(BuiltinClasses::Iterator)));
        unawaited.takeContentsOfSlot(holdersNotSurelyWrapped, steppedValuesName);
        if (step.takesReturnedValue)
            unawaited.takeContentsOfSlot(holdersNotSurelyWrapped, returnedValueName);
    }

    auto stepContainers = [&](ClimbingSet containers, ValueFlowNodeID openedBy) {
        ClimbingSet methods = context.valueOf(openedBy);
        if (mayBeUntrackedObject(context, methods)) {
            context.add(values, universe.anyPrimitive());
            context.add(values, universe.untrackedObjectAlone());
        }
        context.forEachMember(context.restrict(methods, universe.builtinFunctions()), [&](ValueFlowObjectID method) {
            std::optional<BuiltinIteration> iteration = iterationOverReceiverBy(context, universe.builtinOf(method));
            if (!iteration)
                return;
            takeContents(context, context.restrict(containers, iteration->containers), *iteration, step.pair, target, step.indexInPattern);
            if (step.takesReturnedValue)
                context.add(values, universe.undefinedValue());
        });
    };
    auto takeResultsOf = [&](ClimbingSet iterators) {
        ClimbingSet results = context.valueOf(context.lookup(iterators, iteratorResultsName(step.mode)));
        if (step.kind == IteratorKind::Async)
            results = context.valueOf(context.lookup(results, awaitedValueName));
        context.lookup(results, nameOf(context, BuiltinNames::done));
        ValueFlowNodeID handedOut = context.lookup(results, nameOf(context, BuiltinNames::value));
        target.take(handedOut);
        if (step.kind == IteratorKind::Async)
            unawaited.take(handedOut);
    };

    ClimbingSet containers = withoutPlaceholders(context, state);
    stepContainers(containers, context.read(containers, openedByName));
    takeResultsOf(known(context, state));
    if (!mayBeUntrackedObject(context, state))
        return;

    ClimbingSet untracked(universe.untrackedObjectAlone());
    stepContainers(untracked, context.read(ClimbingSet(universe.knownObjects()), openedByName));
    stepContainers(untracked, context.read(untracked, openedByName));
    takeResultsOf(untracked);
}

static void closeIterators(ValueFlowRuleContext& context, ClimbingSet state, IteratorKind kind)
{
    if (mayBeUnknownObject(context, state))
        context.lookup(ClimbingSet(context.universe().unknownObjectAlone()), iteratorResultsName(ResumeMode::Return));
    auto close = [&](ClimbingSet iterators) {
        ValueFlowNodeID results = context.lookup(iterators, iteratorResultsName(ResumeMode::Return));
        if (kind != IteratorKind::Async)
            return;
        ClimbingSet answers = context.valueOf(context.lookup(context.valueOf(results), awaitedValueName));
        context.lookup(answers, nameOf(context, BuiltinNames::done));
        context.lookup(context.valueOf(context.lookup(answers, nameOf(context, BuiltinNames::value))), awaitedValueName);
    };
    close(known(context, state));
    if (mayBeUntrackedObject(context, state))
        close(ClimbingSet(context.universe().untrackedObjectAlone()));
}

static void sendToIterators(ValueFlowRuleContext& context, ClimbingSet iterators, const ValueFlowOperand& sent)
{
    forEachNodeOfSlotWrittenThrough(context, context.restrict(iterators, context.universe().objects()), sentValuesName, [&](ValueFlowNodeID held) {
        context.connect(sent, held);
    });
}

static ValueFlowObjectID pairAllocatedAt(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    ValueFlowObjectID pair = context.universe().siteObject(statement);
    return pair ? pair : context.universe().resultOf(Builtins::MapIteratorPrototypeNext, 1);
}

void runIterationOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    const Value& operation = context.program().at(statement.function).at(statement.site);
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));

    bool isDescribedByRecords = isCodeThatRecordsDescribe(context.universe(), statement.function);
    auto iterated = [&](const ValueFlowOperand& operand, ValueFlowNodeID answer) {
        ClimbingSet held = context.valueOf(operand);
        if (!isDescribedByRecords || !mayBeUnknownObject(context, held))
            return held;
        if (answer)
            context.add(answer, unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee));
        return without(context, held, ValueFlowUniverse::unknownObject());
    };

    switch (statement.opcode) {
    case Opcode::IteratorOpen:
        openIterators(context, iterated(operands[0], statement.result), operation.iteratorKind(), statement.result ? statement.result : nodeThatNobodyReads(context));
        return;
    case Opcode::IteratorResume: {
        ClimbingSet state = iterated(operands[0], statement.result);
        ResumeMode mode = operation.resumeMode();
        ValueFlowNodeID values = statement.result ? statement.result : nodeThatNobodyReads(context);
        if (operation.operand(OperandIndex::IteratorResume::sent))
            sendToIterators(context, state, operands[1]);
        stepIterators(context, state, { operation.iteratorKind(), mode, operation.readsValueWhenDone() || mode != ResumeMode::Next, pairAllocatedAt(statement, context), operation.stepIndex() }, values);
        if (!operation.readsValueWhenDone() && operation.stepIndex() && !readsKeyOrValueOfPair(context, state, operation.stepIndex()))
            context.add(values, context.universe().undefinedValue());
        if (mode == ResumeMode::Return && operation.iteratorKind() == IteratorKind::Async)
            connectAwaitedValue(context, operands[1], values);
        else if (mode == ResumeMode::Return)
            context.connect(operands[1], values);
        if (mode == ResumeMode::Throw && statement.thrown)
            context.connect(operands[1], statement.thrown);
        return;
    }
    case Opcode::IteratorClose:
        closeIterators(context, iterated(operands[0], { }), operation.iteratorKind());
        return;
    case Opcode::AppendSpread: {
        ClimbingSet spread = context.valueOf(operands[1]);
        bool spreadsUnknownOperand = isDescribedByRecords && mayBeUnknownObject(context, spread);
        ClimbingSet state = context.valueOf(context.lookup(iterated(operands[1], { }), openedIteratorsName(IteratorKind::Sync)));
        forEachNodeOfSlotWrittenThrough(context, context.restrict(context.valueOf(operands[0]), context.universe().objects()), elementsName, [&](ValueFlowNodeID held) {
            if (spreadsUnknownOperand)
                context.add(held, unknownValue(context, ValueFlowUnknownSource::ResultOfUnknownCallee));
            stepIterators(context, state, { IteratorKind::Sync, ResumeMode::Next, false, pairAllocatedAt(statement, context) }, held);
        });
        return;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void runOpenedIterators(const ValueFlowStatement& statement, ValueFlowRuleContext& context) { openIterators(context, context.setOfKey(), static_cast<IteratorKind>(context.nameOfKey().payload), statement.result); }

void runIteratedValues(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    IteratorKind kind = static_cast<IteratorKind>(context.nameOfKey().payload);
    ClimbingSet state = context.valueOf(context.lookup(context.setOfKey(), openedIteratorsName(kind)));
    state = context.unite(state, context.restrict(context.setOfKey(), context.universe().objects()));
    stepIterators(context, state, { kind, ResumeMode::Next, false, context.universe().resultOf(Builtins::MapIteratorPrototypeNext, 1) }, statement.result);
    closeIterators(context, state, kind);
}

static std::optional<StepOfBuiltinIterator> stepOfBuiltinIterator(const BuiltinTable& builtins, BuiltinID builtin)
{
    BuiltinID holder = builtins.at(builtin).holder;
    BuiltinClassID iteratorClass = holder ? builtins.at(holder).instanceClass : BuiltinClassID();
    bool holdsWhatItHandsOut = goesOverContainer(iteratorClass) || iteratorClass == BuiltinClasses::Generator || iteratorClass == BuiltinClasses::AsyncGenerator;
    if (!holdsWhatItHandsOut || builtins.at(holder).kind != BuiltinKind::Prototype)
        return std::nullopt;
    for (ResumeMode mode : { ResumeMode::Next, ResumeMode::Throw, ResumeMode::Return }) {
        BuiltinID method = builtins.findOwnMember(holder, methodNameOf(mode));
        if (method && builtins.canonicalID(method) == builtins.canonicalID(builtin))
            return StepOfBuiltinIterator { iteratorClass, mode };
    }
    return std::nullopt;
}

void runIteratorResults(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ClimbingSet iterators = context.setOfKey();
    ResumeMode mode = static_cast<ResumeMode>(context.nameOfKey().payload % numberOfResumeModes);
    Stepper stepper = static_cast<Stepper>(context.nameOfKey().payload / numberOfResumeModes);

    ClimbingSet methods = context.valueOf(context.lookup(iterators, nameOf(context, methodNameOf(mode))));
    ClimbingSet calledMethods = methods;
    if (stepper == Stepper::HolderOfEscapedIterator)
        calledMethods = context.restrict(calledMethods, ValueSets::complement(universe.unknownObjectAlone()));
    context.forEachMember(context.restrict(methods, universe.builtinFunctions()), [&](ValueFlowObjectID method) {
        if (!stepOfBuiltinIterator(builtins, universe.builtinOf(method)))
            return;
        calledMethods = without(context, calledMethods, method);
        context.use(universe.builtinOf(method), BuiltinUse::Called);
    });
    ValueFlowNodeID sent = context.read(iterators, sentValuesName);
    callInProtocol(context, { .callees = calledMethods, .thisValue = iterators, .arguments = singleElementSpan(sent), .result = statement.result });

    if (mode == ResumeMode::Throw)
        context.lookup(iterators, iteratorResultsName(ResumeMode::Return, stepper));
}

void letNativeCodeStepIterators(ValueFlowRuleContext& context, ClimbingSet escaped)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueSetID builtinIterators = context.maker().unite(universe.sureInstancesOf(BuiltinClasses::Iterator), universe.objectsThatMayBeAnything());
    ClimbingSet iterators = context.restrict(escaped, context.maker().intersect(builtinIterators, universe.knownObjects()));
    for (ResumeMode mode : { ResumeMode::Next, ResumeMode::Throw, ResumeMode::Return })
        context.lookup(iterators, iteratorResultsName(mode, Stepper::HolderOfEscapedIterator));
}

void connectOutcomeOfStep(ValueFlowRuleContext& context, ClimbingSet iterators, ResumeMode mode, std::span<const ValueFlowOperand> sent, ValueFlowNodeID target)
{
    connectContentsOfSlot(context, iterators, steppedValuesName, target);
    connectContentsOfSlot(context, iterators, returnedValueName, target);
    context.add(target, context.universe().undefinedValue());
    if (mode != ResumeMode::Return)
        return;
    for (const ValueFlowOperand& operand : sent)
        context.connect(operand, target);
}

ClimbingSet makeIteratorResult(ValueFlowRuleContext& context, ValueFlowObjectID object, ClimbingSet iterators, ResumeMode mode, std::span<const ValueFlowOperand> sent)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet result = only(context, object);
    context.add(context.written(result, prototypeLinkName), only(context, universe.objectOf(Builtins::ObjectPrototype)));
    context.add(context.written(result, allocatedAsName), only(context, universe.builtinTables().objectStandingFor(BuiltinClasses::Object)));
    context.add(context.written(result, nameOf(context, BuiltinNames::done)), kinds(context, { PrimitiveSet::False, PrimitiveSet::True }));
    connectOutcomeOfStep(context, iterators, mode, sent, context.written(result, nameOf(context, BuiltinNames::value)));
    return result;
}

bool applyStepOfBuiltinIterator(BuiltinID builtin, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    if (call.kind != ValueFlowCallKind::Call && call.kind != ValueFlowCallKind::CallByNativeCode)
        return false;
    std::optional<StepOfBuiltinIterator> step = stepOfBuiltinIterator(context.program().builtins(), builtin);
    if (!step)
        return false;

    context.use(builtin, BuiltinUse::Called);
    ClimbingSet iterators = context.restrict(context.valueOf(call.thisValue), universe.objects());
    Vector<ValueFlowOperand, 2> sent;
    if (!call.arguments.empty())
        sent.append(call.arguments[0]);
    if (call.arguments.empty() || call.argumentsMayBeAbsent)
        sent.append(call.otherArguments);
    for (const ValueFlowOperand& operand : sent)
        sendToIterators(context, iterators, operand);

    if (call.thrown) {
        context.add(call.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
        if (step->mode == ResumeMode::Throw) {
            for (const ValueFlowOperand& operand : sent)
                context.connect(operand, call.thrown);
        }
    }

    bool answersPromise = step->iteratorClass == BuiltinClasses::AsyncGenerator;
    if (answersPromise) {
        for (ValueFlowOperand& operand : sent) {
            operand = { context.lookup(context.valueOf(operand), awaitedValueName), ValueSets::nothing, ValueSets::anything };
            if (step->mode == ResumeMode::Return)
                sendToIterators(context, iterators, operand);
        }
    }
    ValueFlowObjectID resultObject = call.resultObject ? call.resultObject : universe.resultOf(builtin, 0);
    ValueFlowObjectID contentsObject = call.contentsObject ? call.contentsObject : universe.resultOf(builtin, 1);
    ClimbingSet result = makeIteratorResult(context, answersPromise ? contentsObject : resultObject, iterators, step->mode, sent.span());
    if (answersPromise) {
        ClimbingSet promise = only(context, resultObject);
        context.add(context.written(promise, prototypeLinkName), only(context, universe.objectOf(Builtins::PromisePrototype)));
        context.add(context.written(promise, allocatedAsName), only(context, universe.builtinTables().objectStandingFor(BuiltinClasses::Promise)));
        context.add(context.written(promise, nameOf(BuiltinSlots::PromiseValue)), result);
        context.add(context.written(promise, nameOf(BuiltinSlots::PromiseReason)), unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
        result = promise;
    }
    if (call.result)
        context.add(call.result, result);
    return true;
}

void holdContentsIteratedByResult(BuiltinID builtin, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    if (call.kind != ValueFlowCallKind::Call && call.kind != ValueFlowCallKind::CallByNativeCode)
        return;
    std::optional<BuiltinIteration> iteration = iterationOverReceiverBy(context, builtin);
    if (!iteration)
        return;
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet iterator = only(context, call.resultObject ? call.resultObject : universe.resultOf(builtin, 0));
    StepTarget held { context, IteratorKind::Sync, context.written(iterator, steppedValuesName) };
    takeContents(context, context.restrict(context.valueOf(call.thisValue), iteration->containers), *iteration, call.contentsObject ? call.contentsObject : universe.resultOf(builtin, 1), held);
    context.add(context.written(iterator, returnedValueName), universe.undefinedValue());
}

void runStepOfLibrary(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet objects = context.restrict(context.valueOf(context.operandsOf(statement)[0]), universe.objects());
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
    if (!statement.result)
        return;

    context.add(statement.result, universe.undefinedValue());
    switch (statement.opcode) {
    case Opcode::StepArrayIterator:
        connectContentsOfSlot(context, objects, steppedValuesName, statement.result);
        return;
    case Opcode::MapNext:
        connectContentsOfSlot(context, objects, nameOf(BuiltinSlots::MapKey), statement.result);
        connectContentsOfSlot(context, objects, nameOf(BuiltinSlots::MapValue), statement.result);
        return;
    case Opcode::SetNext:
        connectContentsOfSlot(context, objects, nameOf(BuiltinSlots::SetElement), statement.result);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
