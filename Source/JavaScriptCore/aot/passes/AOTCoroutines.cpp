/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutines.h"
#include "AOTSummarizeForInlining.h"
#include "AOTSavedFrame.h"
#include "AOTSSARepair.h"
#include "AOTLiveness.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTable.h"
#include "AOTDecidedBranches.h"
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
#include "AOTSort.h"
#include "AOTSpecifiedValues.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

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
        declared.establish(Proven::byConstruction(met != TypeTable::never && types.at(met).isSubsetOf(types.at(plan.allocatedType), context.program) ? met : plan.allocatedType));
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

namespace {

// What the merged await rejects with arrives at the yield as a value that is sent in to be thrown. What is proven of the values that are sent in holds in the arms that do not throw.
struct ProofsOfSentValue {
    struct UseInArm {
        Use use;
        uint32_t proof;
        // None for the arm that throws.
        std::optional<uint32_t> arm;
    };
    struct Arm {
        Edge edge;
        bool needsOwnBlock;
    };

    BlockID block;
    ValueID sent;
    Vector<ValueID, 4> proofs;
    Vector<Arm, 2> otherArms;
    Vector<UseInArm, 8> uses;
};

} // anonymous namespace

static bool provesWithoutTest(const Value& value)
{
    return isCastWithoutTest(value.opcode) || value.opcode == Opcode::ValidateType;
}

// Nothing where a proof cannot leave its place in front of the switch on the mode.
static std::optional<ProofsOfSentValue> findProofsOfSentValue(BodyRewrite& body, BlockID block, ValueID yield)
{
    const Function& function = body.function;
    const UseLists& useLists = body.analyses.useLists();
    ProofsOfSentValue found { .block = block };
    for (const Use& use : useLists.usesOf(yield)) {
        if (use.kind != Use::Kind::Operand || function.at(use.user).opcode != Opcode::ResultAt || function.at(use.user).index() != static_cast<uint32_t>(YieldResult::Sent))
            continue;
        if (found.sent || use.block != block)
            return std::nullopt;
        found.sent = use.user;
    }
    if (!found.sent)
        return found;
    for (ValueID id : function.at(block).values) {
        const Value& value = function.at(id);
        if (provesWithoutTest(value) && (value.operand(0) == found.sent || found.proofs.contains(value.operand(0))))
            found.proofs.append(id);
    }

    const Terminator& terminator = function.at(block).terminator;
    std::optional<Edge> throwingEdge;
    if (terminator.kind == TerminatorKind::Switch) {
        const CFG& cfg = body.analyses.cfg();
        for (uint32_t index = 0; index < terminator.targets.size(); ++index) {
            Edge edge { block, index };
            BlockID target = terminator.targets[index].block;
            if (index < terminator.cases().size() && terminator.cases()[index] == static_cast<int64_t>(ResumeMode::Throw))
                throwingEdge = edge;
            else
                found.otherArms.append({ edge, cfg.singleIncomingEdge(target) != edge || !!function.at(target).handler });
        }
    }
    if (!throwingEdge) {
        if (!found.proofs.isEmpty())
            return std::nullopt;
        found.otherArms.clear();
    }
    auto armOf = [&](const Use& use) -> std::optional<uint32_t> {
        size_t arm = found.otherArms.findIf([&](const ProofsOfSentValue::Arm& candidate) {
            if (use.kind == Use::Kind::EdgeArgument && use.block == block)
                return use.edge() == candidate.edge;
            return body.analyses.dominates(candidate.edge, use.block);
        });
        return arm == notFound ? std::nullopt : std::optional<uint32_t>(static_cast<uint32_t>(arm));
    };

    // Whoever proves something of the value farther on does so in an arm that does not throw.
    for (const Use& use : useLists.usesOf(found.sent)) {
        if (use.kind == Use::Kind::Operand && provesWithoutTest(function.at(use.user)) && !found.proofs.contains(use.user) && !armOf(use))
            return std::nullopt;
    }
    for (uint32_t proof = 0; proof < found.proofs.size(); ++proof) {
        for (const Use& use : useLists.usesOf(found.proofs[proof])) {
            if (use.kind == Use::Kind::Operand && found.proofs.contains(use.user))
                continue;
            if (use.kind == Use::Kind::BlockSubject)
                return std::nullopt;
            if (std::optional<uint32_t> arm = armOf(use)) {
                if (use.block == block)
                    found.otherArms[*arm].needsOwnBlock = true;
                found.uses.append({ use, proof, arm });
                continue;
            }
            // The arm that throws has the value as it came, and nothing there may hang on its type. Somebody else reads it where several arms meet, or in front of the switch.
            bool isThrown = use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.kind == TerminatorKind::Throw;
            if (!isThrown || !body.analyses.dominates(*throwingEdge, use.block))
                return std::nullopt;
            found.uses.append({ use, proof, std::nullopt });
        }
    }
    return found;
}

static void moveProofsOfSentValuesBehindModes(BodyRewrite& body, std::span<const ProofsOfSentValue> all)
{
    Function& function = body.function;
    IRBuilder& builder = body.builder;

    auto keyOf = [](Edge edge) {
        return static_cast<uint64_t>(edge.from.index()) << 32 | edge.targetIndex;
    };
    UncheckedKeyHashMap<uint64_t, BlockID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> blocksOnEdges;
    for (const ProofsOfSentValue& found : all) {
        for (const ProofsOfSentValue::Arm& arm : found.otherArms) {
            if (arm.needsOwnBlock)
                blocksOnEdges.add(keyOf(arm.edge), builder.splitEdge(arm.edge));
        }
    }
    // The arguments of an edge have gone to the block on it.
    auto whereItIsNow = [&](Use use) {
        if (use.kind != Use::Kind::EdgeArgument)
            return use;
        if (BlockID added = blocksOnEdges.get(keyOf(use.edge()))) {
            use.block = added;
            use.targetIndex = 0;
        }
        return use;
    };

    for (const ProofsOfSentValue& found : all) {
        Vector<ValueID, 8> copies;
        for (size_t arm = 0; arm < found.otherArms.size(); ++arm) {
            BlockID head = targetOf(function, found.otherArms[arm].edge).block;
            for (ValueID proof : found.proofs) {
                Value copy = function.at(proof);
                size_t operand = found.proofs.find(copy.operand(0));
                if (operand != notFound)
                    copy.setOperand(0, copies[arm * found.proofs.size() + operand]);
                builder.setOrigin(copy.origin());
                copies.append(builder.insertBefore(ProgramPoint::atHeadOf(head), WTF::move(copy)));
            }
        }
        for (const ProofsOfSentValue::UseInArm& use : found.uses)
            replaceUse(function, whereItIsNow(use.use), use.arm ? copies[*use.arm * found.proofs.size() + use.proof] : found.sent);
    }
    builder.applyInsertions();

    for (const ProofsOfSentValue& found : all) {
        const BlockValues& values = function.at(found.block).values;
        BitVector proofIndexes(values.size());
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (found.proofs.contains(values[index]))
                proofIndexes.quickSet(index);
        }
        builder.eraseValues(found.block, proofIndexes);
    }
    body.analyses.didChangeValues();
}

static void mergeAwaitsIntoYields(BodyRewrite& body)
{
    struct AwaitedYield {
        BlockID block;
        ValueID await;
        ValueID yield;
    };
    const Function& function = body.function;
    const UseLists& useLists = body.analyses.useLists();
    Vector<AwaitedYield> merges;
    Vector<ProofsOfSentValue> proofsToMove;
    auto mergeWhereProofsCanMove = [&](AwaitedYield awaitedYield) {
        std::optional<ProofsOfSentValue> found = findProofsOfSentValue(body, awaitedYield.block, awaitedYield.yield);
        if (!found)
            return;
        merges.append(awaitedYield);
        if (!found->proofs.isEmpty())
            proofsToMove.append(WTF::move(*found));
    };
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
                mergeWhereProofsCanMove({ point.block, await, use->user });
                return;
            }
            if (!isCastWithoutTest(user))
                return;
        }
    });

    // An await that goes may be what reads a proof, so the proofs move while every use is where it was found.
    if (!proofsToMove.isEmpty())
        moveProofsOfSentValuesBehindModes(body, proofsToMove.span());

    for (const AwaitedYield& merge : merges) {
        rewriteInPlace(body, merge.block, merge.yield, Opcode::AsyncGeneratorYield, { body.driver, function.at(merge.await).valueOperand() }, { merge.await, merge.yield });
        const BlockValues& values = function.at(merge.block).values;
        BitVector mergedIndexes;
        for (size_t index = values.find(merge.await); values[index] != merge.yield; ++index)
            mergedIndexes.set(index);
        body.builder.eraseValues(merge.block, mergedIndexes);
    }
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

