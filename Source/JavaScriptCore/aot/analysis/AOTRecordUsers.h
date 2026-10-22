/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTLayout.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

class Program;

struct ChangedRecords {
    bool isEmpty() const { return layouts.empty() && fields.empty() && variables.empty(); }

    // Anything in the record may have changed.
    std::span<const LayoutID> layouts;
    // Only what the record of the layout says of the field may have changed, and not the key.
    std::span<const FieldReference> fields;
    std::span<const VariableID> variables;
};

// The functions that see one of the records. What the body of any other function says by itself stays as it is when these records change.
// A layout or a variable is seen by whoever names it: in an operation, in a type, or by way of a layout that inherits from it or that stands for such a layout.
// A field is seen by an operation that names its layout in one of these ways and looks for its key, for a key that is an operand, or for all the fields there are.
// followsFieldsOfOperands has a byte for each function, and none for those at the end that nothing is kept of. A function with a byte that is set came to layouts
// by way of the fields of others, so it counts as naming whatever the records of the layouts it names lead to.
// isAsked has a byte for each function. A function whose byte is not set is left out, whatever it sees.
Vector<FunctionID> findFunctionsSeeing(const Program&, const ChangedRecords&, std::span<const uint8_t> followsFieldsOfOperands, std::span<const uint8_t> isAsked, const ParallelLoops&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
