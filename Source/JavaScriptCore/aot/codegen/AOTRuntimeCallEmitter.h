/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
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
    // Each argument as the C function takes it.
    struct HostFrameOfDirectCall {
        LValue thisValue { nullptr };
        LValue callee { nullptr };
    };
    static bool leavesRegistersForHostFrame(std::span<const LValue> arguments);
    LValue callDirectFunction(const OperationContext&, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive, std::optional<HostFrameOfDirectCall> = std::nullopt, BuiltinID calledBuiltin = { });
    LValue callDirectFunctionReturningNumber(const OperationContext&, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive);
    // BuiltinDirectFields, in two registers. Such a function neither throws nor allocates.
    StubCallResult callDirectFunctionReturningFields(const OperationContext&, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive);

    StubCallResult callStub(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions& = { });
    StubCallResult callStubWithoutSite(StubKey, std::span<const LValue> operands, const CallOptions& = { });
    void callStubAndBranch(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions&, BranchTargets);
    void callStubWithoutSiteAndBranch(StubKey, std::span<const LValue> operands, const CallOptions&, BranchTargets);
    StubCallResult callStubForTwoReads(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions&, Origin firstRead);
    // Of what the stub declares that it reads, this call reads the location and what lies beside its buckets. The options name a function of memory:
    // a later call with the same operands is this one until something writes there, as a load of the location would be.
    LValue callStubThatReads(StubKey, std::span<const LValue> operands, const MemoryLocation&, const CallOptions&);

    Machine::PatchOperation* createCallPatchpoint(const OperationContext&, const CallPatchpointRequest&);
    void finishCallPatchpoint(const OperationContext&, Machine::PatchOperation*, uint32_t site, const CallPatchpointRequest&);
    enum class CellConversions : uint8_t { AsEffectsSay, None, WhateverEffectsSay };
    void applyEffects(Machine::PatchOperation*, const Effects&, ArgumentBufferAccess, bool accessesCollectorState = false, CellConversions = CellConversions::AsEffectsSay);
    // What both the operation and the callee allow.
    Effects effectsOfCall(const OperationContext&, const Effects& effectsOfCallee) const;

    RegisterSet registersClobberedByCalls() const;
    RecordedSite recordedSiteOf(const OperationContext&, const CallOptions& = { });
    uint32_t addSite(const OperationContext&, const CallOptions& = { });

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
        const StubCallShape* shape { nullptr };
    };

    struct CallRequestWithOwnOptions : CallRequest {
        CallOptions ownOptions;
        Effects ownEffects;
    };

    bool mayConvertCells(const Effects&);
    StubCallShapeCache& stubCallShapes() const;
    void computeEffects(Machine::PatchOperation*, const Effects&, ArgumentBufferAccess, bool accessesCollectorState, CellConversions);
    void fillRequest(CallRequest&, StubKey, std::span<const LValue> operands, const CallOptions&, std::optional<BranchTargets> = std::nullopt, std::optional<Origin> firstRead = std::nullopt) const;
    void fillEntryRequest(CallRequestWithOwnOptions&, AOT::Entry, const CallOptions&) const;
    void fillRequestOfDirectFunction(CallRequestWithOwnOptions&, DirectFunctionCall, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive);
    void placeArguments(CallRequest&, std::span<const LValue> arguments, OperationCall::FirstArgument) const;
    Operand callChecked(const OperationContext*, RuntimeOperation, std::span<const Operand> arguments, Representation result, const CallOptions&);
    StubCallResult emitEntryCall(const OperationContext*, AOT::Entry, std::span<const LValue> arguments, const CallOptions&);
    StubCallResult emitCall(const OperationContext*, const CallRequest&);
    LType typeOfResult(StubResult);
    StubCallResult resultOfCall(StubResult, LValue call);
    LValue transientBuffer(size_t bytes);

    Machine::StackSlot* m_transientBuffer { nullptr };
    mutable StubCallShapeCache* m_stubCallShapes { nullptr };
    PatchEffectsMemo* m_patchEffectsMemo { nullptr };
    // One tuple type serves every call of the procedure that returns two words.
    std::optional<LType> m_typeOfTwoWords;
    // Of applyEffects(), which hands over a copy. It keeps its capacity from one call to the next, so a long list costs no allocation per call.
    HeapList m_scratchHeaps;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
