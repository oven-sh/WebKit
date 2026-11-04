/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeCallEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTExceptionEmitter.h"
#include "AOTHeaps.h"
#include "AOTPureOperations.h"
#include "AOTStubCatalog.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsOperations.h"
#include "AOTThrowEmitter.h"
#include "AllowMacroScratchRegisterUsage.h"
#include <algorithm>
#include <array>
#include <bit>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CallGenerator {
    void generate(CCallHelpers&, const Machine::PatchParameters&) const;
    ArgumentMoves moveArguments(CCallHelpers&, const Machine::PatchParameters&) const;
    ArgumentMoves bringOperands(const Machine::PatchParameters&) const;
    void emitCallInstruction(CCallHelpers&, StubKey, const ArgumentMoves&) const;
    void emitBranchOnFlags(CCallHelpers&, const Machine::PatchParameters&) const;

    CodeRecords* records;
    StubKey key;
    StubSignature signature;
    bool receiverIsInAnyRegister;
    bool resultIsInAnyRegister;
    bool branchesOnFlags;
    bool neverReturns;
    std::optional<uint32_t> immediate;
    std::optional<NumberedImmediate> numberedImmediate;
    OptionalIndex site;
    OptionalIndex firstRead;
    std::optional<ThrowSite> throwSite;
    std::optional<OperationCall> preservingCall;
    std::optional<StubKey> keyWhereNoRegisterIsKept;
    std::optional<StubKey> keyWhereNoFPRIsKept;
};

// It is copied into the arena of the procedure, which runs no destructor for it.
static_assert(std::is_trivially_destructible_v<CallGenerator>);

struct ReturnTraits {
    LType wordType;
    StubResult result;
};

} // anonymous namespace

ArgumentMoves CallGenerator::moveArguments(CCallHelpers& jit, const Machine::PatchParameters& params) const
{
    ArgumentMoves moves;
    Machine::PatchOperation* call = params.value();
    unsigned numberOfChildren = call->numChildren();
    unsigned firstRep = params.size() - numberOfChildren;
    RegisterSet holdsArgument;
    for (unsigned i = 0; i < numberOfChildren; ++i) {
        if (const Machine::ValueRep& rep = params[firstRep + i]; rep.isGPR())
            holdsArgument.add(rep.gpr(), IgnoreVectors);
    }
    unsigned destinationIndex = preservingCall->firstArgument != OperationCall::FirstArgument::None;
    for (unsigned i = 0; i < numberOfChildren; ++i) {
        LType childType = call->child(i)->type();
        if (childType == Machine::Double)
            continue;
        GPRReg destination = argumentGPROfPreservingCall(*preservingCall, entryAtOffset(*immediate), destinationIndex++);
        const Machine::ValueRep& rep = params[firstRep + i];
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
        // A frame that is set up has a record, so the thunk reads the word off the frame pointer.
        RELEASE_ASSERT(rep.isStack() && params.frameIsSetUp());
        bool isNarrow = childType == Machine::Int32;
        appendArgumentMove(moves, destination, isNarrow ? ArgumentSource::Stack32 : ArgumentSource::Stack64, static_cast<int64_t>(rep.offsetFromFP()));
    }
    return moves;
}

// PatchOperation::leaveOperandsWhereTheyLie().
ArgumentMoves CallGenerator::bringOperands(const Machine::PatchParameters& params) const
{
    ArgumentMoves moves;
    Machine::PatchOperation* call = params.value();
    unsigned numberOfChildren = call->numChildren();
    unsigned firstRep = params.size() - numberOfChildren;
    for (unsigned i = 0; i < numberOfChildren; ++i) {
        Reg destination = call->registerToBringOperandTo(i);
        if (!destination)
            continue;
        const Machine::ValueRep& rep = params[firstRep + i];
        if (rep.isGPR()) {
            if (rep.gpr() != destination.gpr())
                appendArgumentMove(moves, destination.gpr(), ArgumentSource::Register, rep.gpr());
            continue;
        }
        bool isNarrow = call->child(i)->type() == Machine::Int32;
        if (rep.isConstant()) {
            appendArgumentMove(moves, destination.gpr(), ArgumentSource::Constant, isNarrow ? static_cast<uint32_t>(rep.value()) : static_cast<uint64_t>(rep.value()));
            continue;
        }
        RELEASE_ASSERT(rep.isStack() && params.frameIsSetUp());
        appendArgumentMove(moves, destination.gpr(), isNarrow ? ArgumentSource::Stack32 : ArgumentSource::Stack64, static_cast<int64_t>(rep.offsetFromFP()));
    }
    return moves;
}

void CallGenerator::emitCallInstruction(CCallHelpers& jit, StubKey called, const ArgumentMoves& moves) const
{
    StubCalls& stubCalls = records->stubCalls;
    RELEASE_ASSERT(!immediate || !numberedImmediate);
    if (!moves.isEmpty()) {
        RELEASE_ASSERT(!throwSite && !firstRead);
        if (numberedImmediate)
            stubCalls.call(jit, called, *numberedImmediate, site, moves);
        else if (preservingCall)
            stubCalls.call(jit, called, immediate, site, moves);
        else
            stubCalls.callWithImmediateOfSite(jit, called, immediate, site, moves);
        return;
    }

    RELEASE_ASSERT(!throwSite || !firstRead);
    if (firstRead && !immediate && !numberedImmediate) {
        stubCalls.callForTwoReads(jit, called, firstRead, site);
        return;
    }
    uint32_t returnSite = numberedImmediate ? stubCalls.call(jit, called, *numberedImmediate, site, firstRead)
        : immediate ? stubCalls.call(jit, called, *immediate, site, firstRead)
        : stubCalls.call(jit, called, site);
    if (!throwSite)
        return;
    // The fault table names the call by its offset.
    Vector<uint32_t, 3> words = throwSiteWords(*throwSite, Representation::JSValue, signature.operands[0].gpr());
    uint8_t wordsThatAreMentions = mentionsAmongThrowSiteWords(*throwSite, words.size());
    records->traps.callThrowSites.append(CallThrowSite { returnSite, WTF::move(words), wordsThatAreMentions });
}

void CallGenerator::emitBranchOnFlags(CCallHelpers& jit, const Machine::PatchParameters& params) const
{
    bool jumpsIfTaken = params.fallsThroughToSuccessor(0);
    MacroAssembler::ResultCondition condition = jumpsIfTaken ? signature.trueCondition : MacroAssembler::invert(signature.trueCondition);
    RELEASE_ASSERT(condition == MacroAssembler::Zero || condition == MacroAssembler::NonZero);
    CCallHelpers::Jump leaves = jit.branchOnFlags(condition == MacroAssembler::Zero ? CCallHelpers::Equal : CCallHelpers::NotEqual);
    CCallHelpers::Jump taken;
    if (!jumpsIfTaken && !params.fallsThroughToSuccessor(1))
        taken = jit.jump();
    std::span<MacroAssembler::Label* const> labels = params.successorLabels();
    params.addLatePath([leaves, taken, targetOfLeaves = labels[jumpsIfTaken], targetOfTaken = labels[1]](CCallHelpers& jit) {
        leaves.linkTo(*targetOfLeaves, &jit);
        if (taken.isSet())
            taken.linkTo(*targetOfTaken, &jit);
    });
}

