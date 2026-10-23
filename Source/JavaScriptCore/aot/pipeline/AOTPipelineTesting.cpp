/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPipeline.h"

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTChooseCallingConventions.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectSummaries.h"
#include "AOTElideStackChecks.h"
#include "AOTEmbedConstantCollections.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIRPrinter.h"
#include "AOTInterningPhases.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTPipelineInternal.h"
#include "AOTPlacement.h"
#include "AOTRegExpRequests.h"
#include "AOTReuseEnvironments.h"
#include "AOTSiteCachePlacement.h"
#include "AOTSlotPlan.h"
#include "AOTSummarizeForInlining.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

static ASCIILiteral originName(RegExpRequestOrigin origin)
{
    switch (origin) {
    case RegExpRequestOrigin::Literal:
        return "literal"_s;
    case RegExpRequestOrigin::Evaluated:
        return "evaluated"_s;
    case RegExpRequestOrigin::Candidate:
        return "candidate"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RegExpRequests::dump(PrintStream& out, const Typed::Program& program) const
{
    out.print("regexp requests: ", m_requests.size(), "\n");
    for (size_t index = 0; index < m_requests.size(); ++index) {
        const RegExpRequest& request = m_requests[index];
        out.print("  ", index, " ", originName(request.origin), " ");
        Typed::printString(out, program, request.pattern);
        Yarr::FlagsString characters = Yarr::flagsString(request.flags);
        out.print(" flags=");
        if (request.flags.isEmpty())
            out.print("-");
        else
            out.print(StringView(characters.span()));
        out.print(" sites=");
        CommaPrinter comma;
        for (const RegExpSite& site : request.sites)
            out.print(comma, "f", site.function.index(), ":v", site.value.index());
        out.print("\n");
    }
}

namespace Typed {

static bool acceptsForm(PassID id, IRForm form)
{
    const PassDescriptor& descriptor = descriptorOf(id);
    return descriptor.firstForm <= form && form <= descriptor.lastForm;
}

// IR text has no PlacedTableSizes, which the verifier needs in form Placed.
static bool canVerify(IRForm form, const VerifierInputs& inputs)
{
    return form != IRForm::Placed || inputs.placed;
}

static PassID producerOf(ProgramResult result)
{
    for (PassID id : allMembersOf<PassID>) {
        if (id != PassID::None && descriptorOf(id).outputs.contains(result))
            return id;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void computeResults(Pipeline& pipeline, OptionSet<ProgramResult> needed, OptionSet<ProgramResult>& computedResults)
{
    for (ProgramResult result : needed) {
        if (computedResults.contains(result))
            continue;
        computedResults.add(result);

        PassID producer = producerOf(result);
        if (!acceptsForm(producer, pipeline.program().form()) || descriptorOf(producer).produces) {
            pipeline.runner().didCompute(result);
            continue;
        }
        computeResults(pipeline, descriptorOf(producer).inputs, computedResults);
        pipeline.runProgramPass(producer);
    }
}

void computeResultsForTesting(Pipeline& pipeline, OptionSet<ProgramResult> needed)
{
    OptionSet<ProgramResult> computedResults;
    computeResults(pipeline, needed, computedResults);
}

namespace {

struct TestedPass {
    PassID id;
    const Program& program;
    const OptimizationInputs& inputs;
    const PassRunner& runner;
};

class SerialCopyLoop {
    WTF_MAKE_NONCOPYABLE(SerialCopyLoop);
public:
    explicit SerialCopyLoop(Pipeline& pipeline)
        : m_pipeline(pipeline)
        , m_program(pipeline.program())
        , m_numberOfFunctions(m_program.functions.size())
        , m_phases(m_program, m_numberOfFunctions)
        , m_derived(*pipeline.output().derivedFunctions, m_program, pipeline.flow(), m_phases)
        , m_copies(m_numberOfFunctions)
    {
    }

    unsigned numberOfFunctions() const { return m_numberOfFunctions; }
    InterningPhases& phases() { return m_phases; }

    void reach(FunctionID);
    void setCopy(FunctionID, std::unique_ptr<Function>);
    void discard() { m_phases.discard(); }
    void commit();

private:
    const Program& program() const { return m_program; }
    void reachReferencesOf(const Function&);
    void buildReachedFunctions();

    Pipeline& m_pipeline;
    Program& m_program;
    const unsigned m_numberOfFunctions;
    InterningPhases m_phases;
    DerivedFunctionPhase m_derived;
    Vector<std::unique_ptr<Function>> m_copies;
    BitVector m_reachedOrdinals;
    Vector<FunctionID> m_worklist;
};

void SerialCopyLoop::reach(FunctionID id)
{
    if (ProvisionalFunctions::contains(id) && m_reachedOrdinals.add(ProvisionalFunctions::ordinalOf(id)))
        m_worklist.append(id);
}

void SerialCopyLoop::reachReferencesOf(const Function& function)
{
    forEachFunctionReference(program(), function, [&](FunctionID target, FunctionReferenceKind) {
        reach(target);
    });
}

void SerialCopyLoop::setCopy(FunctionID id, std::unique_ptr<Function> copy)
{
    reachReferencesOf(*copy);
    m_copies[id.index()] = WTF::move(copy);
}

void SerialCopyLoop::buildReachedFunctions()
{
    while (!m_worklist.isEmpty()) {
        FunctionID id = m_worklist.takeLast();
        auto function = makeUniqueWithoutFastMallocCheck<Function>(program().at(id));
        reachReferencesOf(*function);
        m_derived.setOptimizedBody(id, WTF::move(function));
    }
}

void SerialCopyLoop::commit()
{
    buildReachedFunctions();

    DerivedFunctionCommit committed = m_derived.commit(m_reachedOrdinals);
    Vector<unsigned> taskOrder(m_numberOfFunctions, [](size_t task) {
        return static_cast<unsigned>(task);
    });
    taskOrder.appendVector(committed.tasks);
    BodyRemaps remaps = m_phases.finish(taskOrder.span(), WTF::move(committed.remap));

    ProgramEditor editor(m_program);
    for (size_t position = 0; position < committed.functions.size(); ++position) {
        Function& function = *committed.functions[position];
        applyRemaps(remaps, function);
        FunctionID id = editor.addFunction(WTF::move(function));
        RELEASE_ASSERT(id.index() == committed.firstID.index() + position);
    }
    for (unsigned index = 0; index < m_numberOfFunctions; ++index) {
        if (!m_copies[index])
            continue;
        applyRemaps(remaps, *m_copies[index]);
        editor.replaceFunction(FunctionID(index), WTF::move(*m_copies[index]));
    }

    m_pipeline.analyses().invalidate(resultsAboutBodies);
    m_pipeline.runner().build().resolveFunctionToDump(m_program);
}

} // anonymous namespace

static PassOutcome runOnFunction(const TestedPass& pass, FunctionID id, Function& function, TaskArenas& arenas)
{
    BuildContext& build = pass.runner.build();
    FunctionAnalyses analyses(AnalysisContext { build, pass.program, &pass.inputs.flow, &pass.inputs.effects, id }, function);
    FunctionPassContext context { pass.program, pass.inputs, id, function, analyses, arenas.types, arenas.strings, build };
    return pass.runner.runFunctionPass(pass.id, context);
}

static PassOutcome runFunctionPassInPlace(Pipeline& pipeline, PassID id)
{
    Program& program = pipeline.program();
    OptimizationInputs inputs = pipeline.inputsFor(id);
    TestedPass pass { id, program, inputs, pipeline.runner() };

    TaskArenas arenas(program);
    PassOutcome outcome = PassOutcome::Unchanged;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        Function& function = program.functions[index];
        if (function.hasBody())
            outcome |= runOnFunction(pass, FunctionID(index), function, arenas);
    }
    pipeline.runner().didFinishFunctionLoop();

    if (outcome == PassOutcome::Changed)
        pipeline.analyses().invalidate(resultsAboutBodies);
    return outcome;
}

static PassOutcome runFunctionPassOnCopies(Pipeline& pipeline, PassID id)
{
    const Program& program = pipeline.program();
    OptimizationInputs inputs = pipeline.inputsFor(id);
    TestedPass pass { id, program, inputs, pipeline.runner() };

    SerialCopyLoop loop(pipeline);
    PassOutcome outcome = PassOutcome::Unchanged;
    for (unsigned index = 0; index < loop.numberOfFunctions(); ++index) {
        const Function& source = program.functions[index];
        if (!source.hasBody())
            continue;
        auto copy = makeUniqueWithoutFastMallocCheck<Function>(source);
        TaskArenas arenas(loop.phases(), index);
        if (runOnFunction(pass, FunctionID(index), *copy, arenas) == PassOutcome::Unchanged)
            continue;
        outcome = PassOutcome::Changed;
        loop.setCopy(FunctionID(index), WTF::move(copy));
    }
    pipeline.runner().didFinishFunctionLoop();

    if (pipeline.runner().build().diagnostics().hasErrors()) {
        loop.discard();
        return PassOutcome::Unchanged;
    }
    loop.commit();
    return outcome;
}

static PassOutcome runPass(Pipeline& pipeline, const PassDescriptor& descriptor)
{
    switch (descriptor.scope) {
    case PassScope::Program:
        return pipeline.runProgramPass(descriptor.id);
    case PassScope::Function:
        if (descriptor.derivesFunctions || descriptor.readsOtherBodies)
            return runFunctionPassOnCopies(pipeline, descriptor.id);
        return runFunctionPassInPlace(pipeline, descriptor.id);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void verifyChangedProgram(Pipeline& pipeline, const PassDescriptor& descriptor)
{
    const Program& program = pipeline.program();
    VerifierInputs inputs = pipeline.runner().verifierInputs();
    if (!canVerify(program.form(), inputs) || descriptor.isVerifiedAfterNextPass)
        return;
    inputs.isOptimizing = program.form() == IRForm::Prepared;
    if (descriptor.inputs.contains(ProgramResult::EffectSummaries))
        inputs.effects = &pipeline.analyses().effects(pipeline.flow());
    verifyProgram(program, program.form(), pipeline.runner().build(), inputs);
}

void commitRequestedFunctionsForTesting(Pipeline& pipeline, std::span<const DerivedFunctionKey> keys, PrintStream& out)
{
    DerivedFunctions& derivedFunctions = *pipeline.output().derivedFunctions;
    SerialCopyLoop loop(pipeline);
    IRPrinter printer(out, pipeline.program());
    for (const DerivedFunctionKey& key : keys) {
        FunctionID id = derivedFunctions.ensure(key);
        printer.beginRecord();
        printFields(printer, key);
        printer.endRecord();
        if (ProvisionalFunctions::contains(id))
            out.println(" -> request ", ProvisionalFunctions::ordinalOf(id));
        else {
            out.print(" -> ");
            printer.print(id);
            out.println();
        }
        loop.reach(id);
    }
    loop.commit();
}

std::unique_ptr<PipelineOutput> runPipelineUntil(PassID last, std::unique_ptr<Program> program, BuildContext& build)
{
    build.resolveFunctionToDump(*program);
    PassRunner runner(build, last);
    auto output = runPasses(WTF::move(program), runner);
    if (build.diagnostics().hasErrors()) {
        build.diagnostics().finish(output->program.get());
        return nullptr;
    }
    return output;
}

static void dumpNode(IRPrinter& printer, const DataNode& node)
{
    auto dumpLeaf = [&](auto... payload) {
        printer.out().print(nameOf(node.kind()), "(");
        (printer.print(payload), ...);
        printer.out().print(")");
    };

    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
        dumpLeaf();
        return;
    case DataNode::Kind::Boolean:
        dumpLeaf(node.as<DataNode::Boolean>().value);
        return;
    case DataNode::Kind::Number:
        dumpLeaf(node.as<DataNode::Number>().value);
        return;
    case DataNode::Kind::String:
        dumpLeaf(node.as<DataNode::String>().value);
        return;
    case DataNode::Kind::BigInt:
        dumpLeaf(node.as<DataNode::BigInt>().digits);
        return;
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        printVariant(printer, node);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ConstantCollections::dump(PrintStream& out, const Program& program) const
{
    IRPrinter printer(out, program);
    for (size_t index = 0; index < m_collections.size(); ++index) {
        const ConstantCollection& collection = m_collections[index];
        std::span<const DataNode> entries = program.at(collection.data).root.as<DataNode::Array>().elements.span();
        out.print("table ", index, " keys: ");
        dumpList(out, entries, [&](PrintStream&, const DataNode& entry) {
            dumpNode(printer, collection.kind == ConstantCollectionKind::Set ? entry : entry.as<DataNode::Array>().elements[0]);
        });
        out.print("\n");
        if (collection.kind == ConstantCollectionKind::Set)
            continue;

        out.print("table ", index, " values: ");
        dumpList(out, entries, [&](PrintStream&, const DataNode& entry) {
            dumpNode(printer, entry.as<DataNode::Array>().elements[1]);
        });
        out.print("\n");
    }
}

#define AOT_FUNCTION_PASS_CASE(id, function) case PassID::id:

void Pipeline::dumpResultOf(PassID id, PrintStream& out) const
{
    const PipelineOutput& output = *m_output;
    switch (id) {
    case PassID::AnalyzeFlow:
        output.flow->dump(out, m_program);
        return;
    case PassID::BuildCallGraph: {
        const CallGraph& callGraph = m_analyses.callGraph(flow());
        callGraph.computeRanks(m_build);
        callGraph.dump(out, m_program);
        return;
    }
    // A run of all stages that was stopped after a pass has handed these two to the output, and a stopped runner computes nothing again.
    case PassID::ComputeEffectSummaries:
        if (m_build.isEnabled(PassID::ComputeEffectSummaries, FunctionID()))
            Typed::computeEffectSummaries(m_program, flow(), m_build).dump(out, m_program);
        else
            (m_runner.isStopped() ? *output.effects : m_analyses.effects(flow())).dump(out, m_program);
        return;
    case PassID::EstimateHeat:
        (m_runner.isStopped() ? *output.heat : m_analyses.heat(flow())).dump(out, m_program);
        return;
    case PassID::SummarizeForInlining:
        m_analyses.inlining(flow()).dump(out, m_program);
        return;
    case PassID::ChooseCallingConventions:
        output.callingConventions->dump(out, m_program);
        return;
    case PassID::AllocateInFrames:
        output.frameAllocation->dump(out, m_program);
        return;
    case PassID::ReuseEnvironments:
        output.environmentReuse->dump(out, m_program);
        return;
    case PassID::NumberLiteralStrings:
        output.literalStrings->dump(out, m_program);
        return;
    case PassID::EmbedConstantCollections:
        output.constantCollections->dump(out, m_program);
        return;
    case PassID::CollectRegExpLiterals:
        output.regExpRequests->dump(out, m_program);
        return;
    case PassID::ElideStackChecks:
        output.stackChecks->dump(out, m_program);
        return;
    case PassID::PlanSlots:
        output.slots->dump(out, m_program);
        return;
    case PassID::NumberAndEncode:
        output.placement->dump(out, m_program);
        return;
    case PassID::PlaceSiteCaches:
        output.siteCaches->dump(out, m_program);
        return;
    case PassID::None:
    case PassID::AllocateTaggedLayouts:
    case PassID::CheckTypesAtEntry:
    case PassID::SplitBehindEntryChecks:
    case PassID::SkipAnsweredEntryChecks:
    case PassID::NarrowFieldTypes:
    case PassID::NameCreatedSymbols:
    case PassID::VerifyProgram:
    case PassID::GuardTaintedPositions:
    case PassID::ApplyFlowResults:
    case PassID::CheckLockedBuiltinWrites:
    case PassID::RemoveUnreadExports:
    case PassID::PromoteEnvironmentsToVariables:
    case PassID::DeferDefinitions:
    case PassID::NarrowConstantVariables:
    case PassID::DeferInitializers:
    case PassID::MarkResultsUsedAsKeys:
    case PassID::DeferClosuresInFields:
    case PassID::AllocateInnerObjects:
    case PassID::FlattenInnerObjects:
    case PassID::ReadCapturesFromEnvironments:
    case PassID::RemoveUnreadFields:
    case PassID::RemoveUnusedResults:
    case PassID::RemoveUnusedParameters:
    case PassID::RemoveUnreachableFunctions:
    case PassID::SplitFreshResults:
    case PassID::PromoteAggregateParameters:
    case PassID::SplitCoroutines:
    case PassID::CallModules:
    case PassID::CreateEntrypoints:
    case PassID::OutlineColdOperations:
    case PassID::InlineHeads:
    case PassID::CheckPlacement:
    case PassID::GenerateCode:
    FOR_EACH_AOT_FUNCTION_PASS(AOT_FUNCTION_PASS_CASE)
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#undef AOT_FUNCTION_PASS_CASE

bool canRunPassForTesting(PassID id, IRForm form)
{
    if (id == PassID::None || id == PassID::GenerateCode)
        return false;
    if (id == PassID::VerifyProgram && !canVerify(form, { }))
        return false;
    return acceptsForm(id, form);
}

PassOutcome runPassForTesting(Pipeline& pipeline, PassID id, PrintStream& results)
{
    Program& program = pipeline.program();
    BuildContext& build = pipeline.runner().build();
    RELEASE_ASSERT(canRunPassForTesting(id, program.form()));

    computeResultsForTesting(pipeline, descriptorOf(id).inputs);
    if (pipeline.runner().isStopped())
        return PassOutcome::Unchanged;

    const PassDescriptor& descriptor = descriptorOf(id);
    PassOutcome outcome = runPass(pipeline, descriptor);
    if (build.diagnostics().hasErrors())
        return outcome;

    pipeline.dumpResultOf(id, results);
    if (descriptor.produces)
        program.setForm(*descriptor.produces);
    if (outcome == PassOutcome::Changed)
        verifyChangedProgram(pipeline, descriptor);
    return outcome;
}

PassOutcome runPassForTesting(PassID id, Program& program, PrintStream& results, BuildContext& build)
{
    build.resolveFunctionToDump(program);
    PassRunner runner(build);
    Pipeline pipeline(program, runner);
    return runPassForTesting(pipeline, id, results);
}

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
