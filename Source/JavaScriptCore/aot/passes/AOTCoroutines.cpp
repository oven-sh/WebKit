/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutines.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsCheck.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFlowCoroutines.h"
#include "AOTFlowResults.h"
#include "AOTFlowUnseenCode.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTFuseGeneratorLoops.h"
#include "AOTHeldOperands.h"
#include "AOTIRBuilder.h"
#include "AOTPassRunner.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// SplitCoroutines.

namespace {

struct CoroutinePlan {
    CreatorRecord::Coroutine record;
    Function* bodySlot { nullptr };
    TypeID allocatedType;
    TypeID driverType;
    bool didNarrowResult { false };
    // Only an async function: the slot for its body takes a worker.
    bool neverSuspends { false };
};

struct BodyRewrite {
    Function& function;
    FunctionAnalyses& analyses;
    IRBuilder& builder;
    const CreatorRecord::Coroutine& record;
    ValueID driver;
};

} // anonymous namespace

static ValueID driverOfAsyncBody(const Function& body)
{
    switch (coroutineOfBody(body).kind) {
    case CoroutineKind::AsyncFunction:
    case CoroutineKind::AsyncGenerator:
        return driverOf(body);
    case CoroutineKind::Generator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID appendAwait(IRBuilder& builder, ValueID awaited, std::span<const ValueID> held)
{
    Function& function = builder.function();
    if (function.isUnsplitCoroutine()) {
        Value await = IRBuilder::make(Opcode::Await, { awaited });
        await.setHeldOperands(held);
        return builder.append(WTF::move(await));
    }
    Value await = IRBuilder::make(Opcode::AwaitBySavingFrame, { awaited, driverOfAsyncBody(function) });
    await.setHeldOperands(held);
    return builder.append(WTF::move(await));
}

// Where the iterator does not answer at once the step suspends this frame, and whoever finishes the step resumes the frame through its driver.
ValueID appendAsyncIteratorStep(IRBuilder& builder, ValueID iterator, ValueID sent, std::span<const ValueID> held)
{
    Function& function = builder.function();
    ValueID driver = function.isUnsplitCoroutine() ? builder.appendUndefined() : driverOfAsyncBody(function);
    Value step = IRBuilder::make(Opcode::StepAsyncGenerator, { iterator, sent, driver });
    step.setHeldOperands(held);
    return builder.append(WTF::move(step));
}

Value makeIteratorResultObject(const Program& program, ValueID value, ValueID done, AllocationSiteID site)
{
    LayoutID layoutID = program.layoutOf(BuiltinLayouts::IteratorResult);
    const Layout& layout = program.at(layoutID);
    std::optional<uint32_t> valueField = layout.findField(program.keyOf(BuiltinNames::value));
    std::optional<uint32_t> doneField = layout.findField(program.keyOf(BuiltinNames::done));
    RELEASE_ASSERT(valueField && doneField);

    Value object = IRBuilder::make(Opcode::NewObject);
    object.setLayout(layoutID);
    object.setFields(std::array { *valueField, *doneField });
    object.setValuesOperands(std::array { value, done });
    object.setAllocationSite(site);
    return object;
}

static ValueID appendSettledPromise(IRBuilder& builder, Opcode settle, ValueID outcome)
{
    ValueID promise = builder.append(Opcode::NewPromise, { });
    builder.append(settle, { promise, outcome });
    return promise;
}

static Function buildRamp(const AnalysisContext& context, TypeArena& types, const Function& unsplit, CoroutinePlan& plan)
{
    const CreatorRecord::Coroutine& record = plan.record;
    Function ramp;
    ramp.name = unsplit.name;
    ramp.module = unsplit.module;
    ramp.position = unsplit.position;
    ramp.text = unsplit.text;
    ramp.kind = unsplit.kind;
    ramp.flags = unsplit.flags;
    ramp.length = unsplit.length;
    ramp.enclosingRuntimeFunction = unsplit.enclosingRuntimeFunction;
    ramp.captures = unsplit.captures;
    ramp.thisType = unsplit.thisType;
    ramp.parameters = unsplit.parameters;
    ramp.results = unsplit.results;
    ramp.derivation = unsplit.derivation;
    ramp.constructedClass = unsplit.constructedClass;
    ramp.role = FunctionRole::CoroutineRamp;
    if (unsplit.runsOnlyBehindFailedTest())
        ramp.flags.add(FunctionFlags::IsSplitOffGenericVersion);
    ramp.creator = record;
    ramp.capabilities = capabilitiesInPlaceOf(ramp, unsplit.capabilities, FunctionCapabilities::MayChangeSignature);

    IRBuilder builder(context, ramp, types);
    builder.setOrigin(Origin { ramp.position, 0 });
    BlockID entry = builder.addBlock();
    TypingContext typing { context.program, unsplit, types };
    Vector<ValueID, 8> entryParameters(unsplit.numberOfEntryParameters(), [&](size_t index) {
        return builder.addParameter(entry, declaredEntryTypeOf(typing, unsplit, index));
    });
    RELEASE_ASSERT(ramp.results.size() == 1);
    // A ramp returns what it allocates, whatever is said of its result. What is said of the contents stays in the type, where contents are no proof.
    DeclaredType& declared = ramp.results[0];
    plan.didNarrowResult = declared.isExpected() || !types.at(declared.written()).isSubsetOf(types.at(plan.allocatedType), context.program);
    if (plan.didNarrowResult) {
        TypeID met = types.intersectionOf(plan.allocatedType, declared.expected().predicted());
        declared.establish(Proven::byConstruction(met == TypeTable::never ? plan.allocatedType : met));
    }

    // A ramp is built before its body exists, so the result type of this call cannot come from the callee. A worker takes no driver.
    auto appendStart = [&](ValueID driver) {
        Value call(Opcode::CallDirect);
        call.setFunction(record.body);
        call.setArgumentsOperands(entryParameters.span());
        if (driver)
            call.appendVariadicOperand(driver);
        return builder.appendWithType(WTF::move(call), TypeTable::anything);
    };
    auto emitReturnOfDriver = [&](ValueID returned) {
        emitReturnOfCall(builder, convertTo(builder, returned, DeclaredType(Proven::byPosition(*ramp.results[0].established()))));
    };

    if (plan.neverSuspends) {
        // The worker returns and throws as any function does. Here its result becomes the resolution of a new promise, and what it throws the reason of a rejected one.
        RELEASE_ASSERT(record.kind == CoroutineKind::AsyncFunction && record.worker && record.worker == record.body);
        BlockID rejection = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
        builder.setHandler(Target { rejection, { }, { } });
        BlockID calling = builder.addBlock();
        builder.setHandler(std::nullopt);
        BlockID resolving = builder.addBlock();

        builder.moveToEndOf(entry);
        builder.emitJump(calling);

        builder.moveToEndOf(calling);
        ValueID result = appendStart({ });
        builder.emitJump(resolving);

        builder.moveToEndOf(resolving);
        emitReturnOfDriver(appendSettledPromise(builder, Opcode::ResolvePromise, result));

        builder.moveToEndOf(rejection);
        emitReturnOfDriver(appendSettledPromise(builder, Opcode::RejectPromise, builder.append(Opcode::CatchValue, { })));
        return ramp;
    }

    builder.moveToEndOf(entry);
    switch (record.kind) {
    case CoroutineKind::AsyncFunction:
        // A body that throws before its first suspension returns a new rejected promise, not this one.
        emitReturnOfDriver(appendStart(builder.append(Opcode::NewPromise, { })));
        return ramp;
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncGenerator: {
        std::optional<unsigned> calleeParameter = unsplit.calleeParameterIndex();
        RELEASE_ASSERT(calleeParameter);
        Value allocation = IRBuilder::make(record.kind == CoroutineKind::Generator ? Opcode::NewGenerator : Opcode::NewAsyncGenerator, { entryParameters[*calleeParameter] });
        allocation.setFunction(record.ramp);
        ValueID generator = builder.append(WTF::move(allocation));
        appendStart(generator);
        emitReturnOfDriver(generator);
        return ramp;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Nothing in the function suspends, so its code stays as it is: a return is a return, and what it throws reaches the caller, which is the ramp or a function that awaits the call.
static Function buildAsyncWorker(TypeArena& types, Function&& unsplit, const CoroutinePlan& plan)
{
    const CreatorRecord::Coroutine& record = plan.record;
    RELEASE_ASSERT(plan.neverSuspends && record.kind == CoroutineKind::AsyncFunction && record.worker && record.worker == record.body);

    Function worker = WTF::move(unsplit);
    if (worker.runsOnlyBehindFailedTest())
        worker.flags.add(FunctionFlags::IsSplitOffGenericVersion);
    worker.role = FunctionRole::AsyncWorker;
    worker.creator = record;
    worker.flags.remove(FunctionFlags::IsAsync);
    worker.text = { };

    // The result declared for an async function is its promise. The worker returns what the promise is resolved with, of which only the returned values say anything proven.
    TypeID returned = TypeTable::never;
    for (const Block& block : worker.blocks) {
        if (block.terminator.kind != TerminatorKind::Return)
            continue;
        ValueID value = block.terminator.valueOperand();
        returned = types.unionOf(returned, value ? worker.at(value).type : TypeTable::undefined);
    }
    worker.results = Vector<DeclaredType> { DeclaredType(Proven::byPosition(returned)) };
    worker.derivation = Derivation {
        .source = record.ramp,
        .parameterMap = Vector<ParameterOrigin>(worker.numberOfEntryParameters(), [](size_t index) { return ParameterOrigin { static_cast<uint32_t>(index) }; }),
    };
    worker.capabilities = initialCapabilitiesOf(worker);
    return worker;
}

static void forEachListed(const Function& function, Opcode opcode, NOESCAPE const Invocable<void(ProgramPoint, ValueID)> auto& functor)
{
    for (uint32_t block = 0; block < function.blocks.size(); ++block) {
        const BlockValues& values = function.at(BlockID(block)).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (function.at(values[index]).opcode == opcode)
                functor(ProgramPoint { BlockID(block), index }, values[index]);
        }
    }
}

static ReplacedEffects replacedEffectsOf(const BodyRewrite& body, std::initializer_list<ValueID> replacedValues)
{
    ReplacedEffects replaced;
    for (ValueID id : replacedValues)
        replaced.add(body.analyses.effectContext(), body.function, body.function.at(id));
    return replaced;
}

static void checkSuccessor(const BodyRewrite& body, BlockID block, ValueID id, const ReplacedEffects& replaced)
{
    checkReplacement(body.analyses.effectContext(), body.function, { body.record.body, block, id }, body.function.at(id), replaced, body.analyses.context().build.diagnostics());
}

static void rewriteInPlace(BodyRewrite& body, BlockID block, ValueID id, Opcode opcode, std::initializer_list<ValueID> operands, std::initializer_list<ValueID> replacedValues)
{
    ReplacedEffects replaced = replacedEffectsOf(body, replacedValues);
    Value& value = body.function.edit(id);
    Vector<ValueID, 8> held(heldOperandsOf(value));
    value.become(opcode);
    uint32_t operandIndex = 0;
    for (ValueID operand : operands)
        value.setOperand(operandIndex++, operand);
    setHeldOperands(value, held.span());
    body.analyses.didChangeValues();
    checkSuccessor(body, block, id, replaced);
}

static void prepareHeaderOfBody(const AnalysisContext& context, TypeArena& types, Function& body, const CoroutinePlan& plan)
{
    unsigned sourceParameters = body.numberOfEntryParameters();

    if (body.runsOnlyBehindFailedTest())
        body.flags.add(FunctionFlags::IsSplitOffGenericVersion);
    body.role = FunctionRole::CoroutineBody;
    body.creator = plan.record;
    body.text = { };

    // The driver has to be the last entry parameter, so a hidden rest in front of it becomes an ordinary parameter.
    if (std::optional<unsigned> hiddenRest = body.hiddenRestParameterIndex()) {
        TypingContext typing { context.program, body, types };
        TypeID restType = declaredEntryTypeOf(typing, body, *hiddenRest);
        body.flags.remove(FunctionFlags::HasHiddenRest);
        body.parameters.append(DeclaredType(Proven::byPosition(restType)));
    }
    body.flags.remove(FunctionFlags::HasRestParameter);
    body.parameters.append(DeclaredType(Proven::byPosition(plan.driverType)));
    RELEASE_ASSERT(plan.record.promiseParameter == sourceParameters && body.numberOfEntryParameters() == sourceParameters + 1);

    body.results = Vector<DeclaredType> { DeclaredType(Proven::byPosition(TypeTable::anything)) };
    body.derivation = Derivation {
        .source = plan.record.ramp,
        .parameterMap = Vector<ParameterOrigin>(sourceParameters + 1, [&](size_t index) {
            return index < sourceParameters ? ParameterOrigin { static_cast<uint32_t>(index) } : ParameterOrigin { std::nullopt };
        }),
    };
    body.capabilities = initialCapabilitiesOf(body);
}

static void markRareModeArms(BodyRewrite& body)
{
    Function& function = body.function;
    const UseLists& useLists = body.analyses.useLists();
    forEachListed(function, Opcode::Yield, [&](ProgramPoint, ValueID yield) {
        for (const Use& read : useLists.usesOf(yield)) {
            if (read.kind != Use::Kind::Operand)
                continue;
            const Value& value = function.at(read.user);
            if (value.opcode != Opcode::ResultAt || value.index() != static_cast<uint32_t>(YieldResult::Mode))
                continue;
            for (const Use& use : useLists.usesOf(read.user)) {
                const Terminator& terminator = function.at(use.block).terminator;
                if (use.kind != Use::Kind::TerminatorOperand || terminator.kind != TerminatorKind::Switch)
                    continue;
                BlockID otherwise = terminator.otherwise().block;
                for (const Target& target : terminator.caseTargets()) {
                    if (target.block != otherwise)
                        function.edit(target.block, BlockEdit::Rest).frequency = BlockFrequency::Rare;
                }
            }
        }
    });
    body.analyses.invalidate(FunctionAnalysis::Frequencies);
}

// What the merged await rejects with arrives at the yield as a value that is sent in to be thrown.
static void moveProofsOfSentValueBehindMode(BodyRewrite& body, BlockID block, ValueID yield)
{
    Function& function = body.function;
    IRBuilder& builder = body.builder;
    const Terminator& terminator = function.at(block).terminator;
    if (terminator.kind != TerminatorKind::Switch)
        return;

    const BlockValues& values = function.at(block).values;
    ValueID sent;
    Vector<ValueID, 4> proofs;
    BitVector proofIndexes(values.size());
    for (uint32_t index = 0; index < values.size(); ++index) {
        const Value& value = function.at(values[index]);
        if (value.opcode == Opcode::ResultAt && value.operand(0) == yield && value.index() == static_cast<uint32_t>(YieldResult::Sent)) {
            if (sent)
                return;
            sent = values[index];
            continue;
        }
        if (!sent || (!isCastWithoutTest(value.opcode) && value.opcode != Opcode::ValidateType))
            continue;
        if (value.valueOperand() != sent && !proofs.contains(value.valueOperand()))
            continue;
        proofs.append(values[index]);
        proofIndexes.quickSet(index);
    }
    if (proofs.isEmpty())
        return;

    BlockID throwingArm;
    Vector<BlockID, 2> otherArms;
    auto addArm = [&](BlockID arm) {
        if (!otherArms.contains(arm))
            otherArms.append(arm);
    };
    std::span<const Target> caseTargets = terminator.caseTargets();
    for (size_t index = 0; index < caseTargets.size(); ++index) {
        if (terminator.cases()[index] == static_cast<int64_t>(ResumeMode::Throw))
            throwingArm = caseTargets[index].block;
        else
            addArm(caseTargets[index].block);
    }
    addArm(terminator.otherwise().block);
    if (!throwingArm || otherArms.contains(throwingArm))
        return;
    const CFG& cfg = body.analyses.cfg();
    if (!cfg.singleIncomingEdge(throwingArm))
        return;
    Vector<Edge, 2> edgesIntoOtherArms;
    for (BlockID arm : otherArms) {
        std::optional<Edge> edge = cfg.singleIncomingEdge(arm);
        if (!edge)
            return;
        edgesIntoOtherArms.append(*edge);
    }

    struct UseInArm {
        Use use;
        uint32_t proof;
        // None for the arm that throws.
        std::optional<uint32_t> arm;
    };
    Vector<UseInArm, 8> uses;
    Dominators& dominators = body.analyses.dominators();
    const UseLists& useLists = body.analyses.useLists();
    for (uint32_t proof = 0; proof < proofs.size(); ++proof) {
        for (const Use& use : useLists.usesOf(proofs[proof])) {
            if (use.kind == Use::Kind::Operand && proofs.contains(use.user))
                continue;
            if (use.kind == Use::Kind::BlockSubject)
                return;
            if (dominators.dominates(throwingArm, use.block)) {
                uses.append({ use, proof, std::nullopt });
                continue;
            }
            size_t arm = otherArms.findIf([&](BlockID candidate) { return dominators.dominates(candidate, use.block); });
            // Somebody reads it where several arms meet, or in front of the switch.
            if (arm == notFound)
                return;
            uses.append({ use, proof, static_cast<uint32_t>(arm) });
        }
    }

    for (size_t index = 0; index < otherArms.size(); ++index) {
        if (function.at(otherArms[index]).handler)
            otherArms[index] = builder.splitEdge(edgesIntoOtherArms[index]);
    }

    Vector<ValueID, 8> copies;
    for (size_t arm = 0; arm < otherArms.size(); ++arm) {
        for (ValueID proof : proofs) {
            Value copy = function.at(proof);
            size_t operand = proofs.find(copy.valueOperand());
            if (operand != notFound)
                copy.setValueOperand(copies[arm * proofs.size() + operand]);
            builder.setOrigin(copy.origin());
            copies.append(builder.insertBefore(ProgramPoint::atHeadOf(otherArms[arm]), WTF::move(copy)));
        }
    }
    for (const UseInArm& use : uses)
        replaceUse(function, use.use, use.arm ? copies[*use.arm * proofs.size() + use.proof] : sent);
    builder.applyInsertions();
    builder.eraseValues(block, proofIndexes);
    body.analyses.didChangeValues();
}

static void mergeAwaitsIntoYields(BodyRewrite& body)
{
    struct AwaitedYield {
        ProgramPoint await;
        uint32_t yieldIndex;
        ValueID yield;
    };
    const Function& function = body.function;
    const UseLists& useLists = body.analyses.useLists();
    Vector<AwaitedYield> merges;
    forEachListed(function, Opcode::Await, [&](ProgramPoint point, ValueID await) {
        if (function.at(await).takesExtraTurn())
            return;
        const BlockValues& values = function.at(point.block).values;
        for (uint32_t index = point.index; index + 1 < values.size(); ++index) {
            std::optional<Use> use = useLists.singleUse(values[index]);
            if (!use || use->kind != Use::Kind::Operand || use->user != values[index + 1])
                return;
            Opcode user = function.at(use->user).opcode;
            if (user == Opcode::Yield && use->operandIndex == OperandIndex::Yield::value) {
                merges.append({ point, index + 1, use->user });
                return;
            }
            if (!isCastWithoutTest(user))
                return;
        }
    });

    // The last one of a block first, so the positions of the others still hold.
    for (const AwaitedYield& merge : merges | std::views::reverse) {
        ValueID await = function.at(merge.await.block).values[merge.await.index];
        rewriteInPlace(body, merge.await.block, merge.yield, Opcode::AsyncGeneratorYield, { body.driver, function.at(await).valueOperand() }, { await, merge.yield });
        BitVector mergedIndexes;
        for (uint32_t index = merge.await.index; index < merge.yieldIndex; ++index)
            mergedIndexes.set(index);
        body.builder.eraseValues(merge.await.block, mergedIndexes);
    }
    for (const AwaitedYield& merge : merges)
        moveProofsOfSentValueBehindMode(body, merge.await.block, merge.yield);
}

static void replaceYields(BodyRewrite& body)
{
    forEachListed(body.function, Opcode::Yield, [&](ProgramPoint point, ValueID id) {
        rewriteInPlace(body, point.block, id, Opcode::YieldBySavingFrame, { body.driver, body.function.at(id).valueOperand() }, { id });
    });
}

// A step suspends this frame where the generator does not answer at once, and whoever finishes the step resumes the frame through its driver.
static void replaceStepsAndAwaits(BodyRewrite& body)
{
    Function& function = body.function;
    IRBuilder& builder = body.builder;
    TypeArena& types = builder.types();
    const Program& program = body.analyses.program();
    forEachListed(function, Opcode::StepAsyncGenerator, [&](ProgramPoint, ValueID id) {
        function.edit(id).setDriverOperand(body.driver);
    });
    body.analyses.didChangeValues();

    Vector<ProgramPoint> typedAwaits;
    forEachListed(function, Opcode::Await, [&](ProgramPoint point, ValueID id) {
        ValueID awaited = function.at(id).valueOperand();
        bool takesExtraTurn = function.at(id).takesExtraTurn();
        const Type& successorType = types.at(resultTypeOf(program, function, IRBuilder::make(Opcode::AwaitBySavingFrame, { awaited, body.driver }), types));
        if (!successorType.isSubsetOf(types.at(function.at(id).type), program)) {
            typedAwaits.append(point);
            return;
        }
        rewriteInPlace(body, point.block, id, Opcode::AwaitBySavingFrame, { awaited, body.driver }, { id });
        function.edit(id, ValueEdit::Definition).setTakesExtraTurn(takesExtraTurn);
    });

    for (ProgramPoint point : typedAwaits | std::views::reverse) {
        Value await = function.at(function.at(point.block).values[point.index]);
        ReplacedEffects replaced;
        replaced.add(body.analyses.effectContext(), function, await);

        IRBuilder::Expansion expansion = builder.beginExpansion(point);
        Value successor = IRBuilder::make(Opcode::AwaitBySavingFrame, { await.valueOperand(), body.driver });
        successor.setTakesExtraTurn(await.takesExtraTurn());
        successor.setHeldOperands(heldOperandsOf(await));
        // An expected type is set only where it says more than the type.
        TypeID type = resultTypeOf(program, function, successor, types);
        if (await.expected().predicted() && await.expected().predicted() != type && types.at(await.expected().predicted()).isSubsetOf(types.at(type), program))
            successor.setExpected(await.expected());
        ValueID result = builder.append(WTF::move(successor));
        checkSuccessor(body, point.block, result, replaced);
        builder.endExpansion(expansion, convertTo(builder, result, DeclaredType(Proven::byPosition(await.type))));
    }
}

static void expandInitialYield(BodyRewrite& body)
{
    Function& function = body.function;
    FunctionAnalyses& analyses = body.analyses;
    IRBuilder& builder = body.builder;
    std::optional<ProgramPoint> point;
    for (uint32_t block = 0; block < function.blocks.size() && !point; ++block) {
        const BlockValues& values = function.at(BlockID(block)).values;
        for (uint32_t index = 0; index < values.size() && !point; ++index) {
            if (function.at(values[index]).opcode == Opcode::InitialYield)
                point = ProgramPoint { BlockID(block), index };
        }
    }
    if (!point)
        return;
    auto head = [&] { return function.at(entryBlock).values.span().first(point->index); };

    ReplacedEffects replaced = replacedEffectsOf(body, { function.at(point->block).values[point->index] });
    Vector<ValueID, 8> held(heldOperandsOf(function.at(function.at(point->block).values[point->index])));
    // The ramp allocates the generator, which reads the property `prototype` of the function, and only then does the head of the body evaluate the parameters.
    bool takesPrototypeAgain = point->block != entryBlock || std::ranges::any_of(head(), [&](ValueID id) {
        Effects effects = analyses.effectsOf(id).onAnyEdge();
        return effects.mayCallUnknownCode || effects.writes.contains(AbstractHeap::NamedProperties);
    });

    IRBuilder::Expansion expansion = builder.beginExpansion(*point);
    RELEASE_ASSERT(expansion.results.isEmpty());
    // Nothing tells when a head without an effect has run, so the call that starts the generator may be left to its first step.
    bool headHasNoEffect = body.record.kind == CoroutineKind::Generator && point->block == entryBlock && (std::ranges::all_of(head(), [&](ValueID id) {
        return analyses.isUnobservable(id) && analyses.effectsOf(id).onAnyEdge().reads.restrictedTo(orderedHeaps).isEmpty();
    }) || analyses.context().build.isSabotaged(Sabotage::GeneratorHeadsHaveEffects));
    if (headHasNoEffect) {
        // Whoever takes the first step by calling the body has marked the generator as running, and the body does not suspend here.
        IRBuilder::BranchBlocks blocks = builder.emitBranch(builder.append(Opcode::IsGeneratorRunning, { body.driver }));
        builder.emitJump(expansion.continuation);
        builder.moveToEndOf(blocks.notTaken);
    }
    if (takesPrototypeAgain) {
        std::optional<unsigned> calleeParameter = function.calleeParameterIndex();
        RELEASE_ASSERT(calleeParameter);
        builder.append(Opcode::TakeGeneratorPrototype, { body.driver, function.at(entryBlock).parameters[*calleeParameter] });
    }
    Value firstYield = IRBuilder::make(Opcode::YieldBySavingFrame, { body.driver, builder.appendUndefined() });
    firstYield.setHeldOperands(held.span());
    ValueID yield = builder.append(WTF::move(firstYield));
    checkSuccessor(body, point->block, yield, replaced);
    ValueID sent = builder.append(makeResultAt(yield, static_cast<uint32_t>(YieldResult::Sent)));
    ValueID mode = builder.append(makeResultAt(yield, static_cast<uint32_t>(YieldResult::Mode)));

    BlockID throwArm = builder.addBlock(BlockFrequency::Rare);
    BlockID returnArm = builder.addBlock(BlockFrequency::Rare);
    builder.emitSwitch(mode, { { static_cast<int64_t>(ResumeMode::Throw), Target { throwArm, { }, { } } }, { static_cast<int64_t>(ResumeMode::Return), Target { returnArm, { }, { } } } }, Target { expansion.continuation, { }, { } });

    builder.moveToEndOf(throwArm);
    builder.emitThrow(sent);
    builder.moveToEndOf(returnArm);
    builder.emitReturn(sent);
}

static void replaceReturns(BodyRewrite& body)
{
    IRBuilder& builder = body.builder;
    bool isAsyncFunction = body.record.kind == CoroutineKind::AsyncFunction;
    for (uint32_t blockIndex = 0; blockIndex < body.function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        Terminator& terminator = body.function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
        if (terminator.kind != TerminatorKind::Return)
            continue;
        ValueID value = terminator.valueOperand();
        if (value && !isAsyncFunction)
            continue;

        builder.setOrigin(terminator.origin());
        terminator = Terminator();
        builder.moveToEndOf(block);
        if (!value)
            value = builder.appendUndefined();
        if (isAsyncFunction)
            value = builder.append(Opcode::ReturnAsyncResult, { body.driver, value });
        builder.emitReturn(value);
    }
    body.analyses.didChangeValues();
}

// In each block from the last suspension that can list what is held to the first, with what is live behind it.
static void forEachSuspensionFromLastToFirst(const Function& function, FunctionAnalyses& analyses, NOESCAPE const Invocable<void(ValueID, const Liveness::LocalCalc&)> auto& functor)
{
    Vector<uint32_t, 4> suspensionIndexes;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (canHoldOperands(function.at(values[index]).opcode) && analyses.maySuspend(values[index]))
                suspensionIndexes.append(index);
        }
        if (suspensionIndexes.isEmpty())
            continue;

        Liveness::LocalCalc localCalc(analyses.liveness(), block);
        uint32_t pointIndex = values.size();
        while (!suspensionIndexes.isEmpty()) {
            uint32_t suspensionIndex = suspensionIndexes.takeLast();
            while (pointIndex > suspensionIndex + 1)
                localCalc.execute(--pointIndex);
            functor(values[suspensionIndex], localCalc);
        }
    }
}

static PassOutcome listLiveValues(FunctionPassContext& context)
{
    Function& function = context.function;
    PassOutcome outcome = PassOutcome::Unchanged;
    if (!function.isUnsplitCoroutine())
        return outcome;
    // What the function still captures when it is split it holds until it ends.
    BitVector isHeldUntilTheEnd;
    const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
    for (uint32_t index = 0; index < entryParameters.size(); ++index) {
        if (function.entryParameterKind(index) != EntryParameter::Parameter)
            isHeldUntilTheEnd.set(entryParameters[index].index());
    }
    Vector<ValueID, 16> held;
    BitVector isListed;
    forEachSuspensionFromLastToFirst(function, context.analyses, [&](ValueID suspension, const Liveness::LocalCalc& behind) {
        Opcode opcode = function.at(suspension).opcode;
        if ((opcode == Opcode::IteratorResume || opcode == Opcode::IteratorClose) && function.at(suspension).iteratorKind() == IteratorKind::Sync)
            return;
        held.shrink(0);
        isListed.clearAll();
        for (ValueID listed : heldOperandsOf(function.at(suspension))) {
            held.append(listed);
            isListed.set(listed.index());
        }
        size_t numberListed = held.size();
        auto list = [&](ValueID object) {
            if (!isHeldUntilTheEnd.get(object.index()) && !isListed.set(object.index()))
                held.append(object);
        };
        behind.forEachLive([&](ValueID live) {
            if (live == suspension)
                return;
            // An iteration holds what it walks, and a closure what it captures: one that is only called need not be made.
            const Value& definition = function.at(sameObjectThroughConversionsToObject(function, live));
            if (definition.opcode == Opcode::IteratorOpen)
                list(definition.iterableOperand());
            else if (definition.opcode == Opcode::NewClosure) {
                for (ValueID capture : definition.capturesOperands())
                    list(capture);
            } else
                list(live);
        });
        if (held.size() == numberListed)
            return;
        setHeldOperands(function.edit(suspension), held.span());
        outcome = PassOutcome::Changed;
    });
    return outcome;
}

// A block goes on listing each value that it was read with.
static PassOutcome replaceReadsOfScopes(Function& function)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    for (uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (ValueID id : function.at(BlockID(block)).values) {
            if (function.at(id).opcode != Opcode::ReadScope)
                continue;
            function.edit(id).become(Opcode::Undefined);
            outcome = PassOutcome::Changed;
        }
    }
    return outcome;
}

// What the function as it is written reads behind a suspension it holds there, also where a later pass finds that the read is never reached.
// A read of a scope reads all that the names of the scope hold.
PassOutcome listLiveValuesAtSuspensions(FunctionPassContext& context)
{
    PassOutcome outcome = listLiveValues(context);
    return outcome | replaceReadsOfScopes(context.function);
}

// A constant of a module holds its value as long as the program runs.
static bool isHeldByConstantOfModule(const Program& program, const Function& function, ValueID id)
{
    const Value* definition = &function.at(sameObjectThroughCasts(function, id));
    if (definition->opcode == Opcode::FinishClass)
        definition = &function.at(sameObjectThroughCasts(function, definition->classObjectOperand()));
    if (definition->opcode != Opcode::GetModuleVariable)
        return false;
    const Variable& variable = program.at(definition->variable());
    return variable.flags.contains(VariableFlags::IsImmutable) && variable.module;
}

// A suspended function holds what it reads behind the suspension and, until it ends, its receiver, what it captures, and what the names of its parameters hold, which the suspension lists as it is read.
// A later pass may answer such a read without the object: what the object holds lives on as long as the function would have held the object.
static void holdLiveValuesAtSuspensions(BodyRewrite& body)
{
    Function& function = body.function;
    const Program& program = body.analyses.program();
    body.analyses.didChangeControlFlow();

    auto isHeld = [&](ValueID id) {
        return id != body.driver && mayHoldObject(program.typeOf(function, id)) && !isHeldByConstantOfModule(program, function, id);
    };
    Vector<ValueID, 8> parameters;
    const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
    for (uint32_t index = 0; index < entryParameters.size(); ++index) {
        if (entryParameters[index] != body.driver && function.entryParameterKind(index) != EntryParameter::Parameter && isHeld(entryParameters[index]))
            parameters.append(entryParameters[index]);
    }

    Vector<ValueID, 16> held;
    forEachSuspensionFromLastToFirst(function, body.analyses, [&](ValueID suspension, const Liveness::LocalCalc& behind) {
        held.shrink(0);
        auto holdUnlessLive = [&](ValueID id) {
            if (!behind.isLive(id) && !held.contains(id))
                held.append(id);
        };
        for (ValueID parameter : parameters)
            holdUnlessLive(parameter);
        for (ValueID listed : heldOperandsOf(function.at(suspension))) {
            if (isHeld(listed))
                holdUnlessLive(listed);
        }
        behind.forEachLive([&](ValueID live) {
            if (live != suspension && isHeld(live))
                held.append(live);
        });
        setHeldOperands(function.edit(suspension), held.span());
    });
    body.analyses.didChangeValues();
}

static Function buildCoroutineBody(const AnalysisContext& context, TypeArena& types, Function&& unsplit, const CoroutinePlan& plan)
{
    Function function = WTF::move(unsplit);
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    prepareHeaderOfBody(context, types, function, plan);
    builder.setOrigin(Origin { function.position, 0 });
    BodyRewrite body { function, analyses, builder, plan.record, builder.addParameter(entryBlock, plan.driverType) };

    switch (plan.record.kind) {
    case CoroutineKind::Generator:
        markRareModeArms(body);
        replaceYields(body);
        expandInitialYield(body);
        break;
    case CoroutineKind::AsyncGenerator:
        markRareModeArms(body);
        mergeAwaitsIntoYields(body);
        replaceYields(body);
        replaceStepsAndAwaits(body);
        expandInitialYield(body);
        break;
    case CoroutineKind::AsyncFunction:
        replaceStepsAndAwaits(body);
        break;
    }

    replaceReturns(body);
    holdLiveValuesAtSuspensions(body);
    builder.setHandler(std::nullopt);
    return function;
}

static bool maySuspend(const AnalysisContext& context, const Function& function)
{
    FunctionAnalyses analyses(context, function);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        if (analyses.effectsOfTerminator(block).onAnyEdge().maySuspend)
            return true;
        for (ValueID id : function.at(block).values) {
            if (analyses.maySuspendOnAnyEdge(id))
                return true;
        }
    }
    return false;
}

