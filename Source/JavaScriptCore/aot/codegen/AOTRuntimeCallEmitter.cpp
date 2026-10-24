/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeCallEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTExceptionEmitter.h"
#include "AOTPureOperations.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsOperations.h"
#include "AOTThrowEmitter.h"
#include "AOTVector.h"
#include "AirFrameEmission.h"
#include "AirStackSlot.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3ConstrainedValue.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CallGenerator {
    void generate(CCallHelpers&, const B3::StackmapGenerationParams&) const;

    RegisterSet liveRegistersAtCall(const B3::StackmapGenerationParams&) const;
    StubKey chooseKey(const RegisterSet& live) const;
    ArgumentMoves moveArguments(CCallHelpers&, const B3::StackmapGenerationParams&) const;
    void moveImmediate(CCallHelpers&) const;
    void emitCallWithOperandRegister(CCallHelpers&, const B3::StackmapGenerationParams&, StubKey, const ArgumentMoves&) const;
    void emitStubCallInstruction(CCallHelpers&, StubKey, const ArgumentMoves&) const;
    void emitBranchOnFlags(CCallHelpers&, const B3::StackmapGenerationParams&) const;

    CodeRecords* records;
    StubKey key;
    StubSignature signature;
    bool receiverIsInAnyRegister;
    bool branchesOnFlags;
    bool neverReturns;
    std::optional<uint32_t> immediate;
    OptionalIndex site;
    OptionalIndex firstRead;
    std::optional<ThrowSite> throwSite;
    std::optional<OperationCall> preservingCall;
};

} // anonymous namespace

static CCallHelpers::RelationalCondition conditionOnFlags(MacroAssembler::ResultCondition condition)
{
    RELEASE_ASSERT(condition == MacroAssembler::Zero || condition == MacroAssembler::NonZero);
    return condition == MacroAssembler::Zero ? CCallHelpers::Equal : CCallHelpers::NotEqual;
}

RegisterSet CallGenerator::liveRegistersAtCall(const B3::StackmapGenerationParams& params) const
{
    RegisterSet live = RegisterSet::registersToSaveForCCall(params.usedRegisters());
    if (signature.firstResult)
        live.remove(signature.firstResult);
    if (signature.secondResult)
        live.remove(signature.secondResult);
    return live;
}

StubKey CallGenerator::chooseKey(const RegisterSet& live) const
{
    if (!preservingCall)
        return key;
    OperationCall call = *preservingCall;
    bool savesFPRs = live.numberOfSetFPRs() || call.returns == OperationCall::Returns::Double;
    call.saves = savesFPRs ? OperationCall::Saves::CallerSavedRegisters : OperationCall::Saves::CallerSavedGPRs;
    return callOperationPreservingRegisters(call);
}

ArgumentMoves CallGenerator::moveArguments(CCallHelpers& jit, const B3::StackmapGenerationParams& params) const
{
    ArgumentMoves moves;
    B3::Value* call = params.value();
    unsigned numberOfChildren = call->numChildren();
    unsigned firstRep = params.size() - numberOfChildren;
    RegisterSet holdsArgument;
    for (unsigned i = 0; i < numberOfChildren; ++i) {
        if (const B3::ValueRep& rep = params[firstRep + i]; rep.isGPR())
            holdsArgument.add(rep.gpr(), IgnoreVectors);
    }
    unsigned destinationIndex = preservingCall->firstArgument != OperationCall::FirstArgument::None;
    for (unsigned i = 0; i < numberOfChildren; ++i) {
        B3::Type childType = call->child(i)->type();
        if (childType == B3::Double)
            continue;
        GPRReg destination = operationArgumentGPR(destinationIndex++);
        const B3::ValueRep& rep = params[firstRep + i];
        if (rep.isGPR()) {
            if (rep.gpr() != destination)
                appendArgumentMove(moves, destination, ArgumentSource::Register, rep.gpr());
            continue;
        }
        if (rep.isConstant()) {
            if (static_cast<uint64_t>(rep.value()) > maxConstantInArgumentThunk && !holdsArgument.contains(destination, IgnoreVectors))
                jit.move(CCallHelpers::TrustedImm64(rep.value()), destination);
            else
                appendArgumentMove(moves, destination, ArgumentSource::Constant, rep.value());
            continue;
        }
        RELEASE_ASSERT(rep.isStack());
        bool isNarrow = childType == B3::Int32;
        if (params.code().frameLayout().hasFrameRecord) {
            appendArgumentMove(moves, destination, isNarrow ? ArgumentSource::Stack32 : ArgumentSource::Stack64, static_cast<int64_t>(rep.offsetFromFP()));
            continue;
        }
        // The call of the thunk pushes the return address.
        RELEASE_ASSERT(isX86_64());
        int64_t offsetAtCall = params.code().addressInFrame(rep.offsetFromFP()).offset;
        appendArgumentMove(moves, destination, isNarrow ? ArgumentSource::Stack32OffStackPointer : ArgumentSource::Stack64OffStackPointer, offsetAtCall + static_cast<int64_t>(sizeof(CPURegister)));
    }
    return moves;
}

