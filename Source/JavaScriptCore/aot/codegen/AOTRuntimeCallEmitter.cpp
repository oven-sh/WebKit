/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeCallEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTEffectsNet.h"
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
    void operator()(CCallHelpers& jit, const Machine::PatchParameters& params) const { generate(jit, params); }
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
    const ThrowSite* throwSite;
    const OperationCall* preservingCall;
    std::optional<StubKey> keyWhereNoRegisterIsKept;
    std::optional<StubKey> keyWhereNoFPRIsKept;
    Machine::RegisterMask receiversWithVariant;
};

static_assert(std::is_trivially_destructible_v<CallGenerator>);
static_assert(std::is_trivially_destructible_v<ThrowSite> && std::is_trivially_destructible_v<OperationCall>);

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
        RELEASE_ASSERT(rep.isStack() && params.frameIsSetUp());
        bool isNarrow = childType == Machine::Int32;
        appendArgumentMove(moves, destination, isNarrow ? ArgumentSource::Stack32 : ArgumentSource::Stack64, static_cast<int64_t>(rep.offsetFromFP()));
    }
    return moves;
}

ArgumentMoves CallGenerator::bringOperands(const Machine::PatchParameters& params) const
{
    ArgumentMoves moves;
    Machine::PatchOperation* call = params.value();
    std::span<Machine::Operation* const> operands = call->operands();
    unsigned numberOfChildren = call->numberOfOperands();
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
        bool isNarrow = operands[i]->type() == Machine::Int32;
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
    }
    ArgumentMoves moves = preservingCall ? moveArguments(jit, params) : bringOperands(params);
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
        bool hasVariant = receiver != receiverOfStub && (called == key ? !!(receiversWithVariant & Machine::maskOf(Reg(receiver))) : exists(called.withOperandRegister(receiver)));
        if (hasVariant)
            called = called.withOperandRegister(receiver);
        else {
            RELEASE_ASSERT(receiver == receiverOfStub || descriptorOf(called.family()).operandRegisters == OperandRegisterChoice::AlternateReceivers);
            jit.move(receiver, receiverOfStub);
        }
    }
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

static bool returnsInAnyRegister(StubKey key, const StubSignature& signature, size_t numberOfOperands)
{
    OperandRegisterChoice choice = descriptorOf(key.family()).operandRegisters;
    if (signature.result != StubResult::GPR || operandRegistersOf(choice).empty())
        return false;
    return namesRegisterOfResult(choice) || (choice == OperandRegisterChoice::AnyAllocatableRegister && !numberOfOperands);
}

bool leavesCallerWithoutFrame(StubKey key)
{
    return isX86_64() && descriptorOf(key.family()).leafness != StubLeafness::MayCall;
}

static RegisterSet lateClobbersOf(const StubCatalog& catalog, StubKey key, bool receiverIsInAnyRegister, const StubSignature& signature, const RegisterSet& additionalClobbers)
{
    RegisterSet late = receiverIsInAnyRegister ? catalog.clobberedByRegisterVariantsOf(key) : catalog.summaryOf(key).clobbered;
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
    if (signature.immediate) {
        std::optional<StubKey> variant = receiverIsInAnyRegister ? tryWithOperandRegister(key, *signature.immediate) : std::nullopt;
        if (!variant || signatureOf(*variant).immediate == signature.immediate)
            early.add(*signature.immediate, IgnoreVectors);
    }
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
    Machine::RegisterMask receiversWithVariant { 0 };
    RegisterSet resultRegisters;
    std::optional<StubKey> keyWhereNoRegisterIsKept;
    std::optional<StubKey> keyWhereNoFPRIsKept;
    uint8_t numberOfOperandRegisters { 0 };
    bool needsFrameRecord { false };
    bool leavesCallerWithoutFrame { false };
    bool receiverIsInAnyRegister { false };
    bool resultIsInAnyRegister { false };
    bool hasEffects { false };
    bool effectsArePure { false };
    uint8_t traitsOfLastCall { 0 };
    const RememberedPatchEffects* patchEffectsOfLastCall { nullptr };
    Effects effects;
    Effects effectsOfLastOperation;
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

constexpr unsigned stubCallShapeCacheBits = 11;

} // anonymous namespace

struct StubCallShapeCache {
    ~StubCallShapeCache()
    {
        for (StubCallShape* shape : shapes) {
            if (shape)
                fastFree(shape);
        }
    }

    std::array<StubCallShape*, 1 << stubCallShapeCacheBits> shapes { };
    RegisterSet clobberedByThrowSites;
    uint64_t catalogOfThrowSites { 0 };
    bool knowsThrowSites { false };
};

static_assert(std::is_trivially_destructible_v<StubCallShape>);

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
    shape.receiversWithVariant = shape.receiverIsInAnyRegister && exists(key.withOperandRegister(operandRegistersOf(choice).front())) ? shape.operandRegisters : 0;
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
    shape.patchEffectsOfLastCall = nullptr;
}

static NEVER_INLINE StubCallShape& fillSlotOfStubCallShape(StubCallShape*& slot, const StubCatalog& catalog, StubKey key, StubKey boundKey, StubKey cachedBoundKey, std::optional<uint32_t> immediate, uint32_t traits)
{
    if (!slot)
        slot = new (NotNull, fastMalloc(sizeof(StubCallShape))) StubCallShape;
    StubCallShape& shape = *slot;
    fillStubCallShape(shape, catalog, key, boundKey, immediate, traits);
    shape.catalogIdentifier = catalog.identifier();
    shape.key = key;
    shape.boundKey = cachedBoundKey;
    shape.traits = traits;
    return shape;
}