static void retypeCallers(ProgramEditor& editor, BuildContext& build, const Vector<CoroutinePlan>& plans)
{
    BitVector narrowedRamps;
    for (const CoroutinePlan& plan : plans) {
        if (plan.didNarrowResult)
            narrowedRamps.set(plan.record.ramp.index());
    }
    if (narrowedRamps.isEmpty())
        return;

    struct StaleCalls {
        FunctionID caller;
        Vector<ValueID, 4> calls;
    };
    const Program& program = editor.program();
    Vector<StaleCalls> staleCalls = collectInParallel<StaleCalls>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<StaleCalls>& found) {
        const Function& caller = program.functions[index];
        Vector<ValueID, 4> calls;
        for (const Block& block : caller.blocks) {
            for (ValueID id : block.values) {
                const Value& value = caller.at(id);
                if (value.opcode != Opcode::CallDirect || !narrowedRamps.get(value.function().index()))
                    continue;
                TypeID resultType = program.provenTypeOf(program.at(value.function()).result());
                if (resultType != value.type && program.at(resultType).isSubsetOf(program.at(value.type), program))
                    calls.append(id);
            }
        }
        if (!calls.isEmpty())
            found.append(StaleCalls { FunctionID(index), WTF::move(calls) });
    });
    Vector<FunctionID> callers = staleCalls.map([](const StaleCalls& stale) { return stale.caller; });
    editFunctionsInParallel(editor, build, callers.span(), [&](unsigned index, FunctionID, Function& caller, TaskArenas& arenas) {
        for (ValueID id : staleCalls[index].calls) {
            Value& call = caller.edit(id);
            call.type = resultTypeOf(program, caller, call, arenas.types);
            call.setExpected(ExpectedType(expectedTypeOf(program, caller, call, arenas.types)));
        }
    });
}

