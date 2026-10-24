/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

struct LayoutUses {
    LayoutUses(size_t numberOfLayouts, size_t numberOfTypes)
        : named(numberOfLayouts)
        , typesOfConvertedResults(numberOfTypes)
    {
    }

    void name(LayoutID layout)
    {
        if (layout)
            named.quickSet(layout.index());
    }

    BitVector named;
    BitVector typesOfConvertedResults;
};

// Collects the layouts that function bodies name, one function at a time, so that whoever has a body in cache can collect from it.
class LayoutUseCollector {
public:
    // The types and the type sites of the program must have been read.
    LayoutUseCollector(const Program&, size_t numberOfLayouts, unsigned numberOfWorkers);

    void collect(const Function&, unsigned worker);
    void collect(const Data&, unsigned worker);
    LayoutUses takeUses();
    BitVector takeTypesWithLayouts() { return WTF::move(m_typesWithLayouts); }
    unsigned numberOfTypes() const { return m_numberOfTypes; }

private:
    LayoutUses& usesOf(unsigned worker);

    const Program& m_program;
    size_t m_numberOfLayouts;
    unsigned m_numberOfTypes { 0 };
    BitVector m_typesWithLayouts;
    Vector<std::optional<LayoutUses>> m_usesOfWorkers;
};

void markUnallocatedLayouts(Program&, const ParallelLoops&, LayoutUseCollector&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
