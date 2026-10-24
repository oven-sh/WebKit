/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsTestCommands.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTInterningPhases.h"
#include "AOTPipelineInternal.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTSort.h"
#include "AOTTypeTestReport.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static OptimizationInputs inputsWithUnknownFlow(Pipeline& pipeline)
{
    pipeline.runner().didCompute(ProgramResult::Flow);
    return pipeline.inputsFor(PassID::PlaceTypeTests);
}

namespace {

class PlacementTestSession {
    WTF_MAKE_NONCOPYABLE(PlacementTestSession);
public:
    PlacementTestSession(TestCommandContext& context, FunctionID id)
        : m_runner(context.build)
        , m_pipeline(*context.program, m_runner)
        , m_inputs(inputsWithUnknownFlow(m_pipeline))
        , m_arenas(*context.program)
        , m_analyses(AnalysisContext { context.build, *context.program, &m_inputs.flow, &m_inputs.effects, id }, context.program->at(id))
        , m_pass { *context.program, m_inputs, id, context.program->at(id), m_analyses, m_arenas.types, m_arenas.strings, context.build }
    {
    }

    FunctionPassContext& pass() { return m_pass; }

private:
    PassRunner m_runner;
    Pipeline m_pipeline;
    OptimizationInputs m_inputs;
    TaskArenas m_arenas;
    FunctionAnalyses m_analyses;
    FunctionPassContext m_pass;
};

struct RequestedSite {
    ValueID use;
    ProgramPoint point;
    Vector<Value> tests;
};

} // anonymous namespace

static std::optional<Value> createPrototype(FunctionAnalyses& analyses, ValueID test)
{
    Value prototype = analyses.function().at(test);
    if (std::optional<unsigned> guard = OperandPosition::guard(prototype.opcode))
        prototype.setOperand(*guard, ValueID());
    if (placeabilityOf(analyses, prototype) != TestPlaceability::CanBePlaced)
        return std::nullopt;
    return prototype;
}

static auto requestedSiteParser(FunctionAnalyses& analyses, BitVector& requestedUses)
{
    const Function& function = analyses.function();
    auto parseIDs = pairParser(':', valueParser(function), listParser(valueParser(function)));
    return [&analyses, &requestedUses, parseIDs](StringView text) -> std::optional<RequestedSite> {
        auto ids = parseIDs(text);
        if (!ids)
            return std::nullopt;

        auto& [use, tests] = *ids;
        ValueLocation location = analyses.useLists().locationOf(use);
        if (location.placement != ValuePlacement::InBlock || requestedUses.set(use.index()))
            return std::nullopt;

        RequestedSite site { use, { location.block, location.index }, { } };
        for (ValueID test : tests) {
            std::optional<Value> prototype = createPrototype(analyses, test);
            if (!prototype)
                return std::nullopt;
            site.tests.append(WTF::move(*prototype));
        }
        return site;
    };
}

static TestSites reportRequestedSites(FunctionPassContext& pass, std::span<const RequestedSite> requestedSites, bool usesOwnEffects)
{
    TestSiteReporter reporter(pass);
    for (const RequestedSite& requested : requestedSites) {
        reporter.moveTo(TestSiteRuleID::Operator, requested.use, requested.point);
        reporter.report(requested.tests.span(), usesOwnEffects ? pass.analyses.effectsOf(requested.use).normal : Effects::none());
    }

    TestSites sites = reporter.takeSites();
    const CFG& cfg = pass.analyses.cfg();
    AOT::sort(sites.sites, { }, [&](const TestSite& site) {
        return std::pair { cfg.reversePostOrderIndex(site.point.block), site.point.index };
    });
    return sites;
}

static void printPlansOfAllFunctions(TestCommandContext& context)
{
    const Program& program = *context.program;
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (!program.at(id).hasBody())
            continue;
        PlacementTestSession session(context, id);
        context.out.println("function f", id.index());
        planTestPlacement(session.pass()).dump(context.out, program);
    }
}

static void runTestPlacement(TestCommandContext& context)
{
    CommandWords words(context);
    std::optional<FunctionID> function = words.parseOptional("function"_s, functionParser(*context.program));
    if (!function) {
        if (words.finish())
            printPlansOfAllFunctions(context);
        return;
    }

    PlacementTestSession session(context, *function);
    FunctionPassContext& pass = session.pass();
    BitVector requestedUses;
    Vector<RequestedSite> requestedSites = words.parseAll("site"_s, requestedSiteParser(pass.analyses, requestedUses));
    bool usesOwnEffects = words.parseFlag("effects=own"_s);
    if (requestedSites.isEmpty())
        words.failMissingArgument();
    if (!words.finish())
        return;

    planTestPlacement(pass, reportRequestedSites(pass, requestedSites.span(), usesOwnEffects)).dump(context.out, *context.program);
}

static void printRecord(PrintStream& out, const Wire::TypeTestRecord& record)
{
    out.print("m", record.location.module, ":", record.location.position, " ");
    if (record.function == Wire::none)
        out.print("-");
    else
        out.print("f", record.function);
    out.println(" ", nameOf(record.failure), " copies=", record.copies);
}

static void runTypeTestReport(TestCommandContext& context)
{
    if (!CommandWords(context).finish())
        return;
    for (const Wire::TypeTestRecord& record : collectTypeTestRecords(*context.program))
        printRecord(context.out, record);
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
