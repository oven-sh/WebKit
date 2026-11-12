/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningRules.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTPrimitives.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTWalkCoroutinePromises.h"
#include "AOTWalkCoroutineCalls.h"
#include "AOTWalkCoroutinePromises.h"
#include "AOTWalkLoopsOverGenerators.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

bool walkRampOfCoroutine(Walker&);
Reference addDriverParameterOfCoroutine(Walker&, BlockID entry);
Reference convertReturnedValueOfCoroutine(Walker&, Reference returned, Origin);
Reference buildAwaitBySavingFrame(Walker&, Reference awaited, std::span<const Reference> held, Origin);
Reference buildAwaitOfElement(Walker&, Reference awaited, std::span<const Reference> held, Origin);
Reference buildAwaitOfResolution(Walker&, Reference resolution, std::span<const Reference> held, Origin);
bool canWaitForStepOfAsyncGenerator(Walker&);
Reference buildStepOfAsyncGenerator(Walker&, Reference generator, Reference sent, std::span<const Reference> held, BuiltinID awaitedCallee, Reference goesByDriver, Origin);
bool engineAnswersAboveGenerators(const Walker&);
OperatorForm operatorFormFor(Walker&, const Draft&);

static constexpr OpcodeSet operationsOfFamilyCoroutine { Opcode::Await, Opcode::Yield, Opcode::InitialYield, Opcode::ReadScope };

namespace {

struct SuspendingBodyOfCoroutine {
    CoroutineKind kind;
    Reference driver;
};

} // anonymous namespace

static Draft draftForCoroutine(Walker& walker, Opcode opcode, Origin origin)
{
    origin.isTailCall = false;
    Draft draft(opcode, origin);
    walker.chooseConservativeFields(draft);
    return draft;
}

static Reference boxedUndefinedForCoroutine(Walker& walker)
{
    Draft undefined(Opcode::Undefined);
    undefined.representation = Representation::JSValue;
    return walker.constant(WTF::move(undefined));
}

static bool endBlockOfCoroutine(Walker& walker, TerminatorKind kind, Reference value, Origin origin)
{
    if (!value)
        return false;
    origin.isTailCall = false;
    if (kind == TerminatorKind::Throw)
        walker.endWithThrow(value, origin);
    else
        walker.endWithReturn(singleElementSpan(value), origin);
    return !walker.hasFailed();
}

static std::optional<SuspendingBodyOfCoroutine> findSuspendingBodyOfCoroutine(Walker& walker)
{
    CodeUnit unit = walker.unit();
    if (unit.role != CodeRole::CoroutineBody || walker.currentFunction() != unit.function)
        return std::nullopt;
    const Function& function = walker.program().at(unit.function);
    if (!function.isCoroutine() || walker.plan().conventions().of(unit.function).coroutineNeverSuspends)
        return std::nullopt;

    const Code& code = walker.code();
    if (code.entries().empty())
        return std::nullopt;
    std::span<const ValueID> parameters = code.at(code.entries()[0].block).parameters();
    if (parameters.size() != function.numberOfEntryParameters() + 1)
        return std::nullopt;

    CoroutineKind kind = CoroutineKind::AsyncFunction;
    if (function.has(FunctionFlags::IsGenerator))
        kind = function.has(FunctionFlags::IsAsync) ? CoroutineKind::AsyncGenerator : CoroutineKind::Generator;
    return SuspendingBodyOfCoroutine { kind, Reference::value(parameters.back()) };
}

static Reference buildSuspensionOfCoroutine(Walker& walker, Draft&& suspension)
{
    Reference resumption = walker.build(WTF::move(suspension));
    walker.state().invalidateHeap();
    return resumption;
}

std::optional<CoroutineKind> kindOfSuspendingBody(Walker& walker, Reference& driver)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    driver = body ? body->driver : Reference();
    return body ? std::optional(body->kind) : std::nullopt;
}

Reference buildSuspensionInCoroutine(Walker& walker, Draft&& suspension)
{
    return buildSuspensionOfCoroutine(walker, WTF::move(suspension));
}

Vector<Reference, 8> heldForCells(Walker& walker, std::span<const Reference> held)
{
    constexpr uint16_t unseen = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString | Primitive::BigInt | Primitive::Absent | Primitive::Unspecified;
    Vector<Reference, 8> kept;
    for (Reference reference : held) {
        TypeID type = walker.typeOf(reference);
        if (!type || !walker.types().at(type).hasOnlyPrimitives(unseen))
            kept.append(reference);
    }
    return kept;
}

static Reference buildAwaitInCoroutine(Walker& walker, Reference awaited, std::span<const Reference> held, Origin origin, bool readsThenInFrame)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    if (!body || body->kind == CoroutineKind::Generator) {
        walker.fail("an await stands where no frame of an async function can be saved"_s);
        return { };
    }

    if (Reference given = buildAwaitOfWhatHasNoThen(walker, awaited, held, origin))
        return given;

    Draft await = draftForCoroutine(walker, Opcode::AwaitBySavingFrame, origin);
    await.setAwaitedOperand(awaited);
    await.setPromiseOperand(body->driver);
    await.setHeldOperands(heldForCells(walker, held).span());
    await.setReadsThenInFrame(readsThenInFrame);
    return buildSuspensionOfCoroutine(walker, WTF::move(await));
}