static bool loadsModulesByName(const FlowResults& flow)
{
    return !flow.hasResults() || std::ranges::any_of(flow.unseenCode(), [](const UnseenCodeRecord& record) {
        return loadsModulesByName(record.cause);
    });
}

// A require() evaluates the ES module that it names and what that module imports.
static void markModulesEvaluatedByRequire(ProgramEditor& editor, const FlowResults& flow, const ParallelLoops& loops)
{
    const Program& program = editor.program();
    bool someModuleAwaits = std::ranges::any_of(program.modules, [](const Module& module) {
        return module.flags.contains(ModuleFlags::HasTopLevelAwait);
    });
    if (!someModuleAwaits)
        return;

    Vector<uint32_t> work = collectInParallel<uint32_t>(loops, program.functions.size(), [&](unsigned index, Vector<uint32_t>& found) {
        if (!mayListValueWith(program, FunctionID(index), OpcodeSet { Opcode::RequireModule, Opcode::EvaluateRequiredModule }))
            return;
        for (const Value& value : program.functions[index].values) {
            if (value.opcode == Opcode::RequireModule || value.opcode == Opcode::EvaluateRequiredModule)
                found.append(value.module().index());
        }
    });
    for (const Module& module : program.modules) {
        for (const ModuleRequest& request : module.requests) {
            if (request.kind == ModuleRequestKind::Require)
                work.append(request.target.index());
        }
    }
    // A name computed at run time finds an entry point. A program with one entry point has begun it before any require() runs.
    if (program.entries.size() > 1 && loadsModulesByName(flow)) {
        for (const Entry& entry : program.entries)
            work.append(entry.module.index());
    }
    BitVector seen(program.modules.size());
    while (!work.isEmpty()) {
        uint32_t index = work.takeLast();
        if (program.modules[index].kind != ModuleKind::SourceText || seen.quickSet(index))
            continue;
        editor.at(ModuleID(index)).flags.add(ModuleFlags::IsEvaluatedByRequire);
        for (const ModuleRequest& request : program.modules[index].requests)
            work.append(request.target.index());
    }
}

