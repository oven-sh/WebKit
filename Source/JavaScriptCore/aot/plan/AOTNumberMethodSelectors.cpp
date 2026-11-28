/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumberMethodSelectors.h"

#if ENABLE(AOT)

#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTType.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

FormsOfSelector formsOfSelector(const Program& program, const LinkTables& linked, const CallingConventions& conventions, const MethodFamily& family)
{
    FormsOfSelector forms;
    bool someRowIsGuarded = std::ranges::any_of(family.rows, [](const MethodTableRow& row) {
        return !!row.guardedClass;
    });
    forms.everyRowIsProven = !someRowIsGuarded;
    if (!forms.everyRowIsProven || family.methods.isEmpty())
        return forms;

    std::optional<Representation> formOfFirst;
    for (FunctionID method : family.methods) {
        CodeUnit body { method, CodeRole::Body };
        if (program.at(method).isCoroutine() || conventions.numberOfResults(body) != 1)
            return forms;
        if (method.index() >= linked.functions.size() || !linked.function(method).result)
            return forms;
        Representation returned = conventions.resultRepresentation(body);
        if (formOfFirst && *formOfFirst != returned)
            return forms;
        formOfFirst = returned;
    }
    if (*formOfFirst == Representation::Void)
        forms.returnsNothing = true;
    else
        forms.result = *formOfFirst;
    return forms;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