Reference buildAwaitBySavingFrame(Walker& walker, Reference awaited, std::span<const Reference> held, Origin origin)
{
    return buildAwaitInCoroutine(walker, awaited, held, origin, false);
}

Reference buildAwaitOfElement(Walker& walker, Reference element, std::span<const Reference> held, Origin origin)
{
    return buildAwaitInCoroutine(walker, element, held, origin, true);
}

Reference buildAwaitOfResolution(Walker& walker, Reference resolution, std::span<const Reference> held, Origin origin)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    if (!body || body->kind != CoroutineKind::AsyncFunction) {
        walker.fail("the resolution of a promise is awaited where no frame of an async function can be saved"_s);
        return { };
    }
    Draft await = draftForCoroutine(walker, Opcode::AwaitBySavingFrame, origin);
    await.setAwaitedOperand(resolution);
    await.setPromiseOperand(body->driver);
    await.setHeldOperands(heldForCells(walker, held).span());
    await.setAwaitsResolution(true);
    return buildSuspensionOfCoroutine(walker, WTF::move(await));
}

bool canWaitForStepOfAsyncGenerator(Walker& walker)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    return body && body->kind != CoroutineKind::Generator;
}

Reference buildStepOfAsyncGenerator(Walker& walker, Reference generator, Reference sent, std::span<const Reference> held, BuiltinID awaitedCallee, Reference goesByDriver, Origin origin)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    if (!body || body->kind == CoroutineKind::Generator) {
        walker.fail("a step of an async generator stands where no frame of an async function can be saved"_s);
        return { };
    }

    Draft step = draftForCoroutine(walker, Opcode::StepAsyncGenerator, origin);
    step.setGeneratorOperand(generator);
    step.setValueOperand(sent);
    step.setDriverOperand(body->driver);
    if (goesByDriver)
        step.setGoesByDriverOperand(goesByDriver);
    step.setHeldOperands(heldForCells(walker, held).span());
    step.setCalledBuiltin(awaitedCallee);
    return buildSuspensionOfCoroutine(walker, WTF::move(step));
}

static Reference buildYieldBySavingFrameOfCoroutine(Walker& walker, const SuspendingBodyOfCoroutine& body, Reference yielded, std::span<const Reference> held, Origin origin)
{
    Draft yield = draftForCoroutine(walker, Opcode::YieldBySavingFrame, origin);
    yield.setGeneratorOperand(body.driver);
    yield.setValueOperand(yielded);
    yield.setHeldOperands(heldForCells(walker, held).span());
    return buildSuspensionOfCoroutine(walker, WTF::move(yield));
}

static std::optional<SuspendingBodyOfCoroutine> findYieldingBodyOfCoroutine(Walker& walker)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    if (body && body->kind != CoroutineKind::AsyncFunction)
        return body;
    walker.fail("a yield stands where no frame of a generator can be saved"_s);
    return std::nullopt;
}

static Reference buildResultOfYieldOfCoroutine(Walker& walker, Reference resumption, YieldResult result, Origin origin)
{
    Draft read = draftForCoroutine(walker, Opcode::ResultAt, origin);
    read.setResultsOperand(resumption);
    read.setIndex(static_cast<uint32_t>(result));
    return walker.build(WTF::move(read));
}

static void branchOnResumeModeOfCoroutine(Walker& walker, Reference mode, ResumeMode modeThatLeaves, TerminatorKind kind, Reference sent, Origin origin)
{
    Draft asked(Opcode::Integer);
    asked.setInteger(static_cast<int64_t>(modeThatLeaves));
    Draft test = draftForCoroutine(walker, Opcode::StrictEq, origin);
    test.setLeftOperand(mode);
    test.setRightOperand(walker.constant(WTF::move(asked)));
    test.setForm(operatorFormFor(walker, test));
    Reference isResumedToLeave = walker.build(WTF::move(test));

    BlockID leaves = walker.addBlock(BlockFrequency::Rare);
    BlockID goesOn = walker.addBlock();
    walker.branch(isResumedToLeave, leaves, goesOn);
    walker.continueIn(leaves);
    endBlockOfCoroutine(walker, kind, sent, origin);
    walker.continueIn(goesOn);
}

static bool generatorGoesIntoIterationAloneOfCoroutine(const BodyIndex& body, const LinkedCall& call)
{
    ValueID site = call.site;
    if (body.isParameter(site))
        return false;
    if (call.asksForIterator)
        return body.opcodeOf(site) == Opcode::AppendSpread || (body.opcodeOf(site) == Opcode::IteratorOpen && body.value(site).as<Opcode::IteratorOpen>().iteratorKind() == IteratorKind::Sync);
    if (body.opcodeOf(site) != Opcode::Call)
        return false;
    if (body.isUnused(site))
        return true;
    ValueID taker = body.onlyUsingValue(site);
    if (!taker)
        return false;
    BodyValue taking = body.value(taker);
    if (taking.opcode() == Opcode::AppendSpread)
        return taking.as<Opcode::AppendSpread>().iterableOperand() == site && taking.as<Opcode::AppendSpread>().arrayOperand() != site;
    return taking.opcode() == Opcode::IteratorOpen && taking.as<Opcode::IteratorOpen>().iteratorKind() == IteratorKind::Sync;
}

