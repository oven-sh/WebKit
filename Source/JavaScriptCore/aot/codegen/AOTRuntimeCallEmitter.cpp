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
#include "AOTHeaps.h"
#include "AOTPureOperations.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsOperations.h"
#include "AOTThreshold.h"
#include "AOTThrowEmitter.h"
#include "AOTVector.h"
#include "AirCode.h"
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
    ArgumentMoves moveArguments(CCallHelpers&, const B3::StackmapGenerationParams&) const;
    void emitCallInstruction(CCallHelpers&, StubKey, const ArgumentMoves&) const;
    void emitBranchOnFlags(CCallHelpers&, const B3::StackmapGenerationParams&) const;
    void emitMark(CCallHelpers&) const;

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
    std::optional<MarkOfCallWithoutSite> mark;
};

struct ReturnTraits {
    LType wordType;
    StubResult result;
};

} // anonymous namespace

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

void CallGenerator::emitCallInstruction(CCallHelpers& jit, StubKey called, const ArgumentMoves& moves) const
{
    StubCalls& stubCalls = records->stubCalls;
    if (throwSite || firstRead) {
        RELEASE_ASSERT((!throwSite || !firstRead) && moves.isEmpty());
        if (immediate)
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(*immediate)), *signature.immediate);
        if (firstRead) {
            stubCalls.callForTwoReads(jit, called, firstRead, site);
            return;
        }
        // The fault table names the call by its offset.
        CCallHelpers::Label begin = jit.label();
        uint32_t returnSite = stubCalls.call(jit, called, site);
        records->units.pin(begin, jit.label());
        records->traps.callThrowSites.append({ returnSite, throwSiteWords(*throwSite, Representation::JSValue, signature.operands[0].gpr()) });
        return;
    }
    if (!moves.isEmpty())
        stubCalls.call(jit, called, immediate, site, moves);
    else if (immediate)
        stubCalls.call(jit, called, *immediate, site);
    else
        stubCalls.call(jit, called, site);
}