// For each suspension that can list what is held: what is live after it and after no later suspension. Listed there, a value is live after the earlier ones too,
// so each value is listed about once, however many suspensions it lives across. The frame map of a suspension has all that is live after it.
// The functor lists all that it is given: a value counts as held at the earlier suspensions because it is listed at this one.
static void forEachSuspensionWithLastCrossings(const Function& function, FunctionAnalyses& analyses, NOESCAPE const Invocable<bool(const Value&)> auto& lists, NOESCAPE const Invocable<Liveness::LastCrossing> auto& functor)
{
    analyses.liveness().forEachLastCrossing([&](ValueID id) {
        return canHoldOperands(function.at(id).opcode) && analyses.maySuspend(id) && lists(function.at(id));
    }, functor);
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
    auto lists = [](const Value& suspension) {
        return (suspension.opcode != Opcode::IteratorResume && suspension.opcode != Opcode::IteratorClose) || suspension.iteratorKind() != IteratorKind::Sync;
    };
    forEachSuspensionWithLastCrossings(function, context.analyses, lists, [&](ValueID suspension, std::span<const ValueID> values, const Liveness::IsLive&) {
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
        for (ValueID live : values) {
            // An iteration holds what it walks, and a closure what it captures: one that is only called need not be made.
            const Value& definition = function.at(sameObjectThroughConversionsToObject(function, live));
            if (definition.opcode == Opcode::IteratorOpen)
                list(definition.iterableOperand());
            else if (definition.opcode == Opcode::NewClosure) {
                for (ValueID capture : definition.capturesOperands())
                    list(capture);
            } else
                list(live);
        }
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
    auto lists = [](const Value&) {
        return true;
    };
    forEachSuspensionWithLastCrossings(function, body.analyses, lists, [&](ValueID suspension, std::span<const ValueID> values, const Liveness::IsLive& isLive) {
        held.shrink(0);
        auto holdUnlessLive = [&](ValueID id) {
            if (!isLive(id) && !held.contains(id))
                held.append(id);
        };
        for (ValueID parameter : parameters)
            holdUnlessLive(parameter);
        for (ValueID listed : heldOperandsOf(function.at(suspension))) {
            if (isHeld(listed))
                holdUnlessLive(listed);
        }
        for (ValueID live : values) {
            if (isHeld(live))
                held.append(live);
        }
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
            call.setExpected(expectedTypeOf(program, caller, call, arenas.types));
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
    // A name computed at run time finds an entry point or a file marked IsFoundByName. Where it finds one entry point and nothing else, that one has begun before any require() runs.
    if (BitVector found = modulesThatHostAsksFor(program, flow.findsFilesOfProgramByName()); found.bitCount() > 1 && loadsModulesByName(flow)) {
        for (size_t index : found)
            work.append(index);
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
    Expected<BlockFrequency> frequency = function.at(site.point.block).frequency;

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
    TypeID promiseType = types.intern(Type::builtinClass(BuiltinClasses::Promise));
    // A result that is never a thenable meets a promise only where the worker throws. That promise has an await of its own, so the common await knows that it takes none.
    bool rejectionHasOwnAwait = resultType != TypeTable::never && !protocolMayFindMember(context.program, types.at(resultType), Protocol::Thenable);
    ValueID awaited = builder.addParameter(awaiting, rejectionHasOwnAwait ? resultType : types.unionOf(resultType, promiseType));

    if (resultType == TypeTable::never)
        builder.emitUnreachable();
    else if (rejectionHasOwnAwait)
        builder.emitJump(awaiting, { result });
    else {
        BlockID resolving = builder.addBlock(rarerOf(frequency, BlockFrequency::Unlikely));
        builder.emitFailureOfPredictions(Target { resolving, { }, { } });
        builder.emitBranch(builder.append(Opcode::IsNotThenable, { result }), Target { awaiting, { result }, { } }, Target { resolving, { }, { } });

        builder.moveToEndOf(resolving);
        builder.emitJump(awaiting, { appendSettledPromise(builder, Opcode::ResolvePromise, result) });
    }

    auto appendAwait = [&](ValueID value) {
        builder.setOrigin(awaitOrigin);
        return convertTo(builder, builder.append(makeAwaitLike(context, await, value, await.heldOperands())), DeclaredType(Proven::byPosition(await.type)));
    };
    builder.moveToEndOf(rejection);
    ValueID rejected = appendSettledPromise(builder, Opcode::RejectPromise, builder.append(Opcode::CatchValue, { }));
    if (!rejectionHasOwnAwait) {
        builder.emitJump(awaiting, { rejected });
        builder.moveToEndOf(awaiting);
        builder.endExpansion(expansion, appendAwait(awaited));
        return;
    }

    ValueID awaitedRejection = appendAwait(rejected);
    BlockID resumed = builder.addBlock(frequency);
    ValueID outcome = builder.addParameter(resumed, await.type);
    builder.moveToEndOf(rejection);
    builder.emitJump(resumed, { awaitedRejection });
    builder.moveToEndOf(awaiting);
    builder.emitJump(resumed, { appendAwait(awaited) });
    builder.moveToEndOf(resumed);
    builder.endExpansion(expansion, outcome);
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
    Expected<BlockFrequency> frequency = function.at(site.point.block).frequency;
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
    nameFactsWhereUnspecifiedValuesAreUsed(builder, context.analyses);
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

// MoveGeneratorFramesToHeap.

namespace {

// A frame larger than this keeps the form that copies the stack.
AOT_THRESHOLD(uint32_t, maxWordsOfHeapFrame, 256);

constexpr uint32_t noWord = UINT32_MAX;
constexpr uint32_t wordOfGenerator = 0;

struct YieldInFrame {
    ValueID yield;
    ProgramPoint point;
    Vector<ValueID, 2> resultReads;
    // Live behind the yield, in a word.
    Vector<ValueID, 8> stored;
    // Live behind the yield, and made again there.
    Vector<ValueID, 4> constants;
    // Listed by the yield and not read behind it: they stay alive until the next yield.
    Vector<ValueID, 2> heldOnly;
    // The words that a run that ends at this yield may have found filled and that hold nothing that lives on.
    Vector<uint32_t, 4> wordsToClear;
};

// Where the program has let go of a variable: `x = null`.
struct ReleaseInFrame {
    ValueID hint;
    Vector<ValueID, 8> live;
    // The words that a run may have found filled when it comes here and that hold nothing that lives on.
    Vector<uint32_t, 4> wordsToClear;
};

struct HeapFramePlan {
    FunctionID body;
    FunctionID resume;
    Function* resumeSlot { nullptr };
    ValueID generator;
    Vector<YieldInFrame, 4> yields;
    Vector<ReleaseInFrame, 2> releases;
    Vector<uint32_t> wordOf;
    uint32_t numberOfWords { 1 };
    // Where whoever starts the generator fills the frame himself: the entry parameter of the body that each word takes at the initial yield.
    struct WordAtStart {
        uint32_t word;
        uint32_t parameter;
    };
    Vector<WordAtStart, 8> wordsAtStart;
    bool isStartedByCallers { false };
};

struct StartOfGenerator {
    ValueID call;
    uint32_t plan { 0 };
    bool isRunning { false };
};

} // anonymous namespace

static bool canLiveInWord(const Program& program, const Function& function, ValueID id)
{
    return program.typeOf(function, id).isJavaScriptValue();
}

// An edge that narrows an operand of the test asks the branch's condition which operand, and a word read back is no test: the resume function casts on the edge instead. A guard only the test itself makes.
static bool isTestThatMakesGuards(FunctionAnalyses& analyses, ValueID id)
{
    const Function& function = analyses.function();
    return std::ranges::any_of(analyses.useLists().usesOf(id), [&](const Use& use) {
        if (use.kind != Use::Kind::TerminatorOperand || !function.at(use.block).terminator.isConditionalBranch())
            return false;
        for (uint32_t targetIndex = 0; targetIndex < function.at(use.block).terminator.targets.size(); ++targetIndex) {
            if (!canFollowWithoutTest(function, Edge { use.block, targetIndex }))
                return true;
        }
        return false;
    });
}

static void assignWords(HeapFramePlan& plan, unsigned numberOfValues)
{
    plan.wordOf.fill(noWord, numberOfValues);
    plan.wordOf[plan.generator.index()] = wordOfGenerator;
    Vector<Vector<uint32_t, 2>> yieldsOfValue(numberOfValues);
    Vector<ValueID, 16> values;
    for (uint32_t index = 0; index < plan.yields.size(); ++index) {
        for (auto list : { plan.yields[index].stored.span(), plan.yields[index].heldOnly.span() }) {
            for (ValueID id : list) {
                if (yieldsOfValue[id.index()].isEmpty())
                    values.append(id);
                yieldsOfValue[id.index()].append(index);
            }
        }
    }
    // For each word, the yields at which it is taken.
    Vector<BitVector, 8> yieldsOfWord;
    yieldsOfWord.append(BitVector(plan.yields.size()));
    for (ValueID id : values) {
        const Vector<uint32_t, 2>& yields = yieldsOfValue[id.index()];
        uint32_t word = wordOfGenerator + 1;
        for (; word < yieldsOfWord.size(); ++word) {
            if (std::ranges::none_of(yields, [&](uint32_t yield) { return yieldsOfWord[word].quickGet(yield); }))
                break;
        }
        if (word == yieldsOfWord.size())
            yieldsOfWord.append(BitVector(plan.yields.size()));
        for (uint32_t yield : yields)
            yieldsOfWord[word].quickSet(yield);
        plan.wordOf[id.index()] = word;
    }
    plan.numberOfWords = yieldsOfWord.size();
}

// What a run that begins behind a yield may find filled when it comes to another: the words of each yield from which that one is reached without passing a third.
static void findWordsToClear(HeapFramePlan& plan, const Function& function, FunctionAnalyses& analyses)
{
    const CFG& cfg = analyses.cfg();
    Vector<BitVector, 4> wordsFoundAt(plan.yields.size(), [&](size_t) { return BitVector(plan.numberOfWords); });
    Vector<BitVector, 2> wordsFoundAtRelease(plan.releases.size(), [&](size_t) { return BitVector(plan.numberOfWords); });
    auto wordsOf = [&](const YieldInFrame& yield) {
        BitVector words(plan.numberOfWords);
        for (ValueID id : yield.stored)
            words.quickSet(plan.wordOf[id.index()]);
        for (ValueID id : yield.heldOnly)
            words.quickSet(plan.wordOf[id.index()]);
        return words;
    };
    auto yieldIndexOf = [&](ValueID id) {
        return plan.yields.findIf([&](const YieldInFrame& yield) { return yield.yield == id; });
    };
    for (const YieldInFrame& from : plan.yields) {
        BitVector words = wordsOf(from);
        BitVector isVisited(function.blocks.size());
        Vector<BlockID, 8> worklist;
        auto scan = [&](BlockID block, uint32_t firstIndex) {
            const BlockValues& values = function.at(block).values;
            for (uint32_t index = firstIndex; index < values.size(); ++index) {
                if (function.at(values[index]).opcode == Opcode::ReleaseHint) {
                    size_t release = plan.releases.findIf([&](const ReleaseInFrame& candidate) { return candidate.hint == values[index]; });
                    wordsFoundAtRelease[release].merge(words);
                    continue;
                }
                if (function.at(values[index]).opcode == Opcode::YieldBySavingFrame) {
                    wordsFoundAt[yieldIndexOf(values[index])].merge(words);
                    // What stands in front of the yield may have thrown.
                    if (function.at(block).handler && !isVisited.quickSet(function.at(block).handler->block.index()))
                        worklist.append(function.at(block).handler->block);
                    return;
                }
            }
            for (BlockID successor : cfg.successors(block)) {
                if (!isVisited.quickSet(successor.index()))
                    worklist.append(successor);
            }
        };
        scan(from.point.block, from.point.index + 1);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            scan(block, 0);
        }
    }
    for (unsigned index = 0; index < plan.yields.size(); ++index) {
        YieldInFrame& yield = plan.yields[index];
        BitVector words = wordsOf(yield);
        for (size_t word : wordsFoundAt[index]) {
            if (word != wordOfGenerator && !words.get(word))
                yield.wordsToClear.append(static_cast<uint32_t>(word));
        }
    }
    // A word whose value lives on may be left out of the stores of the next yield, which finds it as it was.
    for (unsigned index = 0; index < plan.releases.size(); ++index) {
        ReleaseInFrame& release = plan.releases[index];
        BitVector words(plan.numberOfWords);
        for (ValueID id : release.live) {
            if (plan.wordOf[id.index()] != noWord)
                words.quickSet(plan.wordOf[id.index()]);
        }
        for (size_t word : wordsFoundAtRelease[index]) {
            if (word != wordOfGenerator && !words.get(word))
                release.wordsToClear.append(static_cast<uint32_t>(word));
        }
    }
}

// The body asks at its entry whether it is running and suspends at once if it is not. Whoever calls it can do that himself where the frame takes nothing there but what he passes:
// then the function that resumes is the only one, and what runs up to the first yield is not there twice.
static bool findWordsAtStart(HeapFramePlan& plan, const Function& function, FunctionAnalyses& analyses)
{
    const Terminator& entry = function.at(entryBlock).terminator;
    if (entry.kind != TerminatorKind::Branch)
        return false;
    const Value& test = function.at(entry.conditionOperand());
    if (test.opcode != Opcode::IsGeneratorRunning || test.generatorOperand() != plan.generator)
        return false;

    // A new frame has the resume index 0.
    const YieldInFrame& initial = plan.yields[0];
    BlockID block = initial.point.block;
    if (block != entry.notTaken().block || !entry.notTaken().arguments.isEmpty() || analyses.cfg().predecessors(block).size() != 1 || function.at(block).handler)
        return false;
    for (ValueID id : function.at(block).values.span().first(initial.point.index)) {
        if (!isRematerializable(function.at(id)))
            return false;
    }
    for (auto list : { initial.stored.span(), initial.heldOnly.span() }) {
        for (ValueID id : list) {
            ValueLocation location = analyses.useLists().locationOf(id);
            if (location.placement != ValuePlacement::Parameter || location.block != entryBlock)
                return false;
            plan.wordsAtStart.append({ plan.wordOf[id.index()], location.index });
        }
    }
    return true;
}

static std::optional<HeapFramePlan> planHeapFrame(const AnalysisContext& context, FunctionID id)
{
    const Program& program = context.program;
    const Function& function = program.at(id);
    FunctionAnalyses analyses(context, function);

    HeapFramePlan plan;
    plan.body = id;
    plan.generator = driverOf(function);

    // Liveness at so many yields costs more than the form is worth.
    unsigned numberOfYields = 0;
    for (const Block& block : function.blocks) {
        for (ValueID value : block.values)
            numberOfYields += function.at(value).opcode == Opcode::YieldBySavingFrame;
    }
    if (numberOfYields > maxWordsOfHeapFrame)
        return std::nullopt;

    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Block& data = function.at(block);
        Vector<uint32_t, 4> indexes;
        Vector<uint32_t, 2> releases;
        for (uint32_t index = 0; index < data.values.size(); ++index) {
            const Value& value = function.at(data.values[index]);
            if (value.opcode == Opcode::YieldBySavingFrame)
                indexes.append(index);
            else if (value.opcode == Opcode::ReleaseHint)
                releases.append(index);
            else if (isBoundToFrame(function, data, index))
                return std::nullopt;
        }
        if (!releases.isEmpty()) {
            Liveness::LocalCalc localCalc(analyses.liveness(), block);
            uint32_t pointIndex = data.values.size();
            while (!releases.isEmpty()) {
                uint32_t index = releases.takeLast();
                while (pointIndex > index + 1)
                    localCalc.execute(--pointIndex);
                ReleaseInFrame release { data.values[index], { }, { } };
                localCalc.forEachLive([&](ValueID live) { release.live.append(live); });
                plan.releases.append(WTF::move(release));
            }
        }
        if (indexes.isEmpty())
            continue;

        Liveness::LocalCalc localCalc(analyses.liveness(), block);
        uint32_t pointIndex = data.values.size();
        while (!indexes.isEmpty()) {
            uint32_t index = indexes.takeLast();
            while (pointIndex > index + 1)
                localCalc.execute(--pointIndex);

            YieldInFrame yield;
            yield.yield = data.values[index];
            yield.point = ProgramPoint { block, index };
            for (const Use& use : analyses.useLists().usesOf(yield.yield)) {
                if (use.kind != Use::Kind::Operand || function.at(use.user).opcode != Opcode::ResultAt || use.block != block)
                    return std::nullopt;
                yield.resultReads.append(use.user);
            }
            bool isStorable = true;
            localCalc.forEachLive([&](ValueID live) {
                if (live == yield.yield || live == plan.generator)
                    return;
                // Its results are read behind the yield, and a word holds one value.
                if (numberOfResultsOf(program, function, function.at(live))) {
                    isStorable = false;
                    return;
                }
                if (isRematerializable(function.at(live))) {
                    yield.constants.append(live);
                    return;
                }
                if (!canLiveInWord(program, function, live) || isTestThatMakesGuards(analyses, live)) {
                    isStorable = false;
                    return;
                }
                yield.stored.append(live);
            });
            if (!isStorable || yield.stored.size() >= maxWordsOfHeapFrame)
                return std::nullopt;
            if (localCalc.isLive(plan.generator))
                yield.constants.append(plan.generator);
            for (ValueID held : heldOperandsOf(function.at(yield.yield))) {
                if (localCalc.isLive(held) || held == plan.generator || isRematerializable(function.at(held)) || !canLiveInWord(program, function, held) || yield.heldOnly.contains(held))
                    continue;
                yield.heldOnly.append(held);
            }
            plan.yields.append(WTF::move(yield));
        }
    }
    if (plan.yields.isEmpty())
        return std::nullopt;

    assignWords(plan, function.values.size());
    if (plan.numberOfWords > maxWordsOfHeapFrame)
        return std::nullopt;
    findWordsToClear(plan, function, analyses);
    plan.isStartedByCallers = findWordsAtStart(plan, function, analyses);
    return plan;
}

static Vector<IRBuilder::Expansion, 4> cutYields(IRBuilder& builder, HeapFramePlan& plan, NOESCAPE const Invocable<void(YieldInFrame&, unsigned index)> auto& emitLeave)
{
    Function& function = builder.function();
    Vector<unsigned, 4> order(plan.yields.size(), [](size_t index) { return static_cast<unsigned>(index); });
    AOT::sort(order, std::ranges::greater { }, [&](unsigned index) { return std::tuple(plan.yields[index].point.block, plan.yields[index].point.index); });
    Vector<IRBuilder::Expansion, 4> cuts;
    cuts.grow(plan.yields.size());
    for (unsigned index : order) {
        YieldInFrame& yield = plan.yields[index];
        cuts[index] = builder.beginExpansion(yield.point, yield.resultReads.span());
        emitLeave(yield, index);
    }
    return cuts;
}

static void appendPut(IRBuilder& builder, ValueID frame, uint32_t word, ValueID value)
{
    Value put = IRBuilder::make(Opcode::PutHeapFrameWord, { frame, value });
    put.setIndex(word);
    builder.append(WTF::move(put));
}

// The generator's own word is written once, by the body.
static void appendPutsOfYield(IRBuilder& builder, const HeapFramePlan& plan, const YieldInFrame& yield, ValueID frame)
{
    for (auto list : { yield.stored.span(), yield.heldOnly.span() }) {
        for (ValueID id : list)
            appendPut(builder, frame, plan.wordOf[id.index()], id);
    }
}

static void rewriteBody(const AnalysisContext& context, TaskArenas& arenas, Function& function, HeapFramePlan& plan, const PassRunner& runner, const OptimizationInputs& inputs)
{
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(analyses.context(), function, arenas.types, &analyses);
    Vector<ValueID, 4> yieldedValues(plan.yields.size(), [&](size_t index) { return function.at(plan.yields[index].yield).valueOperand(); });

    auto emitLeave = [&](YieldInFrame& yield, unsigned index) {
        Value allocation = IRBuilder::make(Opcode::NewHeapFrame, { plan.generator });
        allocation.setFunction(plan.resume);
        allocation.setNumberOfWords(plan.numberOfWords);
        ValueID frame = builder.append(WTF::move(allocation));
        appendPut(builder, frame, wordOfGenerator, plan.generator);
        appendPutsOfYield(builder, plan, yield, frame);
        Value suspend = IRBuilder::make(Opcode::SuspendInHeapFrame, { plan.generator, frame });
        suspend.setResumeIndex(index);
        suspend.setIsFirstRun(true);
        builder.append(WTF::move(suspend));
        builder.emitReturn(yieldedValues[index]);
    };
    cutYields(builder, plan, emitLeave);
    builder.setHandler(std::nullopt);
    builder.emptyUnreachableBlocks();
    analyses.didChangeControlFlow();
    runner.runSchedule(FunctionSchedule::Simplify, context.program, inputs, context.id, function, arenas);
}

// What a branch on a test that lives in a word proves on its edges holds where the branch stands, whatever the condition has become.
static void castOnEdgesOfStoredTests(IRBuilder& builder, FunctionAnalyses& analyses, const HeapFramePlan& plan)
{
    Function& function = builder.function();
    BitVector isStored(function.values.size());
    for (const YieldInFrame& yield : plan.yields) {
        for (ValueID id : yield.stored)
            isStored.quickSet(id.index());
    }
    bool didChange = false;
    for (uint32_t index = 0, end = function.blocks.size(); index < end; ++index) {
        BlockID block(index);
        if (!function.at(block).terminator.isConditionalBranch() || !isStored.quickGet(function.at(block).terminator.conditionOperand().index()))
            continue;
        for (uint32_t targetIndex = 0; targetIndex < function.at(block).terminator.targets.size(); ++targetIndex) {
            Edge edge { block, targetIndex };
            if (!hasEdgeResults(function, edge))
                continue;
            BlockID target = targetOf(function, edge).block;
            Arguments arguments = argumentsWithoutTest(function, edge);
            builder.setOrigin(function.at(block).terminator.origin());
            BlockID onEdge = builder.addBlock(function.at(target).frequency);
            builder.moveToEndOf(onEdge);
            builder.emitJump(target, WTF::move(arguments));
            function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.targets[targetIndex] = Target { onEdge, { }, { } };
            castArgumentsToParameters(builder, onEdge, builder.context().build.validates());
            didChange = true;
        }
    }
    if (!didChange)
        return;
    builder.applyInsertions();
    analyses.didChangeControlFlow();
    analyses.didChangeValues();
}

// The frame is memory that the collector reads: what the program has let go of goes out of it there and then, not at the next yield.
static void clearWordsAtReleases(IRBuilder& builder, FunctionAnalyses& analyses, const HeapFramePlan& plan, ValueID frame)
{
    Function& function = builder.function();
    bool didChange = false;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (function.at(values[index]).opcode != Opcode::ReleaseHint)
                continue;
            size_t release = plan.releases.findIf([&](const ReleaseInFrame& candidate) { return candidate.hint == values[index]; });
            if (release == notFound || plan.releases[release].wordsToClear.isEmpty())
                continue;
            ProgramPoint point { BlockID(blockIndex), index };
            builder.setOrigin(function.at(values[index]).origin());
            ValueID absent = builder.insertBefore(point, IRBuilder::make(Opcode::Absent));
            for (uint32_t word : plan.releases[release].wordsToClear) {
                Value put = IRBuilder::make(Opcode::PutHeapFrameWord, { frame, absent });
                put.setIndex(word);
                builder.insertBefore(point, WTF::move(put));
            }
            didChange = true;
        }
    }
    if (!didChange)
        return;
    builder.applyInsertions();
    analyses.didChangeValues();
}

// Only a yield writes a word that holds something, right in front of its return, so what a run reads from a word the word still holds. A parameter holds it if all that arrives there does.
static void removeStoresOfWhatTheWordHolds(Function& function, FunctionAnalyses& analyses, IRBuilder& builder, ValueID frame)
{
    constexpr int64_t notSeen = -2;
    constexpr int64_t noWordAtAll = -1;
    const CFG& cfg = analyses.cfg();
    Vector<int64_t> wordOfParameter(FillWith { }, function.values.size(), notSeen);
    for (ValueID parameter : function.at(cfg.root()).parameters)
        wordOfParameter[parameter.index()] = noWordAtAll;
    auto wordHeldBy = [&](ValueID id) -> int64_t {
        while (isCastWithoutTest(function.at(id).opcode))
            id = function.at(id).operand(0);
        const Value& value = function.at(id);
        if (value.opcode == Opcode::GetHeapFrameWord)
            return value.frameOperand() == frame ? static_cast<int64_t>(value.index()) : noWordAtAll;
        if (analyses.useLists().locationOf(id).placement == ValuePlacement::Parameter)
            return wordOfParameter[id.index()];
        return noWordAtAll;
    };
    for (bool didChange = true; didChange;) {
        didChange = false;
        for (BlockID block : cfg.reversePostOrder()) {
            const CompactList<ValueID>& parameters = function.at(block).parameters;
            if (block == cfg.root())
                continue;
            for (uint32_t index = 0; index < parameters.size(); ++index) {
                int64_t word = notSeen;
                for (Edge edge : cfg.incomingEdges(block)) {
                    const Target& target = targetOf(function, edge);
                    int64_t arriving = index < target.arguments.size() ? wordHeldBy(target.arguments[index]) : noWordAtAll;
                    if (arriving == notSeen)
                        continue;
                    word = word == notSeen || word == arriving ? arriving : noWordAtAll;
                }
                if (word != wordOfParameter[parameters[index].index()]) {
                    wordOfParameter[parameters[index].index()] = word;
                    didChange = true;
                }
            }
        }
    }
    for (BlockID block : cfg.reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        BitVector indexes(values.size());
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode == Opcode::PutHeapFrameWord && wordHeldBy(value.valueOperand()) == static_cast<int64_t>(value.index()))
                indexes.quickSet(index);
        }
        if (!indexes.isEmpty())
            builder.eraseValues(block, indexes);
    }
}