namespace {

class GeneratorDriver {
public:
    GeneratorDriver(const Walker& walker, FunctionID helper)
        : m_walker(walker)
        , m_helper(helper)
    {
    }

    bool onlyDrives(unsigned entryParameter);

private:
    struct EntryParameterOfFunction {
        FunctionID function;
        unsigned number;
    };

    const BodyIndex* findBody(FunctionID) const;
    bool isOnlyDriven(const BodyIndex&, ValueID);
    bool isUseOfDriven(const BodyIndex&, ValueID driven, const BodyValue& user);
    bool isResultOfApply(const BodyIndex&, ValueID) const;
    bool accessesScopeOnlyForDriving(FunctionID);
    void addFunction(FunctionID function)
    {
        if (!m_functions.contains(function))
            m_functions.append(function);
    }

    const Walker& m_walker;
    const FunctionID m_helper;
    Vector<FunctionID, 8> m_functions;
    Vector<EntryParameterOfFunction, 4> m_parameters;
    LayoutID m_scope;
    FieldIndex m_field { 0 };
};

} // anonymous namespace

template<typename Functor> static bool everyUseIs(const BodyIndex& index, ValueID value, const Functor& isAllowed)
{
    bool isEveryUseAllowed = true;
    for (uint32_t number = 0; isEveryUseAllowed && number < index.numberOfBlocks(); ++number) {
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            bool isUse = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                isUse |= operand == value;
            });
            if (isUse && !isAllowed(operation)) {
                isEveryUseAllowed = false;
                break;
            }
        }
        BodyTerminator terminator = index.terminator(BlockID(number));
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            isEveryUseAllowed &= operand != value;
        });
        for (BodyTarget target : terminator.targets())
            isEveryUseAllowed &= !std::ranges::contains(target.arguments(), value);
    }
    return isEveryUseAllowed;
}

const BodyIndex* GeneratorDriver::findBody(FunctionID function) const
{
    static constexpr OpcodeSet readsMoreThanNamedVariables { Opcode::ScopeHasBinding, Opcode::DirectEval, Opcode::NewClass };
    if (m_walker.facts().summaries().at(function).has(SummaryTrait::HasDirectEval))
        return nullptr;
    const BodyIndex* body = m_walker.facts().bodies().find(function);
    return body && !body->holdsAnyOf(readsMoreThanNamedVariables) && !body->holdsAnyOf(opcodesOfMappedArguments) ? body : nullptr;
}

bool GeneratorDriver::isUseOfDriven(const BodyIndex& index, ValueID driven, const BodyValue& user)
{
    if (opcodesThatStateTypes.contains(user.opcode()))
        return isOnlyDriven(index, user.id());
    auto isDriven = [&](ValueID value) {
        return value && index.stripCasts(value) == index.stripCasts(driven);
    };
    switch (user.opcode()) {
    case Opcode::PropGet: {
        const Program& program = m_walker.program();
        bool applies = user.key() == program.keyOf(BuiltinNames::apply);
        if (user.isFreeName() || (!applies && user.key() != program.keyOf(BuiltinNames::next) && user.key() != program.keyOf(BuiltinNames::throwKeyword) && user.key() != program.keyOf(BuiltinNames::returnKeyword)))
            return false;
        ValueID call = index.onlyUsingValue(user.id());
        return call && index.opcodeOf(call) == Opcode::Call && index.value(call).calleeOperand() == user.id() && isDriven(index.value(call).thisValueOperand()) && (!applies || isOnlyDriven(index, call));
    }
    case Opcode::Call: {
        ValueID callee = user.calleeOperand();
        return callee != driven && isDriven(user.thisValueOperand()) && !std::ranges::contains(user.as<Opcode::Call>().argumentsOperands(), driven)
            && !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::PropGet && isDriven(index.value(callee).objectOperand());
    }
    case Opcode::PutEnvironmentField:
        return m_scope && user.layout() == m_scope && user.field() == m_field && user.valueOperand() == driven && isResultOfApply(index, driven);
    case Opcode::NewClosure:
    case Opcode::CallDirect: {
        FunctionID function = user.opcode() == Opcode::NewClosure ? user.as<Opcode::NewClosure>().function() : user.as<Opcode::CallDirect>().function();
        unsigned numberOfOperands = 0;
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (operand == driven)
                m_parameters.append(EntryParameterOfFunction { function, operandIndex });
            ++numberOfOperands;
        });
        const Function& entered = m_walker.program().at(function);
        return numberOfOperands == (user.opcode() == Opcode::NewClosure ? entered.captures.size() : entered.numberOfEntryParameters());
    }
    case Opcode::NewObject: {
        const Program& program = m_walker.program();
        if (m_scope || !user.layout() || program.at(user.layout()).kind != LayoutKind::Environment)
            return false;
        std::span<const ValueID> values = user.as<Opcode::NewObject>().valuesOperands();
        size_t place = std::ranges::find(values, driven) - values.begin();
        if (place == values.size() || std::ranges::count(values, driven) != 1)
            return false;
        m_scope = user.layout();
        m_field = user.as<Opcode::NewObject>().fields()[place];
        return true;
    }
    default:
        return false;
    }
}