void CallGenerator::moveImmediate(CCallHelpers& jit) const
{
    if (immediate)
        jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(*immediate)), *signature.immediate);
}

void CallGenerator::emitStubCallInstruction(CCallHelpers& jit, StubKey called, const ArgumentMoves& moves) const
{
    StubCalls& stubCalls = records->stubCalls;
    if (throwSite) {
        RELEASE_ASSERT(!firstRead && moves.isEmpty());
        moveImmediate(jit);
        // The fault table names the call by its offset.
        CCallHelpers::Label begin = jit.label();
        uint32_t returnSite = stubCalls.call(jit, called, site);
        records->units.pin(begin, jit.label());
        records->traps.callThrowSites.append({ returnSite, throwSiteWords(*throwSite, Representation::JSValue, signature.operands[0].gpr()) });
        return;
    }
    if (firstRead) {
        RELEASE_ASSERT(moves.isEmpty());
        moveImmediate(jit);
        stubCalls.callForTwoReads(jit, called, firstRead, site);
        return;
    }
    if (!moves.isEmpty()) {
        stubCalls.call(jit, called, immediate, site, moves);
        return;
    }
    if (immediate)
        stubCalls.call(jit, called, *immediate, site);
    else
        stubCalls.call(jit, called, site);
}

void CallGenerator::emitCallWithOperandRegister(CCallHelpers& jit, const B3::StackmapGenerationParams& params, StubKey called, const ArgumentMoves& moves) const
{
    if (!receiverIsInAnyRegister) {
        emitStubCallInstruction(jit, called, moves);
        return;
    }
    GPRReg receiver = params[signature.result == StubResult::None ? 0 : 1].gpr();
    if (std::optional<StubKey> variant = tryWithOperandRegister(called, receiver)) {
        emitStubCallInstruction(jit, *variant, moves);
        return;
    }
    jit.move(receiver, signature.operands[0].gpr());
    emitStubCallInstruction(jit, called, moves);
}

void CallGenerator::emitBranchOnFlags(CCallHelpers& jit, const B3::StackmapGenerationParams& params) const
{
    bool jumpsIfTaken = params.fallsThroughToSuccessor(0);
    MacroAssembler::ResultCondition condition = jumpsIfTaken ? signature.trueCondition : MacroAssembler::invert(signature.trueCondition);
    CCallHelpers::Jump leaves = jit.branchOnFlags(conditionOnFlags(condition));
    CCallHelpers::Jump taken;
    if (!jumpsIfTaken && !params.fallsThroughToSuccessor(1))
        taken = jit.jump();
    WTF::Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
    params.addLatePath([=](CCallHelpers& jit) {
        leaves.linkTo(*labels[jumpsIfTaken], &jit);
        if (taken.isSet())
            taken.linkTo(*labels[1], &jit);
    });
}

void CallGenerator::generate(CCallHelpers& jit, const B3::StackmapGenerationParams& params) const
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    StubKey called = chooseKey(liveRegistersAtCall(params));
    ArgumentMoves moves;
    if (preservingCall)
        moves = moveArguments(jit, params);
    emitCallWithOperandRegister(jit, params, called, moves);
    if (neverReturns) {
        jit.breakpoint();
        return;
    }
    if (branchesOnFlags)
        emitBranchOnFlags(jit, params);
}

static StubKey boundKeyOf(const StubCatalog& catalog, StubKey key, std::optional<uint32_t> immediate)
{
    return immediate && catalog.bindsImmediate(key, *immediate) ? key.withImmediate(*immediate) : key;
}