static void buildResumeFunction(const AnalysisContext& context, TaskArenas& arenas, const Function& body, HeapFramePlan plan, const PassRunner& runner, const OptimizationInputs& inputs)
{
    Function& function = *plan.resumeSlot;
    function = body;
    TypeArena& types = arenas.types;
    TypeID modeType = types.intern(Type::integer(Range { static_cast<int64_t>(ResumeMode::Next), static_cast<int64_t>(ResumeMode::Return) }));

    CreatorRecord::Coroutine record = body.creator.as<CreatorRecord::Coroutine>();
    record.body = plan.resume;
    record.promiseParameter = std::nullopt;
    function.creator = record;
    function.derivation.parameterMap = Vector<ParameterOrigin>(3, [](size_t) { return ParameterOrigin { std::nullopt }; });
    function.captures = { };
    function.flags.remove({ FunctionFlags::TakesCallee, FunctionFlags::HasNewTarget, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest });
    function.parameters = Vector<DeclaredType> { DeclaredType(Proven::byPosition(TypeTable::anything)), DeclaredType(Proven::byPosition(modeType)), DeclaredType(Proven::byPosition(TypeTable::anything)) };
    function.flags.remove(FunctionFlags::HasThis);
    function.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));

    // The entry block takes the three parameters. What it was stands behind it until the words have been given their definitions: all that is defined there reaches what it reached.
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(analyses.context(), function, types, &analyses);
    builder.setHandler(std::nullopt);
    BlockID formerEntry = builder.addBlock(function.at(entryBlock).frequency);
    {
        Block& from = function.edit(entryBlock, { BlockEdit::Values, BlockEdit::Parameters, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest });
        Block& to = function.edit(formerEntry, { BlockEdit::Values, BlockEdit::Parameters, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest });
        to.values = WTF::move(from.values);
        to.parameters = WTF::move(from.parameters);
        to.terminator = std::exchange(from.terminator, Terminator { });
        to.shape = from.shape;
        from.values = { };
        from.parameters = { };
    }
    for (YieldInFrame& yield : plan.yields) {
        if (yield.point.block == entryBlock)
            yield.point.block = formerEntry;
    }
    analyses.didChangeControlFlow();
    analyses.didChangeValues();
    ValueID frame = builder.addParameter(entryBlock, TypeTable::anything);
    ValueID mode = builder.addParameter(entryBlock, modeType);
    ValueID sent = builder.addParameter(entryBlock, TypeTable::anything);
    castOnEdgesOfStoredTests(builder, analyses, plan);

    Vector<ValueID, 4> yieldedValues(plan.yields.size(), [&](size_t index) { return function.at(plan.yields[index].yield).valueOperand(); });
    auto emitLeave = [&](YieldInFrame& yield, unsigned index) {
        appendPutsOfYield(builder, plan, yield, frame);
        if (!yield.wordsToClear.isEmpty()) {
            ValueID absent = builder.append(IRBuilder::make(Opcode::Absent));
            for (uint32_t word : yield.wordsToClear)
                appendPut(builder, frame, word, absent);
        }
        Value suspend = IRBuilder::make(Opcode::SuspendInHeapFrame, { plan.generator, frame });
        suspend.setResumeIndex(index);
        builder.append(WTF::move(suspend));
        builder.emitReturn(yieldedValues[index]);
    };
    Vector<IRBuilder::Expansion, 4> cuts = cutYields(builder, plan, emitLeave);
    clearWordsAtReleases(builder, analyses, plan, frame);

    // One block for each yield defines what lives across it.
    SSARepair repair(builder, analyses);
    Vector<std::optional<SSARepair::Variable>> variableOf(function.values.size());
    auto defineLive = [&](ValueID id, ValueID definition) {
        std::optional<SSARepair::Variable>& variable = variableOf[id.index()];
        if (!variable)
            variable = repair.addVariable(id);
        repair.addDefinition(*variable, definition);
    };
    TypeID anythingOrUnspecified = types.intern(types.at(TypeTable::anything).withPrimitives(Primitive::Unspecified));
    Vector<std::pair<int64_t, Target>> cases;
    for (unsigned index = 0; index < plan.yields.size(); ++index) {
        const YieldInFrame& yield = plan.yields[index];
        builder.setHandler(std::nullopt);
        BlockID resume = builder.addBlock(function.at(cuts[index].continuation).frequency);
        builder.moveToEndOf(resume);
        builder.setOrigin(function.at(yield.yield).origin());
        for (ValueID id : yield.constants) {
            if (id == plan.generator) {
                Value read = IRBuilder::make(Opcode::GetHeapFrameWord, { frame });
                read.setIndex(wordOfGenerator);
                read.setValueType(TypeTable::anything);
                ValueID word = builder.append(WTF::move(read));
                Value cast = IRBuilder::make(Opcode::AssumeType);
                cast.becomeAssumeType(word, Proven::byConstruction(function.at(id).type));
                defineLive(id, builder.append(WTF::move(cast)));
                continue;
            }
            defineLive(id, builder.append(Value(function.at(id))));
        }
        for (ValueID id : yield.stored) {
            TypeID type = function.at(id).type;
            // Where the value is not made yet the word is empty, and only what has a fact behind it reads the value.
            Value read = IRBuilder::make(Opcode::GetHeapFrameWord, { frame });
            read.setIndex(plan.wordOf[id.index()]);
            read.setValueType(types.at(type).mayBeUnspecified() ? anythingOrUnspecified : TypeTable::anything);
            ValueID word = builder.append(WTF::move(read));
            if (type == TypeTable::anything) {
                defineLive(id, word);
                continue;
            }
            Value cast = IRBuilder::make(Opcode::AssumeType);
            cast.becomeAssumeType(word, Proven::byConstruction(type));
            cast.setExpected(function.at(id).expected());
            defineLive(id, builder.append(WTF::move(cast)));
        }
        Arguments arguments;
        arguments.append(sent);
        arguments.append(mode);
        builder.emitJump(cuts[index].continuation, WTF::move(arguments));
        cases.append({ static_cast<int64_t>(index), Target { resume, { }, { } } });
    }

    builder.moveToEndOf(entryBlock);
    builder.setOrigin(function.at(plan.yields[0].yield).origin());
    ValueID resumeIndex = builder.append(Opcode::GetResumeIndex, { frame });
    Arguments unspecified;
    for (ValueID parameter : function.at(formerEntry).parameters) {
        Value seed = IRBuilder::make(Opcode::Unspecified);
        seed.setValueType(function.at(parameter).type);
        unspecified.append(builder.append(WTF::move(seed)));
    }
    Vector<std::pair<int64_t, Target>> casesWithFormerEntry = cases;
    builder.emitSwitch(resumeIndex, WTF::move(casesWithFormerEntry), Target { formerEntry, WTF::move(unspecified), { } });
    repair.run();

    // Nothing enters what the entry block was. The last case is what every index but the others means.
    Terminator& terminator = function.edit(entryBlock, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
    terminator = Terminator { };
    analyses.didChangeControlFlow();
    builder.setHandler(std::nullopt);
    builder.moveToEndOf(entryBlock);
    builder.setOrigin(function.at(plan.yields[0].yield).origin());
    Target last = WTF::move(cases.last().second);
    cases.removeLast();
    if (cases.isEmpty())
        builder.emitJump(last.block);
    else
        builder.emitSwitch(resumeIndex, WTF::move(cases), WTF::move(last));
    builder.emptyUnreachableBlocks();
    analyses.didChangeControlFlow();

    runner.runSchedule(FunctionSchedule::Simplify, context.program, inputs, context.id, function, arenas);
    {
        FunctionAnalyses cleaned(context, function);
        IRBuilder cleaner(cleaned.context(), function, types, &cleaned);
        removeStoresOfWhatTheWordHolds(function, cleaned, cleaner, function.at(entryBlock).parameters[0]);
    }
    runner.runSchedule(FunctionSchedule::Simplify, context.program, inputs, context.id, function, arenas);
}