bool GeneratorDriver::isResultOfApply(const BodyIndex& index, ValueID value) const
{
    ValueID call = index.stripCasts(value);
    if (index.isParameter(call) || index.opcodeOf(call) != Opcode::Call)
        return false;
    ValueID callee = index.value(call).calleeOperand();
    return !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::PropGet && index.value(callee).key() == m_walker.program().keyOf(BuiltinNames::apply);
}

bool GeneratorDriver::isOnlyDriven(const BodyIndex& index, ValueID value)
{
    return everyUseIs(index, value, [&](const BodyValue& user) {
        return isUseOfDriven(index, value, user);
    });
}

bool GeneratorDriver::accessesScopeOnlyForDriving(FunctionID function)
{
    const Program& program = m_walker.program();
    const BodyIndex* index = findBody(function);
    if (!index)
        return false;
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        for (BodyValue operation : index->block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::NewClosure)
                addFunction(operation.as<Opcode::NewClosure>().function());
            if (operation.opcode() == Opcode::CallDirect) {
                const Function& callee = program.at(operation.as<Opcode::CallDirect>().function());
                const Function& helper = program.at(m_helper);
                if (callee.module == helper.module && callee.position >= helper.sourceRange.start && callee.position < helper.sourceRange.end)
                    addFunction(operation.as<Opcode::CallDirect>().function());
            }
            if (!m_scope || (operation.opcode() != Opcode::GetEnvironmentField && operation.opcode() != Opcode::PutEnvironmentField) || operation.layout() != m_scope || operation.field() != m_field)
                continue;
            if (operation.opcode() == Opcode::GetEnvironmentField ? !isOnlyDriven(*index, operation.id()) : !isResultOfApply(*index, operation.valueOperand()))
                return false;
        }
    }
    return true;
}

bool GeneratorDriver::onlyDrives(unsigned entryParameter)
{
    const Function& helper = m_walker.program().at(m_helper);
    if (helper.isCoroutine() || helper.has(FunctionFlags::HasHiddenRest))
        return false;
    m_parameters.append(EntryParameterOfFunction { m_helper, entryParameter });
    for (size_t place = 0; place < m_parameters.size(); ++place) {
        auto [function, number] = m_parameters[place];
        bool isRepeated = std::ranges::any_of(m_parameters.span().first(place), [&](const EntryParameterOfFunction& earlier) {
            return earlier.function == function && earlier.number == number;
        });
        if (isRepeated)
            continue;
        const BodyIndex* index = findBody(function);
        if (!index || index->numberOfIncomingEdges(entryBlock) || index->holdsAnyOf(OpcodeSet { Opcode::CreateUnmappedArguments }) || !isOnlyDriven(*index, index->block(entryBlock).parameter(number)))
            return false;
        addFunction(function);
    }
    for (size_t place = 0; place < m_functions.size(); ++place) {
        if (!accessesScopeOnlyForDriving(m_functions[place]))
            return false;
    }
    return true;
}

static bool helpersDriveGeneratorsOf(const Walker& walker, FunctionID generatorFunction)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    std::optional<BuiltinID> apply = findMemberAboveForIteration(walker, table.at(BuiltinClasses::GeneratorFunction).prototype, BuiltinNames::apply);
    if (!apply || !*apply || table.canonicalID(*apply) != table.canonicalID(Builtins::FunctionPrototypeApply))
        return false;
    const BodyIndex* own = walker.facts().bodies().find(generatorFunction);
    std::optional<unsigned> calleeParameter = program.at(generatorFunction).calleeParameterIndex();
    if (!own || !calleeParameter || !own->isUnused(own->block(entryBlock).parameter(*calleeParameter)))
        return false;
    const LinkTables& linked = walker.facts().linked();
    bool isMade = false;
    for (FunctionID referrer : linked.referrersOf(generatorFunction)) {
        const BodyIndex* index = linked.isGroup(referrer) || referrer == generatorFunction ? nullptr : walker.facts().bodies().find(referrer);
        if (!index)
            continue;
        auto isPassedToDriver = [&](const BodyValue& user, ValueID closure) {
            if (user.opcode() != Opcode::Call || user.calleeOperand() == closure || user.thisValueOperand() == closure)
                return false;
            uint32_t first = linked.firstCallOfCaller[referrer.index()];
            FunctionID helper;
            for (const LinkedCall& call : linked.calls.span().subspan(first, linked.firstCallOfCaller[referrer.index() + 1] - first)) {
                if (call.site != user.id())
                    continue;
                if (helper || call.asksForIterator || linked.isGroup(call.callee))
                    return false;
                helper = call.callee;
            }
            if (!helper || !linked.function(helper).everyCallHasOneCallee)
                return false;
            std::span<const ValueID> arguments = user.as<Opcode::Call>().argumentsOperands();
            for (size_t place = 0; place < arguments.size(); ++place) {
                if (arguments[place] != closure)
                    continue;
                if (place >= program.at(helper).numberOfPositionalParameters() || !GeneratorDriver(walker, helper).onlyDrives(program.at(helper).firstParameterIndex() + place))
                    return false;
            }
            return true;
        };
        for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
            for (BodyValue operation : index->block(BlockID(number)).values()) {
                if (operation.opcode() != Opcode::NewClosure || operation.as<Opcode::NewClosure>().function() != generatorFunction)
                    continue;
                isMade = true;
                bool isOnlyPassedOn = everyUseIs(*index, operation.id(), [&](const BodyValue& user) {
                    return isPassedToDriver(user, operation.id());
                });
                if (!isOnlyPassedOn)
                    return false;
            }
        }
    }
    return isMade;
}

