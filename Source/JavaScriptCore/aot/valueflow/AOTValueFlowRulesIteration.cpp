/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesProtocolsInternal.h"

#if ENABLE(AOT)

#include "AOTValueFlowBuiltins.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// The state of an iteration holds its iterators. A built-in method that makes an iterator over its receiver is stepped over where the language calls it: one iterator object for all those calls
// would pour together what all containers of the program hold. The state then holds the container, which stands for its iterator, and the container holds the method, which says how that iterator goes over it.

namespace {

struct BuiltinIteration {
    BuiltinClassID iteratorClass;
    IterationKind kind { IterationKind::Values };
    // What the method takes as its receiver.
    ValueSetID containers { ValueSets::nothing };
};

// Takes the values that steps hand out. An asynchronous iteration awaits whatever a synchronous iterator hands out.
class StepTarget {
public:
    StepTarget(ValueFlowRuleContext& context, IteratorKind kind, ValueFlowNodeID node)
        : m_context(context)
        , m_kind(kind)
        , m_node(node)
    {
    }

    void take(ValueFlowNodeID values) const
    {
        m_context.connect(m_kind == IteratorKind::Async ? m_context.lookup(m_context.valueOf(values), awaitedValueName) : values, m_node);
    }

    void take(ClimbingSet values) const
    {
        if (m_kind == IteratorKind::Async)
            m_context.connect(m_context.lookup(values, awaitedValueName), m_node);
        else
            m_context.add(m_node, values);
    }

    void takeContentsOfSlot(ClimbingSet objects, ValueFlowName name) const
    {
        forEachNodeOfSlotReadThrough(m_context, objects, name, [&](ValueFlowNodeID held) {
            take(held);
        });
        if (mayBeUnknownObject(m_context, objects))
            m_context.add(m_node, unknownValue(m_context));
    }

private:
    ValueFlowRuleContext& m_context;
    IteratorKind m_kind;
    ValueFlowNodeID m_node;
};

struct IteratorStep {
    IteratorKind kind { IteratorKind::Sync };
    ResumeMode mode { ResumeMode::Next };
    // The step also answers what an iterator that has finished returned.
    bool takesReturnedValue { false };
    // The array that a step allocates for a key and its value.
    ValueFlowObjectID pair;
};

} // anonymous namespace

static constexpr ValueFlowName openedByName = ValueFlowName::of(ValueFlowNameKind::OpenedBy);

static constexpr ValueFlowName openedIteratorsName(IteratorKind kind)
{
    return { ValueFlowNameKind::OpenedIterators, static_cast<uint32_t>(kind) };
}

static constexpr ValueFlowName iteratorResultsName(ResumeMode mode)
{
    return { ValueFlowNameKind::IteratorResults, static_cast<uint32_t>(mode) };
}

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

static ClimbingSet unknownObjectAlone(ValueFlowRuleContext& context)
{
    return ClimbingSet(context.universe().unknownObjectAlone());
}

// Where nobody reads what a protocol gives, its code runs all the same.
static ValueFlowNodeID nodeThatNobodyReads(ValueFlowRuleContext& context)
{
    return context.written(ClimbingSet(), steppedValuesName);
}

static BuiltinClassID classOfResult(const BuiltinTable& builtins, const BuiltinOverload& overload)
{
    for (const BuiltinTypePart& part : builtins.at(overload.result.type).parts) {
        if (part.kind == BuiltinTypePartKind::Instance)
            return part.builtinClass;
    }
    return { };
}

static bool goesOverContainer(BuiltinClassID iteratorClass)
{
    return iteratorClass == BuiltinClasses::ArrayIterator || iteratorClass == BuiltinClasses::MapIterator || iteratorClass == BuiltinClasses::SetIterator || iteratorClass == BuiltinClasses::StringIterator;
}

// For a built-in whose call makes an iterator of the engine over the receiver of the call.
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
    else if (iteratorClass == BuiltinClasses::MapIterator)
        target.takeContentsOfSlot(containers, nameOf(BuiltinSlots::MapKey));
    else
        target.takeContentsOfSlot(containers, nameOf(BuiltinSlots::SetElement));
}