// Only whoever has made the generator knows its state: it runs from MarkGeneratorRunning on.
static std::optional<bool> isRunningAtStart(const Function& function, BlockID block, uint32_t indexOfCall, ValueID generator)
{
    auto objectOf = [&](ValueID id) { return sameObjectThroughCasts(function, id).valueInFrontOfFacts(); };
    ValueID object = objectOf(generator);
    if (function.at(object).opcode != Opcode::NewGenerator)
        return std::nullopt;
    auto marks = [&](ValueID id) { return function.at(id).opcode == Opcode::MarkGeneratorRunning && objectOf(function.at(id).generatorOperand()) == object; };
    const BlockValues& values = function.at(block).values;
    for (uint32_t index = indexOfCall; index--;) {
        if (marks(values[index]))
            return true;
        if (values[index] == object)
            return false;
    }
    for (const Block& other : function.blocks) {
        if (std::ranges::any_of(other.values, marks))
            return std::nullopt;
    }
    return false;
}

static void startGeneratorsInCaller(const AnalysisContext& context, TaskArenas& arenas, Function& function, std::span<const StartOfGenerator> starts, std::span<const HeapFramePlan> plans)
{
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(analyses.context(), function, arenas.types, &analyses);
    for (const StartOfGenerator& start : starts) {
        const HeapFramePlan& plan = plans[start.plan];
        ProgramPoint point = analyses.useLists().pointOf(start.call);
        Vector<ValueID, 8> arguments(function.at(start.call).argumentsOperands());
        ValueID generator = arguments.last();
        builder.setOrigin(function.at(start.call).origin());

        Value allocation = IRBuilder::make(Opcode::NewHeapFrame, { generator });
        allocation.setFunction(plan.resume);
        allocation.setNumberOfWords(plan.numberOfWords);
        ValueID frame = builder.insertBefore(point, WTF::move(allocation));
        auto put = [&](uint32_t word, ValueID value) {
            Value store = IRBuilder::make(Opcode::PutHeapFrameWord, { frame, value });
            store.setIndex(word);
            builder.insertBefore(point, WTF::move(store));
        };
        put(wordOfGenerator, generator);
        for (auto [word, parameter] : plan.wordsAtStart)
            put(word, arguments[parameter]);
        Value suspend = IRBuilder::make(Opcode::SuspendInHeapFrame, { generator, frame });
        suspend.setResumeIndex(0);
        suspend.setIsFirstRun(true);
        suspend.setStaysRunning(start.isRunning);
        builder.insertBefore(point, WTF::move(suspend));
        if (!start.isRunning) {
            becomeUndefined(function, start.call);
            continue;
        }

        // The first step: as if the generator had been resumed behind its initial yield.
        ValueID mode = builder.insertBefore(point, makeInteger(static_cast<int64_t>(ResumeMode::Next)));
        ValueID sent = builder.insertBefore(point, IRBuilder::make(Opcode::Undefined));
        Value& call = function.edit(start.call, ValueEdit::Definition);
        call.setFunction(plan.resume);
        call.setArgumentsOperands(std::initializer_list<ValueID> { frame, mode, sent });
    }
    builder.applyInsertions();
}

