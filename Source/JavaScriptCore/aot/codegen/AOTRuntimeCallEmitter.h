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
#include "B3ValueRep.h"
#include "RegisterSet.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class StubCatalog;

namespace Typed {

bool acceptsRepresentation(RuntimeValueKind, Representation);

struct CallPatchpointRequest {
    LType resultType { B3::Void };
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
    explicit RuntimeCallEmitter(CodeGenerator&);

    Operand callOperation(const OperationContext&, RuntimeOperation, std::span<const Operand> arguments, Representation result, const CallOptions&);
    StubCallResult callOperationReturningPair(const OperationContext&, RuntimeOperation, std::span<const Operand> arguments, const CallOptions& = { });
    Operand callWithoutSite(RuntimeOperation, std::span<const Operand> arguments, Representation result, const CallOptions& = { });
    Operand callLeaf(RuntimeOperation, std::span<const Operand> arguments, Representation result);
    LValue callEntry(const OperationContext&, AOT::Entry, LType, std::span<const LValue> arguments, const CallOptions& = { });

    StubCallResult callStub(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions& = { });
    StubCallResult callStubWithoutSite(StubKey, std::span<const LValue> operands, const CallOptions& = { });
    void callStubAndBranch(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions&, BranchTargets);
    void tailCallStub(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions& = { });
    StubCallResult callStubForTwoReads(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions&, Origin firstRead);

    B3::PatchpointValue* createCallPatchpoint(const OperationContext&, const CallPatchpointRequest&);
    void finishCallPatchpoint(const OperationContext&, B3::PatchpointValue*, uint32_t site, const CallPatchpointRequest&);
    void applyEffects(B3::PatchpointValue*, const Effects&, ArgumentBufferAccess, bool accessesCollectorState = false);
    Effects effectsOfCall(const OperationContext&, const Effects& effectsOfCallee) const;

    RegisterSet registersClobberedByCalls() const;
    // Under validation: where a call patchpoint stores the registers that its callee keeps. One call of a frame runs at a time.
    B3::Air::StackSlot* keptRegisterStorage();
    // The same for the registers that a landing pad relies on, stored before each operation that may throw to one.
    B3::Air::StackSlot* landingPadRegisterStorage();
    RecordedSite recordedSiteOf(const OperationContext&, const CallOptions& = { });
    uint32_t addSite(const OperationContext&, const CallOptions& = { });
    uint32_t addItemSite(const OperationContext&, uint32_t callSite, uint32_t item, Origin);

    Pointer argumentBuffer(std::span<const Boxed>);
    Pointer uninitializedArgumentBuffer(unsigned numberOfWords);

private:
    enum class CallEnd : uint8_t { Returns, BranchesOnFlags, LeavesFunction, NeverReturns };

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
    };

    const StubSummary& summaryOf(StubKey) const;
    CallRequest createRequest(StubKey, std::span<const LValue> operands, const CallOptions&) const;
    CallRequest createEntryRequest(AOT::Entry, LType, const CallOptions&) const;
    void placeArguments(CallRequest&, std::span<const LValue> arguments, OperationCall::FirstArgument) const;
    StubCallResult emitCall(const OperationContext*, const CallRequest&);
    StubCallResult resultOfCall(StubResult, B3::PatchpointValue*);
    void checkArguments(RuntimeOperation, std::span<const Operand> arguments, Representation result) const;
    Operand convertResult(RuntimeValueKind, LValue, Representation);
    LValue transientBuffer(size_t bytes);
    B3::Air::StackSlot* ensureRegisterStorage(B3::Air::StackSlot*&);

    B3::Air::StackSlot* m_transientBuffer { nullptr };
    B3::Air::StackSlot* m_keptRegisterStorage { nullptr };
    B3::Air::StackSlot* m_landingPadRegisterStorage { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