void CallGenerator::emitBranchOnFlags(CCallHelpers& jit, const B3::StackmapGenerationParams& params) const
{
    bool jumpsIfTaken = params.fallsThroughToSuccessor(0);
    MacroAssembler::ResultCondition condition = jumpsIfTaken ? signature.trueCondition : MacroAssembler::invert(signature.trueCondition);
    RELEASE_ASSERT(condition == MacroAssembler::Zero || condition == MacroAssembler::NonZero);
    CCallHelpers::Jump leaves = jit.branchOnFlags(condition == MacroAssembler::Zero ? CCallHelpers::Equal : CCallHelpers::NotEqual);
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

void CallGenerator::emitMark(CCallHelpers& jit) const
{
    for (uint32_t payload : { mark->value.index(), static_cast<uint32_t>(mark->callee) }) {
        uint64_t word = MarkOfCallWithoutSite::wordFor(payload);
#if CPU(X86_64)
        jit.m_assembler.movq_i64r(static_cast<int64_t>(word), CCallHelpers::s_scratchRegister);
#elif CPU(ARM64)
        jit.m_assembler.movz<64>(ARM64Registers::zr, static_cast<uint16_t>(word));
        jit.m_assembler.movk<64>(ARM64Registers::zr, static_cast<uint16_t>(word >> 16), 16);
        jit.m_assembler.movk<64>(ARM64Registers::zr, static_cast<uint16_t>(word >> 32), 32);
#else
        UNUSED_PARAM(jit);
        UNUSED_VARIABLE(word);
#endif
    }
}

void CallGenerator::generate(CCallHelpers& jit, const B3::StackmapGenerationParams& params) const
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    StubKey called = key;
    ArgumentMoves moves;
    if (preservingCall) {
        RegisterSet live = RegisterSet::registersToSaveForCCall(params.usedRegisters());
        if (signature.firstResult)
            live.remove(signature.firstResult);
        if (signature.secondResult)
            live.remove(signature.secondResult);
        OperationCall call = *preservingCall;
        bool savesFPRs = live.numberOfSetFPRs() || call.returns == OperationCall::Returns::Double;
        call.saves = savesFPRs ? OperationCall::Saves::CallerSavedRegisters : OperationCall::Saves::CallerSavedGPRs;
        called = callOperationPreservingRegisters(call);
        moves = moveArguments(jit, params);
    }
    if (receiverIsInAnyRegister) {
        GPRReg receiver = params[signature.result == StubResult::None ? 0 : 1].gpr();
        if (std::optional<StubKey> variant = tryWithOperandRegister(called, receiver))
            called = *variant;
        else
            jit.move(receiver, signature.operands[0].gpr());
    }
    emitCallInstruction(jit, called, moves);
    if (mark)
        emitMark(jit);
    if (neverReturns)
        jit.breakpoint();
    else if (branchesOnFlags)
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

static ReturnTraits returnTraitsOf(OperationCall::Returns returns)
{
    switch (returns) {
    case OperationCall::Returns::Value:
        return { B3::Int64, StubResult::GPR };
    case OperationCall::Returns::Void:
        return { B3::Void, StubResult::None };
    case OperationCall::Returns::Double:
        return { B3::Double, StubResult::FPR };
    case OperationCall::Returns::Pair:
        return { B3::Int64, StubResult::TwoGPRs };
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

static bool acceptsRepresentation(RuntimeValueKind kind, Representation representation)
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

static Vector<LValue, 8> checkedValuesOf(const RuntimeSignature& signature, std::span<const Operand> arguments)
{
    RELEASE_ASSERT(arguments.size() == signature.parameters.size());
    Vector<LValue, 8> values;
    for (size_t i = 0; i < arguments.size(); ++i) {
        RELEASE_ASSERT(acceptsRepresentation(signature.parameters[i], arguments[i].representation));
        values.append(arguments[i].value);
    }
    return values;
}

Operand RuntimeCallEmitter::callChecked(const OperationContext* context, RuntimeOperation operation, std::span<const Operand> arguments, Representation result, const CallOptions& options)
{
    RuntimeSignature signature = signatureOf(operation);
    Vector<LValue, 8> values = checkedValuesOf(signature, arguments);
    RELEASE_ASSERT(acceptsRepresentation(signature.result, result) || (signature.result == RuntimeValueKind::Size && result == Representation::Boolean));
    LValue raw = emitEntryCall(context, entryOf(operation), values.span(), options).first;
    if (signature.result == RuntimeValueKind::Void)
        return { };
    return { result, b3TypeOf(result) == B3::Int32 ? m_out.castToInt32(raw) : raw };
}

Operand RuntimeCallEmitter::callOperation(const OperationContext& context, RuntimeOperation operation, std::span<const Operand> arguments, Representation result, const CallOptions& options)
{
    RELEASE_ASSERT(!isLeaf(operation));
    return callChecked(&context, operation, arguments, result, options);
}

StubCallResult RuntimeCallEmitter::callOperationReturningPair(const OperationContext& context, RuntimeOperation operation, std::span<const Operand> arguments, const CallOptions& options)
{
    RuntimeSignature signature = signatureOf(operation);
    RELEASE_ASSERT(signature.result == RuntimeValueKind::Pair);
    return emitEntryCall(&context, entryOf(operation), checkedValuesOf(signature, arguments).span(), options);
}

Operand RuntimeCallEmitter::callWithoutSite(RuntimeOperation operation, std::span<const Operand> arguments, Representation result, const CallOptions& options)
{
    Effects effects = AOT::effectsOf(entryOf(operation));
    RELEASE_ASSERT(!isLeaf(operation) && !effects.mayExitSideways() && (!effects.mayCollect || options.followsThrow));
    return callChecked(nullptr, operation, arguments, result, options);
}

Operand RuntimeCallEmitter::callLeaf(RuntimeOperation operation, std::span<const Operand> arguments, Representation result)
{
    RELEASE_ASSERT(isLeaf(operation));
    return callChecked(nullptr, operation, arguments, result, { });
}

LValue RuntimeCallEmitter::callEntry(const OperationContext& context, AOT::Entry entry, LType type, std::span<const LValue> arguments, const CallOptions& options)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    RELEASE_ASSERT(operation && type == returnTraitsOf(returnsOf(signatureOf(*operation).result)).wordType);
    return emitEntryCall(&context, entry, arguments, options).first;
}

StubCallResult RuntimeCallEmitter::emitEntryCall(const OperationContext* context, AOT::Entry entry, std::span<const LValue> arguments, const CallOptions& callerOptions)
{
    CallOptions options = callerOptions;
    options.isOnRarePath |= context && context->shape() == CodeShape::Compact;
    OperationCall::FirstArgument first = firstArgumentOf(entry);
    CallRequest request = createEntryRequest(entry, options);
    RELEASE_ASSERT(context || first != OperationCall::FirstArgument::None || request.key == tailCallOperation(first));
    placeArguments(request, arguments, first);
    return emitCall(context, request);
}

RuntimeCallEmitter::CallRequest RuntimeCallEmitter::createEntryRequest(AOT::Entry entry, const CallOptions& callerOptions) const
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    RELEASE_ASSERT(operation);
    RuntimeSignature signature = signatureOf(*operation);
    OperationCall::Returns returns = returnsOf(signature.result);
    OperationCall::FirstArgument first = firstArgumentOf(entry);
    m_records.totals.entriesUsed.set(static_cast<size_t>(entry));

    CallRequest request;
    request.options = callerOptions;
    request.options.immediate = offsetInRuntimeTable(entry);
    request.operationResult = returnTraitsOf(returns).result;
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
    unsigned nextGPR = first != OperationCall::FirstArgument::None;
    unsigned nextFPR = 0;
    for (LValue argument : arguments) {
        if (argument->type() == B3::Double) {
            RELEASE_ASSERT(nextFPR < FPRInfo::numberOfArgumentRegisters);
            request.operands.append({ argument, B3::ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++)) });
            continue;
        }
        RELEASE_ASSERT(nextGPR < numberOfOperationArgumentGPRs);
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

RuntimeCallEmitter::CallRequest RuntimeCallEmitter::requestOfDirectFunction(DirectFunctionCall form, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive)
{
    CallRequest request;
    request.key = AOT::callDirectFunction(form);
    request.options.immediate = static_cast<uint32_t>(number * sizeof(BuiltinDirectFunctionBinding) + OBJECT_OFFSETOF(BuiltinDirectFunctionBinding, function));
    request.effectsOfCallee = inputs().stubCatalog.effectsOf(request.key);
    request.argumentBufferAccess = ArgumentBufferAccess::Reads;
    placeArguments(request, arguments, OperationCall::FirstArgument::None);
    for (LValue cell : keptAlive)
        request.operands.append({ cell, B3::ValueRep::LateColdAny });
    return request;
}

StubCallResult RuntimeCallEmitter::callDirectFunctionReturningFields(const OperationContext& context, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive)
{
    CallRequest request = requestOfDirectFunction(DirectFunctionCall::LeafReturningFields, number, arguments, keptAlive);
    Effects memoryAlone = Effects::none();
    memoryAlone.reads = request.effectsOfCallee.reads;
    memoryAlone.writes = request.effectsOfCallee.writes;
    request.effectsOfCallee = memoryAlone;
    return emitCall(&context, request);
}

LValue RuntimeCallEmitter::callDirectFunctionReturningNumber(const OperationContext& context, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive)
{
    Effects effects = context.effects().onAnyEdge();
    RELEASE_ASSERT(!effects.mayThrow && !effects.mayCollect && !effects.mayCallUnknownCode);
    return emitCall(&context, requestOfDirectFunction(DirectFunctionCall::LeafReturningNumber, number, arguments, keptAlive)).first;
}

// A double takes a register of its own kind.
static size_t countGPRArguments(std::span<const LValue> arguments)
{
    return std::ranges::count_if(arguments, [](LValue argument) {
        return argument->type() != B3::Double;
    });
}

bool RuntimeCallEmitter::leavesRegistersForHostFrame(std::span<const LValue> arguments)
{
    return countGPRArguments(arguments) <= directCalleeArgumentIndex;
}

LValue RuntimeCallEmitter::callDirectFunction(const OperationContext& context, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive, std::optional<HostFrameOfDirectCall> hostFrame, BuiltinID calledBuiltin)
{
    Effects effects = context.effects().onAnyEdge();
    bool isLeaf = !effects.mayThrow && !effects.mayCollect && !effects.mayCallUnknownCode;

    Vector<LValue, 8> argumentsWithHostFrame;
    if (hostFrame && !isLeaf) {
        RELEASE_ASSERT(leavesRegistersForHostFrame(arguments));
        size_t numberOfGPRs = countGPRArguments(arguments);
        argumentsWithHostFrame.append(arguments);
        for (; numberOfGPRs < directThisArgumentIndex; ++numberOfGPRs)
            argumentsWithHostFrame.append(m_out.constIntPtr(0));
        if (numberOfGPRs == directThisArgumentIndex)
            argumentsWithHostFrame.append(hostFrame->thisValue);
        argumentsWithHostFrame.append(hostFrame->callee);
        arguments = argumentsWithHostFrame.span();
    }

    DirectFunctionCall form = isLeaf ? DirectFunctionCall::Leaf : hostFrame ? DirectFunctionCall::WithHostFrame : DirectFunctionCall::WithFrame;
    CallRequest request = requestOfDirectFunction(form, number, arguments, keptAlive);
    request.options.calledBuiltin = calledBuiltin;
    return emitCall(&context, request).first;
}

RuntimeCallEmitter::CallRequest RuntimeCallEmitter::createRequest(StubKey key, std::span<const LValue> operands, const CallOptions& options, std::optional<BranchTargets> targets, std::optional<Origin> firstRead) const
{
    const StubCatalog& catalog = inputs().stubCatalog;
    const StubSignature& signature = catalog.summaryOf(key).signature;
    RELEASE_ASSERT(operands.size() == signature.numberOfOperands);
    CallRequest request;
    request.key = key;
    request.options = options;
    request.end = targets ? CallEnd::BranchesOnFlags : CallEnd::Returns;
    request.targets = targets;
    request.firstRead = firstRead;
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
    const StubSummary& summary = catalog.summaryOf(key);
    RELEASE_ASSERT(!summary.signature.usesCallConvention);
    StubSignature signature = withOperationResult(summary.signature, request.operationResult);
    bool returns = request.end == CallEnd::Returns;
    bool branchesOnFlags = request.end == CallEnd::BranchesOnFlags;
    bool neverReturns = request.end == CallEnd::NeverReturns;
    RELEASE_ASSERT(!branchesOnFlags || signature.result == StubResult::Flags);

    Effects effects = context ? effectsOfCall(*context, request.effectsOfCallee) : request.effectsOfCallee;
    if (options.pureFunction)
        effects.reads = { };
    // A site says where in the source a frame stands and which handler is in force there.
    // What finds no room for what it allocates throws where it returns. In a function with handlers its site says which handler is in force there, or that none is.
    bool mayFindNoRoom = context && returns && effects.mayCollect && !options.followsThrow && analyses().cfg().hasHandlers();
    bool hasSite = effects.mayExitSideways() || options.capturesStackTrace || mayFindNoRoom;
    if (m_options.recorded.isSabotaged(Sabotage::CallsThatThrowHaveNoSite) && returns && context && !effects.mayRunOtherCode() && !options.capturesStackTrace && !options.throwSite && !request.firstRead) [[unlikely]]
        hasSite = false;
    RELEASE_ASSERT(context || (!hasSite && (!effects.mayCollect || options.followsThrow)));
    RELEASE_ASSERT(!generator().isInitializingCell() || (!effects.mayCollect && !effects.mayThrow));
    if (effects.mayCollect && context)
        emitter<BarrierEmitter>().validateBeforeCollection(*context);

    // The operands of the operation are not known to be there either.
    bool validates = generator().validates() && context && !generator().isPassingHandlerArguments();
    RuntimeValidationEmitter::OperandCells cellsBeforeCall;
    if (validates && returns && !options.runsCodeOfHost && !mayConvertCells(effects))
        cellsBeforeCall = emitter<RuntimeValidationEmitter>().captureOperandCells(*context, RuntimeValidationEmitter::ComparedCells::Objects);
    else if (validates && returns && !options.runsCodeOfHost && !AOTHeaps::mayReplaceStorageOfArrays(effects))
        cellsBeforeCall = emitter<RuntimeValidationEmitter>().captureOperandCells(*context, RuntimeValidationEmitter::ComparedCells::Arrays);
    RuntimeValidationEmitter::PhotographedOperands photographedOperands;
    bool comesBack = returns || (branchesOnFlags && !effects.mayThrow);
    if (validates && comesBack && !effects.mayRunOtherCode() && !options.runsCodeOfHost && !generator().isInitializingCell())
        photographedOperands = emitter<RuntimeValidationEmitter>().photographOperands(*context, effects);
    uint32_t calleeNumber = request.entry ? static_cast<uint32_t>(*request.entry) : static_cast<uint32_t>(key.family());
    uint32_t photographedCallee = request.entry ? calleeNumber : PhotographedCall::calleeIsStub | calleeNumber;
    std::optional<Integer> stopsBeforeCall;
    if (validates && comesBack && request.effectsOfCallee.mayCollect && !effects.mayCollect && !options.followsThrow)
        stopsBeforeCall = emitter<RuntimeValidationEmitter>().countStopsForCollector();
    auto checkCollectorDidNotStop = [&] {
        if (stopsBeforeCall)
            emitter<RuntimeValidationEmitter>().checkCollectorDidNotStop(*context, *stopsBeforeCall, request.entry ? CalledCode::Kind::Operation : CalledCode::Kind::Stub, calleeNumber);
    };

    B3::PatchpointValue* patchpoint = m_out.patchpoint(patchpointTypeOf(m_proc, signature.result));
    patchpoint->effects.terminal = !returns;
    patchpoint->setNeedsFrameRecord(readsFrameRecordOfCaller(key.family()));
    patchpoint->setRunsWithoutFrame(!hasSite && !generator().validates() && returns && leavesCallerWithoutFrame(key));

    StubKey boundKey = boundKeyOf(catalog, key, options.immediate);
    bool receiverIsInAnyRegister = returns && !request.firstRead && boundKey == key && acceptsAnyReceiverRegister(key, signature, request.operands.size());
    RELEASE_ASSERT(!receiverIsInAnyRegister || !options.throwSite);
    for (const PlacedOperand& operand : request.operands)
        patchpoint->append(B3::ConstrainedValue(operand.value, receiverIsInAnyRegister ? B3::ValueRep(B3::ValueRep::SomeRegister) : operand.rep));
    setResultConstraints(patchpoint, signature);
    RegisterSet late = lateClobbersOf(catalog, boundKey, receiverIsInAnyRegister, signature, request.additionalClobbers);
    patchpoint->clobberLate(late);
    patchpoint->clobberEarly(earlyClobbersOf(signature, receiverIsInAnyRegister, late));

    Effects effectsSinceThrow = effects;
    effectsSinceThrow.mayCollect |= options.followsThrow;
    applyEffects(patchpoint, effectsSinceThrow, request.argumentBufferAccess, options.accessesCollectorState, options.runsCodeOfHost ? CellConversions::WhateverEffectsSay : options.convertsNoCells ? CellConversions::None : CellConversions::AsEffectsSay, options.pureFunction ? SlotsOfSites::StayAsTheyAre : slotsOfSitesBehindStub(context, effects, !request.entry && key.family() == StubFamily::ClosureOfTable));
    if (options.pureFunction) {
        RELEASE_ASSERT(!effects.mayExitSideways() && !effects.mayCollect && request.argumentBufferAccess == ArgumentBufferAccess::None && !options.accessesCollectorState);
        patchpoint->effects.exitsSideways = false;
        patchpoint->pureFunction = *options.pureFunction;
    }

    OptionalIndex site;
    OptionalIndex firstRead;
    if (hasSite) {
        if (request.firstRead)
            firstRead = { addSite(*context, { .origin = request.firstRead }) };
        site = { addSite(*context, options) };
    }

    std::optional<MarkOfCallWithoutSite> mark;
    if (generator().validates() && !hasSite)
        mark = MarkOfCallWithoutSite { context ? context->valueID() : ValueID(), CalledCode::encode(request.entry ? CalledCode::Kind::Operation : CalledCode::Kind::Stub, calleeNumber) };
    CallGenerator callGenerator { &m_records, key, signature, receiverIsInAnyRegister, branchesOnFlags, neverReturns, options.immediate, site, firstRead, options.throwSite, request.preservingCall, mark };
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
    if ((effects.mayThrow || mayFindNoRoom) && site)
        emitter<ExceptionEmitter>().attachLandingPad(*context, patchpoint, *site, returns);
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
    emitCall(&context, createRequest(key, operands, options, targets));
}

void RuntimeCallEmitter::callStubWithoutSiteAndBranch(StubKey key, std::span<const LValue> operands, BranchTargets targets)
{
    emitCall(nullptr, createRequest(key, operands, { }, targets));
}

StubCallResult RuntimeCallEmitter::callStubForTwoReads(const OperationContext& context, StubKey key, std::span<const LValue> operands, const CallOptions& options, Origin firstRead)
{
    return emitCall(&context, createRequest(key, operands, options, std::nullopt, firstRead));
}

B3::PatchpointValue* RuntimeCallEmitter::createCallPatchpoint(const OperationContext& context, const CallPatchpointRequest& request)
{
    Effects effects = effectsOfCall(context, request.effects);
    // Where no handler of the function takes it, a frame that finds no room ends the function: none of its code runs behind that call.
    if (!effects.mayThrowBeyondLackOfStack() && !emitter<ExceptionEmitter>().unwindsWithinFunction(context))
        effects.mayThrow = false;
    if (effects.mayCollect)
        emitter<BarrierEmitter>().validateBeforeCollection(context);

    B3::PatchpointValue* patchpoint = request.holdsUniqueRecord ? m_out.patchpointWithUniqueRecord(request.resultType) : m_out.patchpoint(request.resultType);
    patchpoint->clobberEarly(request.earlyClobbered);
    patchpoint->clobberLate(request.lateClobbered);
    applyEffects(patchpoint, effects, request.argumentBufferAccess, false, CellConversions::AsEffectsSay, request.callsStub ? slotsOfSitesBehindStub(&context, effects) : SlotsOfSites::MayChange);
    patchpoint->setRunsWithoutFrame(request.runsWithoutFrame);
    patchpoint->effects.terminal = request.neverReturns;
    return patchpoint;
}

void RuntimeCallEmitter::finishCallPatchpoint(const OperationContext& context, B3::PatchpointValue* patchpoint, uint32_t site, const CallPatchpointRequest& request)
{
    if (request.effects.mayThrow || request.effects.mayCollect)
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

RecordedSite RuntimeCallEmitter::recordedSiteOf(const OperationContext& context, const CallOptions& options)
{
    // Whatever a built-in runs, its function object, its inline code or an operation, runs after the tail call.
    Origin origin = options.origin.value_or(context.origin());
    const Value* value = context.valueID() ? &m_function.at(context.valueID()) : nullptr;
    BuiltinID calledBuiltin = options.calledBuiltin;
    if (!calledBuiltin && value && Value::opcodesWithCalledBuiltin.contains(value->opcode))
        calledBuiltin = value->calledBuiltin();
    // Steps of a built-in function that were expanded into its caller record the caller's site and name the function.
    if (BuiltinID builtinOfFrame = origin.inlineFrame ? m_function.inlineFrames[origin.inlineFrame].calledBuiltin : BuiltinID()) {
        const auto& frame = m_function.inlineFrames[origin.inlineFrame];
        calledBuiltin = builtinOfFrame;
        origin = { .position = frame.callPosition, .inlineFrame = frame.parent, .isTailCall = frame.isTailCall, .isPartOfConstruction = frame.callIsPartOfConstruction };
    }
    // No stack trace lists a function that only built-in code can name.
    if (calledBuiltin && BuiltinBindings::linkTimeConstantOf(calledBuiltin))
        calledBuiltin = { };
    bool standsForCallee = origin.isTailCall && !options.isTailCall && !options.constructs;
    // A call, and the read of the method that it calls, can throw before the callee begins.
    bool isInsideCallee = standsForCallee && (calledBuiltin || !value || !runsBeforeCalleeOf(value->opcode));
    std::optional<CallSiteKind> kind = findCallSiteKind({ .isConstruct = options.constructs, .isTailCall = options.isTailCall || origin.isTailCall, .isInsideCallee = isInsideCallee, .namesBuiltin = !!calledBuiltin });
    RELEASE_ASSERT(kind);
    return { .origin = origin, .kind = *kind, .calledBuiltin = calledBuiltin, .constructedCallee = options.constructedCallee, .callsOwnFunctionObject = options.callsOwnFunctionObject };
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

LValue RuntimeCallEmitter::transientBuffer(size_t bytes)
{
    if (!m_transientBuffer) {
        m_transientBuffer = m_proc.addStackSlot(bytes);
        m_transientBuffer->setIsDeadAtSuspensions(true);
    } else
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

Effects RuntimeCallEmitter::effectsOfCall(const OperationContext& context, const Effects& effectsOfCallee) const
{
    Effects effects = context.effects().onAnyEdge().intersectedWith(effectsOfCallee);
    if (m_options.recorded.isSabotaged(Sabotage::NoCallCollects))
        effects.mayCollect = false;
    if (m_options.recorded.isSabotaged(Sabotage::NoCallWrites))
        effects.writes = { };
    return effects;
}

bool RuntimeCallEmitter::mayConvertCells(const Effects& effects)
{
    return AOTHeaps::mayConvertCells(effects) && !build().recordedOptions().has(TestingSwitch::NoCallConvertsCells);
}

// A slot says what a lookup found. It changes where the operation that it belongs to finds no answer in it, where code of the program runs, and where something is written that a lookup may come by.
// A collection empties the slot of a Structure that has died, and whatever makes a cell may make a Structure where that one lay: it would pass for the old one by its number.
RuntimeCallEmitter::SlotsOfSites RuntimeCallEmitter::slotsOfSitesBehindStub(const OperationContext* context, const Effects& effects, bool makesClosureOfTable) const
{
    bool hasSlotOfItsOwn = context && context->valueID() && siteCachesOf(m_function.at(context->valueID()).opcode);
    return hasSlotOfItsOwn || makesClosureOfTable || effects.mayCollect || !effects.writes.containsOnly(heapsWhoseWritesNobodySees) ? SlotsOfSites::MayChange : SlotsOfSites::StayAsTheyAre;
}

void RuntimeCallEmitter::stateWhichCallsChangeWhatOnlyGeneratedCodeReads()
{
    for (const B3::AbstractHeap* heap : { &m_heaps.AOTInstance_siteCaches, &m_heaps.AOTInstance_closureCells, &m_heaps.counters }) {
        if (!m_heaps.isNamedByMemoryAccess(*heap))
            continue;
        for (B3::PatchpointValue* patchpoint : m_callsThatMayChangeSlotsOfSites)
            m_heaps.decoratePatchpointWrite(heap, patchpoint);
    }
}

void RuntimeCallEmitter::applyEffects(B3::PatchpointValue* patchpoint, const Effects& effects, ArgumentBufferAccess access, bool accessesCollectorState, CellConversions conversions, SlotsOfSites slotsOfSites)
{
    RELEASE_ASSERT(!generator().isInitializingCell() || (!effects.mayCollect && !effects.mayThrow));

    bool isTerminal = patchpoint->effects.terminal;
    patchpoint->effects = B3::Effects::none();
    patchpoint->effects.terminal = isTerminal;
    patchpoint->effects.controlDependent = true;
    if (slotsOfSites == SlotsOfSites::MayChange && !m_options.recorded.isSabotaged(Sabotage::SlotsOfSitesStayAsTheyAre))
        m_callsThatMayChangeSlotsOfSites.append(patchpoint);
    bool convertsCells = conversions == CellConversions::WhateverEffectsSay ? !build().recordedOptions().has(TestingSwitch::NoCallConvertsCells) : conversions == CellConversions::AsEffectsSay && mayConvertCells(effects);
    if (effects.isPure() && access == ArgumentBufferAccess::None && !accessesCollectorState && !convertsCells)
        return;

    bool exitsSideways = effects.mayExitSideways();
    B3HeapList reads;
    B3HeapList writes;
    bool replacesStorageOfArrays = conversions == CellConversions::WhateverEffectsSay || AOTHeaps::mayReplaceStorageOfArrays(effects);
    bool saysLessThanItWrites = m_options.recorded.isSabotaged(Sabotage::CallsGiveNoArrayOtherStorage) && !effects.writes.isTop();
    m_heaps.appendHeaps(effects.reads, reads);
    m_heaps.appendHeaps(saysLessThanItWrites ? effects.writes.without(AbstractHeap::ArrayStorage) : effects.writes, writes);
    if (convertsCells)
        m_heaps.appendHeapsOfConvertedCells(writes, replacesStorageOfArrays && !saysLessThanItWrites ? AOTHeaps::ConvertedCells::All : AOTHeaps::ConvertedCells::AllButArrays);
    if (effects.mayCollect)
        m_heaps.appendHeapsWrittenByCollection(writes);
    else if (accessesCollectorState)
        writes.append(&m_heaps.collectorState);
    if (effects.mayCollect)
        reads.append(&m_heaps.frameSlots);
    if (accessesCollectorState)
        reads.append(&m_heaps.collectorState);
    switch (access) {
    case ArgumentBufferAccess::ReadsAndWrites:
        writes.append(&m_heaps.argumentBuffer);
        [[fallthrough]];
    case ArgumentBufferAccess::Reads:
        reads.append(&m_heaps.argumentBuffer);
        break;
    case ArgumentBufferAccess::None:
        break;
    }
    // B3 takes the reads of a value that exits sideways as everything that is read after the exit.
    if (exitsSideways)
        patchpoint->effects.reads = B3::HeapRange::top();
    else if (!reads.isEmpty())
        m_heaps.decoratePatchpointRead(reads.span(), patchpoint);
    if (!writes.isEmpty())
        m_heaps.decoratePatchpointWrite(writes.span(), patchpoint);
    patchpoint->effects.exitsSideways = exitsSideways;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