static Machine::RegisterMask registersToSaveForCCall(Machine::RegisterMask liveRegisters)
{
    static const Machine::RegisterMask callerSavedRegisters = Machine::maskOf(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    return liveRegisters & callerSavedRegisters;
}

// A unit that saves the registers of its callers has saved those that it writes itself. The others still hold what a caller keeps in them, and a C function may write them.
static Machine::RegisterMask registersToKeepAcrossCCall(const Machine::PatchParameters& params)
{
    Machine::RegisterMask held = params.usedRegistersMask();
    if (params.proc().traits().savesRegistersOfCallers) [[unlikely]]
        held |= Machine::maskOf(params.unavailableRegisters());
    return registersToSaveForCCall(held);
}

static bool holdsFPR(Machine::RegisterMask registers)
{
    static const Machine::RegisterMask allFPRs = Machine::maskOf(RegisterSet::allFPRs());
    return registers & allFPRs;
}

void CallGenerator::generate(CCallHelpers& jit, const Machine::PatchParameters& params) const
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    StubKey called = key;
    ArgumentMoves moves;
    if (preservingCall) {
        Machine::RegisterMask live = registersToKeepAcrossCCall(params);
        if (signature.firstResult)
            live &= ~Machine::maskOf(signature.firstResult);
        if (signature.secondResult)
            live &= ~Machine::maskOf(signature.secondResult);
        OperationCall call = *preservingCall;
        bool savesFPRs = holdsFPR(live) || call.returns == OperationCall::Returns::Double;
        call.saves = savesFPRs ? OperationCall::Saves::CallerSavedRegisters : OperationCall::Saves::CallerSavedGPRs;
        called = callOperationPreservingRegisters(call);
        moves = moveArguments(jit, params);
    } else
        moves = bringOperands(params);
    if (keyWhereNoRegisterIsKept || keyWhereNoFPRIsKept) {
        Machine::RegisterMask kept = registersToKeepAcrossCCall(params);
        if (resultIsInAnyRegister)
            kept &= ~Machine::maskOf(Reg(params[0].gpr()));
        else if (signature.firstResult)
            kept &= ~Machine::maskOf(signature.firstResult);
        if (signature.secondResult)
            kept &= ~Machine::maskOf(signature.secondResult);
        if (keyWhereNoRegisterIsKept && !kept)
            called = *keyWhereNoRegisterIsKept;
        else if (keyWhereNoFPRIsKept && !holdsFPR(kept))
            called = *keyWhereNoFPRIsKept;
    }
    if (receiverIsInAnyRegister) {
        bool resultComesFirst = signature.result != StubResult::None && signature.result != StubResult::Flags;
        GPRReg receiver = params[resultComesFirst ? 1 : 0].gpr();
        GPRReg receiverOfStub = signature.operands[0].gpr();
        if (std::optional<StubKey> variant = receiver == receiverOfStub ? std::nullopt : tryWithOperandRegister(called, receiver))
            called = *variant;
        else {
            // Only such a call has said that it writes the register.
            RELEASE_ASSERT(receiver == receiverOfStub || descriptorOf(called.family()).operandRegisters == OperandRegisterChoice::AlternateReceivers);
            jit.move(receiver, receiverOfStub);
        }
    }
    // Where the choice names the register of the result, the stub itself serves the usual one.
    if (resultIsInAnyRegister && (params[0].gpr() != signature.firstResult.gpr() || !namesRegisterOfResult(descriptorOf(called.family()).operandRegisters)))
        called = called.withOperandRegister(params[0].gpr());
    emitCallInstruction(jit, called, moves);
    if (neverReturns)
        jit.breakpoint();
    else if (branchesOnFlags)
        emitBranchOnFlags(jit, params);
}

static StubKey boundKeyOf(const StubCatalog& catalog, StubKey key, std::optional<uint32_t> immediate)
{
    return immediate && catalog.bindsImmediate(key) ? key.withImmediate(*immediate) : key;
}

static bool acceptsAnyReceiverRegister(StubKey key, const StubSignature& signature, size_t numberOfOperands)
{
    OperandRegisterChoice choice = descriptorOf(key.family()).operandRegisters;
    if (operandRegistersOf(choice).empty() || namesRegisterOfResult(choice) || numberOfOperands != 1)
        return false;
    return choice != OperandRegisterChoice::AlternateReceivers || signature.result == StubResult::GPR || signature.result == StubResult::None;
}

// OperandRegisterChoice::AnyAllocatableRegister of a stub without an operand, and the choices that name the register of the result.
static bool returnsInAnyRegister(StubKey key, const StubSignature& signature, size_t numberOfOperands)
{
    OperandRegisterChoice choice = descriptorOf(key.family()).operandRegisters;
    if (signature.result != StubResult::GPR || operandRegistersOf(choice).empty())
        return false;
    return namesRegisterOfResult(choice) || (choice == OperandRegisterChoice::AnyAllocatableRegister && !numberOfOperands);
}

// A leaf calls nothing and leaves the stack as it found it. On x86-64 the call itself stores the return address, so the caller needs no frame for it. Nor for a stub below which nobody walks the stack.
bool leavesCallerWithoutFrame(StubKey key)
{
    return isX86_64() && descriptorOf(key.family()).leafness != StubLeafness::MayCall;
}

static RegisterSet lateClobbersOf(const StubCatalog& catalog, StubKey key, bool receiverIsInAnyRegister, const StubSignature& signature, const RegisterSet& additionalClobbers)
{
    RegisterSet late = receiverIsInAnyRegister ? catalog.clobberedByRegisterVariantsOf(key) : catalog.summaryOf(key).clobbered;
    // A receiver that lies where no variant takes it is moved to where the stub itself does.
    if (receiverIsInAnyRegister && descriptorOf(key.family()).operandRegisters == OperandRegisterChoice::AlternateReceivers)
        late.add(signature.operands[0], IgnoreVectors);
    if (signature.immediate)
        late.add(*signature.immediate, IgnoreVectors);
    late.merge(RegisterSet::macroClobberedGPRs());
    late.merge(additionalClobbers);
    return late;
}

