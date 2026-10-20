/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

enum class StoredKind : uint8_t {
    Int32 = 1 << 0,
    Undefined = 1 << 1,
    Null = 1 << 2,
    Other = 1 << 3,
};

// What the stores that the program shows put into each field, followed through variables, parameters, results and other fields.
// It proves nothing: code that nobody sees, a computed key or a receiver of another type may store anything. It says which values a slot is worth making for.
class VisibleStores {
public:
    VisibleStores() = default;
    VisibleStores(const Program&, const ParallelLoops&);

    OptionSet<StoredKind> kindsStoredIn(LayoutID layout, uint32_t field) const { return m_kinds.isEmpty() ? OptionSet<StoredKind> { } : m_kinds[layout.index()][field]; }

private:
    Vector<Vector<OptionSet<StoredKind>>> m_kinds;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