void moveGeneratorFramesToHeap(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    BuildContext& build = runner.build();
    const Program& program = editor.program();
    Vector<uint32_t> bodies = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        return function.hasBody() && function.role == FunctionRole::CoroutineBody && coroutineOfBody(function).kind == CoroutineKind::Generator && function.creator.as<CreatorRecord::Coroutine>().promiseParameter;
    });
    if (bodies.isEmpty())
        return;

    Vector<std::optional<HeapFramePlan>> found(bodies.size());
    forEachIndexInParallel(build.parallelLoops(), bodies.size(), [&](unsigned index) {
        FunctionID id(bodies[index]);
        found[index] = planHeapFrame(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, id);
    });
    Vector<HeapFramePlan> plans;
    for (std::optional<HeapFramePlan>& plan : found) {
        if (plan)
            plans.append(WTF::move(*plan));
    }
    build.statistics().count(StatisticID::GeneratorsWithHeapFrames, plans.size());
    if (plans.isEmpty())
        return;

    uint32_t nextFunction = editor.reserveFunctions(plans.size()).index();
    for (HeapFramePlan& plan : plans) {
        plan.resume = FunctionID(nextFunction++);
        plan.resumeSlot = &editor.at(plan.resume);
    }
    Vector<FunctionID> ids = plans.map([](const HeapFramePlan& plan) { return plan.body; });
    Vector<FunctionID> resumes = plans.map([](const HeapFramePlan& plan) { return plan.resume; });
    editFunctionsInParallel(editor, build, ids.span(), resumes.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        HeapFramePlan& plan = plans[index];
        buildResumeFunction(AnalysisContext { build, program, &inputs.flow, &inputs.effects, plan.resume }, arenas, function, plan, runner, inputs);
        // A body that its callers no longer call goes. A call that stays finds it as it was, which works beside the other form.
        if (!plan.isStartedByCallers)
            rewriteBody(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, arenas, function, plan, runner, inputs);
    });

    constexpr uint32_t noPlan = UINT32_MAX;
    Vector<uint32_t> planOfBody(FillWith { }, program.functions.size(), noPlan);
    bool hasBodyStartedByCallers = false;
    for (uint32_t index = 0; index < plans.size(); ++index) {
        if (!plans[index].isStartedByCallers)
            continue;
        planOfBody[plans[index].body.index()] = index;
        hasBodyStartedByCallers = true;
    }
    if (!hasBodyStartedByCallers)
        return;

    auto planOfCall = [&](const Value& value) {
        return value.opcode == Opcode::CallDirect ? planOfBody[value.function().index()] : noPlan;
    };
    Vector<uint32_t> callers = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        return std::ranges::any_of(program.functions[index].blocks, [&](const Block& block) {
            return std::ranges::any_of(block.values, [&](ValueID id) { return planOfCall(program.functions[index].at(id)) != noPlan; });
        });
    });
    Vector<Vector<StartOfGenerator, 2>> startsOfCaller(callers.size());
    BitVector hasCallThatStays(plans.size());
    for (uint32_t position = 0; position < callers.size(); ++position) {
        const Function& caller = program.functions[callers[position]];
        for (uint32_t blockIndex = 0; blockIndex < caller.blocks.size(); ++blockIndex) {
            const BlockValues& values = caller.blocks[blockIndex].values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                const Value& call = caller.at(values[index]);
                uint32_t plan = planOfCall(call);
                if (plan == noPlan)
                    continue;
                std::optional<bool> isRunning = call.isTailCall ? std::nullopt : isRunningAtStart(caller, BlockID(blockIndex), index, call.argumentsOperands().back());
                if (isRunning)
                    startsOfCaller[position].append({ values[index], plan, *isRunning });
                else
                    hasCallThatStays.quickSet(plan);
            }
        }
    }

    // The ramp names the function that runs for it.
    Vector<FunctionID> edited = callers.map([](uint32_t index) { return FunctionID(index); });
    Vector<uint32_t> planOfRamp(FillWith { }, program.functions.size(), noPlan);
    for (uint32_t index = 0; index < plans.size(); ++index) {
        if (!plans[index].isStartedByCallers || hasCallThatStays.quickGet(index))
            continue;
        FunctionID ramp = coroutineOfBody(program.at(plans[index].body)).ramp;
        planOfRamp[ramp.index()] = index;
        if (!edited.contains(ramp)) {
            edited.append(ramp);
            startsOfCaller.append({ });
        }
    }
    editFunctionsInParallel(editor, build, edited.span(), [&](unsigned position, FunctionID id, Function& function, TaskArenas& arenas) {
        if (!startsOfCaller[position].isEmpty())
            startGeneratorsInCaller(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, arenas, function, startsOfCaller[position].span(), plans.span());
        if (uint32_t plan = planOfRamp[id.index()]; plan != noPlan)
            function.creator = program.at(plans[plan].resume).creator.as<CreatorRecord::Coroutine>();
    });
    // The run time may enter the body of any coroutine, so nothing else finds out that nobody enters these.
    for (uint32_t index = 0; index < plans.size(); ++index) {
        if (plans[index].isStartedByCallers && !hasCallThatStays.quickGet(index))
            editor.removeBody(plans[index].body);
    }
}

