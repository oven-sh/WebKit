/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <optional>
#include <wtf/Forward.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BranchCounterNumbers;
class Program;
struct Function;
struct Terminator;

struct CountedBranch {
    BlockID block;
    ModuleID module;
    uint32_t position { 0 };
    bool isNegated { false };
    // None where the terminator itself is counted.
    ValueID test;
};

Vector<CountedBranch> findCountedBranches(const Program&, const Function&);
unsigned counterSideOf(const Terminator&, uint32_t targetIndex);

JS_EXPORT_PRIVATE BranchCounterNumbers numberBranchCounters(const Program&, BuildContext&);

class BranchCounterNumbers {
public:
    static constexpr unsigned numberOfSides = 2;

    BranchCounterNumbers() = default;

    bool isCounting() const { return m_limit; }
    uint32_t firstOf(FunctionID) const;
    std::optional<uint32_t> counterFor(FunctionID, uint32_t ordinal, unsigned side) const;
    std::optional<uint32_t> counterOfEntries(FunctionID) const;
    JS_EXPORT_PRIVATE void dump(PrintStream&, const Program&) const;

private:
    friend BranchCounterNumbers numberBranchCounters(const Program&, BuildContext&);

    Vector<uint32_t> m_first;
    uint64_t m_firstOfEntries { 0 };
    uint32_t m_limit { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