bool nobodyLooksAboveGeneratorsOf(const Walker& walker, FunctionID generatorFunction, uint32_t* numberOfListedCalls)
{
    return nobodyLooksAboveGeneratorsOf(walker, generatorFunction, numberOfListedCalls, scopedLambda<bool(const BodyIndex&, const LinkedCall&, FunctionID)>([](const BodyIndex&, const LinkedCall&, FunctionID) {
        return false;
    }));
}

bool nobodyLooksAboveGeneratorsOf(const Walker& walker, FunctionID generatorFunction, uint32_t* numberOfListedCalls, const ScopedLambda<bool(const BodyIndex&, const LinkedCall&, FunctionID caller)>& passesForIteration)
{
    const Function& function = walker.program().at(generatorFunction);
    if (!function.has(FunctionFlags::IsGenerator) || function.has(FunctionFlags::IsAsync) || !walker.facts().mechanisms().contains(Mechanism::TypedOperations))
        return false;
    const LinkTables& linked = walker.facts().linked();
    if (!engineAnswersAboveGenerators(walker))
        return false;
    const BodyIndex* own = walker.facts().bodies().find(generatorFunction);
    if (!own || own->holdsAnyOf(opcodesOfMappedArguments) || walker.facts().summaries().at(generatorFunction).has(SummaryTrait::HasDirectEval))
        return false;
    if (!linked.function(generatorFunction).everyCallHasOneCallee)
        return !numberOfListedCalls && helpersDriveGeneratorsOf(walker, generatorFunction);
    for (FunctionID referrer : linked.referrersOf(generatorFunction)) {
        if (linked.isGroup(referrer))
            return false;
        uint32_t first = linked.firstCallOfCaller[referrer.index()];
        const BodyIndex* body = nullptr;
        for (const LinkedCall& call : linked.calls.span().subspan(first, linked.firstCallOfCaller[referrer.index() + 1] - first)) {
            if (call.callee != generatorFunction)
                continue;
            if (numberOfListedCalls)
                ++*numberOfListedCalls;
            if (!body)
                body = walker.facts().bodies().find(referrer);
            if (!body || (!generatorGoesIntoIterationAloneOfCoroutine(*body, call) && !passesForIteration(*body, call, referrer)))
                return false;
        }
    }
    return true;
}

static bool headOfGeneratorMayReplacePrototypeOfCoroutine(const BodyIndex& input)
{
    BitVector isVisited(input.numberOfBlocks());
    Vector<BlockID, 8> pending { entryBlock };
    while (!pending.isEmpty()) {
        BlockID block = pending.takeLast();
        if (isVisited.quickSet(block.index()))
            continue;
        BodyBlock head = input.block(block);
        if (head.hasHandler())
            return true;
        bool isSuspended = false;
        for (BodyValue operation : head.values()) {
            Opcode opcode = operation.opcode();
            isSuspended = opcode == Opcode::InitialYield;
            if (isSuspended)
                break;
            const Effects& bound = declaredBoundInTable(opcode);
            if (bound.mayRunOtherCode() || operandsMayRunCode(opcode) || bound.writes.contains(AbstractHeap::NamedProperties))
                return true;
        }
        if (isSuspended)
            continue;
        BodyTerminator terminator = input.terminator(block);
        if (terminator.kind() != TerminatorKind::Jump && !terminator.isConditionalBranch())
            return true;
        for (BodyTarget target : terminator.targets())
            pending.append(target.block());
    }
    return false;
}