static RegisterSet earlyClobbersOf(StubKey key, const StubSignature& signature, bool receiverIsInAnyRegister, const RegisterSet& late)
{
    RegisterSet early = RegisterSet::macroClobberedGPRs();
    // A variant may take the receiver where the others take the immediate, and the immediate elsewhere.
    if (signature.immediate) {
        std::optional<StubKey> variant = receiverIsInAnyRegister ? tryWithOperandRegister(key, *signature.immediate) : std::nullopt;
        if (!variant || signatureOf(*variant).immediate == signature.immediate)
            early.add(*signature.immediate, IgnoreVectors);
    }
    // The receiver lies where a variant takes it, or where no variant writes.
    if (receiverIsInAnyRegister) {
        RegisterSet writtenBeforeMove = late;
        for (GPRReg reg : operandRegistersOf(descriptorOf(key.family()).operandRegisters))
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

static void setResultConstraints(Machine::PatchOperation* patchpoint, const StubSignature& signature)
{
    switch (signature.result) {
    case StubResult::None:
    case StubResult::Flags:
        return;
    case StubResult::GPR:
    case StubResult::FPR:
        patchpoint->resultConstraints = { Machine::ValueRep::reg(signature.firstResult) };
        return;
    case StubResult::TwoGPRs:
        patchpoint->resultConstraints = { Machine::ValueRep::reg(signature.firstResult), Machine::ValueRep::reg(signature.secondResult) };
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct StubCallShape {
    uint64_t catalogIdentifier { 0 };
    StubKey key;
    StubKey boundKey;
    uint32_t traits { 0 };

    StubSignature signature;
    Machine::RegisterMask lateClobbered { 0 };
    Machine::RegisterMask earlyClobbered { 0 };
    Machine::RegisterMask operandRegisters { 0 };
    RegisterSet resultRegisters;
    std::optional<StubKey> keyWhereNoRegisterIsKept;
    std::optional<StubKey> keyWhereNoFPRIsKept;
    uint8_t numberOfOperandRegisters { 0 };
    bool needsFrameRecord { false };
    bool leavesCallerWithoutFrame { false };
    bool receiverIsInAnyRegister { false };
    bool resultIsInAnyRegister { false };
    bool hasEffects { false };
    Effects effects;
};

namespace {

struct StubCallTraits {
    static constexpr uint32_t neverReturns = 1 << 3;
    static constexpr uint32_t returns = 1 << 4;
    static constexpr uint32_t hasFirstRead = 1 << 5;
    static constexpr uint32_t hasThrowSite = 1 << 6;
    static constexpr uint32_t hasPreservingCall = 1 << 7;
    static constexpr unsigned numberOfOperandsShift = 8;
};

constexpr unsigned stubCallShapeCacheBits = 9;

} // anonymous namespace

struct StubCallShapeCache {
    std::array<StubCallShape, 1 << stubCallShapeCacheBits> shapes;
};

static void fillStubCallShape(StubCallShape& shape, const StubCatalog& catalog, StubKey key, StubKey boundKey, std::optional<uint32_t> immediate, uint32_t traits)
{
    RELEASE_ASSERT(exists(key));
    const StubSummary& summary = catalog.summaryOf(key);
    RELEASE_ASSERT(!summary.signature.usesCallConvention);
    StubSignature signature = withOperationResult(summary.signature, static_cast<StubResult>(traits & 7));
    bool neverReturns = traits & StubCallTraits::neverReturns;
    bool returns = traits & StubCallTraits::returns;
    bool namesSiteOrOperand = traits & (StubCallTraits::hasFirstRead | StubCallTraits::hasThrowSite);
    size_t numberOfOperands = traits >> StubCallTraits::numberOfOperandsShift;
    OperandRegisterChoice choice = descriptorOf(key.family()).operandRegisters;

    shape.signature = signature;
    shape.needsFrameRecord = readsFrameRecordOfCaller(key.family());
    shape.leavesCallerWithoutFrame = leavesCallerWithoutFrame(key);
    shape.receiverIsInAnyRegister = !neverReturns && !namesSiteOrOperand && boundKey == key && acceptsAnyReceiverRegister(key, signature, numberOfOperands);
    shape.resultIsInAnyRegister = returns && !namesSiteOrOperand && boundKey == key && returnsInAnyRegister(key, signature, numberOfOperands);
    shape.operandRegisters = Machine::maskOf(RegisterSet::fromIterable(operandRegistersOf(choice)));
    shape.numberOfOperandRegisters = static_cast<uint8_t>(operandRegistersOf(choice).size());
    shape.resultRegisters = { };
    if (shape.resultIsInAnyRegister) {
        RegisterSet lost = catalog.clobberedBesideResultByRegisterVariantsOf(key);
        lost.merge(RegisterSet::macroClobberedGPRs());
        shape.resultRegisters = RegisterSet::fromIterable(operandRegistersOf(choice));
        shape.resultRegisters.add(signature.firstResult, IgnoreVectors);
        RegisterSet early = RegisterSet::macroClobberedGPRs();
        if (signature.immediate && *signature.immediate != signature.firstResult.gpr())
            early.add(*signature.immediate, IgnoreVectors);
        shape.lateClobbered = Machine::maskOf(lost);
        shape.earlyClobbered = Machine::maskOf(early);
    } else {
        RegisterSet late = lateClobbersOf(catalog, boundKey, shape.receiverIsInAnyRegister, signature, { });
        shape.lateClobbered = Machine::maskOf(late);
        shape.earlyClobbered = Machine::maskOf(earlyClobbersOf(key, signature, shape.receiverIsInAnyRegister, late));
    }

    auto formThatDiffers = [&](CallerFrame callerFrame) -> std::optional<StubKey> {
        StubKey form = key.withCallerFrame(callerFrame);
        if (key.callerFrame() == CallerFrame::HasFrame && exists(form) && !neverReturns && !shape.receiverIsInAnyRegister && (!shape.resultIsInAnyRegister || namesRegisterOfResult(choice)) && !namesSiteOrOperand && !(traits & StubCallTraits::hasPreservingCall)) {
            RELEASE_ASSERT(!immediate || boundKey != key || !catalog.bindsImmediate(form));
            if (catalog.summaryOf(boundKeyOf(catalog, form, immediate)).clobbered != catalog.summaryOf(boundKey).clobbered)
                return form;
        }
        return std::nullopt;
    };
    shape.keyWhereNoRegisterIsKept = formThatDiffers(CallerFrame::KeepsNoRegisters);
    shape.keyWhereNoFPRIsKept = formThatDiffers(CallerFrame::KeepsNoFPRs);
    shape.hasEffects = false;
}

static StubCallShape& shapeOfStubCall(StubCallShapeCache& cache, const StubCatalog& catalog, StubKey key, std::optional<uint32_t> immediate, uint32_t traits)
{
    StubKey boundKey = boundKeyOf(catalog, key, immediate);
    StubKey cachedBoundKey = boundKey;
    if (boundKey != key) {
        const StubDescriptor& descriptor = descriptorOf(key.family());
        if (descriptor.binding == ImmediateBinding::Every && descriptorOf(descriptor.body).binding == ImmediateBinding::Every)
            cachedBoundKey = key.withImmediate(0);
    }

    uint64_t hash = (key.bits() ^ std::rotl(cachedBoundKey.bits(), 29) ^ traits) * 0x9E3779B97F4A7C15ull;
    StubCallShape& shape = cache.shapes[hash >> (64 - stubCallShapeCacheBits)];
    if (shape.key == key && shape.boundKey == cachedBoundKey && shape.traits == traits && shape.catalogIdentifier == catalog.identifier()) [[likely]]
        return shape;

    fillStubCallShape(shape, catalog, key, boundKey, immediate, traits);
    shape.catalogIdentifier = catalog.identifier();
    shape.key = key;
    shape.boundKey = cachedBoundKey;
    shape.traits = traits;
    return shape;
}

struct PatchEffectsMemo {
    static constexpr unsigned numberOfBits = 10;
    static constexpr size_t numberOfEntries = 1 << numberOfBits;
    static constexpr size_t maxProbes = 8;

    struct Entry {
        Effects effects;
        uint8_t traits { 0 };
        bool isFilled { false };
        Machine::Effects patchEffects;
    };

    ~PatchEffectsMemo()
    {
        for (Machine::HeapIndex* list : lists)
            fastFree(list);
    }

    Machine::HeapSet keep(const Machine::HeapSet& set)
    {
        std::span<const Machine::HeapIndex> heaps = set.heaps();
        if (heaps.empty())
            return set;
        auto* list = static_cast<Machine::HeapIndex*>(fastMalloc(heaps.size_bytes()));
        std::ranges::copy(heaps, list);
        lists.append(list);
        return Machine::HeapSet::of(std::span<const Machine::HeapIndex>(list, heaps.size()));
    }

    std::array<Entry, numberOfEntries> entries;
    Vector<Machine::HeapIndex*> lists;
};

static uint64_t hashOfLocationMasks(const LocationMasks& masks, uint64_t hash)
{
    hash = (std::rotl(hash, 32) ^ static_cast<uint64_t>(masks.heaps().toRaw())) * 0x9E3779B97F4A7C15ull;
    if (!masks.containsAny(indexedHeaps))
        return hash;
    for (AbstractHeap heap : indexedHeapsInOrder)
        hash = (std::rotl(hash, 32) ^ masks.maskOf(heap)) * 0x9E3779B97F4A7C15ull;
    return hash;
}

static PatchEffectsMemo::Entry* findPatchEffects(PatchEffectsMemo& memo, const Effects& effects, uint8_t traits)
{
    uint64_t hash = hashOfLocationMasks(effects.writes, hashOfLocationMasks(effects.reads, traits | (effects.mayCollect ? 0x100 : 0) | (effects.mayExitSideways() ? 0x200 : 0)));
    size_t index = hash >> (64 - PatchEffectsMemo::numberOfBits);
    for (size_t probe = 0; probe < PatchEffectsMemo::maxProbes; ++probe, index = (index + 1) & (PatchEffectsMemo::numberOfEntries - 1)) {
        PatchEffectsMemo::Entry& entry = memo.entries[index];
        if (!entry.isFilled || (entry.traits == traits && entry.effects == effects))
            return &entry;
    }
    return nullptr;
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
        return { Machine::Int64, StubResult::GPR };
    case OperationCall::Returns::Void:
        return { Machine::Void, StubResult::None };
    case OperationCall::Returns::Double:
        return { Machine::Double, StubResult::FPR };
    case OperationCall::Returns::Pair:
        return { Machine::Int64, StubResult::TwoGPRs };
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
    return { result, machineTypeOf(result) == Machine::Int32 ? m_out.castToInt32(raw) : raw };
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

StubCallResult RuntimeCallEmitter::emitEntryCall(const OperationContext* context, AOT::Entry entry, std::span<const LValue> arguments, const CallOptions& options)
{
    OperationCall::FirstArgument first = firstArgumentOf(entry);
    CallRequestWithOwnOptions request;
    fillEntryRequest(request, entry, options);
    RELEASE_ASSERT(context || first != OperationCall::FirstArgument::None || request.key == tailCallOperation(first));
    placeArguments(request, arguments, first);
    return emitCall(context, request);
}

void RuntimeCallEmitter::fillEntryRequest(CallRequestWithOwnOptions& request, AOT::Entry entry, const CallOptions& callerOptions) const
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    RELEASE_ASSERT(operation);
    RuntimeSignature signature = signatureOf(*operation);
    OperationCall::Returns returns = returnsOf(signature.result);
    OperationCall::FirstArgument first = firstArgumentOf(entry);
    m_records.totals.entriesUsed.set(static_cast<size_t>(entry));

    request.ownOptions = callerOptions;
    request.ownOptions.immediate = offsetInRuntimeTable(entry);
    request.options = &request.ownOptions;
    request.operationResult = returnTraitsOf(returns).result;
    request.ownEffects = AOT::effectsOf(entry);
    request.effectsOfCallee = &request.ownEffects;
    request.argumentBufferAccess = argumentBufferAccessOf(*operation);
    request.entry = entry;
    if (entry == AOT::Entry::operationAOTStopAtNativeResult || entry == AOT::Entry::operationAOTStopAtFalseProof) {
        RELEASE_ASSERT(returns == OperationCall::Returns::Void);
        request.end = CallEnd::NeverReturns;
    }
    bool isPure = request.ownEffects.isPure() && request.argumentBufferAccess == ArgumentBufferAccess::None && returns != OperationCall::Returns::Void && signature.parameters.size() <= 3;
    if (isPure && !request.ownOptions.pureFunction)
        request.ownOptions.pureFunction = pureFunction(PureOperation::RuntimeOperation, static_cast<uint32_t>(entry), true);

    bool checksException = request.ownEffects.mayThrow;
    bool canPreserveRegisters = returns != OperationCall::Returns::Pair
        && numberOfGPRArgumentsOf(signature, first) <= GPRInfo::numberOfArgumentRegisters
        && (first != OperationCall::FirstArgument::None || !checksException);
    if (callerOptions.isOnRarePath && canPreserveRegisters) {
        request.preservingCall = OperationCall { .returns = returns, .firstArgument = first, .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = checksException, .pollsBeforeReturning = mayFailToAllocate(entry, first) };
        request.key = callOperationPreservingRegisters(*request.preservingCall);
    } else
        request.key = callOperationFor(entry, returns, first);
}

void RuntimeCallEmitter::placeArguments(CallRequest& request, std::span<const LValue> arguments, OperationCall::FirstArgument first) const
{
    unsigned nextGPR = first != OperationCall::FirstArgument::None;
    unsigned nextFPR = 0;
    for (LValue argument : arguments) {
        if (argument->type() == Machine::Double) {
            RELEASE_ASSERT(nextFPR < FPRInfo::numberOfArgumentRegisters);
            request.operands.append({ argument, Machine::ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++)) });
            continue;
        }
        RELEASE_ASSERT(nextGPR < numberOfOperationArgumentGPRs);
        if (!request.preservingCall) {
            request.operands.append({ argument, Machine::ValueRep::reg(operationArgumentGPR(nextGPR++)) });
            continue;
        }
        GPRReg destination = argumentGPROfPreservingCall(*request.preservingCall, *request.entry, nextGPR++);
        request.additionalClobbers.add(destination, IgnoreVectors);
        // What has no position is made again at each use, so it lies nowhere that a thunk could read. A number of the image among them is moved by the site, never by a thunk.
        if (argument->hasNoPosition() && !argument->isConstant()) {
            request.operands.append({ argument, Machine::ValueRep::reg(destination) });
            continue;
        }
        LValue widened = argument->hasInt32() ? m_out.constInt64(static_cast<uint32_t>(argument->asInt32())) : argument;
        request.operands.append({ widened, Machine::ValueRep::ColdAny });
    }
}

void RuntimeCallEmitter::fillRequestOfDirectFunction(CallRequestWithOwnOptions& request, DirectFunctionCall form, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive)
{
    request.key = AOT::callDirectFunction(form);
    request.ownOptions.immediate = static_cast<uint32_t>(number * sizeof(BuiltinDirectFunctionBinding) + OBJECT_OFFSETOF(BuiltinDirectFunctionBinding, function));
    request.options = &request.ownOptions;
    request.ownEffects = inputs().stubCatalog.effectsOf(request.key);
    request.effectsOfCallee = &request.ownEffects;
    request.argumentBufferAccess = ArgumentBufferAccess::Reads;
    placeArguments(request, arguments, OperationCall::FirstArgument::None);
    for (LValue cell : keptAlive)
        request.operands.append({ cell, Machine::ValueRep::LateColdAny });
}

StubCallResult RuntimeCallEmitter::callDirectFunctionReturningFields(const OperationContext& context, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive)
{
    CallRequestWithOwnOptions request;
    fillRequestOfDirectFunction(request, DirectFunctionCall::LeafReturningFields, number, arguments, keptAlive);
    Effects memoryAlone = Effects::none();
    memoryAlone.reads = request.ownEffects.reads;
    memoryAlone.writes = request.ownEffects.writes;
    request.ownEffects = memoryAlone;
    return emitCall(&context, request);
}

LValue RuntimeCallEmitter::callDirectFunctionReturningNumber(const OperationContext& context, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive)
{
    Effects effects = effectsOf(context);
    RELEASE_ASSERT(!effects.mayThrow && !effects.mayCollect && !effects.mayCallUnknownCode);
    CallRequestWithOwnOptions request;
    fillRequestOfDirectFunction(request, DirectFunctionCall::LeafReturningNumber, number, arguments, keptAlive);
    return emitCall(&context, request).first;
}

// A double takes a register of its own kind.
static size_t countGPRArguments(std::span<const LValue> arguments)
{
    return std::ranges::count_if(arguments, [](LValue argument) {
        return argument->type() != Machine::Double;
    });
}

bool RuntimeCallEmitter::leavesRegistersForHostFrame(std::span<const LValue> arguments)
{
    return countGPRArguments(arguments) <= directCalleeArgumentIndex;
}

LValue RuntimeCallEmitter::callDirectFunction(const OperationContext& context, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive, std::optional<HostFrameOfDirectCall> hostFrame, BuiltinID calledBuiltin)
{
    Effects effects = effectsOf(context);
    bool isLeaf = !effects.mayThrow && !effects.mayCollect && !effects.mayCallUnknownCode;

    DirectFunctionCall form = isLeaf ? DirectFunctionCall::Leaf : hostFrame ? DirectFunctionCall::WithHostFrame : DirectFunctionCall::WithFrame;
    CallRequestWithOwnOptions request;
    fillRequestOfDirectFunction(request, form, number, arguments, keptAlive);
    if (form == DirectFunctionCall::WithHostFrame) {
        RELEASE_ASSERT(leavesRegistersForHostFrame(arguments));
        // The function reads the registers of its own parameters, and the stub the two of the frame. Nobody reads those between them, so nothing is moved there.
        if (countGPRArguments(arguments) <= directThisArgumentIndex)
            request.operands.append({ hostFrame->thisValue, Machine::ValueRep::reg(operationArgumentGPR(directThisArgumentIndex)) });
        request.operands.append({ hostFrame->callee, Machine::ValueRep::reg(operationArgumentGPR(directCalleeArgumentIndex)) });
    }
    request.ownOptions.calledBuiltin = calledBuiltin;
    return emitCall(&context, request).first;
}

StubCallShapeCache& RuntimeCallEmitter::stubCallShapes() const
{
    if (!m_stubCallShapes) [[unlikely]]
        m_stubCallShapes = &m_proc.arena().objectKeptAcrossResets<StubCallShapeCache>();
    return *m_stubCallShapes;
}

void RuntimeCallEmitter::fillRequest(CallRequest& request, StubKey key, std::span<const LValue> operands, const CallOptions& options, std::optional<BranchTargets> targets, std::optional<Origin> firstRead) const
{
    const StubCatalog& catalog = inputs().stubCatalog;
    uint32_t traits = (targets ? 0 : StubCallTraits::returns)
        | (firstRead ? StubCallTraits::hasFirstRead : 0)
        | (options.throwSite ? StubCallTraits::hasThrowSite : 0)
        | static_cast<uint32_t>(operands.size()) << StubCallTraits::numberOfOperandsShift;
    StubCallShape& shape = shapeOfStubCall(stubCallShapes(), catalog, key, options.immediate, traits);
    if (!shape.hasEffects) [[unlikely]] {
        shape.effects = catalog.effectsOf(boundKeyOf(catalog, key, options.immediate));
        shape.hasEffects = true;
    }
    const StubSignature& signature = shape.signature;
    RELEASE_ASSERT(operands.size() == signature.numberOfOperands);
    request.key = key;
    request.options = &options;
    request.end = targets ? CallEnd::BranchesOnFlags : CallEnd::Returns;
    request.targets = targets;
    request.firstRead = firstRead;
    request.effectsOfCallee = &shape.effects;
    request.shape = &shape;
    request.argumentBufferAccess = options.pureFunction ? ArgumentBufferAccess::None : ArgumentBufferAccess::Reads;
    for (size_t i = 0; i < operands.size(); ++i)
        request.operands.append({ operands[i], Machine::ValueRep::reg(signature.operands[i]) });
}

StubCallResult RuntimeCallEmitter::emitCall(const OperationContext* context, const CallRequest& request)
{
    StubKey key = request.key;
    const CallOptions& options = *request.options;
    RELEASE_ASSERT(!options.immediate || !options.numberedImmediate);
    bool returns = request.end == CallEnd::Returns;
    bool branchesOnFlags = request.end == CallEnd::BranchesOnFlags;
    bool neverReturns = request.end == CallEnd::NeverReturns;
    const StubCallShape* shape = request.shape;
    if (!shape) {
        uint32_t traits = static_cast<uint32_t>(request.operationResult)
            | (neverReturns ? StubCallTraits::neverReturns : 0)
            | (returns ? StubCallTraits::returns : 0)
            | (request.firstRead ? StubCallTraits::hasFirstRead : 0)
            | (options.throwSite ? StubCallTraits::hasThrowSite : 0)
            | (request.preservingCall ? StubCallTraits::hasPreservingCall : 0)
            | static_cast<uint32_t>(request.operands.size()) << StubCallTraits::numberOfOperandsShift;
        shape = &shapeOfStubCall(stubCallShapes(), inputs().stubCatalog, key, options.immediate, traits);
    }
    const StubSignature signature = shape->signature;
    RELEASE_ASSERT(!branchesOnFlags || signature.result == StubResult::Flags);

    const Effects& effectsOfCallee = *request.effectsOfCallee;
    bool calleeMayCollect = effectsOfCallee.mayCollect;
    Effects effects = context ? effectsOfCall(*context, effectsOfCallee) : effectsOfCallee;
    if (options.pureFunction && !options.pureFunction->dependsOnMemory)
        effects.reads = { };
    // A site says where in the source a frame stands and which handler is in force there.
    // An operation whose allocation fails throws when it returns. In a function with handlers, its site records which handler, if any, is active there.
    bool mayFailToAllocate = context && returns && calleeMayCollect && !options.pureFunction && !options.followsThrow && functionHasHandlers();
    bool hasSite = effects.mayExitSideways() || options.capturesStackTrace || mayFailToAllocate;
    RELEASE_ASSERT(context || (!hasSite && (!effects.mayCollect || options.followsThrow)));
    RELEASE_ASSERT(!generator().isInitializingCell() || (!effects.mayCollect && !effects.mayThrow));

    RELEASE_ASSERT(!options.returnsWord32 || signature.result == StubResult::GPR);
    LType type = options.returnsWord32 ? Machine::Int32 : typeOfResult(signature.result);
    RELEASE_ASSERT(!options.pureFunction || (returns && !effects.mayExitSideways() && !effects.mayCollect && request.argumentBufferAccess == ArgumentBufferAccess::None && !options.accessesCollectorState));
    // An Extract stands right behind its Patch, so a call that returns two words is shared with no later one.
    bool mayBeShared = options.pureFunction && signature.result != StubResult::TwoGPRs;
    if (mayBeShared) {
        Vector<LValue, 8> values;
        for (const PlacedOperand& operand : request.operands)
            values.append(operand.value);
        if (LValue known = m_out.findPurePatch(*options.pureFunction, type, values.span()))
            return resultOfCall(signature.result, known);
    }

    Machine::PatchOperation* patchpoint = m_out.patchpoint(type);
    patchpoint->effects.terminal = !returns;
    patchpoint->setNeedsFrameRecord(shape->needsFrameRecord);
    patchpoint->setRunsWithoutFrame(!hasSite && !neverReturns && shape->leavesCallerWithoutFrame);

    // The words of a throw site name the register of the operand, which is known here.
    bool receiverIsInAnyRegister = shape->receiverIsInAnyRegister;
    bool resultIsInAnyRegister = shape->resultIsInAnyRegister;
    for (const PlacedOperand& operand : request.operands)
        patchpoint->append(operand.value, receiverIsInAnyRegister ? Machine::ValueRep(Machine::ValueRep::SomeRegister) : operand.rep);
    // The words of a throw site name the register of the operand at the call, and a call for two reads has a record of its own.
    // An operand that stays where it lies keeps its register until the call is made, and the result has a register of its own from the start: one of the list, or the usual one.
    auto liesSomewhere = [](const PlacedOperand& operand) { return !operand.value->hasNoPosition(); };
    bool operandsLeaveRegisterForResult = !resultIsInAnyRegister || static_cast<size_t>(std::ranges::count_if(request.operands, liesSomewhere)) <= shape->numberOfOperandRegisters;
    patchpoint->setBringsOperandsToTheirRegisters(!receiverIsInAnyRegister && operandsLeaveRegisterForResult && !request.preservingCall && !request.firstRead && !options.throwSite);
    Machine::RegisterMask additionalClobbers = request.preservingCall ? Machine::maskOf(request.additionalClobbers) : 0;
    ASSERT(request.preservingCall || request.additionalClobbers.isEmpty());
    if (resultIsInAnyRegister) {
        // A site writes the immediate of a stub without operands there, and nothing else. Any other stub writes the register while it still reads its operands.
        patchpoint->resultConstraints = { Machine::ValueRep(Machine::ValueRep::SomeEarlyRegister) };
        patchpoint->holdResultsTo(shape->resultRegisters);
        patchpoint->clobberLate(shape->lateClobbered | additionalClobbers);
        patchpoint->clobberEarly(shape->earlyClobbered);
    } else {
        setResultConstraints(patchpoint, signature);
        patchpoint->clobberLate(shape->lateClobbered | additionalClobbers);
        patchpoint->clobberEarly(shape->earlyClobbered | (receiverIsInAnyRegister ? additionalClobbers & ~shape->operandRegisters : 0));
    }
    std::optional<StubKey> keyWhereNoRegisterIsKept = shape->keyWhereNoRegisterIsKept;
    std::optional<StubKey> keyWhereNoFPRIsKept = shape->keyWhereNoFPRIsKept;

    bool mayThrow = effects.mayThrow;
    effects.mayCollect |= options.followsThrow;
    applyEffects(patchpoint, effects, request.argumentBufferAccess, options.accessesCollectorState, options.runsCodeOfHost ? CellConversions::WhateverEffectsSay : options.convertsNoCells ? CellConversions::None : CellConversions::AsEffectsSay);
    if (options.pureFunction) {
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

    CallGenerator callGenerator { &m_records, key, signature, receiverIsInAnyRegister, resultIsInAnyRegister, branchesOnFlags, neverReturns, options.immediate, options.numberedImmediate, site, firstRead, options.throwSite, request.preservingCall, keyWhereNoRegisterIsKept, keyWhereNoFPRIsKept };
    patchpoint->setGenerator([callGenerator](CCallHelpers& jit, const Machine::PatchParameters& params) {
        callGenerator.generate(jit, params);
    });
    if (mayBeShared)
        m_out.recordPurePatch(patchpoint);

    if (branchesOnFlags) {
        m_out.appendSuccessor(request.targets->notTaken);
        m_out.appendSuccessor(request.targets->taken);
    }
    if ((mayThrow || mayFailToAllocate) && site)
        emitter<ExceptionEmitter>().attachLandingPad(*context, patchpoint, *site, returns);
    if (neverReturns)
        m_out.appendTo(newBlock());
    if (!returns)
        return { };
    return resultOfCall(signature.result, patchpoint);
}

LType RuntimeCallEmitter::typeOfResult(StubResult result)
{
    switch (result) {
    case StubResult::None:
    case StubResult::Flags:
        return Machine::Void;
    case StubResult::GPR:
        return Machine::Int64;
    case StubResult::FPR:
        return Machine::Double;
    case StubResult::TwoGPRs:
        if (!m_typeOfTwoWords)
            m_typeOfTwoWords = m_proc.addTuple({ Machine::Int64, Machine::Int64 });
        return *m_typeOfTwoWords;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubCallResult RuntimeCallEmitter::resultOfCall(StubResult result, LValue call)
{
    switch (result) {
    case StubResult::None:
    case StubResult::Flags:
        return { };
    case StubResult::GPR:
    case StubResult::FPR:
        return { call, nullptr };
    case StubResult::TwoGPRs:
        return { m_out.extract(call, 0), m_out.extract(call, 1) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubCallResult RuntimeCallEmitter::callStub(const OperationContext& context, StubKey key, std::span<const LValue> operands, const CallOptions& options)
{
    CallRequest request;
    fillRequest(request, key, operands, options);
    return emitCall(&context, request);
}

StubCallResult RuntimeCallEmitter::callStubWithoutSite(StubKey key, std::span<const LValue> operands, const CallOptions& options)
{
    CallRequest request;
    fillRequest(request, key, operands, options);
    return emitCall(nullptr, request);
}

void RuntimeCallEmitter::callStubAndBranch(const OperationContext& context, StubKey key, std::span<const LValue> operands, const CallOptions& options, BranchTargets targets)
{
    CallRequest request;
    fillRequest(request, key, operands, options, targets);
    emitCall(&context, request);
}

void RuntimeCallEmitter::callStubWithoutSiteAndBranch(StubKey key, std::span<const LValue> operands, const CallOptions& options, BranchTargets targets)
{
    CallRequest request;
    fillRequest(request, key, operands, options, targets);
    emitCall(nullptr, request);
}

LValue RuntimeCallEmitter::callStubThatReads(StubKey key, std::span<const LValue> operands, const MemoryLocation& location, const CallOptions& options)
{
    RELEASE_ASSERT(options.pureFunction && options.pureFunction->dependsOnMemory && options.convertsNoCells);
    CallRequest request;
    fillRequest(request, key, operands, options);
    Effects effectsOfCallee = *request.effectsOfCallee;
    RELEASE_ASSERT(effectsOfCallee.writes.isEmpty() && effectsOfCallee.reads.contains(location.heap()));
    LocationMasks read;
    read.add(m_program, location);
    effectsOfCallee.reads = effectsOfCallee.reads.intersectedWith(read);
    // A call that named nothing would be ended by nothing.
    RELEASE_ASSERT(!effectsOfCallee.reads.isEmpty());
    request.effectsOfCallee = &effectsOfCallee;
    return emitCall(nullptr, request).first;
}

Machine::PatchOperation* RuntimeCallEmitter::createCallPatchpoint(const OperationContext& context, const CallPatchpointRequest& request)
{
    Effects effects = effectsOfCall(context, request.effects);
    // If no handler of the function catches it, a stack overflow in the callee ends the function: none of its code runs after that call.
    if (!effects.mayThrowOtherThanStackOverflow() && !emitter<ExceptionEmitter>().unwindsWithinFunction(context))
        effects.mayThrow = false;

    Machine::PatchOperation* patchpoint = request.holdsUniqueRecord ? m_out.patchpointWithUniqueRecord(request.resultType) : m_out.patchpoint(request.resultType);
    patchpoint->clobberEarly(request.earlyClobbered);
    patchpoint->clobberLate(request.lateClobbered);
    applyEffects(patchpoint, effects, request.argumentBufferAccess);
    patchpoint->setRunsWithoutFrame(request.runsWithoutFrame);
    patchpoint->effects.terminal = request.neverReturns;
    return patchpoint;
}

void RuntimeCallEmitter::finishCallPatchpoint(const OperationContext& context, Machine::PatchOperation* patchpoint, uint32_t site, const CallPatchpointRequest& request)
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

RecordedSite RuntimeCallEmitter::recordedSiteOf(const OperationContext& context, const CallOptions& options)
{
    // Whatever a built-in runs, its function object, its inline code or an operation, runs after the tail call.
    Origin origin = options.origin.value_or(context.origin());
    const Operation* operation = context.valueID() ? &operationAt(context.valueID()) : nullptr;
    BuiltinID calledBuiltin = options.calledBuiltin;
    if (!calledBuiltin && operation && Operation::opcodesWithCalledBuiltin.contains(operation->opcode))
        calledBuiltin = operation->calledBuiltin();
    // Steps of a built-in function that were expanded into its caller record the caller's site and name the function.
    if (BuiltinID builtinOfFrame = origin.inlineFrame ? m_code.inlineFrames()[origin.inlineFrame].calledBuiltin : BuiltinID()) {
        const InlineFrame& frame = m_code.inlineFrames()[origin.inlineFrame];
        calledBuiltin = builtinOfFrame;
        origin = { .position = frame.callPosition, .inlineFrame = frame.parent, .isTailCall = frame.isTailCall, .isPartOfConstruction = frame.callIsPartOfConstruction };
    }
    // No stack trace lists a function that only built-in code can name.
    if (calledBuiltin && BuiltinBindings::linkTimeConstantOf(calledBuiltin))
        calledBuiltin = { };
    // `new` inside a built-in is a step of the built-in like any other.
    bool standsForCallee = calledBuiltin ? origin.isTailCall || options.isTailCall : origin.isTailCall && !options.isTailCall && !options.constructs;
    // A call, and the read of the method that it calls, can throw before the callee begins.
    bool isInsideCallee = options.callerHasLeft || (standsForCallee && (calledBuiltin || !operation || !runsBeforeCalleeOf(operation->opcode)));
    std::optional<CallSiteKind> kind = findCallSiteKind({ .isConstruct = options.constructs, .isTailCall = options.isTailCall || origin.isTailCall || options.callerHasLeft, .isInsideCallee = isInsideCallee, .namesBuiltin = !!calledBuiltin });
    RELEASE_ASSERT(kind);
    return { .origin = origin, .kind = *kind, .calledBuiltin = calledBuiltin, .constructedCallee = options.constructedCallee, .callsOwnFunctionObject = options.callsOwnFunctionObject, .keptCalleePlusOne = options.keptCalleePlusOne };
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
    unsigned byteSize = safeCast<unsigned>(bytes);
    if (!m_transientBuffer) {
        m_transientBuffer = m_proc.addStackSlot(byteSize);
        m_transientBuffer->setIsDeadAtSuspensions(true);
    } else
        m_transientBuffer->ensureSize(byteSize);
    return m_out.slotBase(m_transientBuffer);
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
    if (effectsOfCallee.isPure())
        return effectsOfCallee;
    const Effects& effectsOfOperation = effectsOf(context);
    ThrowEffect mayThrow = effectsOfOperation.mayThrow;
    mayThrow &= effectsOfCallee.mayThrow;
    return {
        .reads = effectsOfOperation.reads.intersectedWith(effectsOfCallee.reads),
        .writes = effectsOfOperation.writes.intersectedWith(effectsOfCallee.writes),
        .mayThrow = mayThrow,
        .mayCollect = effectsOfOperation.mayCollect && effectsOfCallee.mayCollect,
        .mayCallUnknownCode = effectsOfOperation.mayCallUnknownCode && effectsOfCallee.mayCallUnknownCode,
        .maySuspend = effectsOfOperation.maySuspend && effectsOfCallee.maySuspend,
        .mayBeTerminated = effectsOfOperation.mayBeTerminated && effectsOfCallee.mayBeTerminated,
    };
}

bool RuntimeCallEmitter::mayConvertCells(const Effects& effects)
{
    return AOTHeaps::mayConvertCells(effects);
}

void RuntimeCallEmitter::applyEffects(Machine::PatchOperation* patchpoint, const Effects& effects, ArgumentBufferAccess access, bool accessesCollectorState, CellConversions conversions)
{
    RELEASE_ASSERT(!generator().isInitializingCell() || (!effects.mayCollect && !effects.mayThrow));

    bool isTerminal = patchpoint->effects.terminal;
    uint8_t traits = static_cast<uint8_t>(static_cast<unsigned>(access) | (accessesCollectorState ? 4 : 0) | static_cast<unsigned>(conversions) << 3);
    if (!m_patchEffectsMemo) [[unlikely]]
        m_patchEffectsMemo = &m_proc.arena().objectKeptAcrossResets<PatchEffectsMemo>();
    PatchEffectsMemo::Entry* known = findPatchEffects(*m_patchEffectsMemo, effects, traits);
    if (known && known->isFilled) [[likely]] {
        patchpoint->effects = known->patchEffects;
        patchpoint->effects.terminal = isTerminal;
        return;
    }
    computeEffects(patchpoint, effects, access, accessesCollectorState, conversions);
    if (known) {
        known->effects = effects;
        known->traits = traits;
        known->patchEffects = patchpoint->effects;
        known->patchEffects.terminal = false;
        known->patchEffects.reads = m_patchEffectsMemo->keep(patchpoint->effects.reads);
        known->patchEffects.writes = m_patchEffectsMemo->keep(patchpoint->effects.writes);
        known->isFilled = true;
    }
    patchpoint->effects.terminal = isTerminal;
}

void RuntimeCallEmitter::computeEffects(Machine::PatchOperation* patchpoint, const Effects& effects, ArgumentBufferAccess access, bool accessesCollectorState, CellConversions conversions)
{
    patchpoint->effects = Machine::Effects::none();
    patchpoint->effects.controlDependent = true;
    patchpoint->effects.mayCollect = effects.mayCollect;
    bool convertsCells = conversions == CellConversions::WhateverEffectsSay || (conversions == CellConversions::AsEffectsSay && mayConvertCells(effects));
    if (effects.isPure() && access == ArgumentBufferAccess::None && !accessesCollectorState && !convertsCells)
        return;

    bool exitsSideways = effects.mayExitSideways();
    patchpoint->effects.exitsSideways = exitsSideways;
    // What runs behind a sideways exit may read anything.
    if (exitsSideways || effects.reads.isTop())
        patchpoint->effects.reads = Machine::HeapSet::top();
    else {
        HeapList& reads = m_scratchHeaps;
        reads.shrink(0);
        m_heaps.appendHeaps(effects.reads, reads);
        if (effects.mayCollect)
            reads.append(&m_heaps.frameSlots);
        if (accessesCollectorState)
            reads.append(&m_heaps.collectorState);
        if (access != ArgumentBufferAccess::None)
            reads.append(&m_heaps.argumentBuffer);
        if (!reads.isEmpty())
            m_heaps.decoratePatchpointRead(reads.span(), patchpoint);
    }

    // Every heap that is added below lies under the root.
    if (effects.writes.isTop()) {
        patchpoint->effects.writes = Machine::HeapSet::top();
        return;
    }

    HeapList& writes = m_scratchHeaps;
    writes.shrink(0);
    m_heaps.appendHeaps(effects.writes, writes);
    if (convertsCells) {
        bool replacesStorageOfArrays = conversions == CellConversions::WhateverEffectsSay || AOTHeaps::mayReplaceStorageOfArrays(effects);
        m_heaps.appendHeapsOfConvertedCells(writes, replacesStorageOfArrays ? AOTHeaps::ConvertedCells::All : AOTHeaps::ConvertedCells::AllButArrays);
    }
    // The cell that the closure table has for a function is filled where the closure is first asked for, and a counter rises wherever its code runs.
    // No operation says so: only generated code reads these words, and a read of one is numbered like any other. Whoever fills a cell of the table makes a cell.
    // The slot of a site says what a lookup found. It changes where something is written that a lookup may come by, and a collection empties the slot of a Structure that has died.
    if (effects.mayCollect || !effects.writes.isEmpty()) {
        writes.append(&m_heaps.AOTInstance_closureCells);
        writes.append(&m_heaps.AOTInstance_siteCaches);
        writes.append(&m_heaps.counters);
    }
    if (effects.mayCollect)
        m_heaps.appendHeapsWrittenByCollection(writes);
    else if (accessesCollectorState)
        writes.append(&m_heaps.collectorState);
    if (access == ArgumentBufferAccess::ReadsAndWrites)
        writes.append(&m_heaps.argumentBuffer);
    if (!writes.isEmpty())
        m_heaps.decoratePatchpointWrite(writes.span(), patchpoint);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
