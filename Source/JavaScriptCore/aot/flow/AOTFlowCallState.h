/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowBuiltinCalls.h"
#include "AOTFlowEscapedInheritors.h"
#include "AOTFlowModules.h"
#include "AOTFlowOrder.h"
#include "AOTFlowThrown.h"
#include "AOTFlowUnseenCode.h"
#include <tuple>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowCallLimits {
    uint64_t maxInstanceValues { 0 };
    uint64_t maxPositionalParts { 0 };
    uint64_t maxDispatchCases { 0 };
};

struct FlowOperandFlowState {
    HashSet<std::tuple<FlowCallLinkID, FlowNodeID, FlowNodeID>> recordedFlows;
    HashMap<FlowNodeID, FlowNodeID> operandOfReceiverHub;
};

struct FlowCallState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowCallState);
    WTF_MAKE_NONCOPYABLE(FlowCallState);

    FlowCallState() = default;

    FlowCallLimits limits;
    FlowCallLinkState links;
    FlowOperandFlowState operandFlows;
    FlowCoroutineState coroutines;
    FlowIterationState iteration;
    FlowBuiltinCallState builtins;
    FlowThrownState thrown;
    FlowUnseenCodeState unseenCode;
    FlowEscapedInheritorState escapedInheritors;
    FlowModuleState modules;
    FlowOrderState order;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