static ALWAYS_INLINE StubCallShape& shapeOfStubCall(StubCallShapeCache& cache, const StubCatalog& catalog, StubKey key, std::optional<uint32_t> immediate, uint32_t traits)
{
    StubKey boundKey = boundKeyOf(catalog, key, immediate);
    StubKey cachedBoundKey = boundKey;
    if (boundKey != key) {
        const StubDescriptor& descriptor = descriptorOf(key.family());
        if (descriptor.binding == ImmediateBinding::Every && descriptorOf(descriptor.body).binding == ImmediateBinding::Every)
            cachedBoundKey = key.withImmediate(0);
    }

    uint64_t hash = (key.bits() ^ std::rotl(cachedBoundKey.bits(), 29) ^ traits) * 0x9E3779B97F4A7C15ull;
    StubCallShape*& slot = cache.shapes[hash >> (64 - stubCallShapeCacheBits)];
    if (StubCallShape* shape = slot; shape && shape->key == key && shape->boundKey == cachedBoundKey && shape->traits == traits && shape->catalogIdentifier == catalog.identifier()) [[likely]]
        return *shape;
    return fillSlotOfStubCallShape(slot, catalog, key, boundKey, cachedBoundKey, immediate, traits);
}

struct RememberedPatchEffects {
    Effects effects;
    uint8_t traits { 0 };
    bool isFilled { false };
    Machine::Effects patchEffects;
};

namespace {

constexpr uint8_t callFollowsThrowInTraits = 1 << 5;
constexpr uint8_t callReadsNoMemoryInTraits = 1 << 6;
constexpr uint8_t callIsOfEntryInTraits = 1 << 7;

} // anonymous namespace

static_assert(static_cast<unsigned>(ArgumentBufferAccess::ReadsAndWrites) < 4 && static_cast<unsigned>(ArgumentBufferAccess::Reads) < 4 && static_cast<unsigned>(ArgumentBufferAccess::None) < 4);
static_assert(static_cast<unsigned>(RuntimeCallEmitter::CellConversions::WhateverEffectsSay) == 2);

static uint8_t traitsOfPatchEffects(ArgumentBufferAccess access, bool accessesCollectorState, RuntimeCallEmitter::CellConversions conversions)
{
    return static_cast<uint8_t>(static_cast<unsigned>(access) | (accessesCollectorState ? 4 : 0) | static_cast<unsigned>(conversions) << 3);
}

struct PatchEffectsMemo {
    static constexpr unsigned numberOfBits = 10;
    static constexpr size_t numberOfEntries = 1 << numberOfBits;
    static constexpr size_t maxProbes = 8;

    using Entry = RememberedPatchEffects;

    struct CallPatchpointSeen {
        Effects effectsOfCallee;
        Effects effectsOfOperation;
        uint8_t traits { 0 };
        const Entry* patchEffects { nullptr };
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
    std::array<CallPatchpointSeen, 4> lastCallPatchpoints;
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
        return representation == Representation::Int64 || representation == Representation::RawWord || representation == Representation::OptionalInt64;
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
    fillEntryRequest(request, entry, first, options);
    RELEASE_ASSERT(context || first != OperationCall::FirstArgument::None || request.key == tailCallOperation(first));
    placeArguments(request, arguments, first);
    return emitCall(context, request);
}

void RuntimeCallEmitter::fillEntryRequest(CallRequestWithOwnOptions& request, AOT::Entry entry, OperationCall::FirstArgument first, const CallOptions& callerOptions) const
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    RELEASE_ASSERT(operation);
    RuntimeSignature signature = signatureOf(*operation);
    OperationCall::Returns returns = returnsOf(signature.result);
    m_records.totals.entriesUsed.set(static_cast<size_t>(entry));

