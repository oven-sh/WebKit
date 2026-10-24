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

inline TypeID expectationOf(const DeclaredType& declared) { return declared.type; }
Vector<TypeAlternative, 4> alternativesOf(const Program&, TypeID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
