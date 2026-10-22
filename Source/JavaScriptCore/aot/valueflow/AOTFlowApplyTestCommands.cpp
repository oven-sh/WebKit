/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTIRPrinter.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowResults.h"
#include "AOTVerifier.h"

namespace JSC { namespace AOT { namespace Typed {

static FlowResults analyzeAndApply(TestCommandContext& context)
{
    FlowResults results = analyzeValueFlow(*context.program, context.program->types, context.build);
    applyFlowResults(*context.program, results, context.build);
    context.program->setForm(IRForm::Prepared);
    return results;
}

template<typename Record>
static void printRecordField(IRPrinter& printer, ASCIILiteral name, const Record& record)
{
    if (record == Record { })
        return;
    printer.beginField(name);
    printer.beginRecord();
    printFields(printer, record);
    printer.endRecord();
}

static bool shouldPrintPosition(const FlowPositionUpdate& update)
{
    return !update.isEmpty() || update.cause != UnprovenCause::Unknown;
}

static void printPosition(IRPrinter& printer, const DeclaredType& declared, const FlowPositionUpdate& update)
{
    printer.field("type"_s, declared.type);
    if (update.isProven)
        printer.out().print(" proven"_s);
    else if (declared.flags.contains(PositionFlags::IsExpected))
        printer.out().print(" expected"_s);
    if (update.cause != UnprovenCause::Unknown)
        printer.field("cause"_s, update.cause);
    if (hasFunction(update.witness)) {
        printer.beginField("witness"_s);
        printer.beginRecord();
        printFields(printer, update.witness);
        printer.endRecord();
    }
}

static void printLayoutLine(IRPrinter& printer, LayoutID id)
{
    const Layout& layout = printer.program().at(id);
    if (layout.flow == LayoutFlow { } && layout.classFlow == ClassFlow { })
        return;
    printer.beginLine("layout"_s);
    printer.out().print(" "_s);
    printer.print(id);
    printRecordField(printer, "flow"_s, layout.flow);
    printRecordField(printer, "classFlow"_s, layout.classFlow);
    printer.endLine();
}

static void printFieldLine(IRPrinter& printer, FieldReference reference, const FlowPositionUpdate& update)
{
    const Field& field = fieldAt(printer.program(), reference);
    printer.beginLine("field"_s);
    printer.out().print(" "_s);
    printer.print(reference.layout);
    printer.out().print("."_s, reference.field);
    if (field.key) {
        printer.out().print(" "_s);
        printer.print(field.key);
    }
    printPosition(printer, field.declared, update);
    if (!field.flags.isEmpty())
        printer.field("flags"_s, field.flags);
    if (field.originLayout)
        printer.field("originLayout"_s, field.originLayout);
    if (field.originField)
        printer.field("originField"_s, *field.originField);
    printRecordField(printer, "flow"_s, field.flow);
    printer.endLine();
}

static void printLayoutLines(IRPrinter& printer, const FlowApplication& application)
{
    const Program& program = printer.program();
    for (uint32_t layoutIndex = 0; layoutIndex < program.layouts.size(); ++layoutIndex) {
        LayoutID id(layoutIndex);
        printLayoutLine(printer, id);

        const Layout& layout = program.at(id);
        const Vector<FlowFieldUpdate>& updates = application.layouts[layoutIndex].fields;
        for (uint32_t fieldIndex = 0; fieldIndex < layout.fields.size(); ++fieldIndex) {
            const Field& field = layout.fields[fieldIndex];
            FlowPositionUpdate update = fieldIndex < updates.size() ? updates[fieldIndex].position : FlowPositionUpdate { };
            bool isNewEntry = field.flags.containsAny({ FieldFlags::IsInherited, FieldFlags::IsAddedLate });
            if (shouldPrintPosition(update) || isNewEntry || field.flow != FieldFlow { })
                printFieldLine(printer, { id, fieldIndex }, update);
        }
    }
}

static void printVariableLines(IRPrinter& printer, const FlowApplication& application)
{
    const Program& program = printer.program();
    for (uint32_t index = 0; index < application.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        const FlowPositionUpdate& update = application.variables[index].position;
        if (!shouldPrintPosition(update) && variable.flow == LocationFlow { })
            continue;
        printer.beginLine("variable"_s, IDPrefix::Variable, index);
        printPosition(printer, variable.declared, update);
        printRecordField(printer, "flow"_s, variable.flow);
        printer.endLine();
    }
}

static void printEntryParameterName(PrintStream& out, const Function& function, unsigned entryParameterIndex)
{
    static_assert(!static_cast<unsigned>(EntryParameter::Capture), "The captures are the first entry parameters.");
    switch (function.entryParameterKind(entryParameterIndex)) {
    case EntryParameter::Capture:
        out.print(" capture "_s, entryParameterIndex);
        return;
    case EntryParameter::This:
        out.print(" this"_s);
        return;
    case EntryParameter::Parameter:
        out.print(" parameter "_s, entryParameterIndex - function.firstParameterIndex());
        return;
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printSignatureLines(IRPrinter& printer, FunctionID id, const FlowSignatureUpdate& update)
{
    const Function& function = printer.program().at(id);
    for (unsigned index = 0; index < update.entryParameters.size(); ++index) {
        const DeclaredType* declared = function.declaredEntryParameter(index);
        if (!declared || !shouldPrintPosition(update.entryParameters[index]))
            continue;
        printer.beginLine("function"_s, IDPrefix::Function, id.index());
        printEntryParameterName(printer.out(), function, index);
        printPosition(printer, *declared, update.entryParameters[index]);
        printer.endLine();
    }
    for (unsigned index = 0; index < update.results.size(); ++index) {
        if (!shouldPrintPosition(update.results[index]))
            continue;
        printer.beginLine("function"_s, IDPrefix::Function, id.index());
        printer.out().print(" result "_s, index);
        printPosition(printer, function.results[index], update.results[index]);
        printer.endLine();
    }
    if (!update.provenCapabilities.isEmpty()) {
        printer.beginLine("function"_s, IDPrefix::Function, id.index());
        printer.field("capabilities"_s, function.capabilities);
        printer.endLine();
    }
}

static void printFunctionLines(IRPrinter& printer, const FlowApplication& application)
{
    for (uint32_t index = 0; index < application.signatures.size(); ++index)
        printSignatureLines(printer, FunctionID(index), application.signatures[index]);
}

static void printAppliedRecords(PrintStream& out, const Program& program, const FlowResults& results)
{
    const FlowApplication* application = FlowApplyAccess::applicationOf(results);
    if (!application)
        return;
    IRPrinter printer(out, program);
    printLayoutLines(printer, *application);
    printVariableLines(printer, *application);
    printFunctionLines(printer, *application);
}

void runFlowTestCommand(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        printBadArgument(context.out, context.arguments[0]);
        return;
    }
    FlowResults results = analyzeAndApply(context);
    const Program& program = *context.program;
    program.types.addTo(context.build.statistics());
    results.dump(context.out, program);
    printAppliedRecords(context.out, program, results);
}

void runFlowApplyTestCommand(TestCommandContext& context)
{
    FlowResults results = analyzeAndApply(context);
    const Program& program = *context.program;
    validateFlowResults(program, results, context.build);
    verifyProgram(program, IRForm::Prepared, context.build, VerifierInputs { .flow = &results, .isOptimizing = true });
    printProgram(context.out, program);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