    request.ownOptions = callerOptions;
    request.ownOptions.immediate = offsetInRuntimeTable(entry);
    request.options = &request.ownOptions;
    request.operationResult = returnTraitsOf(returns).result;
    request.ownEffects = AOT::effectsOf(entry);
    request.effectsOfCallee = &request.ownEffects;
    request.shapeKnowsCallee = true;
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

LValue RuntimeCallEmitter::callDirectFunctionReturningNumber(const OperationContext& context, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive)
{
    Effects effects = effectsOf(context);
    RELEASE_ASSERT(!effects.mayThrow && !effects.mayCollect && !effects.mayCallUnknownCode);
    CallRequestWithOwnOptions request;
    fillRequestOfDirectFunction(request, DirectFunctionCall::LeafReturningNumber, number, arguments, keptAlive);
    return emitCall(&context, request).first;
}

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

PatchEffectsMemo& RuntimeCallEmitter::patchEffectsMemo()
{
    if (!m_patchEffectsMemo) [[unlikely]]
        m_patchEffectsMemo = &m_proc.arena().objectKeptAcrossResets<PatchEffectsMemo>();
    return *m_patchEffectsMemo;
}

ALWAYS_INLINE void RuntimeCallEmitter::fillRequest(CallRequest& request, StubKey key, std::span<const LValue> operands, const CallOptions& options, std::optional<BranchTargets> targets, std::optional<Origin> firstRead) const
{
    const StubCatalog& catalog = inputs().stubCatalog;
    uint32_t traits = (targets ? 0 : StubCallTraits::returns)
        | (firstRead ? StubCallTraits::hasFirstRead : 0)
        | (options.throwSite ? StubCallTraits::hasThrowSite : 0)
        | static_cast<uint32_t>(operands.size()) << StubCallTraits::numberOfOperandsShift;
    StubCallShape& shape = shapeOfStubCall(stubCallShapes(), catalog, key, options.immediate, traits);
    if (!shape.hasEffects) [[unlikely]] {
        shape.effects = catalog.effectsOf(boundKeyOf(catalog, key, options.immediate));
        shape.effectsArePure = shape.effects.isPure();
        shape.hasEffects = true;
    }
    RELEASE_ASSERT(operands.size() == shape.signature.numberOfOperands);
    request.key = key;
    request.options = &options;
    request.end = targets ? CallEnd::BranchesOnFlags : CallEnd::Returns;
    request.targets = targets;
    request.firstRead = firstRead;
    request.effectsOfCallee = &shape.effects;
    request.shape = &shape;
    request.shapeKnowsCallee = true;
    RELEASE_ASSERT(!options.pureFunction || !options.writesArgumentBuffer);
    request.argumentBufferAccess = options.pureFunction ? ArgumentBufferAccess::None : options.writesArgumentBuffer ? ArgumentBufferAccess::ReadsAndWrites : ArgumentBufferAccess::Reads;
    request.operandsOfStub = operands;
}

StubCallResult RuntimeCallEmitter::emitCall(const OperationContext* context, const CallRequest& request)
{
    StubKey key = request.key;
    const CallOptions& options = *request.options;
    RELEASE_ASSERT(!options.immediate || !options.numberedImmediate);
    bool returns = request.end == CallEnd::Returns;
    bool branchesOnFlags = request.end == CallEnd::BranchesOnFlags;
    bool neverReturns = request.end == CallEnd::NeverReturns;
    bool isCallOfStub = !!request.shape;
    StubCallShape* shape = request.shape;
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
    const StubSignature* signature = &shape->signature;
    StubResult resultKind = signature->result;
    RELEASE_ASSERT(!branchesOnFlags || resultKind == StubResult::Flags);

    std::optional<Effects> effectsOfCalleeSayingLess;
    if (m_options.rowThatSaysLess) [[unlikely]] {
        effectsOfCalleeSayingLess = *request.effectsOfCallee;
        RowThatSaysLess row = RowThatSaysLess::decode(m_options.rowThatSaysLess);
        if (request.entry)
            row.cut(KindOfEffectsRow::RuntimeOperation, static_cast<unsigned>(*request.entry), *effectsOfCalleeSayingLess);
        else
            row.cut(KindOfEffectsRow::Stub, static_cast<unsigned>(key.family()), *effectsOfCalleeSayingLess);
    }
    const Effects& effectsOfCallee = effectsOfCalleeSayingLess ? *effectsOfCalleeSayingLess : *request.effectsOfCallee;
    bool calleeMayCollect = effectsOfCallee.mayCollect;
    bool checksEffectsAtRunTime = AOT::hasFlag(m_options.testing, Wire::TestingSwitch::CheckEffectsAtRunTime);
    bool shapeKnowsCallee = request.shapeKnowsCallee && !effectsOfCalleeSayingLess;
    CellConversions conversions = options.runsCodeOfHost ? CellConversions::WhateverEffectsSay : options.convertsNoCells ? CellConversions::None : CellConversions::AsEffectsSay;
    bool readsNoMemory = options.pureFunction && !options.pureFunction->dependsOnMemory;
    const Effects* effectsOfOperation = nullptr;
    if (context && !(isCallOfStub && shapeKnowsCallee ? shape->effectsArePure : effectsOfCallee.isPure()))
        effectsOfOperation = &effectsOf(*context);
    bool remembersEffects = effectsOfOperation && shapeKnowsCallee && !checksEffectsAtRunTime;
    uint8_t traitsOfCall = static_cast<uint8_t>(traitsOfPatchEffects(request.argumentBufferAccess, options.accessesCollectorState, conversions)
        | (options.followsThrow ? callFollowsThrowInTraits : 0)
        | (readsNoMemory ? callReadsNoMemoryInTraits : 0)
        | (isCallOfStub ? 0 : callIsOfEntryInTraits));
    const RememberedPatchEffects* knownEffects = nullptr;
    if (remembersEffects && shape->patchEffectsOfLastCall && shape->traitsOfLastCall == traitsOfCall && shape->effectsOfLastOperation == *effectsOfOperation)
        knownEffects = shape->patchEffectsOfLastCall;
    std::optional<Effects> computedEffects;
    if (!knownEffects) {
        computedEffects.emplace(effectsOfOperation ? effectsOfCall(*context, effectsOfCallee) : effectsOfCallee);
        if (readsNoMemory)
            computedEffects->reads = { };
    }
    const Effects* effects = knownEffects ? &knownEffects->effects : &*computedEffects;
    bool mayThrow = effects->mayThrow;
    bool mayExitSideways = effects->mayExitSideways();
    bool mayCollect = knownEffects ? calleeMayCollect && effectsOfOperation->mayCollect : effects->mayCollect;
    bool mayFailToAllocate = context && returns && calleeMayCollect && !options.pureFunction && !options.followsThrow && functionHasHandlers();
    bool hasSite = mayExitSideways || options.capturesStackTrace || mayFailToAllocate;
    RELEASE_ASSERT(context || (!hasSite && (!mayCollect || options.followsThrow)));
    RELEASE_ASSERT(!generator().isInitializingCell() || (!mayCollect && !mayThrow));

    RELEASE_ASSERT(!options.returnsWord32 || resultKind == StubResult::GPR);
    LType type = options.returnsWord32 ? Machine::Int32 : typeOfResult(resultKind);
    RELEASE_ASSERT(!options.pureFunction || (returns && !mayExitSideways && !mayCollect && request.argumentBufferAccess == ArgumentBufferAccess::None && !options.accessesCollectorState));
    bool mayBeShared = options.pureFunction && resultKind != StubResult::TwoGPRs;
    std::span<const LValue> operandsOfStub = request.operandsOfStub;
    if (mayBeShared) {
        LValue known = nullptr;
        if (isCallOfStub)
            known = m_out.findPurePatch(*options.pureFunction, type, operandsOfStub);
        else {
            Vector<LValue, 8> values;
            for (const PlacedOperand& operand : request.operands)
                values.append(operand.value);
            known = m_out.findPurePatch(*options.pureFunction, type, values.span());
        }
        if (known)
            return resultOfCall(resultKind, known);
    }

    std::optional<WatchOfEffects> watch;
    std::optional<StubCallShape> shapeKeptAcrossWatch;
    if (checksEffectsAtRunTime && context && context->valueID() && returns && request.entry != AOT::Entry::operationAOTStopAtFalseEffects) [[unlikely]] {
        static_assert(numberOfEnumerators<Opcode> <= firstRowOfRuntimeOperationsInEffectsNet - firstRowOfOperationsInEffectsNet);
        static_assert(numberOfEntries <= firstRowOfStubsInEffectsNet - firstRowOfRuntimeOperationsInEffectsNet);
        static_assert(numberOfStubFamilies <= firstRowOfBuiltinsInEffectsNet - firstRowOfStubsInEffectsNet);
        unsigned rowOfCallee = request.entry ? firstRowOfRuntimeOperationsInEffectsNet + static_cast<unsigned>(*request.entry) : firstRowOfStubsInEffectsNet + static_cast<unsigned>(key.family());
        shapeKeptAcrossWatch = *shape;
        shape = &*shapeKeptAcrossWatch;
        signature = &shape->signature;
        beginWatchOfEffects(*context, *effects, EntersCompiledCode::No, (firstRowOfOperationsInEffectsNet + static_cast<unsigned>(operationAt(context->valueID()).opcode)) | rowOfCallee << 16, watch.emplace());
    }

    Machine::PatchOperation* patchpoint = m_out.patchpoint(type);
    patchpoint->effects.terminal = !returns;
    patchpoint->setNeedsFrameRecord(shape->needsFrameRecord);
    patchpoint->setRunsWithoutFrame(!hasSite && !neverReturns && shape->leavesCallerWithoutFrame);

    bool receiverIsInAnyRegister = shape->receiverIsInAnyRegister;
    bool resultIsInAnyRegister = shape->resultIsInAnyRegister;
    if (isCallOfStub) {
        for (size_t i = 0; i < operandsOfStub.size(); ++i)
            patchpoint->append(operandsOfStub[i], receiverIsInAnyRegister ? Machine::ValueRep(Machine::ValueRep::SomeRegister) : Machine::ValueRep::reg(signature->operands[i]));
    } else {
        for (const PlacedOperand& operand : request.operands)
            patchpoint->append(operand.value, receiverIsInAnyRegister ? Machine::ValueRep(Machine::ValueRep::SomeRegister) : operand.rep);
    }
    auto liesSomewhere = [](const Machine::Operation* operand) { return !operand->hasNoPosition(); };
    bool operandsLeaveRegisterForResult = !resultIsInAnyRegister || static_cast<size_t>(std::ranges::count_if(patchpoint->operands(), liesSomewhere)) <= shape->numberOfOperandRegisters;
    patchpoint->setBringsOperandsToTheirRegisters(!receiverIsInAnyRegister && operandsLeaveRegisterForResult && !request.preservingCall && !request.firstRead && !options.throwSite);
    Machine::RegisterMask additionalClobbers = request.preservingCall ? Machine::maskOf(request.additionalClobbers) : 0;
    ASSERT(request.preservingCall || request.additionalClobbers.isEmpty());
    if (resultIsInAnyRegister) {
        patchpoint->resultConstraints = { Machine::ValueRep(Machine::ValueRep::SomeEarlyRegister) };
        patchpoint->holdResultsTo(shape->resultRegisters);
        patchpoint->clobberLate(shape->lateClobbered | additionalClobbers);
        patchpoint->clobberEarly(shape->earlyClobbered);
    } else {
        setResultConstraints(patchpoint, *signature);
        patchpoint->clobberLate(shape->lateClobbered | additionalClobbers);
        patchpoint->clobberEarly(shape->earlyClobbered | (receiverIsInAnyRegister ? additionalClobbers & ~shape->operandRegisters : 0));
    }

    if (knownEffects) {
        RELEASE_ASSERT(!generator().isInitializingCell() || (!knownEffects->effects.mayCollect && !mayThrow));
        patchpoint->effects = knownEffects->patchEffects;
        patchpoint->effects.terminal = !returns;
    } else {
        computedEffects->mayCollect |= options.followsThrow;
        const RememberedPatchEffects* appliedEffects = applyEffectsThroughMemo(patchpoint, *computedEffects, request.argumentBufferAccess, options.accessesCollectorState, conversions);
        if (remembersEffects && appliedEffects && (isCallOfStub || shape->boundKey.immediate() == options.immediate)) {
            shape->effectsOfLastOperation = *effectsOfOperation;
            shape->traitsOfLastCall = traitsOfCall;
            shape->patchEffectsOfLastCall = appliedEffects;
        }
    }
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

    const ThrowSite* throwSite = options.throwSite ? m_proc.arena().create<ThrowSite>(*options.throwSite) : nullptr;
    const OperationCall* preservingCall = request.preservingCall ? m_proc.arena().create<OperationCall>(*request.preservingCall) : nullptr;
    patchpoint->setGenerator(CallGenerator { &m_records, key, *signature, receiverIsInAnyRegister, resultIsInAnyRegister, branchesOnFlags, neverReturns, options.immediate, options.numberedImmediate, site, firstRead, throwSite, preservingCall, shape->keyWhereNoRegisterIsKept, shape->keyWhereNoFPRIsKept, shape->receiversWithVariant });
    if (mayBeShared)
        m_out.recordPurePatch(patchpoint);

    if (branchesOnFlags) {
        m_out.appendSuccessor(request.targets->notTaken);
        m_out.appendSuccessor(request.targets->taken);
    }
    if ((mayThrow || mayFailToAllocate) && site && ExceptionEmitter::unwindsWithinFunction(*context))
        emitter<ExceptionEmitter>().attachLandingPad(*context, patchpoint, *site, returns);
    if (neverReturns)
        m_out.appendTo(newBlock());
    if (!returns)
        return { };
    StubCallResult result = resultOfCall(resultKind, patchpoint);
    if (watch && watch->isSet()) [[unlikely]]
        endWatchOfEffects(*context, *watch, resultKind == StubResult::GPR || resultKind == StubResult::FPR ? patchpoint : nullptr);
    return result;
}

void RuntimeCallEmitter::beginWatchOfOperation(const ValueContext& context, WatchOfEffects& watch)
{
    const Effects* believed = m_code.effectsBelievedOf(context.id());
    if (!believed || context.isBranchCondition() || context.returnCall())
        return;
    const Operation& operation = context.value();
    unsigned rowOfCallee = noRowInEffectsNet;
    if (Operation::opcodesWithOverload.contains(operation.opcode)) {
        unsigned builtin = BuiltinTable::singleton().at(operation.overload()).builtin.index();
        if (builtin < numberOfRowsInEffectsNet - firstRowOfBuiltinsInEffectsNet)
            rowOfCallee = firstRowOfBuiltinsInEffectsNet + builtin;
    }
    EntersCompiledCode entersCompiledCode = effectRuleOf(operation.opcode) == EffectRule::Call ? EntersCompiledCode::Yes : EntersCompiledCode::No;
    beginWatchOfEffects(context, *believed, entersCompiledCode, (firstRowOfOperationsInEffectsNet + static_cast<unsigned>(operation.opcode)) | rowOfCallee << 16, watch);
    m_watchOfOperation = &watch;
}

void RuntimeCallEmitter::abandonWatchOfOperation(const WatchOfEffects& watch)
{
    if (m_watchOfOperation == &watch)
        m_watchOfOperation = watch.around;
}

void RuntimeCallEmitter::beginWatchOfEffects(const OperationContext& context, const Effects& effects, EntersCompiledCode entersCompiledCode, uint32_t rows, WatchOfEffects& watch)
{
    watch.rows = rows;
    watch.around = m_watchOfOperation;
    if (!effects.mayRunOtherCode() && entersCompiledCode == EntersCompiledCode::No)
        watch.entriesInFront = readWordForEffectsNet(AOT_INSTANCE_OFFSET(effectsNet.entriesIntoCode), std::nullopt, nullptr);
    if (!effects.mayCollect)
        watch.collectionsInFront = readCollectionsForEffectsNet(nullptr);
    watch.isHeldToThrowNothing = !effects.mayThrow;
    watch.lanesNotWritten = lanesNotWrittenInEffectsNet(effects);
    if (watch.lanesNotWritten && !generator().isInitializingCell()) {
        const Operation& operation = operationAt(context.valueID());
        for (uint32_t operandIndex = 0; operandIndex < operation.numberOfOperands() && watch.numberOfDigests < watch.digests.size(); ++operandIndex) {
            ValueID operand = operation.operand(operandIndex);
            if (!operand)
                continue;
            Representation representation = representationOf(operand);
            if (representation != Representation::JSValue && representation != Representation::Cell)
                continue;
            LValue cell = generator().wordOf(operand);
            if (!cell || cell->type() != Machine::Int64)
                continue;
            std::array<LValue, 2> arguments { cell, m_out.constInt64(watch.lanesNotWritten) };
            watch.digests[watch.numberOfDigests++] = { operandIndex, cell, emitEntryCall(nullptr, AOT::Entry::operationAOTDigestForEffectsNet, arguments, { }).first };
        }
    }
    if (!effects.mayThrowByRule() && watch.entriesInFront) {
        Origin origin = context.origin();
        uint64_t function = m_code.inlineFrames()[origin.inlineFrame].callee.index();
        watch.callHeldToThrowNothing = 1ull << 63 | function << 32 | origin.position;
        noteCallHeldToThrowNothing(watch.callHeldToThrowNothing, rows);
    }
}

void RuntimeCallEmitter::endWatchOfEffects(const OperationContext& context, const WatchOfEffects& watch, LValue behind)
{
    abandonWatchOfOperation(watch);
    const Operation& operation = operationAt(context.valueID());
    LValue builtinNamed = m_out.constInt64(Operation::opcodesWithBuiltin.contains(operation.opcode) ? operation.builtin().index() : 0);
    Vector<PartOfEffects, 4> parts;
    if (watch.collectionsInFront) {
        stopAtFalseEffects(context, watch.rows, m_out.equal(watch.collectionsInFront, readCollectionsForEffectsNet(behind)), static_cast<uint32_t>(PartOfEffects::Collects), builtinNamed);
        parts.append(PartOfEffects::Collects);
        build().statistics().count(StatisticID::CallsHeldNotToCollect);
    }
    if (watch.callHeldToThrowNothing)
        noteCallHeldToThrowNothing(watch.around ? watch.around->callHeldToThrowNothing : 0, watch.around ? watch.around->rows : 0);
    if (watch.isHeldToThrowNothing) {
        LValue pending = readWordForEffectsNet(AOT_INSTANCE_OFFSET(vm), VM::exceptionOffset(), behind);
        stopAtFalseEffects(context, watch.rows, m_out.isZero64(pending), static_cast<uint32_t>(PartOfEffects::Throws), builtinNamed);
    }
    if (watch.isHeldToThrowNothing || watch.callHeldToThrowNothing) {
        parts.append(PartOfEffects::Throws);
        build().statistics().count(StatisticID::CallsHeldToThrowNothing);
    }
    if (watch.entriesInFront) {
        LValue entriesBehind = readWordForEffectsNet(AOT_INSTANCE_OFFSET(effectsNet.entriesIntoCode), std::nullopt, behind);
        stopAtFalseEffects(context, watch.rows, m_out.equal(watch.entriesInFront, entriesBehind), static_cast<uint32_t>(PartOfEffects::RunsOtherCode), builtinNamed);
        parts.append(PartOfEffects::RunsOtherCode);
        build().statistics().count(StatisticID::CallsHeldToRunNoOtherCode);
    }
    for (const WatchOfEffects::Digest& digest : std::span(watch.digests).first(watch.numberOfDigests)) {
        std::array<LValue, 2> arguments { digest.cell, m_out.constInt64(watch.lanesNotWritten) };
        LValue differs = m_out.bitXor(digest.inFront, emitEntryCall(nullptr, AOT::Entry::operationAOTDigestForEffectsNet, arguments, { }).first);
        stopAtFalseEffects(context, watch.rows, m_out.isZero64(differs), static_cast<uint32_t>(PartOfEffects::Writes) | digest.operandIndex << 16, differs);
    }
    if (watch.numberOfDigests) {
        parts.append(PartOfEffects::Writes);
        build().statistics().count(StatisticID::OperandsHashedRoundCalls, watch.numberOfDigests);
    }
    markRowsExercised(watch.rows, parts.span());
}

void RuntimeCallEmitter::stopAtFalseEffects(const OperationContext& context, uint32_t rows, LValue holds, uint32_t part, LValue differs)
{
    LBasicBlock stops = newBlock();
    LBasicBlock continuation = newBlock();
    ScopeFork fork = m_out.forkScope();
    m_out.branch(holds, normalEdge(continuation), rareEdge(stops));

    m_out.appendToArm(stops, fork);
    Origin origin = context.origin();
    FunctionID writtenIn = m_code.inlineFrames()[origin.inlineFrame].callee;
    m_out.suspendDeclaredAccesses();
    std::array<LValue, 5> arguments { m_out.constInt32(static_cast<int32_t>(writtenIn.index())), m_out.constInt32(static_cast<int32_t>(origin.position)), m_out.constInt32(static_cast<int32_t>(part)), m_out.constInt32(static_cast<int32_t>(rows)), differs };
    emitEntryCall(&context, AOT::Entry::operationAOTStopAtFalseEffects, arguments, { });
    m_out.resumeDeclaredAccesses();
    m_out.jump(continuation);
    m_out.appendToJoin(continuation, fork);
}

LValue RuntimeCallEmitter::readWordForEffectsNet(ptrdiff_t offsetInInstance, std::optional<ptrdiff_t> offsetBehindPointer, LValue behind)
{
    Machine::PatchOperation* read = m_out.patchpoint(Machine::Int64);
    if (behind)
        read->append(behind, Machine::ValueRep::ColdAny);
    read->effects = Machine::Effects::none();
    read->effects.writesLocalState = true;
    read->setRunsWithoutFrame(true);
    read->clobber(RegisterSet::macroClobberedGPRs());
    read->setGenerator([offsetInInstance, offsetBehindPointer](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.load64(CCallHelpers::Address(instanceGPR, offsetInInstance), params[0].gpr());
        if (offsetBehindPointer)
            jit.load64(CCallHelpers::Address(params[0].gpr(), *offsetBehindPointer), params[0].gpr());
    });
    return read;
}

LValue RuntimeCallEmitter::readCollectionsForEffectsNet(LValue behind)
{
    std::array<LValue, 1> arguments { readWordForEffectsNet(AOT_INSTANCE_OFFSET(vm), std::nullopt, behind) };
    return emitEntryCall(nullptr, AOT::Entry::operationAOTCollectionsForEffectsNet, arguments, { }).first;
}

void RuntimeCallEmitter::noteCallHeldToThrowNothing(uint64_t call, uint32_t rows)
{
    Machine::PatchOperation* note = m_out.patchpoint(Machine::Void);
    note->effects = Machine::Effects::none();
    note->effects.writesLocalState = true;
    note->setRunsWithoutFrame(true);
    note->clobber(RegisterSet::macroClobberedGPRs());
    note->setGenerator([call, rows](CCallHelpers& jit, const Machine::PatchParameters&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (call)
            jit.store64(CCallHelpers::TrustedImm64(rows), CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(effectsNet.rowsOfCallHeldToThrowNothing)));
        jit.store64(CCallHelpers::TrustedImm64(static_cast<int64_t>(call)), CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(effectsNet.callHeldToThrowNothing)));
    });
}

