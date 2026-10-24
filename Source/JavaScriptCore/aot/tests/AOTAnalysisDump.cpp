/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"

#include "AOTVector.h"
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Tag>
static void dumpIDWithPrefix(PrintStream& out, ASCIILiteral prefix, ID<Tag> id)
{
    if (id)
        out.print(prefix, id.index());
    else
        out.print("none");
}

void dumpID(PrintStream& out, ValueID id) { dumpIDWithPrefix(out, "v"_s, id); }
void dumpID(PrintStream& out, BlockID id) { dumpIDWithPrefix(out, "b"_s, id); }
void dumpID(PrintStream& out, ObjectID id) { dumpIDWithPrefix(out, "o"_s, id); }
void dumpPoint(PrintStream& out, ProgramPoint point) { dumpItems(out, point.block, ":"_s, point.index); }

void dumpEdge(PrintStream& out, Edge edge)
{
    dumpID(out, edge.from);
    switch (edge.kind) {
    case EdgeKind::Normal:
        out.print(".", edge.targetIndex);
        return;
    case EdgeKind::Handler:
        out.print(".handler");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void dumpUse(PrintStream& out, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        dumpID(out, use.user);
        break;
    case Use::Kind::TerminatorOperand:
        dumpItems(out, use.block, ".terminator"_s);
        break;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        dumpEdge(out, use.edge());
        break;
    case Use::Kind::BlockSubject:
        dumpItems(out, use.block, ".subject"_s);
        return;
    }
    out.print("(", use.operandIndex, ")");
}

template<typename Flag>
static void dumpFlags(PrintStream& out, OptionSet<Flag> flags)
{
    Vector<ASCIILiteral> names;
    for (Flag flag : allMembersOf<Flag>) {
        if (flags.contains(flag))
            names.append(nameOf(flag));
    }
    dumpItem(out, names);
}

void dumpItem(PrintStream& out, OperandUses uses)
{
    if (uses == OperandUses::all())
        out.print("all");
    else
        dumpFlags(out, uses);
}

void dumpItem(PrintStream& out, EscapeReasons reasons) { dumpFlags(out, reasons); }

static ASCIILiteral placementTextOf(ValuePlacement placement)
{
    switch (placement) {
    case ValuePlacement::Parameter:
        return "parameter"_s;
    case ValuePlacement::InBlock:
        return "value"_s;
    case ValuePlacement::Detached:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void dumpUseLists(PrintStream& out, const Function& function, const UseLists& useLists)
{
    Vector<ValueID> detachedValues;
    for (size_t index = 0; index < function.values.size(); ++index) {
        ValueID value(index);
        ValueLocation location = useLists.locationOf(value);
        if (location.placement == ValuePlacement::Detached) {
            detachedValues.append(value);
            continue;
        }
        auto uses = useLists.usesOf(value);
        Vector<ProgramPoint> usePoints;
        for (const Use& use : uses)
            usePoints.append(useLists.pointOf(use));

        dumpLine(out, value, " location: "_s, location.block, " "_s, placementTextOf(location.placement), " "_s, location.index);
        dumpLine(out, value, " uses: "_s, uses);
        dumpLine(out, value, " use points: "_s, usePoints);
    }
    dumpLine(out, "detached: "_s, detachedValues);
}

static void dumpGuardOrigin(PrintStream& out, const GuardOrigin& origin)
{
    switch (origin.source) {
    case GuardSource::Test:
        dumpID(out, origin.test);
        return;
    case GuardSource::SwitchCase:
        dumpItems(out, origin.switchBlock, ".case"_s, origin.caseIndex);
        return;
    case GuardSource::Allocation:
        dumpItems(out, "allocation "_s, origin.test);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpGuardFact(PrintStream& out, const Program& program, const GuardFact& fact)
{
    constexpr ASCIILiteral emptyText = "-"_s;
    CommaPrinter proofSeparator(" "_s);
    for (Proof proof : allMembersOf<Proof>) {
        if (fact.proofs.contains(proof))
            out.print(proofSeparator, nameOf(proof));
    }
    out.print(" of ");
    if (fact.subject)
        dumpID(out, fact.subject);
    else
        out.print(emptyText);
    out.print(" by ");
    CommaPrinter alternative(" or "_s);
    for (const GuardOrigin& origin : fact.origins) {
        out.print(alternative);
        dumpGuardOrigin(out, origin);
    }
    out.print(" reads ");
    if (fact.dependsOn.isEmpty())
        out.print(emptyText);
    CommaPrinter separator(" "_s);
    for (const MemoryLocation& location : fact.dependsOn)
        out.print(separator, MemoryLocationDump { program, location });
}

void GuardAvailability::dump(PrintStream& out) const
{
    solveNamedGuards();
    solveUnnamedAllocations();

    for (ValueID guard : m_guards) {
        const GuardFacts& facts = factsOf(guard);
        dumpItems(out, guard, " facts: "_s);
        if (!facts.isKnown()) {
            out.println("unknown");
            continue;
        }
        out.print("[");
        CommaPrinter separator;
        for (const GuardFact& fact : facts.facts) {
            out.print(separator);
            dumpGuardFact(out, m_analyses.program(), fact);
        }
        out.println("]");
    }

    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        if (!m_solution.solvedBlocks.get(block.index()))
            continue;
        dumpItems(out, block, " available at head: ["_s);
        CommaPrinter separator(" "_s);
        for (size_t bit : m_solution.availableAtHead[block.index()]) {
            const Component& component = m_components[bit];
            out.print(separator);
            dumpID(out, component.guard);
            out.print(".", component.factIndex);
        }
        out.println("]");
    }
}

void BlockFrequencies::dump(PrintStream& out, const Function& function) const
{
    RELEASE_ASSERT(function.blocks.size() == m_blocks.size());
    for (size_t index = 0; index < m_blocks.size(); ++index) {
        if (m_cfg.isReachable(BlockID(index)))
            out.println("b", index, " frequency: ", nameOf(classOf(BlockID(index))), " count: ", WTF::format("%.6f", frequencyOf(BlockID(index))));
    }
    for (size_t index = 0; index < m_blocks.size(); ++index) {
        if (!m_cfg.isReachable(BlockID(index)))
            continue;
        forEachOutgoingEdge(function, BlockID(index), [&](Edge edge, const Target& target) {
            BlockFrequency frequency = classOf(edge);
            if (classForBlockOrder(edge) != frequency)
                dumpLine(out, edge, " off the usual path"_s);
            if (rarerOf(classOf(edge.from), classOf(target.block)) < frequency)
                dumpLine(out, edge, " frequency: "_s, nameOf(frequency));
        });
    }
}

static void dumpItem(PrintStream& out, const InductionStep& step)
{
    switch (step.kind) {
    case InductionStepKind::Unknown:
        out.print(enumName(step.kind));
        return;
    case InductionStepKind::Constant:
        out.print(step.constant < 0 ? "" : "+", step.constant);
        return;
    case InductionStepKind::Invariant:
        dumpItems(out, step.isSubtracted ? "-"_s : "+"_s, step.invariant);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void dumpInductionVariable(PrintStream& out, const InductionVariable& variable)
{
    dumpItems(out, variable.parameter, " start "_s, variable.start, " step "_s, variable.step, " "_s, enumName(variable.direction));
    dumpItems(out, " stepped "_s, variable.steppedValues.span(), " tests "_s);
    dumpList(out, variable.tests.span(), [&variable](PrintStream& out, const InductionTest& test) {
        dumpItems(out, test.block, " "_s, variable.parameter, " "_s, nameOf(test.relation), " "_s, test.bound, " "_s, enumName(test.boundKind), " "_s, enumName(test.tested));
        dumpItems(out, " entry "_s, enumName(test.entryRelation), " exits "_s, enumName(test.exits));
    });
}

void InductionVariables::dump(PrintStream& out) const
{
    for (const LoopInduction& loop : m_loops) {
        dumpLine(out, "loop "_s, loop.header, ":"_s);
        for (const InductionVariable& variable : loop.variables) {
            dumpInductionVariable(out, variable);
            out.print("\n");
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