void splitCoroutines(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    BuildContext& build = runner.build();
    const Program& program = editor.program();
    Vector<uint32_t> coroutines = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        return program.functions[index].isUnsplitCoroutine() && program.functions[index].hasBody();
    });
    if (coroutines.isEmpty())
        return;
    markModulesEvaluatedByRequire(editor, inputs.flow, build.parallelLoops());
    Vector<CoroutinePlan> plans(coroutines.size(), [&](size_t index) {
        CoroutinePlan plan;
        plan.record.kind = *coroutineKindOf(program.functions[coroutines[index]]);
        plan.record.ramp = FunctionID(coroutines[index]);
        return plan;
    });

    // An async function without an await needs no frame that can be saved: it is an ordinary function whose result goes into a promise.
    forEachIndexInParallel(build.parallelLoops(), plans.size(), [&](unsigned index) {
        CoroutinePlan& plan = plans[index];
        FunctionID id = plan.record.ramp;
        if (plan.record.kind == CoroutineKind::AsyncFunction)
            plan.neverSuspends = !maySuspend(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, program.at(id));
    });

    TypeTable& types = editor.types();
    TypeID promise = types.intern(Type::builtinClass(resultClassOf(CoroutineKind::AsyncFunction)));
    TypeID promiseOrUndefined = types.intern(types.at(promise).withPrimitives(Primitive::Undefined));
    TypeID generator = types.intern(Type::builtinClass(resultClassOf(CoroutineKind::Generator)));
    TypeID asyncGenerator = types.intern(Type::builtinClass(resultClassOf(CoroutineKind::AsyncGenerator)));
    auto allocatedBy = [&](CoroutineKind kind) {
        switch (kind) {
        case CoroutineKind::Generator:
            return generator;
        case CoroutineKind::AsyncFunction:
            return promise;
        case CoroutineKind::AsyncGenerator:
            return asyncGenerator;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto driverParameterOf = [&](CoroutineKind kind) {
        switch (kind) {
        case CoroutineKind::Generator:
            return generator;
        case CoroutineKind::AsyncFunction:
            return promiseOrUndefined;
        case CoroutineKind::AsyncGenerator:
            return asyncGenerator;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    // The list of functions grows here and not again in this pass, so a pointer to a body stays valid.
    uint32_t nextFunction = editor.reserveFunctions(plans.size()).index();
    for (CoroutinePlan& plan : plans) {
        CreatorRecord::Coroutine& record = plan.record;
        record.body = FunctionID(nextFunction++);
        plan.bodySlot = &editor.at(record.body);
        plan.allocatedType = allocatedBy(record.kind);
        plan.driverType = driverParameterOf(record.kind);
        if (plan.neverSuspends) {
            record.promiseType = promise;
            record.worker = record.body;
            continue;
        }

        record.promiseParameter = program.at(record.ramp).numberOfEntryParameters();
        switch (record.kind) {
        case CoroutineKind::Generator:
            break;
        case CoroutineKind::AsyncFunction:
            record.promiseType = promise;
            record.entryWithoutPromise = record.body;
            break;
        case CoroutineKind::AsyncGenerator:
            record.promiseType = promise;
            break;
        }
    }

    Vector<FunctionID> ramps = plans.map([](const CoroutinePlan& plan) { return plan.record.ramp; });
    Vector<FunctionID> bodies = plans.map([](const CoroutinePlan& plan) { return plan.record.body; });
    editFunctionsInParallel(editor, build, ramps.span(), bodies.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        CoroutinePlan& plan = plans[index];
        AnalysisContext context { build, program, &inputs.flow, &inputs.effects, id };
        Function ramp = buildRamp(context, arenas.types, function, plan);
        context.id = plan.record.body;
        *plan.bodySlot = plan.neverSuspends ? buildAsyncWorker(arenas.types, WTF::move(function), plan) : buildCoroutineBody(context, arenas.types, WTF::move(function), plan);
        function = WTF::move(ramp);
    });

    retypeCallers(editor, build, plans);
    build.statistics().count(StatisticID::CoroutinesSplit, plans.size());
    build.statistics().count(StatisticID::AsyncWorkersMade, static_cast<uint64_t>(std::ranges::count_if(plans, &CoroutinePlan::neverSuspends)));
}

// ResolveAwaitedCalls.

namespace {

// One of the two is set: the body of a function that may suspend, which is entered without a promise, or the worker of one that never does.
struct AwaitableCallee {
    FunctionID entryWithoutPromise;
    FunctionID worker;

    explicit operator bool() const { return entryWithoutPromise || worker; }
};

// The call stands in the block of the await or in a block from which lone jumps lead there.
struct AwaitedCall {
    ValueID call;
    ValueID await;
    ProgramPoint point;
    AwaitableCallee callee;
    Vector<ValueID, 1> casts;
    // Every use of an iterator result that nobody but the reads of its two members gets to see: casts, tests for primitives, reads of `value` and `done`. Nothing where somebody gets to see it: then it is made.
    std::optional<Vector<ValueID, 4>> resultReads;
};

struct AwaitOfMergedValues {
    ValueID await;
    BlockID block;
    unsigned parameterIndex { 0 };
    Vector<BlockID, 4> predecessors;
};

// The tests at the head of a typed async function are a function of their own, which calls a ramp in each of its two arms. Only who asks what will stand here once the tests are taken in looks behind them.
enum class TestsAtHeadOfCallee : uint8_t { AreNoCallee, StandForTypedBody };

enum class AwaitableCalls : uint8_t { All, OnlyNextOfAsyncGenerator };

struct DirectCallElement {
    ValueID promiseAll;
    uint32_t elementIndex { 0 };
    ValueID call;
    ProgramPoint callPoint;
    FunctionID entry;
    Vector<ValueID, 1> casts;
};

} // anonymous namespace

// directCalls has a bit for each of the first elements.
AOT_THRESHOLD(uint32_t, maxDirectCallElements, 32);
AOT_THRESHOLD(size_t, maxJumpsBetweenCallAndAwait, 4);
AOT_THRESHOLD(unsigned, maxJoinsBetweenCallAndAwait, 3);

static AwaitableCallee findAwaitableCallee(const Program& program, const Value& call, TestsAtHeadOfCallee tests = TestsAtHeadOfCallee::AreNoCallee)
{
    if (call.opcode != Opcode::CallDirect)
        return { };
    const Function* callee = &program.at(call.function());
    if (const auto* entryChecks = callee->creator.tryAs<CreatorRecord::EntryChecks>(); entryChecks && tests == TestsAtHeadOfCallee::StandForTypedBody)
        callee = &program.at(entryChecks->body);
    const CreatorRecord::Coroutine* record = coroutineOf(*callee);
    if (!record || callee->role != FunctionRole::CoroutineRamp)
        return { };
    return { record->entryWithoutPromise, record->worker };
}

static bool isNextOfAsyncGenerator(FunctionPassContext& context, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || call.needsPristineReceiver())
        return false;
    if (context.program.builtins().at(call.overload()).builtin != Builtins::AsyncGeneratorPrototypeNext || call.argumentsOperands().size() != 1)
        return false;
    ValueID receiver = call.receiverOperand();
    if (!receiver)
        return false;
    const Type& generators = context.types.at(context.types.intern(Type::builtinClass(BuiltinClasses::AsyncGenerator)));
    return context.program.typeOf(context.function, receiver).isSubsetOf(generators, context.program);
}

// The engine makes the result afresh for each step, with `value` and `done` as its own data properties. While only those are read, nobody can tell whether it was made.
static bool collectIteratorResultReads(FunctionPassContext& context, ValueID result, Vector<ValueID, 4>& reads)
{
    const Program& program = context.program;
    for (const Use& use : context.analyses.useLists().usesOf(result)) {
        if (use.kind != Use::Kind::Operand)
            return false;
        // A user may take the result in two operands. It is listed once, because the edit goes by the opcode that a read has then.
        if (reads.contains(use.user))
            continue;
        const Value& user = context.function.at(use.user);
        reads.append(use.user);
        if (isCastWithoutTest(user.opcode)) {
            if (!collectIteratorResultReads(context, use.user, reads))
                return false;
            continue;
        }
        if (user.opcode == Opcode::TypeTest && !context.types.at(user.testedType()).hasObjects()) {
            // Behind a branch on the test the result has another name.
            for (ValueID parameter : findNarrowedParameters(context.function, context.analyses.useLists(), use)) {
                if (!context.analyses.cfg().singleIncomingEdge(context.analyses.useLists().blockOf(parameter)) || !collectIteratorResultReads(context, parameter, reads))
                    return false;
            }
            continue;
        }
        if (user.opcode != Opcode::PropGet || user.type != TypeTable::anything || (user.key() != program.keyOf(BuiltinNames::value) && user.key() != program.keyOf(BuiltinNames::done)))
            return false;
    }
    return true;
}

static Vector<ValueID, 8> listAwaits(FunctionPassContext& context)
{
    Vector<ValueID, 8> awaits;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID value : context.function.at(block).values) {
            if (context.function.at(value).opcode == Opcode::AwaitBySavingFrame)
                awaits.append(value);
        }
    }
    return awaits;
}