// KeepValuesInHeapWords.

namespace {

// A stretch is the code that runs from one point on without a suspension: from the entry, from behind a suspension, or from the head of a handler or of a block that different stretches lead to.
// Its first point stands in front of all of it on every way, and no way inside it leaves a block in the middle.
struct Stretches {
    static constexpr uint32_t unknown = UINT32_MAX;
    static constexpr uint32_t ofEntry = 0;

    uint32_t behind(ValueID id) const { return isSuspension.quickGet(id.index()) ? stretchBehindSuspension[id.index()] : inFrontOfValue[id.index()]; }

    Vector<uint32_t> inFrontOfValue;
    Vector<uint32_t> stretchBehindSuspension;
    Vector<uint32_t> atHead;
    Vector<uint32_t> atTail;
    // With the blocks one behind the other in reverse postorder: how many suspensions stand in front of a place.
    Vector<uint32_t> suspensionsInFrontOfValue;
    Vector<uint32_t> suspensionsAtTail;
    BitVector isSuspension;
    uint32_t numberOfSuspensions { 0 };
};

struct KeptWordsPlan {
    FunctionID body;
    Vector<ValueID> values;
};

struct PlaceOfKeptUse {
    uint32_t stretch;
    BlockID block;
    uint32_t index;
    Use use;
};

} // anonymous namespace

