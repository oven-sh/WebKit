/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSelectTypedResultsNodes.h"

namespace JSC { namespace AOT { namespace Typed {

struct TypeAlternative {
    ResultShape shape { ResultShape::Unknown };
    ResultLiteral literal;
    LayoutID layout;
    TypeID contents;
};

TypeID expectationOf(const DeclaredType&);
Vector<TypeAlternative, 4> alternativesOf(const Program&, TypeID);
ResultNode describeTypeNode(const Program&, TypeID, uint32_t part);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