static bool hasOnlyPureValues(FunctionPassContext& context, std::span<const ValueID> values)
{
    return std::ranges::all_of(values, [&](ValueID value) { return context.analyses.hasNoEffects(value); });
}

// Nothing if a cast on the way has another use.
static ValueID definitionBehindOwnCasts(FunctionPassContext& context, ValueID value, Vector<ValueID, 1>& casts)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    for (; isCastWithoutTest(function.at(value).opcode); value = function.at(value).operand(0)) {
        if (!useLists.singleUse(value))
            return { };
        casts.append(value);
    }
    return value;
}

static void markErased(BitVector& marked, std::span<const ValueID> values)
{
    for (ValueID value : values)
        marked.set(value.index());
}

// What an edit leaves without a use stays listed until the edits are through, so the positions that were found still hold.
static void eraseEverywhere(IRBuilder& builder, const BitVector& marked)
{
    if (marked.isEmpty())
        return;
    for (uint32_t block = 0; block < builder.function().blocks.size(); ++block) {
        const BlockValues& values = builder.function().at(BlockID(block)).values;
        BitVector indexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (marked.get(values[index].index()))
                indexes.set(index);
        }
        if (!indexes.isEmpty())
            builder.eraseValues(BlockID(block), indexes);
    }
}

static bool isAwaitableCallAtEndOf(FunctionPassContext& context, BlockID block, ValueID incoming, AwaitableCalls wanted = AwaitableCalls::All)
{
    const Function& function = context.function;
    Vector<ValueID, 1> casts;
    ValueID call = definitionBehindOwnCasts(context, incoming, casts);
    if (!call || ((wanted == AwaitableCalls::OnlyNextOfAsyncGenerator || !findAwaitableCallee(context.program, function.at(call))) && !isNextOfAsyncGenerator(context, function.at(call))))
        return false;

    const UseLists& useLists = context.analyses.useLists();
    ValueLocation location = useLists.locationOf(call);
    if (!useLists.singleUse(call) || location.placement != ValuePlacement::InBlock || location.block != block)
        return false;
    return hasOnlyPureValues(context, function.at(block).values.span().subspan(location.index + 1));
}