static void takeValues(ValueFlowRuleContext& context, ClimbingSet containers, BuiltinClassID iteratorClass, const StepTarget& target)
{
    const ValueFlowUniverse& universe = context.universe();
    if (iteratorClass == BuiltinClasses::ArrayIterator) {
        ClimbingSet objects = context.restrict(containers, universe.objects());
        target.take(context.lookup(objects, elementsName));
        // Of what is no array the iterator reads `length` like any other property.
        context.lookup(context.restrict(objects, ValueSets::complement(universe.sureInstancesOf(BuiltinClasses::Array))), nameOf(context, BuiltinNames::length));
        if (context.meet(containers, universe.strings()))
            target.take(kinds(context, { PrimitiveSet::String }));
    } else if (iteratorClass == BuiltinClasses::MapIterator)
        target.takeContentsOfSlot(containers, nameOf(BuiltinSlots::MapValue));
    else
        target.takeContentsOfSlot(containers, nameOf(BuiltinSlots::SetElement));
}

// What the steps of a built-in iterator over the containers hand out.
static void takeContents(ValueFlowRuleContext& context, ClimbingSet containers, const BuiltinIteration& iteration, ValueFlowObjectID pair, const StepTarget& target)
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
        takeValues(context, containers, iteratorClass, target);
        return;
    case IterationKind::Entries: {
        ClimbingSet pairs = pair ? only(context, pair) : unknownObjectAlone(context);
        StepTarget contents(context, IteratorKind::Sync, pair ? context.written(pairs, elementsName) : ValueFlowProgramNodes::escaped());
        if (pair && !universe.classOf(pair))
            context.add(context.written(pairs, prototypeLinkName), only(context, universe.objectOf(Builtins::ArrayPrototype)));
        takeKeys(context, containers, iteratorClass, contents);
        takeValues(context, containers, iteratorClass, contents);
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

static void openIteratorsOf(ValueFlowRuleContext& context, ClimbingSet receivers, IteratorKind kind, ValueFlowNodeID state)
{
    const ValueFlowUniverse& universe = context.universe();

    // Where there is no method under @@asyncIterator the iteration goes over what the one under @@iterator answers.
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
        ClimbingSet containers = context.restrict(receivers, iteration->containers);
        if (!holdsSomething(context, containers))
            return;
        context.add(state, containers);
        context.add(context.written(containers, openedByName), only(context, method));
        context.use(builtin, BuiltinUse::Called);
        useStepOf(context, iteration->iteratorClass);
    });
    callInProtocol(context, { .callees = calledMethods, .thisValue = receivers, .result = state });
}

// The unknown object stands for built-in containers and iterators too, so it finds the members of the table. It is looked at by itself: a method nobody knows would get every other receiver as `this`.
static void forUnknownObjectAndOthers(ValueFlowRuleContext& context, ClimbingSet receivers, NOESCAPE const Invocable<void(ClimbingSet)> auto& functor)
{
    if (mayBeUnknownObject(context, receivers))
        functor(unknownObjectAlone(context));
    functor(without(context, receivers, ValueFlowUniverse::unknownObject()));
}

static void openIterators(ValueFlowRuleContext& context, ClimbingSet iterables, IteratorKind kind, ValueFlowNodeID state)
{
    forUnknownObjectAndOthers(context, iterables, [&](ClimbingSet receivers) {
        openIteratorsOf(context, receivers, kind, state);
    });
}

static void stepIterators(ValueFlowRuleContext& context, ClimbingSet state, const IteratorStep& step, ValueFlowNodeID values)
{
    const ValueFlowUniverse& universe = context.universe();
    StepTarget target(context, step.kind, values);
    if (mayBeUnknownObject(context, state)) {
        context.add(values, unknownValue(context));
        context.lookup(unknownObjectAlone(context), iteratorResultsName(step.mode));
    }

    ClimbingSet iterators = known(context, state);
    target.takeContentsOfSlot(iterators, steppedValuesName);
    if (step.takesReturnedValue)
        target.takeContentsOfSlot(iterators, returnedValueName);

    ClimbingSet containers = without(context, state, ValueFlowUniverse::unknownObject());
    context.forEachMember(context.valueOf(context.read(containers, openedByName)), [&](ValueFlowObjectID method) {
        std::optional<BuiltinIteration> iteration = iterationOverReceiverBy(context, universe.builtinOf(method));
        if (!iteration)
            return;
        takeContents(context, context.restrict(containers, iteration->containers), *iteration, step.pair, target);
        if (step.takesReturnedValue)
            context.add(values, universe.undefinedValue());
    });

    // The methods of an asynchronous iterator answer promises of iterator results.
    ClimbingSet results = context.valueOf(context.lookup(iterators, iteratorResultsName(step.mode)));
    if (step.kind == IteratorKind::Async)
        results = context.valueOf(context.lookup(results, awaitedValueName));
    context.lookup(results, nameOf(context, BuiltinNames::done));
    target.take(context.lookup(results, nameOf(context, BuiltinNames::value)));
}