void RuntimeCallEmitter::markRowsExercised(uint32_t rows, std::span<const PartOfEffects> parts)
{
    Machine::PatchOperation* mark = m_out.patchpoint(Machine::Int64);
    mark->effects = Machine::Effects::none();
    mark->effects.writesLocalState = true;
    mark->setRunsWithoutFrame(true);
    mark->clobber(RegisterSet::macroClobberedGPRs());
    std::array<unsigned, 8> bytes { };
    unsigned numberOfBytes = 0;
    for (PartOfEffects part : parts) {
        bytes[numberOfBytes++] = byteInEffectsNet(rows & UINT16_MAX, part);
        if (rows >> 16 != noRowInEffectsNet)
            bytes[numberOfBytes++] = byteInEffectsNet(rows >> 16, part);
    }
    mark->setGenerator([bytes, numberOfBytes](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg table = params[0].gpr();
        jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(effectsNet.exercised)), table);
        for (unsigned index = 0; index < numberOfBytes; ++index)
            jit.store8(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(table, bytes[index]));
    });
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
    RELEASE_ASSERT(!effectsOfCallee.reads.isEmpty());
    request.effectsOfCallee = &effectsOfCallee;
    request.shapeKnowsCallee = false;
    return emitCall(nullptr, request).first;
}