// An expected type is set only where it says more than the type.
static Value makeAwaitLike(FunctionPassContext& context, const Value& original, ValueID awaited, std::span<const ValueID> held)
{
    Value await = IRBuilder::make(Opcode::AwaitBySavingFrame, { awaited, original.promiseOperand() });
    await.setHeldOperands(held);
    TypeID expected = original.expected().predicted();
    if (!expected)
        return await;
    TypeID type = resultTypeOf(context.program, context.function, await, context.types);
    if (expected != type && context.types.at(expected).isSubsetOf(context.types.at(type), context.program))
        await.setExpected(ExpectedType(expected));
    return await;
}

static bool hasTypeOfAwait(FunctionPassContext& context, const Value& await, ValueID awaited)
{
    Value moved = IRBuilder::make(Opcode::AwaitBySavingFrame, { awaited, await.promiseOperand() });
    const Type& type = context.types.at(resultTypeOf(context.program, context.function, moved, context.types));
    return type.isSubsetOf(context.types.at(await.type), context.program);
}

// The function that tests the arguments of a typed async function is inlined into the caller: the typed and the untyped version are called in two arms, and the await stands behind their merge.
static std::optional<AwaitOfMergedValues> findAwaitOfMergedValues(FunctionPassContext& context, ValueID await)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    if (function.at(await).takesExtraTurn() || function.kind == FunctionKind::Module)
        return std::nullopt;

    Vector<ValueID, 1> casts;
    ValueID parameter = definitionBehindOwnCasts(context, function.at(await).awaitedOperand(), casts);
    if (!parameter || !useLists.singleUse(parameter))
        return std::nullopt;

    ValueLocation awaitLocation = useLists.locationOf(await);
    ValueLocation parameterLocation = useLists.locationOf(parameter);
    RELEASE_ASSERT(awaitLocation.placement == ValuePlacement::InBlock);
    if (parameterLocation.placement != ValuePlacement::Parameter || parameterLocation.block != awaitLocation.block)
        return std::nullopt;

    BlockID block = awaitLocation.block;
    const Block& merge = function.at(block);
    if (merge.isHandlerBlock() || !hasOnlyPureValues(context, merge.values.span().first(awaitLocation.index)) || context.analyses.isEnteredFromUnreachableBlock(block))
        return std::nullopt;

    // An await that moves in front of the merge holds what is defined there.
    for (ValueID held : heldOperandsOf(function.at(await))) {
        ValueLocation location = useLists.locationOf(held);
        if (location.placement == ValuePlacement::InBlock && location.block == block)
            return std::nullopt;
    }

    AwaitOfMergedValues found { await, block, parameterLocation.index, { } };
    bool awaitsCall = false;
    for (Edge edge : context.analyses.cfg().incomingEdges(block)) {
        const Block& predecessor = function.at(edge.from);
        if (edge.isHandlerEdge() || predecessor.terminator.kind != TerminatorKind::Jump || predecessor.handler != merge.handler)
            return std::nullopt;
        const Arguments& arguments = predecessor.terminator.next().arguments;
        if (arguments.size() != merge.parameters.size())
            return std::nullopt;
        ValueID incoming = arguments[found.parameterIndex];
        if (!hasTypeOfAwait(context, function.at(await), incoming))
            return std::nullopt;
        awaitsCall |= isAwaitableCallAtEndOf(context, edge.from, incoming);
        found.predecessors.append(edge.from);
    }
    if (!awaitsCall)
        return std::nullopt;
    return found;
}

static unsigned sinkAwaitsOfMergedValues(FunctionPassContext& context, IRBuilder& builder)
{
    Function& function = context.function;
    Vector<AwaitOfMergedValues> sites;
    for (ValueID await : listAwaits(context)) {
        if (std::optional<AwaitOfMergedValues> site = findAwaitOfMergedValues(context, await))
            sites.append(WTF::move(*site));
    }
    BitVector casts;
    for (const AwaitOfMergedValues& site : sites) {
        Value await = function.at(site.await);
        ValueID parameter = function.at(site.block).parameters[site.parameterIndex];
        for (ValueID cast = await.awaitedOperand(); cast != parameter; cast = function.at(cast).operand(0))
            casts.set(cast.index());

        builder.setOrigin(await.origin());
        for (BlockID predecessor : site.predecessors) {
            const Arguments& arguments = function.at(predecessor).terminator.next().arguments;
            ValueID incoming = arguments[site.parameterIndex];
            Vector<ValueID, 8> held = WTF::map<8>(await.heldOperands(), [&](ValueID held) {
                size_t index = function.at(site.block).parameters.find(held);
                return index == notFound ? held : arguments[index];
            });
            ValueID result = builder.insertBefore(pointBeforeTerminatorOf(function, predecessor), makeAwaitLike(context, await, incoming, held.span()));
            function.edit(predecessor, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.next().arguments[site.parameterIndex] = result;
        }
        builder.applyInsertions();

        function.edit(parameter, ValueEdit::Type).type = await.type;
        function.edit(parameter, ValueEdit::Type).setExpected(await.expected());
        becomeUpcast(function, site.await, parameter);
    }
    eraseEverywhere(builder, casts);
    if (!sites.isEmpty())
        context.analyses.didChangeValues();
    return sites.size();
}

// The block has one parameter, and the values in front of `end` are casts of it, each with one use. Answers the last of them. Nothing of the block is missed by whoever goes round it with the value.
static ValueID valueHandedThrough(FunctionPassContext& context, BlockID block, uint32_t end)
{
    const UseLists& useLists = context.analyses.useLists();
    const Block& contents = context.function.at(block);
    if (contents.isHandlerBlock() || contents.parameters.size() != 1 || context.analyses.isEnteredFromUnreachableBlock(block))
        return { };
    ValueID handed = contents.parameters[0];
    for (uint32_t index = 0; index < end; ++index) {
        const Value& cast = context.function.at(contents.values[index]);
        if (!isCastWithoutTest(cast.opcode) || cast.operand(0) != handed || !useLists.singleUse(handed))
            return { };
        handed = contents.values[index];
    }
    return useLists.singleUse(handed) ? handed : ValueID();
}

static void collectBlocksThatCallNext(FunctionPassContext& context, const Value& await, BlockID join, const std::optional<Target>& handlerOfAwait, unsigned joinsLeft, Vector<BlockID, 2>& found)
{
    for (Edge edge : context.analyses.cfg().incomingEdges(join)) {
        const Block& predecessor = context.function.at(edge.from);
        if (edge.isHandlerEdge() || predecessor.terminator.kind != TerminatorKind::Jump || predecessor.terminator.next().arguments.size() != 1)
            continue;
        ValueID incoming = predecessor.terminator.next().arguments[0];
        if (isAwaitableCallAtEndOf(context, edge.from, incoming, AwaitableCalls::OnlyNextOfAsyncGenerator)) {
            if (predecessor.handler == handlerOfAwait && hasTypeOfAwait(context, await, incoming))
                found.append(edge.from);
            continue;
        }
        if (joinsLeft && valueHandedThrough(context, edge.from, predecessor.values.size()) == incoming)
            collectBlocksThatCallNext(context, await, edge.from, handlerOfAwait, joinsLeft - 1, found);
    }
}

// `next` of an async generator is called in one arm of several, and the arms meet, perhaps more than once, in front of the await. The arm that calls gets an await of its own and goes on behind the await of the others.
static bool giveCallersOfNextTheirOwnAwait(FunctionPassContext& context, IRBuilder& builder, ValueID id)
{
    Function& function = context.function;
    if (function.at(id).takesExtraTurn())
        return false;
    ValueLocation location = context.analyses.useLists().locationOf(id);
    RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
    if (valueHandedThrough(context, location.block, location.index) != function.at(id).awaitedOperand())
        return false;
    Vector<BlockID, 2> blocksThatCall;
    collectBlocksThatCallNext(context, function.at(id), location.block, function.at(location.block).handler, maxJoinsBetweenCallAndAwait - 1, blocksThatCall);
    if (blocksThatCall.isEmpty())
        return false;

    Value await = function.at(id);
    IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { location.block, location.index });
    builder.endExpansion(expansion, builder.append(makeAwaitLike(context, await, await.awaitedOperand(), await.heldOperands())));

    builder.setOrigin(await.origin());
    Vector<ValueID, 2> ownAwaits;
    for (BlockID block : blocksThatCall)
        ownAwaits.append(builder.insertBefore(pointBeforeTerminatorOf(function, block), makeAwaitLike(context, await, function.at(block).terminator.next().arguments[0], await.heldOperands())));
    builder.applyInsertions();
    for (size_t index = 0; index < ownAwaits.size(); ++index)
        builder.redirectEdge(Edge { blocksThatCall[index], 0 }, expansion.continuation, { ownAwaits[index] });
    context.analyses.didChangeValues();
    return true;
}

