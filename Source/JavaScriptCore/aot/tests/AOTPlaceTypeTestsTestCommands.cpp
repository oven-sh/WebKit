/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAnalysisTestCommands.h"

#if ENABLE(AOT)

#include "AOTInterningPhases.h"
#include "AOTPipelineInternal.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTSort.h"
#include "AOTTypeTestReport.h"

namespace JSC { namespace AOT { namespace Typed {

static OptimizationInputs inputsWithUnknownFlow(Pipeline& pipeline)
{
    pipeline.runner().didCompute(ProgramResult::Flow);
    return pipeline.inputsFor(PassID::PlaceTypeTests);
}

namespace {

struct PlacementTestSession {
    PlacementTestSession(TestCommandContext& context, FunctionID id)
        : runner(context.build)
        , pipeline(*context.program, runner)
        , inputs(inputsWithUnknownFlow(pipeline))
        , arenas(*context.program)
        , analyses(AnalysisContext { context.build, *context.program, &inputs.flow, &inputs.effects, id }, context.program->at(id))
        , pass { *context.program, inputs, id, context.program->at(id), analyses, arenas.types, arenas.strings, context.build }
    {
    }

    PassRunner runner;
    Pipeline pipeline;
    OptimizationInputs inputs;
    TaskArenas arenas;
    FunctionAnalyses analyses;
    FunctionPassContext pass;
};

struct RequestedSite {
    ValueID use;
    ProgramPoint point;
    Vector<Value> tests;
};

} // anonymous namespace

static auto requestedSiteParser(FunctionAnalyses& analyses, BitVector& requestedUses)
{
    const Function& function = analyses.function();
    auto parseIDs = pairParser(':', valueParser(function), listParser(valueParser(function)));
    return [&analyses, &function, &requestedUses, parseIDs](StringView text) -> std::optional<RequestedSite> {
        auto ids = parseIDs(text);
        if (!ids)
            return std::nullopt;

        auto& [use, tests] = *ids;
        ValueLocation location = analyses.useLists().locationOf(use);
        if (location.placement != ValuePlacement::InBlock || requestedUses.set(use.index()))
            return std::nullopt;

        RequestedSite site { use, { location.block, location.index }, { } };
        for (ValueID test : tests) {
            Value prototype = function.at(test);
            if (std::optional<unsigned> guard = OperandPosition::guard(prototype.opcode))
                prototype.setOperand(*guard, ValueID());
            if (placeabilityOf(analyses, prototype) != TestPlaceability::CanBePlaced)
                return std::nullopt;
            site.tests.append(WTF::move(prototype));
        }
        return site;
    };
}

static void runTestPlacement(TestCommandContext& context)
{
    const Program& program = *context.program;
    CommandWords words(context);
    std::optional<FunctionID> function = words.parseOptional("function"_s, functionParser(program));
    if (!function) {
        if (!words.finish())
            return;
        for (size_t index = 0; index < program.functions.size(); ++index) {
            if (!program.at(FunctionID(index)).hasBody())
                continue;
            PlacementTestSession session(context, FunctionID(index));
            context.out.println("function f", index);
            planTestPlacement(session.pass).dump(context.out, program);
        }
        return;
    }

    PlacementTestSession session(context, *function);
    FunctionPassContext& pass = session.pass;
    BitVector requestedUses;
    Vector<RequestedSite> requestedSites = words.parseAll("site"_s, requestedSiteParser(pass.analyses, requestedUses));
    bool usesOwnEffects = words.parseFlag("effects=own"_s);
    if (requestedSites.isEmpty())
        words.failMissingArgument();
    if (!words.finish())
        return;

    TestSiteReporter reporter(pass);
    for (const RequestedSite& requested : requestedSites) {
        reporter.moveTo(TestSiteRuleID::Operator, requested.use, requested.point);
        reporter.report(requested.tests.span(), usesOwnEffects ? pass.analyses.effectsOf(requested.use).normal : Effects::none());
    }
    TestSites sites = reporter.takeSites();
    const CFG& cfg = pass.analyses.cfg();
    AOT::sort(sites.sites, { }, [&](const TestSite& site) { return std::pair { cfg.reversePostOrderIndex(site.point.block), site.point.index }; });
    planTestPlacement(pass, WTF::move(sites)).dump(context.out, program);
}

static void runTypeTestReport(TestCommandContext& context)
{
    if (!CommandWords(context).finish())
        return;
    for (const Wire::TypeTestRecord& record : collectTypeTestRecords(*context.program)) {
        context.out.print("m", record.location.module, ":", record.location.position, " ");
        if (record.function == Wire::none)
            context.out.print("-");
        else
            context.out.print("f", record.function);
        context.out.println(" ", nameOf(record.failure), " copies=", record.copies);
    }
}

std::span<const TestCommand> testPlacementTestCommands()
{
    static constexpr TestCommand commands[] {
        { "test-placement"_s, IRForm::Prepared, runTestPlacement },
        { "type-test-report"_s, IRForm::Prepared, runTypeTestReport },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