static void replaceInitialYieldOfCoroutine(Walker& walker, const BodyValue& value)
{
    if (walker.unit().role == CodeRole::Body && walker.walksOwnBodyOfUnit())
        return;
    std::optional<SuspendingBodyOfCoroutine> body = findYieldingBodyOfCoroutine(walker);
    if (!body)
        return;
    Draft initialYield = walker.draftOf(value);
    Origin origin = initialYield.origin();

    std::optional<unsigned> calleeParameter = walker.program().at(walker.unit().function).calleeParameterIndex();
    if (!calleeParameter) {
        walker.fail("a generator function does not take its function object, which its generators read their prototype from"_s);
        return;
    }
    if (!nobodyLooksAboveGeneratorsOf(walker, walker.unit().function, nullptr) && (!walker.has(Mechanism::TypedOperations) || headOfGeneratorMayReplacePrototypeOfCoroutine(walker.currentBodyIndex()))) {
        Draft takePrototype = draftForCoroutine(walker, Opcode::TakeGeneratorPrototype, origin);
        takePrototype.setGeneratorOperand(body->driver);
        takePrototype.setCalleeOperand(walker.referenceOf(walker.currentBodyIndex().block(entryBlock).parameter(*calleeParameter)));
        walker.build(WTF::move(takePrototype));
    }

    Reference resumption = buildYieldBySavingFrameOfCoroutine(walker, *body, boxedUndefinedForCoroutine(walker), initialYield.heldOperands(), origin);
    if (!resumption)
        return;
    Reference sent = buildResultOfYieldOfCoroutine(walker, resumption, YieldResult::Sent, origin);
    Reference mode = buildResultOfYieldOfCoroutine(walker, resumption, YieldResult::Mode, origin);
    if (!sent || !mode)
        return;
    branchOnResumeModeOfCoroutine(walker, mode, ResumeMode::Throw, TerminatorKind::Throw, sent, origin);
    branchOnResumeModeOfCoroutine(walker, mode, ResumeMode::Return, TerminatorKind::Return, sent, origin);
}

static bool isResultOfYieldOfCoroutine(const BodyIndex& index, ValueID value, ValueID yield, YieldResult result)
{
    if (!value || index.opcodeOf(value) != Opcode::ResultAt)
        return false;
    auto read = index.value(value).as<Opcode::ResultAt>();
    return read.resultsOperand() == yield && read.index() == static_cast<uint32_t>(result);
}

static bool blocksShareHandlerOfCoroutine(BodyBlock first, BodyBlock second)
{
    if (!first.hasHandler() || !second.hasHandler())
        return first.hasHandler() == second.hasHandler();
    BodyTarget handlerOfFirst = first.handler();
    BodyTarget handlerOfSecond = second.handler();
    return handlerOfFirst.block() == handlerOfSecond.block() && std::ranges::equal(handlerOfFirst.arguments(), handlerOfSecond.arguments());
}

static bool throwArmRethrowsSentValueOfCoroutine(const BodyIndex& index, BodyBlock block, ValueID yield)
{
    BodyTerminator terminator = block.terminator();
    if (terminator.kind() != TerminatorKind::Switch)
        return false;
    auto modeSwitch = terminator.as<TerminatorKind::Switch>();
    if (!isResultOfYieldOfCoroutine(index, modeSwitch.subjectOperand(), yield, YieldResult::Mode))
        return false;

    BodyList<int64_t> cases = modeSwitch.cases();
    for (uint32_t place = 0; place < cases.size(); ++place) {
        if (cases[place] != static_cast<int64_t>(ResumeMode::Throw))
            continue;
        BodyBlock arm = index.block(terminator.targets()[place].block());
        if (arm.numberOfParameters() || arm.numberOfValues() || !blocksShareHandlerOfCoroutine(block, arm))
            return false;
        BodyTerminator endOfArm = arm.terminator();
        return endOfArm.kind() == TerminatorKind::Throw && isResultOfYieldOfCoroutine(index, endOfArm.as<TerminatorKind::Throw>().valueOperand(), yield, YieldResult::Sent);
    }
    return false;
}

static bool isCastBetweenAwaitAndYieldOfCoroutine(BodyValue operation)
{
    if (operation.opcode() == Opcode::Upcast)
        return true;
    return operation.opcode() == Opcode::Convert && !operation.as<Opcode::Convert>().declared().isEstablished();
}

static ValueID findYieldThatAloneTakesAwaitOfCoroutine(const BodyIndex& index, BodyBlock block, ValueID await)
{
    ValueID value = await;
    for (uint32_t next = await.index() + 1; next < block.endOfValues().index(); ++next) {
        ValueID user(next);
        if (index.onlyUsingValue(value) != user)
            return { };
        BodyValue operation = index.value(user);
        if (operation.opcode() == Opcode::Yield) {
            if (operation.as<Opcode::Yield>().valueOperand() != value || !throwArmRethrowsSentValueOfCoroutine(index, block, user))
                return { };
            return user;
        }
        if (!isCastBetweenAwaitAndYieldOfCoroutine(operation))
            return { };
        value = user;
    }
    return { };
}

static ValueID findAwaitThatYieldAloneTakesOfCoroutine(const BodyIndex& index, BodyBlock block, ValueID yield)
{
    for (uint32_t place = yield.index(); place-- > block.firstValue().index();) {
        BodyValue operation = index.value(ValueID(place));
        if (operation.opcode() == Opcode::Await)
            return findYieldThatAloneTakesAwaitOfCoroutine(index, block, operation.id()) == yield ? operation.id() : ValueID();
        if (!isCastBetweenAwaitAndYieldOfCoroutine(operation))
            return { };
    }
    return { };
}

