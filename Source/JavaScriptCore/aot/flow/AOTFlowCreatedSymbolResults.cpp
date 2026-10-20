/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTFlowSymbols.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isCreatedOnce(const FlowFamilyInputs& inputs, const FlowSymbolCreation& creation)
{
    return !creation.isInCycle && inputs.executionsByFunction[creation.function.index()] == ExecutionCount::AtMostOnce;
}

static bool isCalledWithoutArguments(const FlowSummary& summary, ValueID call)
{
    const Vector<FlowCall>& calls = summary.statements.calls;
    auto statement = std::ranges::find(calls, call, &FlowCall::origin);
    return statement != calls.end() && !statement->arguments.count && !statement->argumentList;
}

static bool hasKnownDescription(const FlowFamilyInputs& inputs, const FlowSymbolCreation& creation)
{
    return inputs.program().at(creation.key).text
        || isCalledWithoutArguments(inputs.graph().summaryOf(creation.function), creation.call);
}

static bool canReplaceCall(const FlowFamilyInputs& inputs, const FlowSymbolCreation& creation)
{
    uint32_t subject = creation.function.index();
    return inputs.isEnabled(FlowResultKind::LiteralValues, subject) && inputs.isEnabled(FlowResultKind::NarrowedTypes, subject);
}

Vector<PropertyKeyID> computeCreatedSymbolResults(const FlowFamilyInputs& inputs)
{
    Vector<PropertyKeyID> symbolsCreatedOnce;
    for (const FlowSymbolCreation& creation : symbolCreationsOf(inputs.solution)) {
        if (isCreatedOnce(inputs, creation) && hasKnownDescription(inputs, creation) && canReplaceCall(inputs, creation))
            symbolsCreatedOnce.append(creation.key);
    }
    std::ranges::sort(symbolsCreatedOnce);
    return symbolsCreatedOnce;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
