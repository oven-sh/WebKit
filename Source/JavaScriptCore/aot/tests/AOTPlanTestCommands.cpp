/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTIRPrinter.h"
#include "AOTPlan.h"
#include <bit>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

static ASCIILiteral nameInPlanText(LayoutStorage storage)
{
    switch (storage) {
    case LayoutStorage::None:
        return "None"_s;
    case LayoutStorage::CompiledObject:
        return "CompiledObject"_s;
    case LayoutStorage::OrdinaryObject:
        return "OrdinaryObject"_s;
    case LayoutStorage::Environment:
        return "Environment"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInPlanText(SlotStorage storage)
{
    switch (storage) {
    case SlotStorage::None:
        return "None"_s;
    case SlotStorage::Inline:
        return "Inline"_s;
    case SlotStorage::OutOfLine:
        return "OutOfLine"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInPlanText(VariableStorage storage)
{
    switch (storage) {
    case VariableStorage::Statics:
        return "Statics"_s;
    case VariableStorage::ClosureTable:
        return "ClosureTable"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInPlanText(VariableEncoding encoding)
{
    switch (encoding) {
    case VariableEncoding::Plain:
        return "Plain"_s;
    case VariableEncoding::FunctionAsZero:
        return "FunctionAsZero"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInPlanText(ModuleStartOperation::Kind kind)
{
    switch (kind) {
    case ModuleStartOperation::Kind::CallStart:
        return "CallStart"_s;
    case ModuleStartOperation::Kind::CallBody:
        return "CallBody"_s;
    case ModuleStartOperation::Kind::Begin:
        return "Begin"_s;
    case ModuleStartOperation::Kind::BeginOrReturn:
        return "BeginOrReturn"_s;
    case ModuleStartOperation::Kind::Finish:
        return "Finish"_s;
    case ModuleStartOperation::Kind::ReturnIfLoadingStops:
        return "ReturnIfLoadingStops"_s;
    case ModuleStartOperation::Kind::AbandonOnThrow:
        return "AbandonOnThrow"_s;
    case ModuleStartOperation::Kind::NeverComesBack:
        return "NeverComesBack"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printPlacedSlot(IRPrinter& printer, const PlacedSlot& slot)
{
    PrintStream& out = printer.out();
    out.print(" "_s, nameOf(slot.representation), " "_s, nameInPlanText(slot.storage));
    if (slot.hasStorage())
        out.print(" offset="_s, slot.offset);
    if (slot.indexInLayoutRecord)
        out.print(" record="_s, *slot.indexInLayoutRecord);
    if (slot.type) {
        out.print(" type="_s);
        printer.print(slot.type);
    }
    if (slot.provenType) {
        out.print(" doors="_s);
        printer.print(slot.provenType);
    }
    if (slot.typeOfRawValue) {
        out.print(" raw="_s);
        printer.print(slot.typeOfRawValue);
    }
    if (slot.holdsAccessor)
        out.print(" accessor"_s);
    if (slot.isFilledAtBirth)
        out.print(" filled-at-birth"_s);
    if (slot.mayExistWithoutStorage)
        out.print(" may-exist-without-storage"_s);
    out.println();
}

static void printPlannedLayouts(IRPrinter& printer, const Plan& plan)
{
    PrintStream& out = printer.out();
    const Program& program = printer.program();
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const PlannedLayout& layout = plan.layout(id);
        out.print("layout "_s);
        printer.print(id);
        out.print(" "_s, nameInPlanText(layout.storage));
        if (layout.isRecordAlone)
            out.print(" record-alone"_s);
        if (!plan.isInUse(id))
            out.print(" never-allocated"_s);
        if (layout.hasSlots())
            out.print(" size="_s, layout.size);
        if (layout.cellsAreHandedBack)
            out.print(" handed-back link-at="_s, layout.offsetOfLinkToNextCellHandedBack);
        if (layout.extensionSize)
            out.print(" extension="_s, layout.extensionSize);
        if (ShapeRange shapes = plan.shapes().shapesOf(id); !shapes.isEmpty())
            out.print(" shapes="_s, shapes.first, ".."_s, shapes.end() - 1);
        if (uint32_t numberOfOrders = plan.keyOrders().numberOfOrdersOf(id))
            out.print(" orders="_s, numberOfOrders);
        if (layout.granulesOfFirstExtension)
            out.print(" first-extension="_s, layout.granulesOfFirstExtension);
        out.println();

        for (FieldIndex field = 0; field < layout.numberOfFields(); ++field) {
            out.print("  slot "_s, field);
            if (PropertyKeyID key = plan.slots().keyOf(program, { id, field })) {
                out.print(" "_s);
                printer.print(key);
            }
            if (field >= program.at(id).fields.size())
                out.print(" appended"_s);
            printPlacedSlot(printer, layout.slots[field]);
        }
        if (layout.hasExtension()) {
            out.print("  extension"_s);
            printPlacedSlot(printer, layout.slots[layout.extensionWordIndex()]);
        }
        if (layout.hasExtraPropertiesWord()) {
            out.print("  extra-properties"_s);
            printPlacedSlot(printer, layout.slots[layout.extraPropertiesWordIndex()]);
        }
    }
}

static void printPlannedKeyOrders(IRPrinter& printer, const Plan& plan)
{
    PrintStream& out = printer.out();
    const Program& program = printer.program();
    const KeyOrders& orders = plan.keyOrders();
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (!orders.numberOfOrdersOf(id))
            continue;
        out.print("orders "_s);
        printer.print(id);
        out.println();
        for (uint32_t number = 0; number < orders.numberOfOrdersOf(id); ++number) {
            out.print("  order "_s, number, ":"_s);
            std::span<const FieldIndex> fields = orders.order(id, number);
            std::span<const KeyOrderSource> sources = orders.sourcesOfFields(id, number);
            for (size_t place = 0; place < fields.size(); ++place) {
                if (sources[place] == KeyOrderSource::StoresInSequence && (!place || sources[place - 1] != KeyOrderSource::StoresInSequence))
                    out.print(" |"_s);
                out.print(" "_s);
                if (PropertyKeyID key = plan.slots().keyOf(program, { id, fields[place] }))
                    printer.print(key);
                else
                    out.print(fields[place]);
            }
            out.println();
        }
    }
}

static void printPlannedVariables(IRPrinter& printer, const Plan& plan)
{
    PrintStream& out = printer.out();
    for (uint32_t index = 0; index < plan.slots().numberOfVariables(); ++index) {
        PlannedVariable variable = plan.variable(VariableID(index));
        out.print("variable "_s);
        printer.print(VariableID(index));
        out.print(" "_s, nameInPlanText(variable.storage), " "_s, nameOf(variable.representation), " "_s, nameInPlanText(variable.encoding));
        if (variable.function) {
            out.print(" function="_s);
            printer.print(variable.function);
        }
        out.println();
    }
}

static void printDeferredVariables(IRPrinter& printer, const Plan& plan)
{
    PrintStream& out = printer.out();
    for (const DeferredVariable& deferred : plan.deferredDefinitions().variables) {
        out.print("deferred "_s);
        printer.print(deferred.variable);
        out.print(" made-by "_s);
        printer.print(deferred.body);
        out.print(" "_s);
        printer.print(deferred.made);
        out.print(" "_s, nameOf(deferred.opcode));
        out.print(" mark="_s, deferred.mark == Primitive::Absent ? "absent"_s : deferred.mark == Primitive::Undefined ? "undefined"_s : "null"_s);
        out.println(deferred.isMarkedAtDeclaration ? " at-declaration"_s : " from-birth"_s);
    }
}

static void printPlannedShapes(IRPrinter& printer, const Plan& plan)
{
    PrintStream& out = printer.out();
    const ShapeTable& shapes = plan.shapes();
    out.println("shapes "_s, shapes.numberOfShapes(), " first-environment="_s, shapes.firstEnvironmentShape());
    for (uint32_t shape = 1; shape <= shapes.numberOfShapes(); ++shape) {
        if (!shapes.isAssigned(shape))
            continue;
        out.print("  shape "_s, shape, " "_s);
        printer.print(shapes.shape(shape).layout);
        out.println();
    }
}

static std::optional<unsigned> numberOfParametersInPlanText(const CallingConventions& conventions, CodeUnit unit)
{
    switch (unit.role) {
    case CodeRole::Body:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
        return conventions.of(unit.function).numberOfEntryParameters;
    case CodeRole::CoroutineBody:
        return conventions.of(unit.function).numberOfEntryParameters + 1;
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
        return std::nullopt;
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        return 1 + conventions.methodSelector(unit.placeOfSelector()).numberOfArguments;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printPlannedUnits(IRPrinter& printer, const Plan& plan)
{
    PrintStream& out = printer.out();
    const CallingConventions& conventions = plan.conventions();
    for (uint32_t index = 0; index < conventions.numberOfUnits(); ++index) {
        CodeUnit unit = conventions.unitAt(index);
        if (!conventions.exists(unit))
            continue;
        out.print("unit "_s);
        printCodeUnit(out, unit);
        if (isGenericEntry(unit.role)) {
            out.print(" -> "_s);
            printCodeUnit(out, conventions.genericEntry(unit).target);
            if (uint8_t onlyCalled = conventions.genericEntry(unit).borrowedArguments)
                out.print(" only-calls="_s, onlyCalled & 1 ? "0"_s : ""_s, onlyCalled & 2 ? "1"_s : ""_s);
            out.println();
            continue;
        }
        if (std::optional<unsigned> numberOfParameters = numberOfParametersInPlanText(conventions, unit)) {
            out.print(" ("_s);
            CommaPrinter comma;
            for (unsigned parameter = 0; parameter < *numberOfParameters; ++parameter)
                out.print(comma, nameOf(conventions.parameterRepresentation(printer.program(), unit, parameter)));
            out.print(") -> ("_s);
            CommaPrinter commaOfResults;
            for (unsigned result = 0; result < conventions.numberOfResults(unit); ++result)
                out.print(commaOfResults, nameOf(conventions.resultRepresentation(unit, result)));
            out.print(")"_s);
            if (walksInputBody(unit.role) && conventions.stackCheckOf(unit) == StackCheck::CoveredByCallers)
                out.print(" covered-by-callers"_s);
            if (conventions.savesRegistersOfCallers(unit))
                out.print(" saves-registers-of-callers"_s);
            if (uint32_t reserved = conventions.stackWordsReservedByCallersOf(printer.program(), unit))
                out.print(" reserves="_s, reserved);
            if (unit.role == CodeRole::Body && plan.endsItsTailCalls(unit.function))
                out.print(" ends-its-tail-calls"_s);
            if (uint64_t mustEndTheirs = unit.role == CodeRole::Body ? plan.entryParametersThatMustEndTheirTailCalls(unit.function) : 0) {
                out.print(" ends-tail-calls-if-arguments-do=["_s);
                CommaPrinter commaOfParameters;
                for (; mustEndTheirs; mustEndTheirs &= mustEndTheirs - 1)
                    out.print(commaOfParameters, std::countr_zero(mustEndTheirs));
                out.print("]"_s);
            }
            if (unit.role == CodeRole::CoroutineBody && conventions.of(unit.function).coroutineNeverSuspends)
                out.print(" never-suspends"_s);
            if (unit.role == CodeRole::ParameterGuard) {
                out.print(" tests ("_s);
                CommaPrinter commaOfTests;
                for (unsigned parameter = 0; parameter < *numberOfParameters; ++parameter) {
                    out.print(commaOfTests);
                    if (TypeID tested = conventions.typeBehindGuard(unit.function, parameter))
                        printer.print(tested);
                    else
                        out.print("-"_s);
                }
                out.print(")"_s);
            }
        }
        out.println();
    }
}

static void printPlannedModuleStarts(IRPrinter& printer, const Plan& plan)
{
    PrintStream& out = printer.out();
    const ModuleEvaluationPlan& evaluation = plan.moduleEvaluation();
    for (const ModuleStart& start : evaluation.starts()) {
        out.print("module-start "_s);
        printer.print(start.module);
        out.print(" body="_s);
        printer.print(start.body);
        CommaPrinter comma(", "_s, ": "_s);
        for (const ModuleStartOperation& operation : evaluation.operationsOf(start)) {
            out.print(comma, nameInPlanText(operation.kind));
            if (operation.callee) {
                out.print(" "_s);
                printer.print(operation.callee);
            }
            if (operation.module) {
                out.print(" "_s);
                printer.print(operation.module);
            }
        }
        out.println();
    }
    for (const BareModuleSite& site : evaluation.bareSites()) {
        out.print("bare-site "_s);
        printer.print(site.function);
        out.print(" v"_s, site.value.index());
        CommaPrinter comma(", "_s, ": "_s);
        for (const ModuleStep& step : evaluation.stepsOf(site)) {
            out.print(comma, step.callee ? (step.calleeIsStart ? "CallStart "_s : "CallBody "_s) : step.moduleThatBegins ? "Begin "_s : "HasRun "_s);
            if (step.callee)
                printer.print(step.callee);
            else
                printer.print(step.moduleThatBegins ? step.moduleThatBegins : step.moduleThatHasRun);
        }
        out.println();
    }
    for (uint32_t index = 0; index < printer.program().modules.size(); ++index) {
        ModuleID module(index);
        bool isCalled = evaluation.isCalledWhereRequired(module);
        bool isAlone = evaluation.isEvaluatedByCodeAlone(module);
        bool recordsNothing = evaluation.canSkipRecordRequire(module);
        if (!isCalled && !isAlone && !evaluation.startsItself(module) && !recordsNothing)
            continue;
        out.print("module "_s);
        printer.print(module);
        out.println(isCalled ? " called-where-required"_s : ""_s, evaluation.startsItself(module) ? " starts-itself"_s : ""_s, isAlone ? " evaluated-by-code-alone"_s : ""_s, recordsNothing ? " records-nothing"_s : ""_s);
    }
}

static void printPlannedClasses(IRPrinter& printer, const Plan& plan)
{
    PrintStream& out = printer.out();
    for (uint32_t index = 0; index < plan.slots().numberOfLayouts(); ++index) {
        const ClassCreationPlan& creation = plan.slots().classCreation(LayoutID(index));
        bool saysSomething = creation.createsMembersLazily || creation.createsPrototypeBeforeClassObject || creation.createsClassObjectOnMutation || creation.guardsHoldWhileClear || creation.guardsOfPrototypeSideHoldWhileClear;
        if (!saysSomething && !creation.members.contains(MemberCreation::ByClassRecord))
            continue;
        out.print("class "_s);
        printer.print(LayoutID(index));
        if (creation.createsMembersLazily)
            out.print(" lazy-members"_s);
        if (creation.createsPrototypeBeforeClassObject)
            out.print(" prototype-first"_s);
        if (creation.createsClassObjectOnMutation)
            out.print(" class-object-on-mutation"_s);
        if (creation.guardsHoldWhileClear)
            out.print(" guards-hold-while-clear"_s);
        out.print(" members=["_s);
        CommaPrinter comma;
        for (MemberCreation member : creation.members)
            out.print(comma, member == MemberCreation::ByClassRecord ? "ByClassRecord"_s : "AtDefinition"_s);
        out.println("]"_s);
    }
}

struct NamedPartOfPlan {
    ASCIILiteral name;
    void (*print)(IRPrinter&, const Plan&);
};

static constexpr NamedPartOfPlan namedPartsOfPlan[] {
    { "layouts"_s, printPlannedLayouts },
    { "variables"_s, printPlannedVariables },
    { "deferred"_s, printDeferredVariables },
    { "shapes"_s, printPlannedShapes },
    { "units"_s, printPlannedUnits },
    { "module-starts"_s, printPlannedModuleStarts },
    { "orders"_s, printPlannedKeyOrders },
    { "classes"_s, printPlannedClasses },
};

static void runPlanCommand(TestCommandContext& context)
{
    Vector<const NamedPartOfPlan*, 8> parts;
    for (const String& word : context.arguments) {
        size_t numberOfPartsBefore = parts.size();
        for (const NamedPartOfPlan& named : namedPartsOfPlan) {
            if (word == named.name)
                parts.append(&named);
        }
        if (parts.size() == numberOfPartsBefore) {
            context.out.println("bad argument "_s, word);
            return;
        }
    }
    if (parts.isEmpty()) {
        for (const NamedPartOfPlan& named : namedPartsOfPlan)
            parts.append(&named);
    }

    StagedBuild stage = context.stage();
    const Plan* plan = stage.plan();
    if (!plan)
        return;

    context.out.print("mechanisms "_s);
    printMechanisms(context.out, stage.mechanisms());
    context.out.println();
    IRPrinter printer(context.out, stage.program());
    for (const NamedPartOfPlan* part : parts)
        part->print(printer, *plan);
}

std::span<const TestCommand> planTestCommands()
{
    static constexpr TestCommand commands[] {
        { "plan"_s, runPlanCommand, ProgramOfCommand::AsWritten, "[layouts|variables|deferred|shapes|units|module-starts|orders|classes].."_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