static std::optional<AwaitedCall> findAwaitedCall(FunctionPassContext& context, ValueID await, TestsAtHeadOfCallee tests = TestsAtHeadOfCallee::AreNoCallee)
{
    const Function& function = context.function;
    // Which queue resumes the body of a module depends on whether it awaits a promise, so the callee makes one.
    if (function.at(await).takesExtraTurn() || function.kind == FunctionKind::Module)
        return std::nullopt;

    Vector<ValueID, 1> casts;
    ValueID awaited = definitionBehindOwnCasts(context, function.at(await).awaitedOperand(), casts);
    if (!awaited)
        return std::nullopt;
    AwaitableCallee callee = findAwaitableCallee(context.program, function.at(awaited), tests);
    if (!callee && !isNextOfAsyncGenerator(context, function.at(awaited)))
        return std::nullopt;
    std::optional<Vector<ValueID, 4>> resultReads;
    if (!callee && !collectIteratorResultReads(context, await, resultReads.emplace()))
        resultReads = std::nullopt;
    // Where somebody takes the result itself, the step makes it, and a layout of which flow has seen no allocation has no cell.
    if (!callee && !resultReads && context.program.at(context.program.layoutOf(BuiltinLayouts::IteratorResult)).flow.allocation == AllocationState::NeverAllocated)
        return std::nullopt;

    const UseLists& useLists = context.analyses.useLists();
    if (!useLists.singleUse(awaited))
        return std::nullopt;

    ValueLocation callLocation = useLists.locationOf(awaited);
    ValueLocation awaitLocation = useLists.locationOf(await);
    RELEASE_ASSERT(awaitLocation.placement == ValuePlacement::InBlock);
    if (callLocation.placement != ValuePlacement::InBlock)
        return std::nullopt;

    // A pass that cuts a block behind a call leaves a jump there. Nothing else enters the block behind it, so what stood in one block still runs in one go.
    BlockID block = awaitLocation.block;
    uint32_t end = awaitLocation.index;
    for (size_t numberOfJumps = 0; block != callLocation.block; ++numberOfJumps) {
        const Block& contents = function.at(block);
        std::optional<Edge> edge = context.analyses.cfg().singleIncomingEdge(block);
        if (!edge || edge->isHandlerEdge() || contents.isHandlerBlock() || !contents.parameters.isEmpty() || context.analyses.isEnteredFromUnreachableBlock(block))
            return std::nullopt;
        const Block& predecessor = function.at(edge->from);
        if (predecessor.terminator.kind != TerminatorKind::Jump || predecessor.handler != contents.handler)
            return std::nullopt;
        if (numberOfJumps == maxJumpsBetweenCallAndAwait || !hasOnlyPureValues(context, contents.values.span().first(end)))
            return std::nullopt;
        block = edge->from;
        end = predecessor.values.size();
    }
    if (callLocation.index >= end || !hasOnlyPureValues(context, function.at(block).values.subspan(callLocation.index + 1, end - callLocation.index - 1)))
        return std::nullopt;
    return AwaitedCall { awaited, await, { awaitLocation.block, awaitLocation.index }, callee, WTF::move(casts), WTF::move(resultReads) };
}

static void enterCalleeWithoutPromise(FunctionPassContext& context, IRBuilder& builder, const AwaitedCall& site)
{
    Function& function = context.function;
    const EffectContext& effects = context.analyses.effectContext();
    ReplacedEffects replaced;
    replaced.add(effects, function, function.at(site.call));
    replaced.add(effects, function, function.at(site.await));

    TypeID type = function.at(site.await).type;
    auto becomeAwaitOfCall = [&, promise = function.at(site.await).promiseOperand(), expected = function.at(site.await).expected().predicted(), held = Vector<ValueID, 8>(function.at(site.await).heldOperands())](Value& value) {
        const Value& call = function.at(site.call);
        value.become(Opcode::AwaitCallBySavingFrame);
        value.setFunction(site.callee.entryWithoutPromise);
        value.setPromiseOperand(promise);
        value.setArgumentsOperands(call.argumentsOperands());
        setHeldOperands(value, held.span());
        value.setOrigin(call.origin());
        value.setAccessSite(call.accessSite());
        value.setExpected(ExpectedType(expected));
    };
    Value successor;
    becomeAwaitOfCall(successor);
    if (context.types.at(resultTypeOf(context.program, function, successor, context.types)).isSubsetOf(context.types.at(type), context.program)) {
        becomeAwaitOfCall(function.edit(site.await));
        checkReplacement(effects, function, { context.id, site.point.block, site.await }, function.at(site.await), replaced, context.build.diagnostics());
        return;
    }

    IRBuilder::Expansion expansion = builder.beginExpansion(site.point);
    Origin awaitOrigin = builder.origin();
    builder.setOrigin(successor.origin());
    ValueID result = builder.append(WTF::move(successor));
    checkReplacement(effects, function, { context.id, site.point.block, result }, function.at(result), replaced, context.build.diagnostics());

    builder.setOrigin(awaitOrigin);
    builder.endExpansion(expansion, convertTo(builder, result, DeclaredType(Proven::byPosition(type))));
}

// The promise of a function that never suspends is settled when the function returns, so the caller awaits what it would have been settled with.
static void expandAwaitOfWorker(FunctionPassContext& context, IRBuilder& builder, const AwaitedCall& site)
{
    Function& function = context.function;
    TypeArena& types = context.types;
    Value await = function.at(site.await);
    Value call = function.at(site.call);
    call.setFunction(site.callee.worker);
    call.setExpected({ });

    IRBuilder::Expansion expansion = builder.beginExpansion(site.point);
    Origin awaitOrigin = builder.origin();
    BlockFrequency frequency = function.at(site.point.block).frequency;

    BlockID rejection = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
    BlockID awaiting = builder.addBlock(frequency);
    builder.setHandler(Target { rejection, { }, { } });
    BlockID calling = builder.addBlock(frequency);
    builder.setHandler(expansion.handler);

    builder.setOrigin(call.origin());
    builder.emitJump(calling);
    builder.moveToEndOf(calling);
    ValueID result = builder.append(WTF::move(call));
    TypeID resultType = function.at(result).type;
    ValueID awaited = builder.addParameter(awaiting, types.unionOf(resultType, types.intern(Type::builtinClass(BuiltinClasses::Promise))));

    if (resultType == TypeTable::never)
        builder.emitUnreachable();
    else if (!protocolMayFindMember(context.program, types.at(resultType), Protocol::Thenable))
        builder.emitJump(awaiting, { result });
    else {
        BlockID resolving = builder.addBlock(rarerOf(frequency, BlockFrequency::Unlikely));
        builder.emitFailureOfPredictions(Target { resolving, { }, { } });
        builder.emitBranch(builder.append(Opcode::IsNotThenable, { result }), Target { awaiting, { result }, { } }, Target { resolving, { }, { } });

        builder.moveToEndOf(resolving);
        builder.emitJump(awaiting, { appendSettledPromise(builder, Opcode::ResolvePromise, result) });
    }

    builder.moveToEndOf(rejection);
    builder.emitJump(awaiting, { appendSettledPromise(builder, Opcode::RejectPromise, builder.append(Opcode::CatchValue, { })) });

    builder.setOrigin(awaitOrigin);
    builder.moveToEndOf(awaiting);
    builder.endExpansion(expansion, convertTo(builder, builder.append(makeAwaitLike(context, await, awaited, await.heldOperands())), DeclaredType(Proven::byPosition(await.type))));
}

// `for await` is Await(Call(next)) as well, so the step that serves it takes as many turns of the job queue as the call and the await.
static void expandAwaitOfGeneratorStep(FunctionPassContext& context, IRBuilder& builder, const AwaitedCall& site, BitVector& erasedValues)
{
    Function& function = context.function;
    PropertyKeyID valueKey = context.program.keyOf(BuiltinNames::value);
    PropertyKeyID doneKey = context.program.keyOf(BuiltinNames::done);
    ValueID generator = function.at(site.call).receiverOperand();
    ValueID sent = function.at(site.call).argumentsOperands()[0];
    Origin callOrigin = function.at(site.call).origin();
    const BuiltinTable& builtins = context.program.builtins();
    BuiltinID calledBuiltin = builtins.canonicalID(builtins.at(function.at(site.call).overload()).builtin);
    BlockFrequency frequency = function.at(site.point.block).frequency;
    TypeID type = function.at(site.await).type;
    Vector<ValueID, 8> held(heldOperandsOf(function.at(site.await)));

    auto appendWithKey = [&](Opcode opcode, std::initializer_list<ValueID> operands, PropertyKeyID key) {
        Value access = IRBuilder::make(opcode, operands);
        access.setKey(key);
        return builder.append(WTF::move(access));
    };

    IRBuilder::Expansion expansion = builder.beginExpansion(site.point);
    builder.setOrigin(callOrigin);
    ValueID driver = builder.append(Opcode::Absent, { });
    ValueID value = site.resultReads ? builder.addParameter(expansion.continuation, TypeTable::anything) : ValueID();
    ValueID done = site.resultReads ? builder.addParameter(expansion.continuation, TypeTable::anything) : ValueID();

    Value stepOfGenerator = IRBuilder::make(Opcode::StepAsyncGenerator, { generator, sent, driver });
    stepOfGenerator.setCalledBuiltin(calledBuiltin);
    stepOfGenerator.setHeldOperands(held.span());
    ValueID step = builder.append(WTF::move(stepOfGenerator));
    ValueID stepped = builder.append(makeResultAt(step, static_cast<uint32_t>(StepGeneratorResult::Value)));
    ValueID status = builder.append(makeResultAt(step, static_cast<uint32_t>(StepGeneratorResult::Status)));
    auto appendStatusTest = [&](AsyncGeneratorStepStatus expected) {
        return builder.append(Opcode::StrictEq, { status, builder.appendInteger(static_cast<int64_t>(expected)) });
    };
    IRBuilder::BranchBlocks blocks = builder.emitBranch(appendStatusTest(AsyncGeneratorStepStatus::Result), BlockFrequency::Rare, frequency);

    if (!site.resultReads) {
        builder.endExpansion(expansion, convertTo(builder, stepped, DeclaredType(Proven::byPosition(type))));

        builder.moveToEndOf(blocks.notTaken);
        ValueID object = builder.append(makeIteratorResultObject(context.program, stepped, appendStatusTest(AsyncGeneratorStepStatus::Completed)));
        builder.endExpansion(expansion, convertTo(builder, object, DeclaredType(Proven::byPosition(type))));
        return;
    }

    auto continueWith = [&](ValueID stepValue, ValueID stepDone) {
        Value unread = IRBuilder::make(Opcode::Unspecified);
        unread.setValueType(type);
        builder.emitJump(expansion.continuation, { builder.append(WTF::move(unread)), stepValue, stepDone });
    };
    Value check = IRBuilder::make(Opcode::RequireObject, { stepped });
    check.setRuntimeError(RuntimeErrorKind::IteratorResultNotObject);
    ValueID object = builder.append(WTF::move(check));
    ValueID valueOfObject = appendWithKey(Opcode::PropGet, { object }, valueKey);
    continueWith(valueOfObject, appendWithKey(Opcode::PropGet, { object }, doneKey));

    builder.moveToEndOf(blocks.notTaken);
    continueWith(stepped, appendStatusTest(AsyncGeneratorStepStatus::Completed));

    for (ValueID read : *site.resultReads) {
        const Value& user = function.at(read);
        if (isCastWithoutTest(user.opcode))
            erasedValues.set(read.index());
        else if (user.opcode == Opcode::TypeTest) {
            // The result is an object, and so is the generator: a test for primitives answers the same for both.
            function.edit(read, ValueEdit::Definition).setValueOperand(generator);
        } else {
            RELEASE_ASSERT(user.opcode == Opcode::PropGet);
            becomeUpcast(function, read, user.key() == valueKey ? value : done);
        }
    }
}