static bool acceptsAnyReceiverRegister(StubKey key, const StubSignature& signature, size_t numberOfOperands)
{
    OperandRegisterChoice choice = descriptorOf(key.family()).operandRegisters;
    if (operandRegistersOf(choice).empty())
        return false;
    switch (choice) {
    case OperandRegisterChoice::NoChoice:
        return false;
    case OperandRegisterChoice::AlternateReceivers:
        return numberOfOperands == 1 && (signature.result == StubResult::GPR || signature.result == StubResult::None);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A leaf calls nothing and leaves the stack as it found it. On x86-64 the call itself stores the return address, so the caller needs no frame for it.
bool leavesCallerWithoutFrame(StubKey key)
{
    return isX86_64() && descriptorOf(key.family()).leafness == StubLeafness::Leaf;
}

static RegisterSet lateClobbersOf(const StubCatalog& catalog, StubKey key, bool receiverIsInAnyRegister, const StubSignature& signature, const RegisterSet& additionalClobbers)
{
    RegisterSet late = catalog.summaryOf(key).clobbered;
    if (receiverIsInAnyRegister) {
        forEachRegisterVariant(key, [&](StubKey variant) {
            late.merge(catalog.summaryOf(variant).clobbered);
        });
        // A receiver that lies where no variant takes it is moved to where the stub itself does.
        late.add(signature.operands[0], IgnoreVectors);
    }
    if (signature.immediate)
        late.add(*signature.immediate, IgnoreVectors);
    late.merge(RegisterSet::macroClobberedGPRs());
    late.merge(additionalClobbers);
    return late;
}

static RegisterSet earlyClobbersOf(const StubSignature& signature, bool receiverIsInAnyRegister, const RegisterSet& late)
{
    RegisterSet early = RegisterSet::macroClobberedGPRs();
    if (signature.immediate)
        early.add(*signature.immediate, IgnoreVectors);
    if (receiverIsInAnyRegister) {
        RegisterSet writtenBeforeMove = late;
        for (GPRReg reg : operandRegistersOf(OperandRegisterChoice::AlternateReceivers))
            writtenBeforeMove.remove(reg);
        early.merge(writtenBeforeMove);
    }
    return early;
}

static StubSignature withOperationResult(StubSignature signature, StubResult operationResult)
{
    if (signature.result != StubResult::None)
        return signature;
    signature.result = operationResult;
    switch (operationResult) {
    case StubResult::None:
        return signature;
    case StubResult::GPR:
        signature.firstResult = GPRInfo::returnValueGPR;
        return signature;
    case StubResult::FPR:
        signature.firstResult = FPRInfo::returnValueFPR;
        return signature;
    case StubResult::TwoGPRs:
        signature.firstResult = GPRInfo::returnValueGPR;
        signature.secondResult = GPRInfo::returnValueGPR2;
        return signature;
    case StubResult::Flags:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LType patchpointTypeOf(B3::Procedure& proc, StubResult result)
{
    switch (result) {
    case StubResult::None:
    case StubResult::Flags:
        return B3::Void;
    case StubResult::GPR:
        return B3::Int64;
    case StubResult::FPR:
        return B3::Double;
    case StubResult::TwoGPRs:
        return proc.addTuple({ B3::Int64, B3::Int64 });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void setResultConstraints(B3::PatchpointValue* patchpoint, const StubSignature& signature)
{
    switch (signature.result) {
    case StubResult::None:
    case StubResult::Flags:
        return;
    case StubResult::GPR:
    case StubResult::FPR:
        patchpoint->resultConstraints = { B3::ValueRep::reg(signature.firstResult) };
        return;
    case StubResult::TwoGPRs:
        patchpoint->resultConstraints = { B3::ValueRep::reg(signature.firstResult), B3::ValueRep::reg(signature.secondResult) };
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void applyPureFunction(B3::PatchpointValue* patchpoint, const B3::PureFunction& function)
{
    patchpoint->effects.exitsSideways = false;
    patchpoint->pureFunction = function;
}

static void setTailCallGenerator(B3::PatchpointValue* patchpoint, CodeRecords* records, StubKey key, std::optional<uint32_t> immediate)
{
    patchpoint->setRunsWithoutFrame(true);
    patchpoint->setGenerator([records, key, immediate](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (params.context().frameIsSetUp)
            B3::Air::emitFrameTeardown(jit, params.code(), jit.scratchRegister());
        if (immediate)
            records->stubCalls.tailCall(jit, key, *immediate);
        else
            records->stubCalls.tailCall(jit, key);
    });
}

static OperationCall::FirstArgument firstArgumentOf(AOT::Entry entry)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    RELEASE_ASSERT(operation);
    return isLeaf(*operation) ? OperationCall::FirstArgument::None : OperationCall::FirstArgument::Instance;
}

static OperationCall::Returns returnsOf(RuntimeValueKind result)
{
    switch (result) {
    case RuntimeValueKind::Void:
        return OperationCall::Returns::Void;
    case RuntimeValueKind::Double:
        return OperationCall::Returns::Double;
    case RuntimeValueKind::Pair:
        return OperationCall::Returns::Pair;
    case RuntimeValueKind::Size:
    case RuntimeValueKind::Int32:
    case RuntimeValueKind::UInt32:
    case RuntimeValueKind::Int64:
    case RuntimeValueKind::UInt64:
    case RuntimeValueKind::EncodedJSValue:
    case RuntimeValueKind::Cell:
    case RuntimeValueKind::SiteCache:
    case RuntimeValueKind::Pointer:
        return OperationCall::Returns::Value;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LType resultWordTypeOf(OperationCall::Returns returns)
{
    switch (returns) {
    case OperationCall::Returns::Value:
    case OperationCall::Returns::Pair:
        return B3::Int64;
    case OperationCall::Returns::Void:
        return B3::Void;
    case OperationCall::Returns::Double:
        return B3::Double;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StubResult operationResultOf(OperationCall::Returns returns)
{
    switch (returns) {
    case OperationCall::Returns::Value:
        return StubResult::GPR;
    case OperationCall::Returns::Void:
        return StubResult::None;
    case OperationCall::Returns::Double:
        return StubResult::FPR;
    case OperationCall::Returns::Pair:
        return StubResult::TwoGPRs;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static unsigned numberOfGPRArgumentsOf(const RuntimeSignature& signature, OperationCall::FirstArgument first)
{
    unsigned count = first != OperationCall::FirstArgument::None;
    for (RuntimeValueKind kind : signature.parameters)
        count += kind != RuntimeValueKind::Double;
    return count;
}

static bool acceptsResultRepresentation(RuntimeValueKind kind, Representation result)
{
    return acceptsRepresentation(kind, result) || (kind == RuntimeValueKind::Size && result == Representation::Boolean);
}

static void checkParameters(const RuntimeSignature& signature, std::span<const Operand> arguments)
{
    RELEASE_ASSERT(arguments.size() == signature.parameters.size());
    for (size_t i = 0; i < arguments.size(); ++i)
        RELEASE_ASSERT(acceptsRepresentation(signature.parameters[i], arguments[i].representation));
}

static Vector<LValue, 8> valuesOf(std::span<const Operand> operands)
{
    Vector<LValue, 8> values;
    for (const Operand& operand : operands)
        values.append(operand.value);
    return values;
}

bool acceptsRepresentation(RuntimeValueKind kind, Representation representation)
{
    switch (kind) {
    case RuntimeValueKind::Void:
        return representation == Representation::Void;
    case RuntimeValueKind::EncodedJSValue:
        return representation == Representation::JSValue;
    case RuntimeValueKind::Cell:
        return representation == Representation::Cell;
    case RuntimeValueKind::Int32:
    case RuntimeValueKind::UInt32:
        return representation == Representation::Int32 || representation == Representation::Boolean;
    case RuntimeValueKind::Size:
    case RuntimeValueKind::Int64:
    case RuntimeValueKind::UInt64:
        return representation == Representation::Int64 || representation == Representation::RawWord || representation == Representation::OptionalInt64 || representation == Representation::LiteralString;
    case RuntimeValueKind::Double:
        return representation == Representation::Double || representation == Representation::OptionalDouble;
    case RuntimeValueKind::SiteCache:
    case RuntimeValueKind::Pointer:
        return representation == Representation::RawWord;
    case RuntimeValueKind::Pair:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RuntimeCallEmitter::RuntimeCallEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void RuntimeCallEmitter::checkArguments(RuntimeOperation operation, std::span<const Operand> arguments, Representation result) const
{
    RuntimeSignature signature = signatureOf(operation);
    checkParameters(signature, arguments);
    RELEASE_ASSERT(acceptsResultRepresentation(signature.result, result));
}

Operand RuntimeCallEmitter::convertResult(RuntimeValueKind kind, LValue raw, Representation result)
{
    if (kind == RuntimeValueKind::Void)
        return { };
    if (b3TypeOf(result) == B3::Int32)
        return { result, m_out.castToInt32(raw) };
    return { result, raw };
}

Operand RuntimeCallEmitter::callOperation(const OperationContext& context, RuntimeOperation operation, std::span<const Operand> arguments, Representation result, const CallOptions& options)
{
    RELEASE_ASSERT(!isLeaf(operation));
    checkArguments(operation, arguments, result);
    RuntimeValueKind resultKind = signatureOf(operation).result;
    LValue raw = callEntry(context, entryOf(operation), resultWordTypeOf(returnsOf(resultKind)), valuesOf(arguments).span(), options);
    return convertResult(resultKind, raw, result);
}

StubCallResult RuntimeCallEmitter::callOperationReturningPair(const OperationContext& context, RuntimeOperation operation, std::span<const Operand> arguments, const CallOptions& options)
{
    RuntimeSignature signature = signatureOf(operation);
    RELEASE_ASSERT(signature.result == RuntimeValueKind::Pair);
    checkParameters(signature, arguments);
    AOT::Entry entry = entryOf(operation);
    CallRequest request = createEntryRequest(entry, resultWordTypeOf(OperationCall::Returns::Pair), options);
    placeArguments(request, valuesOf(arguments).span(), firstArgumentOf(entry));
    return emitCall(&context, request);
}

Operand RuntimeCallEmitter::callWithoutSite(RuntimeOperation operation, std::span<const Operand> arguments, Representation result, const CallOptions& options)
{
    RELEASE_ASSERT(!isLeaf(operation));
    checkArguments(operation, arguments, result);
    AOT::Entry entry = entryOf(operation);
    Effects effects = AOT::effectsOf(entry);
    RELEASE_ASSERT(!effects.mayExitSideways() && !effects.mayCollect);
    RuntimeValueKind resultKind = signatureOf(operation).result;
    CallRequest request = createEntryRequest(entry, resultWordTypeOf(returnsOf(resultKind)), options);
    placeArguments(request, valuesOf(arguments).span(), firstArgumentOf(entry));
    return convertResult(resultKind, emitCall(nullptr, request).first, result);
}

Operand RuntimeCallEmitter::callLeaf(RuntimeOperation operation, std::span<const Operand> arguments, Representation result)
{
    RELEASE_ASSERT(isLeaf(operation));
    checkArguments(operation, arguments, result);
    RuntimeValueKind resultKind = signatureOf(operation).result;
    CallRequest request = createEntryRequest(entryOf(operation), resultWordTypeOf(returnsOf(resultKind)), { });
    RELEASE_ASSERT(request.key == tailCallOperation(OperationCall::FirstArgument::None));
    placeArguments(request, valuesOf(arguments).span(), OperationCall::FirstArgument::None);
    return convertResult(resultKind, emitCall(nullptr, request).first, result);
}

LValue RuntimeCallEmitter::callEntry(const OperationContext& context, AOT::Entry entry, LType type, std::span<const LValue> arguments, const CallOptions& callerOptions)
{
    CallOptions options = callerOptions;
    options.isOnRarePath |= context.shape() == CodeShape::Compact;
    CallRequest request = createEntryRequest(entry, type, options);
    placeArguments(request, arguments, firstArgumentOf(entry));
    return emitCall(&context, request).first;
}

RuntimeCallEmitter::CallRequest RuntimeCallEmitter::createEntryRequest(AOT::Entry entry, LType type, const CallOptions& callerOptions) const
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    RELEASE_ASSERT(operation);
    RuntimeSignature signature = signatureOf(*operation);
    OperationCall::Returns returns = returnsOf(signature.result);
    OperationCall::FirstArgument first = firstArgumentOf(entry);
    RELEASE_ASSERT(type == resultWordTypeOf(returns));
    m_records.totals.entriesUsed.set(static_cast<size_t>(entry));

    CallRequest request;
    request.options = callerOptions;
    request.options.immediate = offsetInRuntimeTable(entry);
    request.operationResult = operationResultOf(returns);
    request.effectsOfCallee = AOT::effectsOf(entry);
    request.argumentBufferAccess = argumentBufferAccessOf(*operation);
    request.entry = entry;
    if (entry == AOT::Entry::operationAOTReportValidationFailure && !callerOptions.reportMayReturn) {
        RELEASE_ASSERT(returns == OperationCall::Returns::Void);
        request.end = CallEnd::NeverReturns;
    }
    bool isPure = request.effectsOfCallee.isPure() && request.argumentBufferAccess == ArgumentBufferAccess::None && returns != OperationCall::Returns::Void && signature.parameters.size() <= 3;
    if (isPure && !request.options.pureFunction)
        request.options.pureFunction = pureFunction(PureOperation::RuntimeOperation, static_cast<uint32_t>(entry), true);

    bool checksException = request.effectsOfCallee.mayThrow;
    bool canPreserveRegisters = returns != OperationCall::Returns::Pair
        && numberOfGPRArgumentsOf(signature, first) <= GPRInfo::numberOfArgumentRegisters
        && (first != OperationCall::FirstArgument::None || !checksException);
    if (callerOptions.isOnRarePath && canPreserveRegisters) {
        request.preservingCall = OperationCall { .returns = returns, .firstArgument = first, .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = checksException };
        request.key = callOperationPreservingRegisters(*request.preservingCall);
    } else
        request.key = callOperationFor(entry, returns, first);
    return request;
}

void RuntimeCallEmitter::placeArguments(CallRequest& request, std::span<const LValue> arguments, OperationCall::FirstArgument first) const
{
    // Only CallOperation passes on the arguments for which x86-64 has no argument register.
    unsigned maxNumberOfGPRs = request.key.family() == StubFamily::CallOperation ? numberOfOperationArgumentGPRs : GPRInfo::numberOfArgumentRegisters;
    unsigned nextGPR = first != OperationCall::FirstArgument::None;
    unsigned nextFPR = 0;
    for (LValue argument : arguments) {
        if (argument->type() == B3::Double) {
            RELEASE_ASSERT(nextFPR < FPRInfo::numberOfArgumentRegisters);
            request.operands.append({ argument, B3::ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++)) });
            continue;
        }
        RELEASE_ASSERT(nextGPR < maxNumberOfGPRs);
        GPRReg destination = operationArgumentGPR(nextGPR++);
        if (!request.preservingCall) {
            request.operands.append({ argument, B3::ValueRep::reg(destination) });
            continue;
        }
        LValue widened = argument->hasInt32() ? m_out.constInt64(static_cast<uint32_t>(argument->asInt32())) : argument;
        request.operands.append({ widened, B3::ValueRep::ColdAny });
        request.additionalClobbers.add(destination, IgnoreVectors);
    }
}

LValue RuntimeCallEmitter::callDirectFunction(const OperationContext& context, uint32_t number, std::span<const LValue> arguments, LValue keptAlive, std::optional<HostFrameOfDirectCall> hostFrame)
{
    Effects effects = context.effects().onAnyEdge();
    bool isLeaf = !effects.mayThrow && !effects.mayCollect && !effects.mayCallUnknownCode;

    Vector<LValue, 8> argumentsWithHostFrame;
    if (hostFrame && !isLeaf) {
        RELEASE_ASSERT(arguments.size() <= directCalleeArgumentIndex);
        argumentsWithHostFrame.append(arguments);
        while (argumentsWithHostFrame.size() < directThisArgumentIndex)
            argumentsWithHostFrame.append(m_out.constIntPtr(0));
        if (argumentsWithHostFrame.size() == directThisArgumentIndex)
            argumentsWithHostFrame.append(hostFrame->thisValue);
        argumentsWithHostFrame.append(hostFrame->callee);
        arguments = argumentsWithHostFrame.span();
    }

    CallRequest request;
    request.key = AOT::callDirectFunction(isLeaf ? DirectFunctionCall::Leaf : hostFrame ? DirectFunctionCall::WithHostFrame : DirectFunctionCall::WithFrame);
    request.options.immediate = static_cast<uint32_t>(number * sizeof(BuiltinDirectFunctionBinding) + OBJECT_OFFSETOF(BuiltinDirectFunctionBinding, function));
    request.effectsOfCallee = inputs().stubCatalog.effectsOf(request.key);
    request.argumentBufferAccess = ArgumentBufferAccess::Reads;
    placeArguments(request, arguments, OperationCall::FirstArgument::None);
    if (keptAlive)
        request.operands.append({ keptAlive, B3::ValueRep::LateColdAny });
    return emitCall(&context, request).first;
}

RuntimeCallEmitter::CallRequest RuntimeCallEmitter::createRequest(StubKey key, std::span<const LValue> operands, const CallOptions& options) const
{
    const StubCatalog& catalog = inputs().stubCatalog;
    const StubSignature& signature = summaryOf(key).signature;
    RELEASE_ASSERT(operands.size() == signature.numberOfOperands);
    CallRequest request;
    request.key = key;
    request.options = options;
    request.effectsOfCallee = catalog.effectsOf(boundKeyOf(catalog, key, options.immediate));
    request.argumentBufferAccess = options.pureFunction ? ArgumentBufferAccess::None : ArgumentBufferAccess::Reads;
    for (size_t i = 0; i < operands.size(); ++i)
        request.operands.append({ operands[i], B3::ValueRep::reg(signature.operands[i]) });
    return request;
}

StubCallResult RuntimeCallEmitter::emitCall(const OperationContext* context, const CallRequest& request)
{
    const StubCatalog& catalog = inputs().stubCatalog;
    StubKey key = request.key;
    const CallOptions& options = request.options;
    RELEASE_ASSERT(exists(key));
    const StubSummary& summary = summaryOf(key);
    RELEASE_ASSERT(!summary.signature.usesCallConvention);
    StubSignature signature = withOperationResult(summary.signature, request.operationResult);
    RELEASE_ASSERT(request.end != CallEnd::BranchesOnFlags || signature.result == StubResult::Flags);

    Effects effects = context ? effectsOfCall(*context, request.effectsOfCallee) : request.effectsOfCallee;
    if (options.pureFunction)
        effects.reads = { };
    // A site says where in the source a frame stands and which handler is in force there.
    bool hasSite = effects.mayExitSideways() || options.capturesStackTrace;
    RELEASE_ASSERT(context || (!hasSite && !effects.mayCollect));
    RELEASE_ASSERT(!generator().isInitializingCell() || (!effects.mayCollect && !effects.mayThrow));

    RuntimeValidationEmitter::OperandCells cellsBeforeCall;
    if (generator().validates() && context && request.end == CallEnd::Returns && !mayConvertCells(effects) && !options.runsCodeOfHost)
        cellsBeforeCall = emitter<RuntimeValidationEmitter>().captureOperandCells(*context);
    RuntimeValidationEmitter::PhotographedOperands photographedOperands;
    bool comesBack = request.end == CallEnd::Returns || (request.end == CallEnd::BranchesOnFlags && !effects.mayThrow);
    if (generator().validates() && context && comesBack && !effects.mayRunOtherCode() && !options.runsCodeOfHost && !generator().isInitializingCell())
        photographedOperands = emitter<RuntimeValidationEmitter>().photographOperands(*context, effects);
    uint32_t photographedCallee = request.entry ? static_cast<uint32_t>(*request.entry) : PhotographedCall::calleeIsStub | static_cast<uint32_t>(key.family());
    std::optional<Integer> stopsBeforeCall;
    if (generator().validates() && context && comesBack && request.effectsOfCallee.mayCollect && !effects.mayCollect && !options.followsThrow)
        stopsBeforeCall = emitter<RuntimeValidationEmitter>().countStopsForCollector();
    auto checkCollectorDidNotStop = [&] {
        if (!stopsBeforeCall)
            return;
        if (request.entry)
            emitter<RuntimeValidationEmitter>().checkCollectorDidNotStop(*context, *stopsBeforeCall, CalledCode::Kind::Operation, static_cast<uint32_t>(*request.entry));
        else
            emitter<RuntimeValidationEmitter>().checkCollectorDidNotStop(*context, *stopsBeforeCall, CalledCode::Kind::Stub, static_cast<uint32_t>(key.family()));
    };

    B3::PatchpointValue* patchpoint = m_out.patchpoint(patchpointTypeOf(m_proc, signature.result));
    patchpoint->effects.terminal = request.end != CallEnd::Returns;
    patchpoint->setNeedsFrameRecord(readsFrameRecordOfCaller(key.family()));
    patchpoint->setRunsWithoutFrame(!hasSite && !generator().validates() && request.end == CallEnd::Returns && leavesCallerWithoutFrame(key));

    StubKey boundKey = boundKeyOf(catalog, key, options.immediate);
    bool receiverIsInAnyRegister = request.end == CallEnd::Returns && !request.firstRead && boundKey == key && acceptsAnyReceiverRegister(key, signature, request.operands.size());
    RELEASE_ASSERT(!receiverIsInAnyRegister || !options.throwSite);
    for (const PlacedOperand& operand : request.operands)
        patchpoint->append(B3::ConstrainedValue(operand.value, receiverIsInAnyRegister ? B3::ValueRep(B3::ValueRep::SomeRegister) : operand.rep));
    setResultConstraints(patchpoint, signature);
    RegisterSet late = lateClobbersOf(catalog, boundKey, receiverIsInAnyRegister, signature, request.additionalClobbers);
    patchpoint->clobberLate(late);
    patchpoint->clobberEarly(earlyClobbersOf(signature, receiverIsInAnyRegister, late));

    Effects effectsSinceThrow = effects;
    effectsSinceThrow.mayCollect |= options.followsThrow;
    applyEffects(patchpoint, effectsSinceThrow, request.argumentBufferAccess, options.accessesCollectorState, options.runsCodeOfHost ? CellConversions::WhateverEffectsSay : CellConversions::AsEffectsSay);
    if (options.pureFunction) {
        RELEASE_ASSERT(!effects.mayExitSideways() && !effects.mayCollect && request.argumentBufferAccess == ArgumentBufferAccess::None && !options.accessesCollectorState);
        applyPureFunction(patchpoint, *options.pureFunction);
    }

    if (request.end == CallEnd::LeavesFunction) {
        setTailCallGenerator(patchpoint, &m_records, key, options.immediate);
        return { };
    }

    OptionalIndex site;
    OptionalIndex firstRead;
    if (hasSite) {
        if (request.firstRead)
            firstRead = { addSite(*context, { .origin = request.firstRead }) };
        site = { addSite(*context, options) };
        if (options.throwSite && options.origin)
            addItemSite(*context, *site, 0, context->origin());
    }

    bool branchesOnFlags = request.end == CallEnd::BranchesOnFlags;
    bool neverReturns = request.end == CallEnd::NeverReturns;
    CallGenerator callGenerator { &m_records, key, signature, receiverIsInAnyRegister, branchesOnFlags, neverReturns, options.immediate, site, firstRead, options.throwSite, request.preservingCall };
    patchpoint->setGenerator([callGenerator = WTF::move(callGenerator)](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        callGenerator.generate(jit, params);
    });
    if (branchesOnFlags && photographedOperands.isEmpty() && !stopsBeforeCall) {
        m_out.m_block->appendSuccessor(request.targets->notTaken);
        m_out.m_block->appendSuccessor(request.targets->taken);
    } else if (branchesOnFlags) {
        // The operands are compared on both ways out.
        LBasicBlock notTaken = newBlock();
        LBasicBlock taken = newBlock();
        m_out.m_block->appendSuccessor(B3::FrequentedBlock(notTaken, request.targets->notTaken.frequency()));
        m_out.m_block->appendSuccessor(B3::FrequentedBlock(taken, request.targets->taken.frequency()));
        for (auto [block, target] : { std::pair { notTaken, request.targets->notTaken.block() }, std::pair { taken, request.targets->taken.block() } }) {
            m_out.appendTo(block);
            checkCollectorDidNotStop();
            emitter<RuntimeValidationEmitter>().compareWithPhotographs(*context, photographedOperands, photographedCallee);
            m_out.jump(target);
        }
        return { };
    }
    if (effects.mayThrow)
        emitter<ExceptionEmitter>().attachLandingPad(*context, patchpoint, *site, request.end == CallEnd::Returns);
    if (neverReturns) {
        m_out.appendTo(newBlock());
        return { };
    }
    StubCallResult result = resultOfCall(signature.result, patchpoint);
    checkCollectorDidNotStop();
    if (!cellsBeforeCall.isEmpty())
        emitter<RuntimeValidationEmitter>().checkCellsWereNotConverted(cellsBeforeCall);
    if (!photographedOperands.isEmpty())
        emitter<RuntimeValidationEmitter>().compareWithPhotographs(*context, photographedOperands, photographedCallee);
    return result;
}

StubCallResult RuntimeCallEmitter::resultOfCall(StubResult result, B3::PatchpointValue* patchpoint)
{
    switch (result) {
    case StubResult::None:
    case StubResult::Flags:
        return { };
    case StubResult::GPR:
    case StubResult::FPR:
        return { patchpoint, nullptr };
    case StubResult::TwoGPRs:
        return { m_out.extract(patchpoint, 0), m_out.extract(patchpoint, 1) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubCallResult RuntimeCallEmitter::callStub(const OperationContext& context, StubKey key, std::span<const LValue> operands, const CallOptions& options)
{
    return emitCall(&context, createRequest(key, operands, options));
}

StubCallResult RuntimeCallEmitter::callStubWithoutSite(StubKey key, std::span<const LValue> operands, const CallOptions& options)
{
    return emitCall(nullptr, createRequest(key, operands, options));
}

void RuntimeCallEmitter::callStubAndBranch(const OperationContext& context, StubKey key, std::span<const LValue> operands, const CallOptions& options, BranchTargets targets)
{
    CallRequest request = createRequest(key, operands, options);
    request.end = CallEnd::BranchesOnFlags;
    request.targets = targets;
    emitCall(&context, request);
}

void RuntimeCallEmitter::callStubWithoutSiteAndBranch(StubKey key, std::span<const LValue> operands, BranchTargets targets)
{
    CallRequest request = createRequest(key, operands, { });
    request.end = CallEnd::BranchesOnFlags;
    request.targets = targets;
    emitCall(nullptr, request);
}

StubCallResult RuntimeCallEmitter::callStubForTwoReads(const OperationContext& context, StubKey key, std::span<const LValue> operands, const CallOptions& options, Origin firstRead)
{
    CallRequest request = createRequest(key, operands, options);
    request.firstRead = firstRead;
    return emitCall(&context, request);
}

B3::PatchpointValue* RuntimeCallEmitter::createCallPatchpoint(const OperationContext& context, const CallPatchpointRequest& request)
{
    Effects effects = effectsOfCall(context, request.effects);
    B3::PatchpointValue* patchpoint = request.holdsUniqueRecord ? m_out.patchpointWithUniqueRecord(request.resultType) : m_out.patchpoint(request.resultType);
    patchpoint->clobberEarly(request.earlyClobbered);
    patchpoint->clobberLate(request.lateClobbered);
    applyEffects(patchpoint, effects, request.argumentBufferAccess);
    patchpoint->setRunsWithoutFrame(request.runsWithoutFrame);
    patchpoint->effects.terminal = request.neverReturns;
    return patchpoint;
}

void RuntimeCallEmitter::finishCallPatchpoint(const OperationContext& context, B3::PatchpointValue* patchpoint, uint32_t site, const CallPatchpointRequest& request)
{
    if (request.effects.mayThrow)
        emitter<ExceptionEmitter>().attachLandingPad(context, patchpoint, site, !request.neverReturns);
    if (request.neverReturns)
        m_out.appendTo(newBlock());
}

RegisterSet RuntimeCallEmitter::registersClobberedByCalls() const
{
    RegisterSet clobbered = registersClobberedByCompiledCalls();
    clobbered.merge(RegisterSet::macroClobberedGPRs());
    return clobbered;
}

B3::Air::StackSlot* RuntimeCallEmitter::ensureRegisterStorage(B3::Air::StackSlot*& storage)
{
    RELEASE_ASSERT(generator().validates());
    if (!storage) {
        storage = m_proc.addStackSlot(KeptRegisterCheck::storageSize);
        m_records.totals.entriesUsed.set(static_cast<size_t>(AOT::Entry::operationAOTReportValidationFailure));
    }
    return storage;
}

B3::Air::StackSlot* RuntimeCallEmitter::keptRegisterStorage()
{
    return ensureRegisterStorage(m_keptRegisterStorage);
}

B3::Air::StackSlot* RuntimeCallEmitter::landingPadRegisterStorage()
{
    return ensureRegisterStorage(m_landingPadRegisterStorage);
}

// Whatever a built-in runs, its function object, its inline code or an operation, runs after the tail call.
static bool isDemotedTailCallOfBuiltin(const Function& function, const OperationContext& context)
{
    if (!context.valueID())
        return false;
    const Value& value = function.at(context.valueID());
    return builtinCallOpcodes.contains(value.opcode) && value.isDemotedTailCall();
}

RecordedSite RuntimeCallEmitter::recordedSiteOf(const OperationContext& context, const CallOptions& options)
{
    bool isTailCall = options.isTailCall || isDemotedTailCallOfBuiltin(m_function, context);
    return { .origin = options.origin.value_or(context.origin()), .constructs = options.constructs, .isTailCall = isTailCall };
}

uint32_t RuntimeCallEmitter::addSite(const OperationContext& context, const CallOptions& options)
{
    uint32_t number = m_records.sites.addSite(recordedSiteOf(context, options));
    uint64_t numbersInUse = uint64_t { number } + 1;
    if (numbersInUse <= build().limit(LimitID::SitesPerFunction))
        return number;
    generator().failAtLimit(context, LimitID::SitesPerFunction, numbersInUse);
    return 1;
}

uint32_t RuntimeCallEmitter::addItemSite(const OperationContext& context, uint32_t callSite, uint32_t item, Origin origin)
{
    uint32_t number = m_records.sites.addSite({ .origin = origin });
    uint64_t numbersInUse = uint64_t { number } + 1;
    if (numbersInUse > build().limit(LimitID::SitesPerFunction)) {
        generator().failAtLimit(context, LimitID::SitesPerFunction, numbersInUse);
        number = 1;
    }
    m_records.sites.itemSites.append({ callSite, item, number });
    return number;
}

LValue RuntimeCallEmitter::transientBuffer(size_t bytes)
{
    if (!m_transientBuffer)
        m_transientBuffer = m_proc.addStackSlot(bytes);
    else
        m_transientBuffer->ensureSize(bytes);
    return m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), m_transientBuffer);
}

Pointer RuntimeCallEmitter::argumentBuffer(std::span<const Boxed> values)
{
    LValue slot = transientBuffer(std::max<size_t>(values.size(), 1) * sizeof(EncodedJSValue));
    for (size_t i = 0; i < values.size(); ++i)
        m_out.store64(values[i].value(), m_out.address(m_heaps.argumentBuffer, slot, i * sizeof(EncodedJSValue)));
    return Pointer(slot);
}

Pointer RuntimeCallEmitter::uninitializedArgumentBuffer(unsigned numberOfWords)
{
    return Pointer(transientBuffer(std::max<size_t>(numberOfWords, 1) * sizeof(EncodedJSValue)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