Machine::PatchOperation* RuntimeCallEmitter::createCallPatchpoint(const OperationContext& context, const CallPatchpointRequest& request)
{
    bool unwindsWithinFunction = ExceptionEmitter::unwindsWithinFunction(context);
    uint8_t traits = traitsOfPatchEffects(request.argumentBufferAccess, false, CellConversions::AsEffectsSay);
    const Effects* effectsOfOperation = request.effects.isPure() ? nullptr : &effectsOf(context);
    PatchEffectsMemo::CallPatchpointSeen* seen = nullptr;
    const RememberedPatchEffects* knownEffects = nullptr;
    if (effectsOfOperation) {
        seen = &patchEffectsMemo().lastCallPatchpoints[(unwindsWithinFunction ? 1 : 0) | (effectsOfOperation->mayThrowByRule() ? 2 : 0)];
        if (seen->patchEffects && seen->traits == traits && seen->effectsOfOperation == *effectsOfOperation && seen->effectsOfCallee == request.effects)
            knownEffects = seen->patchEffects;
    }
    std::optional<Effects> computedEffects;
    if (!knownEffects) {
        computedEffects.emplace(effectsOfCall(context, request.effects));
        if (!computedEffects->mayThrowOtherThanStackOverflow() && !unwindsWithinFunction)
            computedEffects->mayThrow = false;
    }

    Machine::PatchOperation* patchpoint = request.holdsUniqueRecord ? m_out.patchpointWithUniqueRecord(request.resultType) : m_out.patchpoint(request.resultType);
    patchpoint->clobberEarly(request.earlyClobbered);
    patchpoint->clobberLate(request.lateClobbered);
    if (knownEffects) {
        RELEASE_ASSERT(!generator().isInitializingCell() || (!knownEffects->effects.mayCollect && !knownEffects->effects.mayThrow));
        patchpoint->effects = knownEffects->patchEffects;
    } else {
        const RememberedPatchEffects* appliedEffects = applyEffectsThroughMemo(patchpoint, *computedEffects, request.argumentBufferAccess, false, CellConversions::AsEffectsSay);
        if (seen && appliedEffects) {
            seen->effectsOfCallee = request.effects;
            seen->effectsOfOperation = *effectsOfOperation;
            seen->traits = traits;
            seen->patchEffects = appliedEffects;
        }
    }
    patchpoint->setRunsWithoutFrame(request.runsWithoutFrame);
    patchpoint->effects.terminal = request.neverReturns;
    return patchpoint;
}

