/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTCFG.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

// MayPass: start-up marks the Structure of new instances, whose prototypes are locked, and a change of an instance clears the mark.
// ClosureAsCreated: a closure of the program has no mark. It keeps the Structure it is created with until it gains a property, gets another prototype or is made
// non-extensible, so the test compares with the Structures that plain functions are created with. A closure that passes is out of the way of a lookup, which goes on
// at Function.prototype. The test says nothing about what lies there, and every other function fails it: a host function, a class object, the closure of a coroutine.
enum class PristineTest : uint8_t { NeverPasses, MayPass, ClosureAsCreated };

PristineTest pristineTestOf(const BuiltinTable&, BuiltinClassID);
bool canOnlyBeProvenPristine(const BuiltinTable&, BuiltinClassID);

// Nothing in an object says that it stays as created, or that it has no holes. Only flow proves these.
inline constexpr PristineAspectSet aspectsWithoutTest { PristineAspects::AsCreated, PristineAspects::Dense };

enum class PristineProof : uint8_t { Unknown, Proven };

struct PristineQuery {
    const Program& program;
    const Function& function;
    FunctionAnalyses* analyses { nullptr };
    std::span<const Value> passedTests { };
    // Set where an answer follows from more than the type of the value: from where the point lies.
    bool* asksAboutPlace { nullptr };
};

PristineProof pristineProofOf(const PristineQuery&, ValueID, const PristineRequirement&, ProgramPoint);
PristineAspectSet aspectsProvenAt(const PristineQuery&, ValueID, BuiltinClassID, ProgramPoint);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
