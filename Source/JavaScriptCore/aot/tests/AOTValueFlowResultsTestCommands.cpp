/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTIRPrinter.h"
#include "AOTSort.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowResults.h"
#include "AOTVerifier.h"
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static bool refusesArguments(TestCommandContext& context)
{
    if (context.arguments.empty())
        return false;
    printBadArgument(context.out, context.arguments[0]);
    return true;
}

static FlowResults analyzeAndApply(TestCommandContext& context, const ListenerForSetsOutsideTypes* setsOutsideTypes = nullptr)
{
    FlowResults results = analyzeValueFlow(*context.program, context.program->types, context.build, setsOutsideTypes);
    applyFlowResults(*context.program, results, context.build);
    context.program->setForm(IRForm::Prepared);
    return results;
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

static void printAppliedRecords(PrintStream& out, const Program& program, const FlowResults& results)
{
    const FlowApplication* application = FlowApplyAccess::applicationOf(results);
    if (!application)
        return;
    IRPrinter printer(out, program);
    auto printRecord = [&](ASCIILiteral name, const auto& record) {
        printer.beginField(name);
        printer.beginRecord();
        printFields(printer, record);
        printer.endRecord();
    };
    auto printFlow = [&](ASCIILiteral name, const auto& record) {
        if (record != std::decay_t<decltype(record)> { })
            printRecord(name, record);
    };
    auto shouldPrint = [](const FlowPositionUpdate& update) {
        return !update.isEmpty() || update.cause != UnprovenCause::Unknown;
    };
    auto printPosition = [&](const DeclaredType& declared, const FlowPositionUpdate& update) {
        printer.field("type"_s, declared.type);
        if (update.isProven)
            out.print(" proven"_s);
        else if (declared.flags.contains(PositionFlags::IsExpected))
            out.print(" expected"_s);
        if (update.cause != UnprovenCause::Unknown)
            printer.field("cause"_s, update.cause);
        if (hasFunction(update.witness))
            printRecord("witness"_s, update.witness);
    };

    for (uint32_t layoutIndex = 0; layoutIndex < program.layouts.size(); ++layoutIndex) {
        LayoutID id(layoutIndex);
        const Layout& layout = program.at(id);
        if (layout.flow != LayoutFlow { } || layout.classFlow != ClassFlow { }) {
            printer.beginLine("layout"_s);
            out.print(" "_s);
            printer.print(id);
            printFlow("flow"_s, layout.flow);
            printFlow("classFlow"_s, layout.classFlow);
            printer.endLine();
        }
        const Vector<FlowFieldUpdate>& updates = application->layouts[layoutIndex].fields;
        for (uint32_t fieldIndex = 0; fieldIndex < layout.fields.size(); ++fieldIndex) {
            const Field& field = layout.fields[fieldIndex];
            FlowPositionUpdate update = fieldIndex < updates.size() ? updates[fieldIndex].position : FlowPositionUpdate { };
            if (!shouldPrint(update) && !field.flags.containsAny({ FieldFlags::IsInherited, FieldFlags::IsAddedLate }) && field.flow == FieldFlow { })
                continue;
            printer.beginLine("field"_s);
            out.print(" "_s);
            printer.print(id);
            out.print("."_s, fieldIndex);
            if (field.key) {
                out.print(" "_s);
                printer.print(field.key);
            }
            printPosition(field.declared, update);
            if (!field.flags.isEmpty())
                printer.field("flags"_s, field.flags);
            if (field.originLayout)
                printer.field("originLayout"_s, field.originLayout);
            if (field.originField)
                printer.field("originField"_s, *field.originField);
            printFlow("flow"_s, field.flow);
            printer.endLine();
        }
    }

    for (uint32_t index = 0; index < application->variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        const FlowPositionUpdate& update = application->variables[index].position;
        if (!shouldPrint(update) && variable.flow == LocationFlow { })
            continue;
        printer.beginLine("variable"_s, IDPrefix::Variable, index);
        printPosition(variable.declared, update);
        printFlow("flow"_s, variable.flow);
        printer.endLine();
    }

    for (uint32_t functionIndex = 0; functionIndex < application->signatures.size(); ++functionIndex) {
        const Function& function = program.functions[functionIndex];
        const FlowSignatureUpdate& update = application->signatures[functionIndex];
        for (unsigned index = 0; index < update.entryParameters.size(); ++index) {
            const DeclaredType* declared = function.declaredEntryParameter(index);
            if (!declared || !shouldPrint(update.entryParameters[index]))
                continue;
            printer.beginLine("function"_s, IDPrefix::Function, functionIndex);
            printEntryParameterName(out, function, index);
            printPosition(*declared, update.entryParameters[index]);
            printer.endLine();
        }
        for (unsigned index = 0; index < update.results.size(); ++index) {
            if (!shouldPrint(update.results[index]))
                continue;
            printer.beginLine("function"_s, IDPrefix::Function, functionIndex);
            out.print(" result "_s, index);
            printPosition(function.results[index], update.results[index]);
            printer.endLine();
        }
        if (!update.provenCapabilities.isEmpty()) {
            printer.beginLine("function"_s, IDPrefix::Function, functionIndex);
            printer.field("capabilities"_s, function.capabilities);
            printer.endLine();
        }
    }
}

void runFlowTestCommand(TestCommandContext& context)
{
    if (refusesArguments(context))
        return;
    FlowResults results = analyzeAndApply(context);
    const Program& program = *context.program;
    program.types.addTo(context.build.statistics());
    results.dump(context.out, program);
    printAppliedRecords(context.out, program, results);
}

void runValueFlowResultsTestCommand(TestCommandContext& context)
{
    if (refusesArguments(context))
        return;
    prepareForFlow(context);
    Vector<String> setsOutsideTypes;
    auto describe = [&](const String& place, const Type& held, const Type& declared) {
        StringPrintStream line;
        line.print(place, " holds "_s);
        printType(line, *context.program, held);
        line.print(", declared "_s);
        printType(line, *context.program, declared);
        setsOutsideTypes.append(line.toString());
    };
    ListenerForSetsOutsideTypes listener(describe);
    analyzeAndApply(context, &listener).dump(context.out, *context.program);
    AOT::sort(setsOutsideTypes, codePointCompareLessThan);
    for (const String& line : setsOutsideTypes)
        context.out.println("outside-type "_s, line);
}

void runValueFlowApplyTestCommand(TestCommandContext& context)
{
    if (refusesArguments(context))
        return;
    prepareForFlow(context);
    FlowResults results = analyzeAndApply(context);
    const Program& program = *context.program;
    verifyProgram(program, IRForm::Prepared, context.build, VerifierInputs { .flow = &results, .isOptimizing = true });
    printProgram(context.out, program);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