void RuntimeCallEmitter::finishCallPatchpoint(const OperationContext& context, Machine::PatchOperation* patchpoint, uint32_t site, const CallPatchpointRequest& request)
{
    if ((request.effects.mayThrow || request.effects.mayCollect) && ExceptionEmitter::unwindsWithinFunction(context))
        emitter<ExceptionEmitter>().attachLandingPad(context, patchpoint, site, !request.neverReturns);
    if (request.neverReturns)
        m_out.appendTo(newBlock());
}

void RuntimeCallEmitter::findRegistersClobberedByCalls() const
{
    RegisterSet clobbered = registersClobberedByCompiledCalls();
    clobbered.merge(RegisterSet::macroClobberedGPRs());
    m_registersClobberedByCalls = clobbered;
}

RegisterSet RuntimeCallEmitter::registersThatThrowSitesClobber() const
{
    StubCallShapeCache& cache = stubCallShapes();
    const StubCatalog& catalog = inputs().stubCatalog;
    if (!cache.knowsThrowSites || cache.catalogOfThrowSites != catalog.identifier()) [[unlikely]] {
        cache.clobberedByThrowSites = registersClobberedByThrowSites(catalog);
        cache.catalogOfThrowSites = catalog.identifier();
        cache.knowsThrowSites = true;
    }
    return cache.clobberedByThrowSites;
}