static constexpr uint32_t wordsPerHeapCell = SavedFrame::maxNumberOfWords;

static Stretches findStretches(const Function& function, FunctionAnalyses& analyses)
{
    const CFG& cfg = analyses.cfg();
    Stretches stretches;
    stretches.inFrontOfValue.fill(Stretches::unknown, function.values.size());
    stretches.stretchBehindSuspension.fill(Stretches::unknown, function.values.size());
    stretches.suspensionsInFrontOfValue.fill(0, function.values.size());
    stretches.atHead.fill(Stretches::unknown, function.blocks.size());
    stretches.atTail.fill(Stretches::unknown, function.blocks.size());
    stretches.suspensionsAtTail.fill(0, function.blocks.size());
    stretches.isSuspension.ensureSize(function.values.size());

    Vector<uint32_t> behindLastSuspensionOf(FillWith { }, function.blocks.size(), Stretches::unknown);
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID parameter : function.at(block).parameters)
            stretches.suspensionsInFrontOfValue[parameter.index()] = stretches.numberOfSuspensions;
        for (ValueID id : function.at(block).values) {
            stretches.suspensionsInFrontOfValue[id.index()] = stretches.numberOfSuspensions;
            if (!maySuspend(function.at(id).opcode) || !analyses.maySuspend(id))
                continue;
            stretches.isSuspension.quickSet(id.index());
            behindLastSuspensionOf[block.index()] = stretches.stretchBehindSuspension[id.index()] = ++stretches.numberOfSuspensions;
        }
        stretches.suspensionsAtTail[block.index()] = stretches.numberOfSuspensions;
    }

    auto ownStretchOf = [&](BlockID block) { return stretches.numberOfSuspensions + 1 + block.index(); };
    for (bool didChange = true; didChange;) {
        didChange = false;
        for (BlockID block : cfg.reversePostOrder()) {
            uint32_t atHead = block == entryBlock ? Stretches::ofEntry : Stretches::unknown;
            for (Edge edge : cfg.incomingEdges(block)) {
                if (atHead == ownStretchOf(block) || !cfg.isReachable(edge.from))
                    continue;
                // What is defined in a block is not there for its handler, so a handler begins a stretch.
                uint32_t arriving = stretches.atTail[edge.from.index()];
                if (arriving == Stretches::unknown && !edge.isHandlerEdge())
                    continue;
                atHead = !edge.isHandlerEdge() && (atHead == Stretches::unknown || atHead == arriving) ? arriving : ownStretchOf(block);
            }
            if (atHead == stretches.atHead[block.index()])
                continue;
            didChange = true;
            stretches.atHead[block.index()] = atHead;
            stretches.atTail[block.index()] = behindLastSuspensionOf[block.index()] != Stretches::unknown ? behindLastSuspensionOf[block.index()] : atHead;
        }
    }

    for (BlockID block : cfg.reversePostOrder()) {
        uint32_t stretch = stretches.atHead[block.index()];
        RELEASE_ASSERT(stretch != Stretches::unknown);
        for (ValueID parameter : function.at(block).parameters)
            stretches.inFrontOfValue[parameter.index()] = stretch;
        for (ValueID id : function.at(block).values) {
            stretches.inFrontOfValue[id.index()] = stretch;
            stretch = stretches.behind(id);
        }
        RELEASE_ASSERT(stretch == stretches.atTail[block.index()]);
    }
    return stretches;
}

static PlaceOfKeptUse placeOfKeptUse(const Function& function, const UseLists& useLists, const Stretches& stretches, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return { stretches.inFrontOfValue[use.user.index()], use.block, useLists.locationOf(use.user).index, use };
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return { stretches.atTail[use.block.index()], use.block, static_cast<uint32_t>(function.at(use.block).values.size()), use };
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        // Whatever throws in the block passes it on.
        return { stretches.atHead[use.block.index()], use.block, 0, use };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isHeldAtSuspension(const Function& function, const Use& use)
{
    return use.kind == Use::Kind::Operand && isHeldOperand(function.at(use.user), use.operandIndex);
}

// A guard speaks of the value that its test looked at. The test and whoever names the guard read one and the same value only within a stretch.
static bool namesOnlyGuardsOfStretch(const Function& function, const Stretches& stretches, const PlaceOfKeptUse& place)
{
    auto isOfStretch = [&](ValueID guard) { return stretches.behind(guard) == place.stretch; };
    switch (place.use.kind) {
    case Use::Kind::Operand: {
        const Value& user = function.at(place.use.user);
        for (uint32_t index = 0; index < user.numberOfOperands(); ++index) {
            if (ValueID operand = user.operand(index); operand && user.operandKind(index) == OperandKind::Guard && !isOfStretch(operand))
                return false;
        }
        return true;
    }
    case Use::Kind::TerminatorOperand: {
        const Terminator& terminator = function.at(place.use.block).terminator;
        for (uint32_t index = 0; index < terminator.numberOfOperands(); ++index) {
            if (ValueID operand = terminator.operand(index); operand && terminator.operandKind(index) == OperandKind::Guard && !isOfStretch(operand))
                return false;
        }
        return true;
    }
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canBeKeptInHeapWord(const Program& program, const Function& function, FunctionAnalyses& analyses, const Stretches& stretches, ValueID id)
{
    const Value& value = function.at(id);
    const Type& type = program.typeOf(function, id);
    if (!type.isJavaScriptValue() || type.mayBeUnspecified() || type.isNever())
        return false;
    if (isRematerializable(value) || numberOfResultsOf(program, function, value) || isDeferredClosure(function, id) || opcodesWithIndirectUsers.contains(value.opcode))
        return false;
    const UseLists& useLists = analyses.useLists();
    return std::ranges::all_of(useLists.usesOf(id), [&](const Use& use) {
        if (!analyses.cfg().isReachable(use.block) || isHeldAtSuspension(function, use))
            return true;
        PlaceOfKeptUse place = placeOfKeptUse(function, useLists, stretches, use);
        if (place.stretch == stretches.behind(id))
            return true;
        // An edge that narrows an operand of a test asks the condition of the branch which operand, and a word read back is no test.
        if (use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.isConditionalBranch())
            return false;
        // What a block passes to its handler is there at its head.
        if (use.kind == Use::Kind::HandlerArgument)
            return false;
        return acceptsCastOfValue(function, use) && namesOnlyGuardsOfStretch(function, stretches, place);
    });
}

static std::optional<KeptWordsPlan> planKeptWords(const AnalysisContext& context, FunctionID body)
{
    const Program& program = context.program;
    const Function& function = program.at(body);
    uint64_t mostCrossed = context.build.limit(LimitID::MaxSuspensionsCrossedOnStack);
    uint64_t mostAcrossOne = context.build.limit(LimitID::MaxValuesOnStackAcrossSuspension);

    // Nearly every function ends here.
    uint64_t numberOfOperationsThatMaySuspend = 0;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            numberOfOperationsThatMaySuspend += maySuspend(function.at(id).opcode);
    }
    if (!numberOfOperationsThatMaySuspend || (numberOfOperationsThatMaySuspend <= mostCrossed && function.values.size() <= mostAcrossOne))
        return std::nullopt;

    FunctionAnalyses analyses(context, function);
    Stretches stretches = findStretches(function, analyses);
    if (!stretches.numberOfSuspensions)
        return std::nullopt;

    // With the blocks one behind the other, a value lives across the suspensions from `first` to the one in front of `end`.
    struct Crossing {
        ValueID value;
        uint32_t first;
        uint32_t end;
    };
    const UseLists& useLists = analyses.useLists();
    ValueID driver = driverOf(function);
    KeptWordsPlan plan { body, { } };
    Vector<Crossing> crossingFew;
    auto consider = [&](ValueID id) {
        if (id == driver)
            return;
        uint32_t first = stretches.suspensionsInFrontOfValue[id.index()] + stretches.isSuspension.quickGet(id.index());
        uint32_t end = first;
        for (const Use& use : useLists.usesOf(id)) {
            if (!analyses.cfg().isReachable(use.block))
                continue;
            // What a suspension holds lives across it.
            end = std::max(end, use.kind == Use::Kind::Operand ? stretches.suspensionsInFrontOfValue[use.user.index()] + isHeldAtSuspension(function, use) : stretches.suspensionsAtTail[use.block.index()]);
        }
        if (end == first)
            return;
        if (end - first <= mostCrossed)
            crossingFew.append({ id, first, end });
        else if (canBeKeptInHeapWord(program, function, analyses, stretches, id))
            plan.values.append(id);
    };
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID parameter : function.at(block).parameters)
            consider(parameter);
        for (ValueID id : function.at(block).values)
            consider(id);
    }

    // Every suspension copies the frame out and every resumption copies it back, and a frame has a largest size.
    if (crossingFew.size() > mostAcrossOne) {
        Vector<int64_t> numberAcross(FillWith { }, stretches.numberOfSuspensions + 1, static_cast<int64_t>(0));
        for (const Crossing& crossing : crossingFew) {
            ++numberAcross[crossing.first];
            --numberAcross[crossing.end];
        }
        // Then: how many of the suspensions in front of this one too many values live across.
        int64_t across = 0;
        int64_t numberWithTooMany = 0;
        for (int64_t& entry : numberAcross) {
            across += entry;
            entry = numberWithTooMany;
            numberWithTooMany += across > static_cast<int64_t>(mostAcrossOne);
        }
        for (const Crossing& crossing : crossingFew) {
            if (numberAcross[crossing.end] > numberAcross[crossing.first] && canBeKeptInHeapWord(program, function, analyses, stretches, crossing.value))
                plan.values.append(crossing.value);
        }
    }
    if (plan.values.isEmpty())
        return std::nullopt;
    return plan;
}

