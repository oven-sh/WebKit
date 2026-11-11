/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
#include "AOTEffectsNet.h"
#include "AOTHeaps.h"
#include "AOTOperationEmitter.h"
#include "AOTStubAssembler.h"
#include "AOTStubDescriptor.h"
#include "RegisterSet.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class StubCatalog;
enum class DirectFunctionCall : uint8_t;

namespace Typed {

bool leavesCallerWithoutFrame(StubKey);
struct PatchEffectsMemo;
struct RememberedPatchEffects;
struct StubCallShape;
struct StubCallShapeCache;

struct CallPatchpointRequest {
    LType resultType { Machine::Void };
    RegisterSet earlyClobbered;
    RegisterSet lateClobbered;
    Effects effects;
    ArgumentBufferAccess argumentBufferAccess { ArgumentBufferAccess::None };
    bool runsWithoutFrame { false };
    bool neverReturns { false };
    bool holdsUniqueRecord { false };
};

class RuntimeCallEmitter final : public OperationEmitter {
public:
    explicit RuntimeCallEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Operand callOperation(const OperationContext&, RuntimeOperation, std::span<const Operand> arguments, Representation result, const CallOptions&);
    StubCallResult callOperationReturningPair(const OperationContext&, RuntimeOperation, std::span<const Operand> arguments, const CallOptions& = { });
    Operand callWithoutSite(RuntimeOperation, std::span<const Operand> arguments, Representation result, const CallOptions& = { });
    Operand callLeaf(RuntimeOperation, std::span<const Operand> arguments, Representation result);
    LValue callEntry(const OperationContext&, AOT::Entry, LType, std::span<const LValue> arguments, const CallOptions& = { });
    struct HostFrameOfDirectCall {
        LValue thisValue { nullptr };
        LValue callee { nullptr };
    };
    static bool leavesRegistersForHostFrame(std::span<const LValue> arguments);
    LValue callDirectFunction(const OperationContext&, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive, std::optional<HostFrameOfDirectCall> = std::nullopt, BuiltinID calledBuiltin = { });
    LValue callDirectFunctionReturningNumber(const OperationContext&, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive);

    StubCallResult callStub(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions& = { });
    StubCallResult callStubWithoutSite(StubKey, std::span<const LValue> operands, const CallOptions& = { });
    void callStubAndBranch(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions&, BranchTargets);
    void callStubWithoutSiteAndBranch(StubKey, std::span<const LValue> operands, const CallOptions&, BranchTargets);
    LValue callStubThatReads(StubKey, std::span<const LValue> operands, const MemoryLocation&, const CallOptions&);

    Machine::PatchOperation* createCallPatchpoint(const OperationContext&, const CallPatchpointRequest&);
    void finishCallPatchpoint(const OperationContext&, Machine::PatchOperation*, uint32_t site, const CallPatchpointRequest&);
    enum class CellConversions : uint8_t { AsEffectsSay, None, WhateverEffectsSay };
    void applyEffects(Machine::PatchOperation*, const Effects&, ArgumentBufferAccess, bool accessesCollectorState = false, CellConversions = CellConversions::AsEffectsSay);
    Effects effectsOfCall(const OperationContext&, const Effects& effectsOfCallee) const;

    RegisterSet registersClobberedByCalls() const
    {
        if (!m_registersClobberedByCalls) [[unlikely]]
            findRegistersClobberedByCalls();
        return *m_registersClobberedByCalls;
    }
    RegisterSet registersThatThrowSitesClobber() const;
    RecordedSite recordedSiteOf(const OperationContext&, const CallOptions& = { });
    uint32_t addSite(const OperationContext&, const CallOptions& = { });
    uint32_t addRecordedSite(const OperationContext&, const RecordedSite&);

    struct WatchOfEffects {
        struct Digest {
            uint32_t operandIndex { 0 };
            LValue cell { nullptr };
            LValue inFront { nullptr };
            LValue restInFront { nullptr };
            LValue secondInFront { nullptr };
            LValue restOfSecondInFront { nullptr };
            VariableID variable;
        };

        bool isSet() const { return entriesInFront || collectionsInFront || isHeldToThrowNothing || callHeldToThrowNothing || numberOfDigests || countsInFront || deedsInFront || loudDeedsInFront; }

        uint32_t rows { 0 };
        LValue entriesInFront { nullptr };
        LValue collectionsInFront { nullptr };
        bool isHeldToThrowNothing { false };
        uint64_t callHeldToThrowNothing { 0 };
        const WatchOfEffects* around { nullptr };
        uint8_t lanesNotWritten { 0 };
        uint8_t secondLanesNotWritten { 0 };
        uint8_t stableCounts { 0 };
        LValue countsInFront { nullptr };
        LValue deedsInFront { nullptr };
        LValue loudDeedsInFront { nullptr };
        static constexpr unsigned mostOperandsDigested = 6;
        std::array<Digest, 14> digests { };
        unsigned numberOfDigests { 0 };
        Vector<std::pair<VariableID, LValue>, 4> wordsOfVariablesInFront;
    };
    void beginWatchOfOperation(const ValueContext&, WatchOfEffects&);
    void endWatchOfEffects(const OperationContext&, const WatchOfEffects&, LValue behind);
    void abandonWatchOfOperation(const WatchOfEffects&);
    void countStoreForEffectsNet(CountInEffectsNet);
    void beginWatchOfWhatMayRunAgain(const ValueContext& beginning);
    void endWatchOfWhatRunsAgain(const ValueContext& end, ValueID beginning);
    struct DeedsInFrontOfQuietCall {
        LValue all { nullptr };
        LValue loud { nullptr };
    };
    DeedsInFrontOfQuietCall countDeedOf(const ValueContext&);
    void putBackDeedsBehindQuietCall(DeedsInFrontOfQuietCall);