namespace {

constexpr uint8_t noCallSiteKind = UINT8_MAX;

constexpr std::array<uint8_t, 16> callSiteKindsByColumns = [] {
    std::array<uint8_t, 16> kinds { };
    for (unsigned columns = 0; columns < kinds.size(); ++columns) {
        std::optional<CallSiteKind> kind = findCallSiteKind({ .isConstruct = !!(columns & 1), .isTailCall = !!(columns & 2), .isInsideCallee = !!(columns & 4), .namesBuiltin = !!(columns & 8) });
        kinds[columns] = kind ? static_cast<uint8_t>(*kind) : noCallSiteKind;
    }
    return kinds;
}();

} // anonymous namespace

RecordedSite RuntimeCallEmitter::recordedSiteOf(const OperationContext& context, const CallOptions& options)
{
    Origin origin = options.origin.value_or(context.origin());
    const Operation* operation = context.valueID() ? static_cast<const Operation*>(&context.instruction()) : nullptr;
    BuiltinID calledBuiltin = options.calledBuiltin;
    if (!calledBuiltin && operation && Operation::opcodesWithCalledBuiltin.contains(operation->opcode))
        calledBuiltin = operation->calledBuiltin();
    if (BuiltinID builtinOfFrame = origin.inlineFrame ? m_code.inlineFrames()[origin.inlineFrame].calledBuiltin : BuiltinID()) {
        const InlineFrame& frame = m_code.inlineFrames()[origin.inlineFrame];
        calledBuiltin = builtinOfFrame;
        origin = { .position = frame.callPosition, .inlineFrame = frame.parent, .isTailCall = frame.isTailCall, .isPartOfConstruction = frame.callIsPartOfConstruction };
    }
    if (calledBuiltin && BuiltinBindings::linkTimeConstantOf(calledBuiltin))
        calledBuiltin = { };
    bool standsForCallee = origin.isTailCall && (calledBuiltin || !options.constructs);
    bool isInsideCallee = options.callerHasLeft || (standsForCallee && (calledBuiltin || !operation || !runsBeforeCalleeOf(operation->opcode)));
    bool isTailCall = origin.isTailCall || options.callerHasLeft;
    uint8_t kind = callSiteKindsByColumns[(options.constructs ? 1 : 0) | (isTailCall ? 2 : 0) | (isInsideCallee ? 4 : 0) | (calledBuiltin ? 8 : 0)];
    RELEASE_ASSERT(kind != noCallSiteKind);
    return { .origin = origin, .kind = static_cast<CallSiteKind>(kind), .calledBuiltin = calledBuiltin, .constructedCallee = options.constructedCallee };
}

