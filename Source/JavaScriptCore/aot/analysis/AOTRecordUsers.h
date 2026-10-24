/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTLayout.h"
#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

class Program;

struct ChangedRecords {
    bool isEmpty() const { return layouts.empty() && fields.empty() && variables.empty(); }

    std::span<const LayoutID> layouts;
    std::span<const FieldReference> fields;
    std::span<const VariableID> variables;
};

Vector<FunctionID> findFunctionsSeeing(const Program&, const ChangedRecords&, std::span<const uint8_t> followsFieldsOfOperands, std::span<const uint8_t> isAsked, const ParallelLoops&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