static void closeIterators(ValueFlowRuleContext& context, ClimbingSet state, IteratorKind kind)
{
    if (mayBeUnknownObject(context, state))
        context.lookup(unknownObjectAlone(context), iteratorResultsName(ResumeMode::Return));
    ValueFlowNodeID results = context.lookup(known(context, state), iteratorResultsName(ResumeMode::Return));
    if (kind == IteratorKind::Async)
        context.lookup(context.valueOf(results), awaitedValueName);
}

static void sendToIterators(ValueFlowRuleContext& context, ClimbingSet iterators, const ValueFlowOperand& sent)
{
    forEachNodeOfSlotWrittenThrough(context, context.restrict(iterators, context.universe().objects()), sentValuesName, [&](ValueFlowNodeID held) {
        context.connect(sent, held);
    });
}

static ValueFlowObjectID pairAllocatedAt(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    if (ValueFlowObjectID pair = universe.siteObject(statement))
        return pair;
    return universe.resultOf(Builtins::MapIteratorPrototypeNext, 1);
}

void runIterationOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    const Value& operation = context.program().at(statement.function).at(statement.site);
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context));

    switch (statement.opcode) {
    case Opcode::IteratorOpen:
        openIterators(context, context.valueOf(operands[0]), operation.iteratorKind(), statement.result ? statement.result : nodeThatNobodyReads(context));
        return;
    case Opcode::IteratorResume: {
        ClimbingSet state = context.valueOf(operands[0]);
        ResumeMode mode = operation.resumeMode();
        ValueFlowNodeID values = statement.result ? statement.result : nodeThatNobodyReads(context);
        sendToIterators(context, state, operands[1]);
        stepIterators(context, state, { operation.iteratorKind(), mode, operation.readsValueWhenDone() || mode != ResumeMode::Next, pairAllocatedAt(statement, context) }, values);
        if (!operation.readsValueWhenDone())
            context.add(values, context.universe().undefinedValue());
        // An iterator without the method: the value comes back, and the exception goes on.
        if (mode == ResumeMode::Return && operation.iteratorKind() == IteratorKind::Async)
            context.connect(context.lookup(context.valueOf(operands[1]), awaitedValueName), values);
        else if (mode == ResumeMode::Return)
            context.connect(operands[1], values);
        if (mode == ResumeMode::Throw && statement.thrown)
            context.connect(operands[1], statement.thrown);
        return;
    }
    case Opcode::IteratorClose:
        closeIterators(context, context.valueOf(operands[0]), operation.iteratorKind());
        return;
    case Opcode::AppendSpread: {
        ClimbingSet state = context.valueOf(context.lookup(context.valueOf(operands[1]), openedIteratorsName(IteratorKind::Sync)));
        forEachNodeOfSlotWrittenThrough(context, context.restrict(context.valueOf(operands[0]), context.universe().objects()), elementsName, [&](ValueFlowNodeID held) {
            stepIterators(context, state, { IteratorKind::Sync, ResumeMode::Next, false, pairAllocatedAt(statement, context) }, held);
        });
        return;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void runOpenedIterators(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    openIterators(context, context.setOfKey(), static_cast<IteratorKind>(context.nameOfKey().payload), statement.result);
}

// What an iteration over any member of the set hands out, from its first step to its end.
void runIteratedValues(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    IteratorKind kind = static_cast<IteratorKind>(context.nameOfKey().payload);
    ClimbingSet state = context.valueOf(context.lookup(context.setOfKey(), openedIteratorsName(kind)));
    stepIterators(context, state, { kind, ResumeMode::Next, false, context.universe().resultOf(Builtins::MapIteratorPrototypeNext, 1) }, statement.result);
    closeIterators(context, state, kind);
}

namespace {

struct StepOfBuiltinIterator {
    BuiltinClassID iteratorClass;
    ResumeMode mode { ResumeMode::Next };
};

} // anonymous namespace

// For next, return and throw of the built-in iterators and generators that hold what they hand out.
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

// What the methods of the iterators in the set answer.
void runIteratorResults(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ClimbingSet iterators = context.setOfKey();
    ResumeMode mode = static_cast<ResumeMode>(context.nameOfKey().payload);

    ClimbingSet methods = context.valueOf(context.lookup(iterators, nameOf(context, methodNameOf(mode))));
    ClimbingSet calledMethods = methods;
    context.forEachMember(context.restrict(methods, universe.builtinFunctions()), [&](ValueFlowObjectID method) {
        // Whoever steps such an iterator reads what it holds. The built-in runs all the same.
        if (!stepOfBuiltinIterator(builtins, universe.builtinOf(method)))
            return;
        calledMethods = without(context, calledMethods, method);
        context.use(universe.builtinOf(method), BuiltinUse::Called);
    });
    ValueFlowNodeID sent = context.read(iterators, sentValuesName);
    callInProtocol(context, { .callees = calledMethods, .thisValue = iterators, .arguments = singleElementSpan(sent), .result = statement.result });

    // An iterator without a method under `throw` is closed.
    if (mode == ResumeMode::Throw)
        context.lookup(iterators, iteratorResultsName(ResumeMode::Return));
}

void letNativeCodeStepIterators(ValueFlowRuleContext& context, ClimbingSet escaped)
{
    const ValueFlowUniverse& universe = context.universe();
    // A built-in iterator is what a built-in has allocated, a generator what a generator function answers.
    ValueSetID builtinIterators = context.maker().unite(universe.sureInstancesOf(BuiltinClasses::Iterator), universe.objectsThatMayBeAnything());
    ClimbingSet iterators = context.restrict(escaped, context.maker().intersect(builtinIterators, universe.knownObjects()));
    for (ResumeMode mode : { ResumeMode::Next, ResumeMode::Throw, ResumeMode::Return })
        context.lookup(iterators, iteratorResultsName(mode));
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
    if (!object) {
        connectOutcomeOfStep(context, iterators, mode, sent, ValueFlowProgramNodes::escaped());
        return unknownObjectAlone(context);
    }
    ClimbingSet result = only(context, object);
    context.add(context.written(result, prototypeLinkName), only(context, universe.objectOf(Builtins::ObjectPrototype)));
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
        context.add(call.thrown, unknownValue(context));
        if (step->mode == ResumeMode::Throw) {
            for (const ValueFlowOperand& operand : sent)
                context.connect(operand, call.thrown);
        }
    }

    // An async generator awaits the value that it is told to return.
    bool answersPromise = step->iteratorClass == BuiltinClasses::AsyncGenerator;
    if (answersPromise) {
        for (ValueFlowOperand& operand : sent)
            operand = { context.lookup(context.valueOf(operand), awaitedValueName), ValueSets::nothing, ValueSets::anything };
    }
    bool isFollowed = call.resultObject && (!answersPromise || call.contentsObject);
    ClimbingSet result = makeIteratorResult(context, !isFollowed ? ValueFlowObjectID() : answersPromise ? call.contentsObject : call.resultObject, iterators, step->mode, sent.span());
    if (answersPromise && isFollowed) {
        ClimbingSet promise = only(context, call.resultObject);
        context.add(context.written(promise, prototypeLinkName), only(context, universe.objectOf(Builtins::PromisePrototype)));
        context.add(context.written(promise, nameOf(BuiltinSlots::PromiseValue)), result);
        context.add(context.written(promise, nameOf(BuiltinSlots::PromiseReason)), unknownValue(context));
        result = promise;
    }
    if (call.result)
        context.add(call.result, result);
    return true;
}

void holdContentsIteratedByResult(BuiltinID builtin, const ValueFlowBuiltinCall& call, ValueFlowRuleContext& context)
{
    if ((call.kind != ValueFlowCallKind::Call && call.kind != ValueFlowCallKind::CallByNativeCode) || !call.resultObject)
        return;
    std::optional<BuiltinIteration> iteration = iterationOverReceiverBy(context, builtin);
    if (!iteration)
        return;
    ClimbingSet iterator = only(context, call.resultObject);
    StepTarget held(context, IteratorKind::Sync, context.written(iterator, steppedValuesName));
    takeContents(context, context.restrict(context.valueOf(call.thisValue), iteration->containers), *iteration, call.contentsObject, held);
    context.add(context.written(iterator, returnedValueName), context.universe().undefinedValue());
}

// StepArrayIterator, MapNext, SetNext: what the functions of the library step built-in iterators and collections with.
void runStepOfLibrary(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet objects = context.restrict(context.valueOf(context.operandsOf(statement)[0]), universe.objects());
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context));
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