uint32_t RuntimeCallEmitter::addSite(const OperationContext& context, const CallOptions& options)
{
    return addRecordedSite(context, recordedSiteOf(context, options));
}

uint32_t RuntimeCallEmitter::addRecordedSite(const OperationContext& context, const RecordedSite& site)
{
    uint32_t number = m_records.sites.addSite(site);
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
    applyEffectsThroughMemo(patchpoint, effects, access, accessesCollectorState, conversions);
}

const RememberedPatchEffects* RuntimeCallEmitter::applyEffectsThroughMemo(Machine::PatchOperation* patchpoint, const Effects& effects, ArgumentBufferAccess access, bool accessesCollectorState, CellConversions conversions)
{
    RELEASE_ASSERT(!generator().isInitializingCell() || (!effects.mayCollect && !effects.mayThrow));

    bool isTerminal = patchpoint->effects.terminal;
    uint8_t traits = traitsOfPatchEffects(access, accessesCollectorState, conversions);
    PatchEffectsMemo& memo = patchEffectsMemo();
    PatchEffectsMemo::Entry* known = findPatchEffects(memo, effects, traits);
    if (known && known->isFilled) [[likely]] {
        patchpoint->effects = known->patchEffects;
        patchpoint->effects.terminal = isTerminal;
        return known;
    }
    computeEffects(patchpoint, effects, access, accessesCollectorState, conversions);
    if (known) {
        known->effects = effects;
        known->traits = traits;
        known->patchEffects = patchpoint->effects;
        known->patchEffects.terminal = false;
        known->patchEffects.reads = memo.keep(patchpoint->effects.reads);
        known->patchEffects.writes = memo.keep(patchpoint->effects.writes);
        known->isFilled = true;
    }
    patchpoint->effects.terminal = isTerminal;
    return known;
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
