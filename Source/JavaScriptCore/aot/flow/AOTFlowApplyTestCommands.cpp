/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowAnalysis.h"
#include "AOTFlowApplication.h"
#include "AOTIRPrinter.h"
#include "AOTSupportTestCommands.h"
#include "AOTVerifier.h"

namespace JSC { namespace AOT { namespace Typed {

static FlowResults analyzeAndApply(TestCommandContext& context)
{
    FlowResults results = analyzeFlow(*context.program, context.program->types, context.build);
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

static void printPosition(IRPrinter& printer, const DeclaredType& declared)
{
    printer.field("type"_s, declared.type);
    if (declared.predicted)
        printer.field("predicted"_s, declared.predicted);
    if (declared.written)
        printer.field("written"_s, declared.written);
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

static void printFieldLine(IRPrinter& printer, FieldReference reference)
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
    printPosition(printer, field.declared);
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
            bool hasNewPosition = fieldIndex < updates.size() && !updates[fieldIndex].position.isEmpty();
            bool isNewEntry = field.flags.containsAny({ FieldFlags::IsInherited, FieldFlags::IsAppendedByFlow });
            if (hasNewPosition || isNewEntry || field.flow != FieldFlow { })
                printFieldLine(printer, { id, fieldIndex });
        }
    }
}

static void printVariableLines(IRPrinter& printer, const FlowApplication& application)
{
    const Program& program = printer.program();
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (application.variables[index].position.isEmpty() && variable.flow == LocationFlow { })
            continue;
        printer.beginLine("variable"_s, IDPrefix::Variable, index);
        printPosition(printer, variable.declared);
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
        if (!declared || update.entryParameters[index].isEmpty())
            continue;
        printer.beginLine("function"_s, IDPrefix::Function, id.index());
        printEntryParameterName(printer.out(), function, index);
        printPosition(printer, *declared);
        printer.endLine();
    }
    for (unsigned index = 0; index < update.results.size(); ++index) {
        if (update.results[index].isEmpty())
            continue;
        printer.beginLine("function"_s, IDPrefix::Function, id.index());
        printer.out().print(" result "_s, index);
        printPosition(printer, function.results[index]);
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

    for (FunctionID proven : application.versions.provenVersions) {
        FunctionID generic = application.versions.genericVersionOf(proven);
        printer.beginLine("function"_s, IDPrefix::Function, generic.index());
        printer.field("role"_s, printer.program().at(generic).role);
        printer.out().print(" of "_s);
        printer.print(proven);
        printer.endLine();
    }
}

static void printResultSiteLines(PrintStream& out, const FlowResults& results)
{
    for (uint32_t index = 0; index < results.numberOfAccessSites(); ++index) {
        if (AllocationSiteID site = results.resultSiteOf(AccessSiteID(index)))
            out.println("result-site S"_s, index, " A"_s, site.index());
    }
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
    printResultSiteLines(out, results);
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
