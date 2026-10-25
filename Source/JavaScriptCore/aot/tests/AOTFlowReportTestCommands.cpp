/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowReport.h"
#include "AOTFlowResults.h"
#include "AOTIRPrinter.h"
#include "AOTPassID.h"
#include "AOTPipeline.h"
#include "AOTSupportTestCommands.h"
#include "AOTTestRunner.h"
#include <cinttypes>

namespace JSC { namespace AOT { namespace Typed {

static void printFlowReport(PrintStream& out, const Program& program, const FlowReport& report)
{
    IRPrinter printer(out, program);
    auto print = [&](const auto& value) {
        out.print(" "_s);
        printer.print(value);
    };
    auto printWitness = [&](const Witness& witness) {
        out.print(" witness "_s);
        printer.print(FunctionID(witness.function));
        if (!hasFunction(witness) || witness.value == Wire::none)
            return;
        out.print(":"_s);
        printer.print(ValueID(witness.value));
    };
    auto printLocation = [&](const SourceLocation& location) {
        print(ModuleID(location.module));
        out.print(":"_s, location.position);
    };
    auto printOptionalBuiltin = [&](uint32_t index) {
        if (BuiltinID builtin { index })
            print(builtin);
    };
    auto printCounts = [&](ASCIILiteral name, std::span<const uint32_t> counts) {
        printer.beginField(name);
        printer.list(counts, [&](uint32_t count) {
            printer.print(count);
        });
    };

    for (const UnseenCodeRecord& record : report.unseenCode) {
        printer.beginLine("unseen-code"_s);
        print(record.cause);
        printOptionalBuiltin(record.builtin);
        printWitness(record.witness);
        printer.endLine();
    }
    for (const BuiltinUseRecord& record : report.builtinsUsed) {
        printer.beginLine("builtin-used"_s);
        print(BuiltinID(record.builtin));
        print(OptionSet<BuiltinUse>::fromRaw(static_cast<uint32_t>(record.uses)));
        printWitness(record.witness);
        printer.endLine();
    }
    for (const FeatureUseRecord& record : report.featuresUsed) {
        printer.beginLine("feature-used"_s);
        print(record.feature);
        printOptionalBuiltin(record.builtin);
        if (record.kind != FeatureUseKind::Named)
            print(record.kind);
        printWitness(record.witness);
        printer.endLine();
    }
    for (const LocaleUseRecord& record : report.localesUsed) {
        printer.beginLine("locale-used"_s);
        print(record.kind);
        if (record.kind == LocaleUseKind::Constant)
            print(report.localeTags.span().subspan(record.tag.offset, record.tag.length));
        printOptionalBuiltin(record.builtin);
        printWitness(record.witness);
        printer.endLine();
    }
    for (const AccessCountRecord& record : report.accessCounts) {
        printer.beginLine("access-count"_s);
        print(ModuleID(record.module));
        printCounts("accesses"_s, record.accesses);
        printCounts("accessCosts"_s, record.accessCosts);
        printCounts("calls"_s, record.calls);
        printer.field("typeTests"_s, record.typeTests);
        printer.endLine();
    }
    for (const AccessRecord& record : report.accesses) {
        printer.beginLine("access"_s);
        printLocation(record.location);
        print(record.kind);
        print(record.level);
        print(record.cost);
        if (record.cause != UnprovenCause::Unknown) {
            printer.field("cause"_s, record.cause);
            printWitness(record.witness);
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
    for (const RemainingCheckRecord& record : report.remainingChecks) {
        printer.beginLine("check"_s);
        out.printf(" %012" PRIx64, record.id);
        printLocation(record.origin);
        printer.field("cause"_s, record.cause);
        printer.field("site"_s, static_cast<TypeSiteKind>(record.siteKind));
        printWitness(record.witness);
        printer.endLine();
    }
    for (const TypeTestRecord& record : report.typeTests) {
        printer.beginLine("type-test"_s);
        printLocation(record.location);
        print(FunctionID(record.function));
        print(record.failure);
        printer.field("copies"_s, record.copies);
        printer.endLine();
    }
}

void runFlowReportTestCommand(TestCommandContext& context)
{
    constexpr ASCIILiteral prefix = "through="_s;
    if (context.arguments.size() > 1)
        return printBadArgument(context.out, context.arguments[1]);
    std::optional<PassID> last = PassID::ApplyFlowResults;
    if (!context.arguments.empty()) {
        StringView word = context.arguments[0];
        last = word.startsWith(prefix) ? passNamed(word.substring(prefix.length())) : std::nullopt;
        if (!last)
            return printBadArgument(context.out, word);
    }
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
        printFlowReport(context.out, *output->program, buildFlowReport(*output->program, *output->flow, gradeAccessSites(*output->program, *output->flow, WTF::move(output->accessSiteObservations), context.build), output->remainingChecks.span(), collectFlowReportBodyRecords(*output->program, context.build), context.build));
    else
        context.out.println("no results"_s);

    context.program = WTF::move(output->program);
    context.form = context.program->form();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