static void keepValuesInWords(const AnalysisContext& context, TaskArenas& arenas, Function& function, const KeptWordsPlan& plan)
{
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(analyses.context(), function, arenas.types, &analyses);
    builder.setOrigin(Origin { });
    // A handler of the first block would be entered from in front of the cell.
    if (function.at(entryBlock).handler) {
        builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
        builder.removeHandler(entryBlock);
    }
    Stretches stretches = findStretches(function, analyses);
    // Every place is read before the first change.
    const UseLists& useLists = analyses.useLists();
    const Dominators& dominators = analyses.dominators();

    Vector<ValueID, 1> cells;
    for (size_t first = 0; first < plan.values.size(); first += wordsPerHeapCell) {
        Value allocation = IRBuilder::make(Opcode::NewHeapWords);
        allocation.setNumberOfWords(static_cast<uint32_t>(std::min<size_t>(wordsPerHeapCell, plan.values.size() - first)));
        cells.append(builder.insertBefore(ProgramPoint::atHeadOf(entryBlock), WTF::move(allocation)));
    }

    BitVector isKept(function.values.size());
    Vector<ValueID> suspensionsThatHold;
    Vector<PlaceOfKeptUse, 8> places;
    for (size_t word = 0; word < plan.values.size(); ++word) {
        ValueID kept = plan.values[word];
        ValueID cell = cells[word / wordsPerHeapCell];
        uint32_t index = word % wordsPerHeapCell;
        isKept.quickSet(kept.index());

        ValueLocation definition = useLists.locationOf(kept);
        RELEASE_ASSERT(definition.placement != ValuePlacement::Detached);
        Value store = IRBuilder::make(Opcode::PutHeapFrameWord, { cell, kept });
        store.setIndex(index);
        store.setRemembersFrame(true);
        builder.setOrigin(function.at(kept).origin());
        builder.insertBefore(definition.placement == ValuePlacement::Parameter ? ProgramPoint::atHeadOf(definition.block) : ProgramPoint { definition.block, definition.index + 1 }, WTF::move(store));

        places.shrink(0);
        for (const Use& use : useLists.usesOf(kept)) {
            if (!analyses.cfg().isReachable(use.block))
                continue;
            if (isHeldAtSuspension(function, use)) {
                suspensionsThatHold.append(use.user);
                continue;
            }
            PlaceOfKeptUse place = placeOfKeptUse(function, useLists, stretches, use);
            if (place.stretch != stretches.behind(kept))
                places.append(place);
        }
        AOT::sort(places, { }, [](const PlaceOfKeptUse& place) { return std::tuple(place.stretch, place.block, place.index); });

        // One read for all the uses of a stretch, in front of them on every way.
        for (size_t first = 0; first < places.size();) {
            size_t end = first;
            BlockID block = places[first].block;
            for (; end < places.size() && places[end].stretch == places[first].stretch; ++end)
                block = dominators.nearestCommonDominator(block, places[end].block);
            uint32_t indexInBlock = function.at(block).values.size();
            for (size_t position = first; position < end; ++position) {
                if (places[position].block == block)
                    indexInBlock = std::min(indexInBlock, places[position].index);
            }

            Value load = IRBuilder::make(Opcode::GetHeapFrameWord, { cell });
            load.setIndex(index);
            load.setValueType(TypeTable::anything);
            ValueID read = builder.insertBefore(ProgramPoint { block, indexInBlock }, WTF::move(load));
            if (TypeID type = function.at(kept).type; type != TypeTable::anything) {
                Value cast = IRBuilder::make(Opcode::AssumeType);
                cast.becomeAssumeType(read, Proven::byConstruction(type));
                cast.setExpected(function.at(kept).expected());
                read = builder.insertBefore(ProgramPoint { block, indexInBlock }, WTF::move(cast));
            }
            for (; first < end; ++first)
                replaceUse(function, places[first].use, read);
        }
    }

    // What a suspension held lies in a cell, so the suspension holds the cell.
    Vector<uint32_t> wordOf(FillWith { }, function.values.size(), 0u);
    for (size_t word = 0; word < plan.values.size(); ++word)
        wordOf[plan.values[word].index()] = word;
    AOT::sort(suspensionsThatHold);
    suspensionsThatHold.shrink(std::ranges::unique(suspensionsThatHold).begin() - suspensionsThatHold.begin());
    for (ValueID suspension : suspensionsThatHold) {
        Vector<ValueID, 8> held;
        for (ValueID operand : heldOperandsOf(function.at(suspension))) {
            ValueID holder = isKept.quickGet(operand.index()) ? cells[wordOf[operand.index()] / wordsPerHeapCell] : operand;
            if (!held.contains(holder))
                held.append(holder);
        }
        setHeldOperands(function.edit(suspension), held.span());
    }
    builder.applyInsertions();
    analyses.didChangeValues();
}

void keepValuesInHeapWords(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    BuildContext& build = runner.build();
    const Program& program = editor.program();
    Vector<uint32_t> bodies = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        return function.hasBody() && function.role == FunctionRole::CoroutineBody && function.creator.as<CreatorRecord::Coroutine>().promiseParameter;
    });
    if (bodies.isEmpty())
        return;

    Vector<std::optional<KeptWordsPlan>> found(bodies.size());
    forEachIndexInParallel(build.parallelLoops(), bodies.size(), [&](unsigned index) {
        FunctionID id(bodies[index]);
        found[index] = planKeptWords(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, id);
    });
    Vector<KeptWordsPlan> plans;
    for (std::optional<KeptWordsPlan>& plan : found) {
        if (plan)
            plans.append(WTF::move(*plan));
    }
    if (plans.isEmpty())
        return;

    uint64_t numberOfValues = 0;
    for (const KeptWordsPlan& plan : plans)
        numberOfValues += plan.values.size();
    build.statistics().count(StatisticID::ValuesKeptInHeapWords, numberOfValues);

    Vector<FunctionID> ids = plans.map([](const KeptWordsPlan& plan) { return plan.body; });
    editFunctionsInParallel(editor, build, ids.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        keepValuesInWords(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, arenas, function, plans[index]);
        simplifyEditedFunction(runner, program, inputs, id, function, arenas);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
