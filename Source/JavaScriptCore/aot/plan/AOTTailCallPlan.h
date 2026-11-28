/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTCodeRole.h"
#include "AOTID.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <algorithm>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallingConventions;
class Facts;
class Plan;
class Program;
struct StackTestPlan;

struct TailCallPlan {
    BitVector functionsThatSaveRegistersOfCallers;

    bool savesRegistersOfCallers(CodeUnit unit) const { return unit.role == CodeRole::Body && functionsThatSaveRegistersOfCallers.get(unit.function.index()); }
};

struct FunctionsThatEndTheirTailCalls {
    struct Condition {
        FunctionID function;
        uint64_t entryParametersThatMustEndTheirs { 0 };
    };

    uint64_t entryParametersThatMustEndTheirs(FunctionID id) const
    {
        if (!ifArgumentsDo.get(id.index()))
            return 0;
        auto found = std::ranges::lower_bound(conditions, id, { }, &Condition::function);
        RELEASE_ASSERT(found != conditions.end() && found->function == id);
        return found->entryParametersThatMustEndTheirs;
    }

    BitVector whateverArrives;
    BitVector ifArgumentsDo;
    Vector<Condition> conditions;
};

JS_EXPORT_PRIVATE FunctionsThatEndTheirTailCalls findFunctionsThatEndTheirTailCalls(const Program&, const Facts&);
void checkFormsRoundRingsOfTailCalls(const Program&, const Facts&, const Plan&, BuildContext&);
BitVector findFunctionsThatSaveRegistersOfCallers(const Program&, const Facts&, const CallingConventions&);
// planProgram(), behind the guards and in front of the fused entries. In this order: ends of tail calls > registers > stack tests. With DirectCalls withheld: nothing.
void planTailCallsAndStackTests(const Program&, const Facts&, Plan&, StackTestPlan&&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
