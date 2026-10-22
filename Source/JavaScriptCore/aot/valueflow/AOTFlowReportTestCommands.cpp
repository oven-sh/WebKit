/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowReport.h"
#include "AOTFlowResults.h"
#include "AOTIRPrinter.h"
#include "AOTPassID.h"
#include "AOTPipeline.h"
#include "AOTSupportTestCommands.h"
#include "AOTTestRunner.h"
#include <array>
#include <cinttypes>
#include <memory>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static void printBlank(IRPrinter& printer)
{
    printer.out().print(" "_s);
}

static void printWitness(IRPrinter& printer, const Witness& witness)
{
    printer.out().print(" witness "_s);
    printer.print(FunctionID(witness.function));
    if (!hasFunction(witness) || witness.value == Wire::none)
        return;
    printer.out().print(":"_s);
    printer.print(ValueID(witness.value));
}

static void printLocation(IRPrinter& printer, const SourceLocation& location)
{
    printBlank(printer);
    printer.print(ModuleID(location.module));
    printer.out().print(":"_s, location.position);
}

static void printOptionalBuiltin(IRPrinter& printer, uint32_t index)
{
    BuiltinID builtin(index);
    if (!builtin)
        return;
    printBlank(printer);
    printer.print(builtin);
}

static void printCounts(IRPrinter& printer, ASCIILiteral name, std::span<const uint32_t> counts)
{
    printer.beginField(name);
    printer.list(counts, [&](uint32_t count) {
        printer.print(count);
    });
}

static void printUnseenCode(IRPrinter& printer, const UnseenCodeRecord& record)
{
    printer.beginLine("unseen-code"_s);
    printBlank(printer);
    printer.print(record.cause);
    printOptionalBuiltin(printer, record.builtin);
    printWitness(printer, record.witness);
    printer.endLine();
}

static void printBuiltinUse(IRPrinter& printer, const BuiltinUseRecord& record)
{
    printer.beginLine("builtin-used"_s);
    printBlank(printer);
    printer.print(BuiltinID(record.builtin));
    printBlank(printer);
    printer.print(OptionSet<BuiltinUse>::fromRaw(static_cast<uint32_t>(record.uses)));
    printWitness(printer, record.witness);
    printer.endLine();
}

static void printFeatureUse(IRPrinter& printer, const FeatureUseRecord& record)
{
    printer.beginLine("feature-used"_s);
    printBlank(printer);
    printer.print(record.feature);
    printOptionalBuiltin(printer, record.builtin);
    printWitness(printer, record.witness);
    printer.endLine();
}

static void printLocaleUse(IRPrinter& printer, const LocaleUseRecord& record, std::span<const uint8_t> localeTags)
{
    printer.beginLine("locale-used"_s);
    printBlank(printer);
    printer.print(record.kind);
    if (record.kind == LocaleUseKind::Constant) {
        printBlank(printer);
        printer.print(localeTags.subspan(record.tag.offset, record.tag.length));
    }
    printOptionalBuiltin(printer, record.builtin);
    printWitness(printer, record.witness);
    printer.endLine();
}

static void printAccessCount(IRPrinter& printer, const AccessCountRecord& record)
{
    printer.beginLine("access-count"_s);
    printBlank(printer);
    printer.print(ModuleID(record.module));
    printCounts(printer, "accesses"_s, record.accesses);
    printCounts(printer, "accessCosts"_s, record.accessCosts);
    printCounts(printer, "calls"_s, record.calls);
    printer.field("typeTests"_s, record.typeTests);
    printer.endLine();
}

static void printAccess(IRPrinter& printer, const AccessRecord& record)
{
    printer.beginLine("access"_s);
    printLocation(printer, record.location);
    printBlank(printer);
    printer.print(record.kind);
    printBlank(printer);
    printer.print(record.level);
    printBlank(printer);
    printer.print(record.cost);
    if (record.cause != UnprovenCause::Unknown) {
        printer.field("cause"_s, record.cause);
        printWitness(printer, record.witness);
    }
    printer.field("copies"_s, record.copies);
    if (record.limit != LimitID::None)
        printer.field("limit"_s, record.limit);
    if (LayoutID layout { record.layout })
        printer.field("layout"_s, layout);
    if (FunctionID function { record.function })
        printer.field("function"_s, function);
    if (record.illTypedCause != IllTypedCause::None)
        printer.field("illTypedCause"_s, record.illTypedCause);
    printer.endLine();
}

static void printRemainingCheck(IRPrinter& printer, const RemainingCheckRecord& record)
{
    printer.beginLine("check"_s);
    printer.out().printf(" %012" PRIx64, record.id);
    printLocation(printer, record.origin);
    printer.field("cause"_s, record.cause);
    printer.field("site"_s, static_cast<TypeSiteKind>(record.siteKind));
    printWitness(printer, record.witness);
    printer.endLine();
}

static void printTypeTest(IRPrinter& printer, const TypeTestRecord& record)
{
    printer.beginLine("type-test"_s);
    printLocation(printer, record.location);
    printBlank(printer);
    printer.print(FunctionID(record.function));
    printBlank(printer);
    printer.print(record.failure);
    printer.field("copies"_s, record.copies);
    printer.endLine();
}

static void printFlowReport(PrintStream& out, const Program& program, const FlowReport& report)
{
    IRPrinter printer(out, program);
    for (const UnseenCodeRecord& record : report.unseenCode)
        printUnseenCode(printer, record);
    for (const BuiltinUseRecord& record : report.builtinsUsed)
        printBuiltinUse(printer, record);
    for (const FeatureUseRecord& record : report.featuresUsed)
        printFeatureUse(printer, record);
    for (const LocaleUseRecord& record : report.localesUsed)
        printLocaleUse(printer, record, report.localeTags.span());
    for (const AccessCountRecord& record : report.accessCounts)
        printAccessCount(printer, record);
    for (const AccessRecord& record : report.accesses)
        printAccess(printer, record);
    for (const RemainingCheckRecord& record : report.remainingChecks)
        printRemainingCheck(printer, record);
    for (const TypeTestRecord& record : report.typeTests)
        printTypeTest(printer, record);
}

static std::optional<PassID> parseLastPass(TestCommandContext& context)
{
    constexpr ASCIILiteral prefix = "through="_s;
    if (context.arguments.empty())
        return PassID::ApplyFlowResults;
    if (context.arguments.size() > 1) {
        printBadArgument(context.out, context.arguments[1]);
        return std::nullopt;
    }

    StringView word = context.arguments[0];
    std::optional<PassID> last = word.startsWith(prefix) ? passNamed(word.substring(prefix.length())) : std::nullopt;
    if (!last)
        printBadArgument(context.out, word);
    return last;
}

void runFlowReportTestCommand(TestCommandContext& context)
{
    std::optional<PassID> last = parseLastPass(context);
    if (!last)
        return;
    if (context.form != IRForm::AsRead) {
        context.out.println("error: flow-report starts from AsRead"_s);
        return;
    }

    std::unique_ptr<PipelineOutput> output = runPipelineUntil(*last, WTF::move(context.program), context.build);
    if (!output) {
        reopenDiagnostics(context.build);
        return;
    }

    if (output->flow)
        printFlowReport(context.out, *output->program, buildFlowReport(*output->program, *output->flow, gradeAccessSites(*output->flow, WTF::move(output->accessSiteObservations), context.build).span(), output->remainingChecks.span(), collectFlowReportBodyRecords(*output->program, context.build), context.build));
    else
        context.out.println("no results"_s);

    context.program = WTF::move(output->program);
    context.form = context.program->form();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
