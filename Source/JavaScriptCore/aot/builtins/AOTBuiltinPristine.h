/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTCFG.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

enum class Receiver : uint8_t;

namespace Typed {

class FunctionAnalyses;

// MayPass: start-up marks the Structure of new instances, whose prototypes are locked, and a change of an instance clears the mark.
enum class PristineTest : uint8_t { NeverPasses, MayPass, ClosureAsCreated, InstanceStructure };

PristineTest pristineTestOf(const BuiltinTable&, BuiltinClassID);
std::optional<Receiver> receiverWithTestedStructure(BuiltinClassID);
BuiltinClassID classBelowWithTestedStructure(const BuiltinTable&, BuiltinClassID);
PristineAspects aspectThatFindsMember(const BuiltinTable&, BuiltinClassID receiverClass, BuiltinID member);
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
