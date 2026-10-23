/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT) && (ASSERT_ENABLED || defined(AOT_IR_PRINTER_FOR_TESTAOT))

#include "AOTPlaceTypeTestsPlan.h"

#include "AOTAnalysisDump.h"
#include "AOTIRPrinter.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

void TestConditions::dump(PrintStream& out, const Program& program, TestConditionID id) const
{
    const Value& test = at(id).test;
    IRPrinter printer(out, program);
    out.print(nameOf(test.opcode));
    printFields(printer, test);
}

static void dumpPlannedTest(PrintStream& out, const Program& program, const TestPlacementPlan& plan, const PlannedTest& test)
{
    out.print("test ");
    plan.conditions->dump(out, program, test.condition);

    switch (test.position) {
    case PlannedTestPosition::BeforeUse:
        out.print(" before ");
        dumpID(out, plan.sites.sites[plan.requirements[test.requirement].site].use);
        return;
    case PlannedTestPosition::OnEdge:
        out.print(" on ");
        dumpEdge(out, test.edge);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpExistingProof(PrintStream& out, const ExistingProof& proof)
{
    switch (proof.source) {
    case ProofSource::TakenEdge:
        out.print("proof taken-edge ");
        dumpEdge(out, proof.edge);
        return;
    case ProofSource::SwitchCase:
        out.print("proof switch-case ");
        dumpEdge(out, proof.edge);
        return;
    case ProofSource::GuardValue:
        out.print("proof guard ");
        dumpID(out, proof.value);
        return;
    case ProofSource::Allocation:
        out.print("proof allocation ");
        dumpID(out, proof.value);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TestPlacementPlan::dump(PrintStream& out, const Program& program) const
{
    unsigned numberOfPrintedStretches = 0;
    for (unsigned stretch = 0; stretch < stretches.size(); ++stretch) {
        Vector<ValueID> usedValues;
        BitVector requiredConditions;
        for (const PlannedUse& use : uses) {
            if (use.stretch != stretch)
                continue;
            const TestSite& site = sites.sites[use.site];
            usedValues.append(site.use);
            for (const TestRequirement& requirement : requirements.subspan(use.firstRequirement, site.numberOfTests))
                requiredConditions.set(requirement.condition.index());
        }
        if (usedValues.isEmpty())
            continue;

        out.print("stretch ", numberOfPrintedStretches++, ": ");
        CommaPrinter separator("; "_s);
        for (const PlannedTest& test : tests) {
            if (test.stretch != stretch)
                continue;
            out.print(separator);
            dumpPlannedTest(out, program, *this, test);
        }
        for (const ExistingProof& proof : proofs) {
            if (!requiredConditions.get(proof.condition.index()))
                continue;
            out.print(separator);
            dumpExistingProof(out, proof);
        }
        for (const PlannedSlowPath& slowPath : slowPaths) {
            if (slowPath.stretch != stretch)
                continue;
            out.print(separator, "slow path leaves on ");
            dumpEdge(out, slowPath.edge);
        }

        AOT::sort(usedValues);
        out.print(separator, "uses");
        for (ValueID value : usedValues) {
            out.print(" ");
            dumpID(out, value);
        }
        out.print(separator, "cloned ", stretches[stretch].clonedBytes);
        for (const PlannedWayBackIntoLoop& wayBack : waysBackIntoLoops) {
            if (wayBack.stretch != stretch)
                continue;
            out.print(separator, "way back into ");
            dumpID(out, wayBack.head);
            out.print(" joins ");
            dumpEdge(out, wayBack.enteringEdge);
            for (const PlannedTest& test : wayBack.tests) {
                out.print(", test ");
                conditions->dump(out, program, test.condition);
            }
        }
        out.print("\n");
    }

    if (!numberOfPrintedStretches || rejoinPoints.isEmpty())
        return;
    out.print("copy comes back at");
    for (ProgramPoint point : rejoinPoints) {
        out.print(" ");
        dumpPoint(out, point);
    }
    out.print("\n");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && (ASSERT_ENABLED || defined(AOT_IR_PRINTER_FOR_TESTAOT))