    LValue readCollectionCount() { return readCollectionsForEffectsNet(nullptr); }

    Pointer argumentBuffer(std::span<const Boxed>);
    Pointer uninitializedArgumentBuffer(unsigned numberOfWords);

private:
    enum class CallEnd : uint8_t { Returns, BranchesOnFlags, NeverReturns };

    struct PlacedOperand {
        LValue value;
        Machine::ValueRep rep;
    };

    struct CallRequest {
        WTF_MAKE_NONCOPYABLE(CallRequest);
    public:
        CallRequest() = default;

        StubKey key;
        Vector<PlacedOperand, 8> operands;
        std::span<const LValue> operandsOfStub;
        const CallOptions* options { nullptr };
        CallEnd end { CallEnd::Returns };
        std::optional<BranchTargets> targets;
        std::optional<Origin> firstRead;
        StubResult operationResult { StubResult::None };
        std::optional<OperationCall> preservingCall;
        RegisterSet additionalClobbers;
        const Effects* effectsOfCallee { nullptr };
        ArgumentBufferAccess argumentBufferAccess { ArgumentBufferAccess::None };
        std::optional<AOT::Entry> entry;
        StubCallShape* shape { nullptr };
        bool shapeKnowsCallee { false };
    };

    struct CallRequestWithOwnOptions : CallRequest {
        CallOptions ownOptions;
        Effects ownEffects;
    };

    bool mayConvertCells(const Effects&);
    StubCallShapeCache& stubCallShapes() const;
    PatchEffectsMemo& patchEffectsMemo();
    void findRegistersClobberedByCalls() const;
    const RememberedPatchEffects* applyEffectsThroughMemo(Machine::PatchOperation*, const Effects&, ArgumentBufferAccess, bool accessesCollectorState, CellConversions);
    void computeEffects(Machine::PatchOperation*, const Effects&, ArgumentBufferAccess, bool accessesCollectorState, CellConversions);
    void fillRequest(CallRequest&, StubKey, std::span<const LValue> operands, const CallOptions&, std::optional<BranchTargets> = std::nullopt, std::optional<Origin> firstRead = std::nullopt) const;
    void fillEntryRequest(CallRequestWithOwnOptions&, AOT::Entry, OperationCall::FirstArgument, const CallOptions&) const;
    void fillRequestOfDirectFunction(CallRequestWithOwnOptions&, DirectFunctionCall, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive);
    void placeArguments(CallRequest&, std::span<const LValue> arguments, OperationCall::FirstArgument) const;
    Operand callChecked(const OperationContext*, RuntimeOperation, std::span<const Operand> arguments, Representation result, const CallOptions&);
    StubCallResult emitEntryCall(const OperationContext*, AOT::Entry, std::span<const LValue> arguments, const CallOptions&);
    StubCallResult emitCall(const OperationContext*, const CallRequest&);
    enum class EntersCompiledCode : bool { No, Yes };
    void beginWatchOfEffects(const OperationContext&, const Effects&, EntersCompiledCode, uint32_t rows, WatchOfEffects&, uint8_t exemptCounts = 0);
    LValue readCountsForEffectsNet(uint8_t counts, LValue behind);
    LValue digestForEffectsNet(LValue cell, uint64_t lanes);
    LValue restOfDigestForEffectsNet(LValue digest);
    LValue lanesThatDifferInEffectsNet(LValue inFront, LValue restInFront, LValue cell, uint64_t lanes);
    void digestInFrontOfCall(WatchOfEffects&, uint32_t operandIndex, LValue cell, VariableID = { });
    void digestVariablesThatUnitReads(const Effects&, WatchOfEffects&);
    LValue loadVariableForEffectsNet(VariableID);
    LValue loadWordOfVariableForEffectsNet(VariableID);
    void keepWordsOfVariablesThatUnitStores(WatchOfEffects&);
    std::optional<Vector<VariableID, 8>> m_variablesThatUnitReads;
    std::optional<Vector<VariableID, 8>> m_variablesThatUnitStores;
    LValue readWordForEffectsNet(ptrdiff_t offsetInInstance, std::optional<ptrdiff_t> offsetBehindPointer, LValue behind);
    LValue readCollectionsForEffectsNet(LValue behind);
    Vector<std::pair<ValueID, std::unique_ptr<WatchOfEffects>>, 2> m_watchesOfWhatMayRunAgain;
    void noteCallHeldToThrowNothing(uint64_t call, uint32_t rows);
    void markRowsExercised(uint32_t rows, std::span<const PartOfEffects>);
    void stopAtFalseEffects(const OperationContext&, uint32_t rows, LValue holds, uint32_t part, LValue differs);
    LType typeOfResult(StubResult);
    StubCallResult resultOfCall(StubResult, LValue call);
    LValue transientBuffer(size_t bytes);

    const WatchOfEffects* m_watchOfOperation { nullptr };
    Machine::StackSlot* m_transientBuffer { nullptr };
    mutable StubCallShapeCache* m_stubCallShapes { nullptr };
    PatchEffectsMemo* m_patchEffectsMemo { nullptr };
    mutable std::optional<RegisterSet> m_registersClobberedByCalls;
    std::optional<LType> m_typeOfTwoWords;
    HeapList m_scratchHeaps;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
