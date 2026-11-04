/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRPrinter.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTTypeArena.h"
#include "AOTWalkJSONWritePlan.h"
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

static ASCIILiteral nameOfRowInJSONPlanText(Opcode row)
{
    switch (row) {
    case Opcode::JSONAppendBoolean:
        return "JSONAppendBoolean"_s;
    case Opcode::JSONAppendInteger:
        return "JSONAppendInteger"_s;
    case Opcode::JSONAppendNumber:
        return "JSONAppendNumber"_s;
    case Opcode::JSONAppendString:
        return "JSONAppendString"_s;
    case Opcode::JSONAppendDate:
        return "JSONAppendDate"_s;
    default:
        return "JSONAppendValue"_s;
    }
}

//     Scalar JSONAppendInteger
//     Array of string nullish
//     Objects [L0, L1] expected
//     Parts (Scalar JSONAppendDate, Objects [L0])
static void printJSONValuePlan(IRPrinter& printer, const JSONValuePlan& plan)
{
    PrintStream& out = printer.out();
    switch (plan.kind) {
    case JSONValueKind::Generic:
        out.print("Generic"_s);
        break;
    case JSONValueKind::Nullish:
        out.print("Nullish"_s);
        break;
    case JSONValueKind::Scalar:
        out.print("Scalar "_s, nameOfRowInJSONPlanText(plan.row));
        break;
    case JSONValueKind::Array:
        out.print("Array of "_s);
        printer.print(plan.element);
        break;
    case JSONValueKind::Objects: {
        out.print("Objects ["_s);
        CommaPrinter comma;
        for (LayoutID layout : plan.layouts) {
            out.print(comma);
            printer.print(layout);
        }
        out.print("]"_s);
        break;
    }
    case JSONValueKind::ObjectParts: {
        out.print("Parts ("_s);
        CommaPrinter comma;
        for (const JSONValuePlan& part : plan.parts) {
            out.print(comma);
            printJSONValuePlan(printer, part);
        }
        out.print(")"_s);
        break;
    }
    }
    if (plan.isExpected)
        out.print(" expected"_s);
    if (plan.mayBeNullish)
        out.print(" nullish"_s);
}

static void printLooksOfJSONObjectPlan(IRPrinter& printer, const JSONObjectPlan& plan)
{
    struct NamedFact {
        FactOfCell fact;
        ASCIILiteral name;
    };
    static constexpr NamedFact facts[] {
        { FactOfCell::NotFrozen, "NotFrozen"_s },
        { FactOfCell::NoFieldOutsideSlot, "NoFieldOutsideSlot"_s },
        { FactOfCell::HasLayoutPrototype, "HasLayoutPrototype"_s },
        { FactOfCell::OrderWordIsKnown, "OrderWordIsKnown"_s },
        { FactOfCell::IsLikelyAsAllocated, "IsLikelyAsAllocated"_s },
        { FactOfCell::IsLikelyAsConstructed, "IsLikelyAsConstructed"_s },
    };
    PrintStream& out = printer.out();
    out.print(" looks=["_s);
    CommaPrinter commaOfLooks;
    for (const NamedFact& named : facts) {
        if (plan.looksAtCell.contains(named.fact))
            out.print(commaOfLooks, named.name);
    }
    out.print("] guards=["_s);
    CommaPrinter commaOfGuards;
    for (LayoutID guarded : plan.classesGuardedAgainstToJSON) {
        out.print(commaOfGuards);
        printer.print(guarded);
    }
    out.print("]"_s);
}

//     object L0 looks=[] guards=[] members-written-out=2 whole
//       order "id" "name"
//         "id": Scalar JSONAppendInteger
//         "name": Scalar JSONAppendString
//     code L0 runs-no-code cannot-throw
// `object L1 has no plan`. `whole`: writesWholeJSONValue() of a value that is proven to be an object of the layout, and ` calls-serializers` behind it. `code`: codeRunByJSONStringifyOf() of the same.
static void printJSONPlanOfLayout(IRPrinter& printer, const AccessContext& context, LayoutID layout)
{
    PrintStream& out = printer.out();
    TypeID objects = context.types.forLayout(layout);
    out.print("object "_s);
    printer.print(layout);
    std::optional<JSONObjectPlan> plan = planJSONObject(context, layout);
    if (!plan)
        out.println(" has no plan"_s);
    else {
        printLooksOfJSONObjectPlan(printer, *plan);
        out.print(" members-written-out="_s, numberOfJSONMembersWrittenOutFor(context, *plan));
        bool callsSerializers = false;
        if (writesWholeJSONValue(context, planJSONValue(context, objects, { }), objects, callsSerializers))
            out.print(callsSerializers ? " whole calls-serializers"_s : " whole"_s);
        out.println();
        for (const JSONKeyOrderPlan& order : plan->orders) {
            out.print("  order"_s);
            for (uint32_t field : order.created) {
                out.print(" "_s);
                if (PropertyKeyID key = context.plan.slots().keyOf(context.program, { layout, field }))
                    printer.print(key);
                else
                    out.print(field);
            }
            out.println();
            for (const JSONMemberPlan& member : order.members) {
                out.print("    "_s);
                printer.print(member.key);
                out.print(": "_s);
                TypeID proven = provenTypeOfJSONMember(context, layout, member.field);
                if (hasNoJSONText(context, proven))
                    out.print("no text"_s);
                else
                    printJSONValuePlan(printer, planJSONValue(context, proven, writtenTypeOfJSONMember(context, layout, member.field)));
                out.println();
            }
        }
    }

    CodeRunByJSONStringify code = codeRunByJSONStringifyOf(context, objects);
    out.print("code "_s);
    printer.print(layout);
    out.println(!code.runsNoCode ? " may-run-code"_s : code.cannotThrow ? " runs-no-code cannot-throw"_s : " runs-no-code may-throw"_s);
}

// The first line is `mechanisms <list>`. Then each record and each instance layout in use, in the order of the layouts.
static void runJSONPlanCommand(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        context.out.println("bad argument "_s, context.arguments[0]);
        return;
    }
    StagedBuild stage = context.stage();
    if (!stage.prepared())
        return;
    const Facts* facts = stage.facts();
    const Plan* plan = stage.plan();
    if (!facts || !plan)
        return;

    context.out.print("mechanisms "_s);
    printMechanisms(context.out, stage.mechanisms());
    context.out.println();
    const Program& program = stage.program();
    IRPrinter printer(context.out, program, stage.types());
    AccessContext access { program, WalkFacts(*facts), *plan, stage.types(), FunctionID() };
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutKind kind = program.layouts[index].kind;
        if ((kind == LayoutKind::Record || kind == LayoutKind::Instance) && plan->isInUse(LayoutID(index)))
            printJSONPlanOfLayout(printer, access, LayoutID(index));
    }
}

std::span<const TestCommand> jsonPlanTestCommands()
{
    static constexpr TestCommand commands[] {
        { "json-plan"_s, runJSONPlanCommand, ProgramOfCommand::AsWritten, ""_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