static size_t rewriteAwaitedCalls(FunctionPassContext& context, IRBuilder& builder, AwaitableCalls wanted)
{
    const Function& function = context.function;
    Vector<AwaitedCall> sites;
    for (ValueID await : listAwaits(context)) {
        if (wanted == AwaitableCalls::OnlyNextOfAsyncGenerator && function.at(sameObjectThroughCasts(function, function.at(await).awaitedOperand())).opcode != Opcode::CallBuiltin)
            continue;
        if (std::optional<AwaitedCall> site = findAwaitedCall(context, await))
            sites.append(WTF::move(*site));
    }
    if (sites.isEmpty())
        return 0;

    // Sites of one block are rewritten last position first: an expansion cuts its block, and what stands in front of the cut keeps its position.
    BitVector erasedValues;
    for (const AwaitedCall& site : sites | std::views::reverse) {
        if (!site.callee)
            expandAwaitOfGeneratorStep(context, builder, site, erasedValues);
        else if (site.callee.worker)
            expandAwaitOfWorker(context, builder, site);
        else
            enterCalleeWithoutPromise(context, builder, site);
        erasedValues.set(site.call.index());
        markErased(erasedValues, site.casts.span());
    }
    eraseEverywhere(builder, erasedValues);
    context.analyses.didChangeValues();
    context.build.statistics().count(StatisticID::AwaitedCallsResolved, sites.size());
    return sites.size();
}

// What a call without a promise returns is for the operation alone. Until the operation has it, nothing runs but the calls for the other elements.
static void findDirectCallElementsOf(FunctionPassContext& context, ProgramPoint promiseAllPoint, Vector<DirectCallElement>& found)
{
    const Function& function = context.function;
    const BlockValues& values = function.at(promiseAllPoint.block).values;
    ValueID promiseAll = values[promiseAllPoint.index];

    Vector<DirectCallElement, 4> candidates;
    uint32_t numberOfElements = std::min<uint32_t>(function.at(promiseAll).elementsOperands().size(), maxDirectCallElements);
    for (uint32_t elementIndex = 0; elementIndex < numberOfElements; ++elementIndex) {
        DirectCallElement element { .promiseAll = promiseAll, .elementIndex = elementIndex };
        element.call = definitionBehindOwnCasts(context, function.at(promiseAll).elementsOperands()[elementIndex], element.casts);
        if (!element.call)
            continue;
        element.entry = findAwaitableCallee(context.program, function.at(element.call)).entryWithoutPromise;
        if (element.entry && context.analyses.useLists().singleUse(element.call))
            candidates.append(WTF::move(element));
    }

    for (uint32_t index = promiseAllPoint.index; index-- && !candidates.isEmpty();) {
        size_t position = candidates.findIf([&](const DirectCallElement& candidate) { return candidate.call == values[index]; });
        if (position != notFound) {
            candidates[position].callPoint = ProgramPoint { promiseAllPoint.block, index };
            found.append(WTF::move(candidates[position]));
            candidates.removeAt(position);
            continue;
        }
        if (!context.analyses.hasNoEffects(values[index]))
            return;
    }
}

static bool retargetElementsOfPromiseAll(FunctionPassContext& context, IRBuilder& builder)
{
    Function& function = context.function;
    Vector<DirectCallElement> elements;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (function.at(values[index]).opcode == Opcode::PromiseAllOfElements)
                findDirectCallElementsOf(context, ProgramPoint { block, index }, elements);
        }
    }
    if (elements.isEmpty())
        return false;

    BitVector casts;
    for (const DirectCallElement& element : elements) {
        markErased(casts, element.casts.span());
        builder.setOrigin(function.at(element.call).origin());
        ValueID driver = builder.insertBefore(element.callPoint, IRBuilder::make(Opcode::Undefined));

        Value& call = function.edit(element.call);
        call.setFunction(element.entry);
        call.appendVariadicOperand(driver);
        call.type = resultTypeOf(context.program, function, call, context.types);
        call.setExpected({ });

        Value& promiseAll = function.edit(element.promiseAll);
        promiseAll.setOperand(OperandIndex::PromiseAllOfElements::elements + element.elementIndex, element.call);
        promiseAll.setDirectCalls(promiseAll.directCalls() | 1u << element.elementIndex);
    }
    builder.applyInsertions();
    eraseEverywhere(builder, casts);
    context.analyses.didChangeValues();
    return true;
}

static bool isBodyOfAsyncFunction(const Function& function)
{
    return function.role == FunctionRole::CoroutineBody && coroutineOfBody(function).kind == CoroutineKind::AsyncFunction;
}

bool willEnterAwaitedCalleeWithoutPromise(FunctionPassContext& context, ValueID call)
{
    const Function& function = context.function;
    if (!isBodyOfAsyncFunction(function) || !findAwaitableCallee(context.program, function.at(call), TestsAtHeadOfCallee::StandForTypedBody))
        return false;

    const UseLists& useLists = context.analyses.useLists();
    std::optional<Use> mergingUse;
    for (ValueID promise = call;;) {
        std::optional<Use> use = useLists.singleUse(promise);
        if (!use)
            return false;
        if (use->kind == Use::Kind::EdgeArgument && !mergingUse) {
            const CompactList<ValueID>& parameters = function.at(targetOf(function, use->edge()).block).parameters;
            if (use->operandIndex >= parameters.size() || !isAwaitableCallAtEndOf(context, use->block, promise))
                return false;
            mergingUse = use;
            promise = parameters[use->operandIndex];
            continue;
        }
        if (use->kind != Use::Kind::Operand)
            return false;
        Opcode user = function.at(use->user).opcode;
        if (isCastWithoutTest(user)) {
            promise = use->user;
            continue;
        }
        if (user != Opcode::AwaitBySavingFrame || use->operandIndex != OperandIndex::AwaitBySavingFrame::awaited)
            return false;
        if (mergingUse)
            return findAwaitOfMergedValues(context, use->user).has_value();
        std::optional<AwaitedCall> site = findAwaitedCall(context, use->user, TestsAtHeadOfCallee::StandForTypedBody);
        return site && site->call == call;
    }
}

unsigned stepAwaitedAsyncGenerators(FunctionPassContext& context)
{
    const Function& function = context.function;
    if (function.role != FunctionRole::CoroutineBody || coroutineOfBody(function).kind == CoroutineKind::Generator)
        return 0;
    if (!context.analyses.opcodesOfValues().contains(Opcode::AwaitBySavingFrame))
        return 0;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    unsigned numberOfOwnAwaits = 0;
    if (context.analyses.opcodesOfValues().contains(Opcode::CallBuiltin)) {
        // Each one changes the blocks, so the next one is looked for anew.
        while (listAwaits(context).containsIf([&](ValueID await) { return giveCallersOfNextTheirOwnAwait(context, builder, await); }))
            ++numberOfOwnAwaits;
    }
    context.build.statistics().count(StatisticID::AwaitsSunkIntoPredecessors, numberOfOwnAwaits);
    return rewriteAwaitedCalls(context, builder, AwaitableCalls::OnlyNextOfAsyncGenerator) + numberOfOwnAwaits;
}

PassOutcome resolveAwaitedCalls(FunctionPassContext& context)
{
    bool awaitsCalls = isBodyOfAsyncFunction(context.function);
    bool collectsCalls = context.function.values.containsIf([](const Value& value) { return value.opcode == Opcode::PromiseAllOfElements; });
    if (!awaitsCalls && !collectsCalls)
        return PassOutcome::Unchanged;

    size_t numberOfBlocksBefore = context.function.blocks.size();
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    bool didChange = collectsCalls && retargetElementsOfPromiseAll(context, builder);
    if (awaitsCalls) {
        if (unsigned numberOfSunkAwaits = sinkAwaitsOfMergedValues(context, builder)) {
            context.build.statistics().count(StatisticID::AwaitsSunkIntoPredecessors, numberOfSunkAwaits);
            didChange = true;
        }
        if (rewriteAwaitedCalls(context, builder, AwaitableCalls::All))
            didChange = true;
    }
    if (!didChange)
        return PassOutcome::Unchanged;
    // Only an await that is expanded changes an edge, and it adds blocks.
    if (context.function.blocks.size() == numberOfBlocksBefore)
        context.keptAnalyses = controlFlowAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
