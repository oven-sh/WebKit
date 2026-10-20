/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSolution.h"
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowSymbolCreation {
    PropertyKeyID key;
    FunctionID function;
    ValueID call;
    bool isInCycle { false };

    friend bool operator==(const FlowSymbolCreation&, const FlowSymbolCreation&) = default;
};

struct FlowSymbolKeyAsRead {
    ModuleID module;
    uint32_t position { 0 };
    PropertyKeyID key;
};

struct FlowSymbolState {
    Vector<FlowSymbolKeyAsRead> keysAsRead;
    Vector<FlowSymbolCreation> creations;
    HashMap<PropertyKeyID, uint32_t> creationIndexes;
};

FlowSymbolState createFlowSymbolState(const Program&);

struct FlowSymbolCall {
    FlowStatementSite site;
    ValueID origin;
    bool isInCycle { false };
};

FlowValue valueOfCreatedSymbol(FlowSolver&, const FlowSymbolCall&);
FlowValue valueOfRegisteredSymbols(FlowSolver&, const FlowValue& registryKeys);

std::span<const FlowSymbolCreation> symbolCreationsOf(const FlowSolution&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
