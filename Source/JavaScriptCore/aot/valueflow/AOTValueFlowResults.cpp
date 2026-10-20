/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResults.h"

#if ENABLE(AOT)

#include "AOTPhaseTimes.h"
#include "AOTValueFlowResultsInternal.h"

namespace JSC { namespace AOT { namespace Typed {

FlowResults analyzeValueFlow(Program& program, TypeTable& types, BuildContext& build, Vector<String>* setsOutsideTypes)
{
    RELEASE_ASSERT(&types == &program.types);
    RELEASE_ASSERT(program.form() == IRForm::AsRead);

    PhaseTimes times(build, PassID::AnalyzeFlow);
    FlowResultTables tables = createUnknownResultTables(program, build);
    times.didFinish("unknown results"_s);
    // Code that comes in anywhere at run time does anything to anything.
    if (build.options().admitsRuntimeCode())
        return FlowResults(WTF::move(tables));

    auto solution = makeUnique<ValueFlowSolution>(program, build);
    times.didFinish("solve"_s);
    if (!solution->hasResult()) {
        build.gaveUp(LimitID::MaxValueFlowEdgesLogged, Subject { });
        tables.wholeProgram.unseenCode.append(UnseenCodeRecord { .cause = UnseenCodeCause::LimitReached });
        build.destroyLater(WTF::move(solution));
        return FlowResults(WTF::move(tables));
    }

    ValueFlowResultInputs inputs { program, *solution, build };
    computeBuiltinResults(inputs, tables);
    times.didFinish("built-ins"_s);
    ValueFlowStampedCalls calls = computeCallResults(inputs, types, tables);
    times.didFinish("calls"_s);
    computeFunctionResults(inputs, calls, tables);
    times.didFinish("functions"_s);
    computeExecutionResults(inputs, calls, tables);
    times.didFinish("executions"_s);
    computeLocationResults(inputs, tables);
    times.didFinish("locations"_s);
    computeOrderResults(inputs, calls, tables);
    times.didFinish("order"_s);
    BitVector layoutsThatKeepTheirShape = computeObjectResults(inputs, calls, tables);
    times.didFinish("objects"_s);
    computeClassResults(inputs, tables);
    times.didFinish("classes"_s);
    ValueFlowFieldContents fieldContents = computeFieldResults(inputs, tables);
    times.didFinish("fields"_s);
    computeTypeResults(inputs, layoutsThatKeepTheirShape, fieldContents, types, tables, setsOutsideTypes);
    computeLayoutsNeverStoredOtherTypes(inputs, tables);
    times.didFinish("types"_s);

    build.destroyLater(WTF::move(solution));
    return FlowResults(WTF::move(tables));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
