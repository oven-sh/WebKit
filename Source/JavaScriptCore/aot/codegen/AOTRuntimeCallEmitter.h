/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
#include "AOTOperationEmitter.h"
#include "AOTStubAssembler.h"
#include "AOTStubDescriptor.h"
#include "AOTVector.h"
#include "B3ValueRep.h"
#include "RegisterSet.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class StubCatalog;
enum class DirectFunctionCall : uint8_t;

namespace Typed {

bool leavesCallerWithoutFrame(StubKey);

struct CallPatchpointRequest {
    LType resultType { B3::Void };
    RegisterSet earlyClobbered;
    RegisterSet lateClobbered;
    Effects effects;
    ArgumentBufferAccess argumentBufferAccess { ArgumentBufferAccess::None };
    bool runsWithoutFrame { false };
    bool neverReturns { false };
    bool holdsUniqueRecord { false };
    // Not a function of the program. Whether one is called from there is among the effects.
    bool callsStub { false };
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
    // The function takes no realm, so it neither throws, allocates nor calls back, whatever the operation does around the call.
    StubCallResult callDirectFunctionReturningFields(const OperationContext&, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive);

    StubCallResult callStub(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions& = { });
    StubCallResult callStubWithoutSite(StubKey, std::span<const LValue> operands, const CallOptions& = { });
    void callStubAndBranch(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions&, BranchTargets);
    void callStubWithoutSiteAndBranch(StubKey, std::span<const LValue> operands, BranchTargets);
    StubCallResult callStubForTwoReads(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions&, Origin firstRead);

    B3::PatchpointValue* createCallPatchpoint(const OperationContext&, const CallPatchpointRequest&);
    void finishCallPatchpoint(const OperationContext&, B3::PatchpointValue*, uint32_t site, const CallPatchpointRequest&);
    enum class CellConversions : uint8_t { AsEffectsSay, None, WhateverEffectsSay };
    // The slot of a site is filled, emptied and filled again by the stubs and the run time. No operation of the IR says so: only generated code reads the slots.
    enum class SlotsOfSites : uint8_t { MayChange, StayAsTheyAre };
    SlotsOfSites slotsOfSitesBehindStub(const OperationContext*, const Effects&) const;
    // For the end of a procedure. A write matters to a reader in the same procedure, and a call that writes nothing may be moved, merged or left out.
    void stateWhichCallsChangeSlotsOfSites();
    void applyEffects(B3::PatchpointValue*, const Effects&, ArgumentBufferAccess, bool accessesCollectorState = false, CellConversions = CellConversions::AsEffectsSay, SlotsOfSites = SlotsOfSites::MayChange);
    Effects effectsOfCall(const OperationContext&, const Effects& effectsOfCallee) const;

    RegisterSet registersClobberedByCalls() const;
    // Under validation: where a call patchpoint stores the registers that its callee keeps. One call of a frame runs at a time.
    B3::Air::StackSlot* keptRegisterStorage() { return ensureRegisterStorage(m_keptRegisterStorage); }
    // The same for the registers that a landing pad relies on, stored before each operation that may throw to one.
    B3::Air::StackSlot* landingPadRegisterStorage() { return ensureRegisterStorage(m_landingPadRegisterStorage); }
    RecordedSite recordedSiteOf(const OperationContext&, const CallOptions& = { });
    uint32_t addSite(const OperationContext&, const CallOptions& = { });

    Pointer argumentBuffer(std::span<const Boxed>);
    Pointer uninitializedArgumentBuffer(unsigned numberOfWords);

private:
    enum class CallEnd : uint8_t { Returns, BranchesOnFlags, NeverReturns };

    struct PlacedOperand {
        LValue value;
        B3::ValueRep rep;
    };

    struct CallRequest {
        StubKey key;
        Vector<PlacedOperand, 8> operands;
        CallOptions options;
        CallEnd end { CallEnd::Returns };
        std::optional<BranchTargets> targets;
        std::optional<Origin> firstRead;
        StubResult operationResult { StubResult::None };
        std::optional<OperationCall> preservingCall;
        RegisterSet additionalClobbers;
        Effects effectsOfCallee;
        ArgumentBufferAccess argumentBufferAccess { ArgumentBufferAccess::None };
        std::optional<AOT::Entry> entry;
    };

    bool mayConvertCells(const Effects&);
    CallRequest createRequest(StubKey, std::span<const LValue> operands, const CallOptions&, std::optional<BranchTargets> = std::nullopt, std::optional<Origin> firstRead = std::nullopt) const;
    CallRequest createEntryRequest(AOT::Entry, const CallOptions&) const;
    CallRequest requestOfDirectFunction(DirectFunctionCall, uint32_t number, std::span<const LValue> arguments, std::span<const LValue> keptAlive);
    void placeArguments(CallRequest&, std::span<const LValue> arguments, OperationCall::FirstArgument) const;
    Operand callChecked(const OperationContext*, RuntimeOperation, std::span<const Operand> arguments, Representation result, const CallOptions&);
    StubCallResult emitEntryCall(const OperationContext*, AOT::Entry, std::span<const LValue> arguments, const CallOptions&);
    StubCallResult emitCall(const OperationContext*, const CallRequest&);
    StubCallResult resultOfCall(StubResult, B3::PatchpointValue*);
    LValue transientBuffer(size_t bytes);
    B3::Air::StackSlot* ensureRegisterStorage(B3::Air::StackSlot*&);

    B3::Air::StackSlot* m_transientBuffer { nullptr };
    Vector<B3::PatchpointValue*, 16> m_callsThatMayChangeSlotsOfSites;
    B3::Air::StackSlot* m_keptRegisterStorage { nullptr };
    B3::Air::StackSlot* m_landingPadRegisterStorage { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