static bool runTimeAwaitsWhatCoroutineYields(Walker& walker)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    return body && body->kind == CoroutineKind::AsyncGenerator;
}

static Reference buildAsyncGeneratorYieldOfCoroutine(Walker& walker, const SuspendingBodyOfCoroutine& body, Reference yielded, std::span<const Reference> held, Origin origin)
{
    Draft yield = draftForCoroutine(walker, Opcode::AsyncGeneratorYield, origin);
    yield.setGeneratorOperand(body.driver);
    yield.setValueOperand(yielded);
    yield.setHeldOperands(held);
    return buildSuspensionOfCoroutine(walker, WTF::move(yield));
}

bool replaceCoroutineOperation(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!operationsOfFamilyCoroutine.contains(opcode))
        return false;

    if (replaceSuspensionOfCoroutine(walker, value))
        return true;

    if (opcode == Opcode::ReadScope)
        return true;

    if (opcode == Opcode::InitialYield) {
        replaceInitialYieldOfCoroutine(walker, value);
        return true;
    }

    Draft written = walker.draftOf(value);
    Reference built;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(value.id()));
    std::optional<Walker::HeardAsArm> heardAsArm;
    if (walker.isInLoop() && walker.walksOwnBodyOfUnit() && awaitsInSomeIteration(index, value.id()))
        heardAsArm.emplace(walker);
    if (opcode == Opcode::Await) {
        if (runTimeAwaitsWhatCoroutineYields(walker) && findYieldThatAloneTakesAwaitOfCoroutine(index, block, value.id())) {
            walker.bind(value.id(), written.valueOperand());
            return true;
        }
        built = buildAwaitBySavingFrame(walker, written.valueOperand(), written.heldOperands(), written.origin());
    } else if (std::optional<SuspendingBodyOfCoroutine> body = findYieldingBodyOfCoroutine(walker)) {
        ValueID await = runTimeAwaitsWhatCoroutineYields(walker) ? findAwaitThatYieldAloneTakesOfCoroutine(index, block, value.id()) : ValueID();
        if (await && walker.referenceOf(await) == walker.referenceOf(index.value(await).as<Opcode::Await>().valueOperand()))
            built = buildAsyncGeneratorYieldOfCoroutine(walker, *body, written.valueOperand(), written.heldOperands(), written.origin());
        else
            built = buildYieldBySavingFrameOfCoroutine(walker, *body, written.valueOperand(), written.heldOperands(), written.origin());
    }
    if (built)
        walker.bind(value.id(), built);
    return true;
}

static TypeID typeOfDriverOfCoroutine(TypeArena& types, const Function& coroutine, FunctionID id)
{
    const ObjectTable& objects = types.objects();
    BuiltinClassID classOfResult = objects.classOf(objects.coroutineResultOf(id));
    TypeID driver = classOfResult ? types.builtinClass(classOfResult) : TypeTable::object;
    return coroutine.has(FunctionFlags::IsGenerator) ? driver : types.withPrimitives(driver, Primitive::Undefined);
}

Reference addDriverParameterOfCoroutine(Walker& walker, BlockID entry)
{
    CodeUnit unit = walker.unit();
    const Function& function = walker.program().at(unit.function);
    return walker.addParameter(entry, typeOfDriverOfCoroutine(walker.types(), function, unit.function), walker.plan().conventions().parameterRepresentation(walker.program(), unit, function.numberOfEntryParameters()));
}

Reference convertReturnedValueOfCoroutine(Walker& walker, Reference returned, Origin origin)
{
    CodeUnit unit = walker.unit();
    if (unit.role != CodeRole::CoroutineBody || walker.currentFunction() != unit.function)
        return returned;
    if (!returned)
        returned = boxedUndefinedForCoroutine(walker);

    if (walker.program().at(unit.function).has(FunctionFlags::IsGenerator) || walker.plan().conventions().of(unit.function).coroutineNeverSuspends)
        return returned;

    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    if (!body) {
        walker.fail("the body of an async function has no parameter for its driver"_s);
        return { };
    }
    Draft result = draftForCoroutine(walker, Opcode::ReturnAsyncResult, origin);
    result.setPromiseOperand(body->driver);
    result.setValueOperand(returned);
    return walker.build(WTF::move(result));
}

static bool buildRampOfCoroutine(Walker&, Vector<Reference, 12>&& operands, Origin);

