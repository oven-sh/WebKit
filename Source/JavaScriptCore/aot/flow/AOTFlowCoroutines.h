/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowCalls.h"
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowCoroutineState {
    HashMap<FlowObjectID, FlowInstanceID> bodyOfResult;
};

std::optional<CoroutineKind> coroutineKindOf(const Function&);
BuiltinClassID resultClassOf(CoroutineKind);

FlowObjectID linkCoroutineResult(FlowSolver&, const FlowCallRequest&, FlowInstanceID callee);
FlowInstanceID coroutineBodyOf(const FlowCallState&, FlowObjectID result);

struct FlowAwaitRequest {
    FlowStatementSite site;
    ValueID origin;
    FlowNodeID result;
    FlowNodeID thrown;
};

void awaitValuesOf(FlowSolver&, const FlowAwaitRequest&, FlowNodeID value);
void awaitObjects(FlowSolver&, const FlowAwaitRequest&, const FlowNewObjects&);
void awaitThenable(FlowSolver&, const FlowAwaitRequest&, FlowObjectID);
void resolvePromiseWith(FlowSolver&, FlowStatementSite, ValueID origin, FlowObjectID promise, FlowNodeID value);
void rejectPromiseWith(FlowSolver&, FlowObjectID promise, FlowNodeID reason, const Witness&);

struct FlowGeneratorResumption {
    FlowStatementSite site;
    ValueID origin;
    ResumeMode mode { ResumeMode::Next };
    FlowNodeID sent;
    FlowNodeID value;
    FlowNodeID thrown;
};

enum class FlowGeneratorBody : uint8_t {
    Unknown,
    Resumed,
};

FlowGeneratorBody resumeGenerator(FlowSolver&, const FlowGeneratorResumption&, FlowObjectID generator);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
