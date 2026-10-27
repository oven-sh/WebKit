/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTPlaceTypeTestsPlan.h"

#include "AOTAnalysisDump.h"
#include "AOTIRPrinter.h"
#include "AOTSort.h"

namespace JSC { namespace AOT { namespace Typed {

void TestConditions::dump(PrintStream& out, const Program& program, TestConditionID id) const
{
    const Value& test = at(id).test;
    IRPrinter printer(out, program);
    out.print(nameOf(test.opcode));
    printFields(printer, test);
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
            out.print(separator, "test ");
            conditions->dump(out, program, test.condition);
            out.print(test.position == PlannedTestPosition::BeforeUse ? " before " : " on ");
            if (test.position == PlannedTestPosition::BeforeUse)
                dumpID(out, sites.sites[requirements[test.requirement].site].use);
            else
                dumpEdge(out, test.edge);
        }
        for (const ExistingProof& proof : proofs) {
            if (!requiredConditions.get(proof.condition.index()))
                continue;
            switch (proof.source) {
            case ProofSource::TakenEdge:
            case ProofSource::SwitchCase:
                out.print(separator, proof.source == ProofSource::TakenEdge ? "proof taken-edge " : "proof switch-case ");
                dumpEdge(out, proof.edge);
                break;
            case ProofSource::GuardValue:
            case ProofSource::Allocation:
                out.print(separator, proof.source == ProofSource::GuardValue ? "proof guard " : "proof allocation ");
                dumpID(out, proof.value);
                break;
            }
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

#endif // ENABLE(AOT)