bool walkRampOfCoroutine(Walker& walker)
{
    CodeUnit unit = walker.unit();
    const Program& program = walker.program();
    const Function& function = program.at(unit.function);
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit body { unit.function, CodeRole::CoroutineBody };
    if (unit.role != CodeRole::Body || !function.isCoroutine() || function.results.size() != 1 || !conventions.exists(body)) {
        walker.fail("a ramp was asked for of a function that is no coroutine with one result and a body"_s);
        return false;
    }

    bool isGenerator = function.has(FunctionFlags::IsGenerator);
    bool neverSuspends = conventions.of(unit.function).coroutineNeverSuspends;
    if (isGenerator && neverSuspends) {
        walker.fail("the plan says that a generator never suspends"_s);
        return false;
    }

    Origin origin { .position = function.position, .inlineFrame = 0, .isTailCall = false, .isPartOfConstruction = false };
    TypingContext withoutFacts { program, walker.types(), PositionTypes::knowingNothing(), TypeLevel::Proven };
    BlockID entry = walker.addBlock();
    Vector<Reference, 12> operands;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        TypeID type = declaredEntryTypeOf(withoutFacts, unit.function, index);
        operands.append(walker.addParameter(entry, type ? type : TypeTable::anything, conventions.parameterRepresentation(program, unit, index)));
    }
    walker.code().addEntry(entry);
    walker.continueIn(entry);
    return buildRampOfCoroutine(walker, WTF::move(operands), origin);
}

bool buildRampUnlessDelegated(Walker& walker, std::span<const Reference> parameters)
{
    const Function& function = walker.program().at(walker.unit().function);
    Origin origin { .position = function.position, .inlineFrame = 0, .isTailCall = false, .isPartOfConstruction = false };
    Vector<Reference, 12> operands(parameters);
    Draft test = draftForCoroutine(walker, Opcode::IsCallDelegated, origin);
    test.setCalleeOperand(operands[*function.calleeParameterIndex()]);
    Reference isDelegated = walker.build(WTF::move(test));
    if (!isDelegated)
        return false;
    BlockID runsBody = walker.addBlock();
    BlockID createsGenerator = walker.addBlock();
    walker.branch(isDelegated, runsBody, createsGenerator);
    walker.continueIn(createsGenerator);
    if (!buildRampOfCoroutine(walker, WTF::move(operands), origin))
        return false;
    walker.continueIn(runsBody);
    return true;
}

bool runsBodyAtDelegatedCall(const Walker& walker, CodeUnit unit)
{
    if (unit.role != CodeRole::Body || !walker.plan().conventions().of(unit.function).generatorNeverYields || !walker.has(Mechanism::TypedOperations))
        return false;
    const BodyIndex* body = walker.facts().bodies().find(unit.function);
    return body && engineAnswersAboveGenerators(walker) && !headOfGeneratorMayReplacePrototypeOfCoroutine(*body) && walker.plan().conventions().resultRepresentation(unit) == Representation::JSValue;
}

static bool buildRampOfCoroutine(Walker& walker, Vector<Reference, 12>&& operands, Origin origin)
{
    CodeUnit unit = walker.unit();
    const Program& program = walker.program();
    const Function& function = program.at(unit.function);
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit body { unit.function, CodeRole::CoroutineBody };
    bool isGenerator = function.has(FunctionFlags::IsGenerator);
    bool neverSuspends = conventions.of(unit.function).coroutineNeverSuspends;

    Reference driver;
    if (isGenerator) {
        std::optional<unsigned> calleeParameter = function.calleeParameterIndex();
        if (!calleeParameter) {
            walker.fail("a generator function does not take its function object, which its generators read their prototype from"_s);
            return false;
        }
        bool readsPrototype = !nobodyLooksAboveGeneratorsOf(walker, unit.function, nullptr);
        if (!readsPrototype && !walker.facts().linked().function(unit.function).everyCallHasOneCallee)
            walker.count(StatisticID::GeneratorsDrivenByHelpers);
        bool hasNoPrototypeOfItsOwn = walker.plan().functionKind(program, conventions.closureEntryOf(unit.function)).isInRuntimeModule;
        Opcode birth = function.has(FunctionFlags::IsAsync) ? Opcode::NewAsyncGenerator : readsPrototype && hasNoPrototypeOfItsOwn ? Opcode::NewGeneratorOfLibrary : Opcode::NewGenerator;
        Draft allocation = draftForCoroutine(walker, birth, origin);
        allocation.setFunction(unit.function);
        if (readsPrototype)
            allocation.setCalleeOperand(operands[*calleeParameter]);
        driver = walker.build(WTF::move(allocation));
    } else if (neverSuspends)
        driver = boxedUndefinedForCoroutine(walker);
    else
        driver = walker.build(draftForCoroutine(walker, Opcode::NewPromise, origin));
    if (!driver)
        return false;
    operands.append(driver);

    Draft start = draftForCoroutine(walker, Opcode::CallDirect, origin);
    start.setFunction(body.function);
    start.setRole(body.role);
    start.setNumberOfBoundArguments(body.numberOfBoundArguments);
    start.setIsNeverInlined(true);
    start.setVariadicOperands(operands.span());
    Reference started = walker.build(WTF::move(start));
    if (!started)
        return false;

    Reference result = started;
    if (isGenerator)
        result = driver;
    else if (neverSuspends) {
        return buildReturnsOfPromise(walker, started, origin, scopedLambda<bool(Reference)>([&](Reference promise) {
            return endBlockOfCoroutine(walker, TerminatorKind::Return, promise, origin);
        }));
    }
    return endBlockOfCoroutine(walker, TerminatorKind::Return, result, origin);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
