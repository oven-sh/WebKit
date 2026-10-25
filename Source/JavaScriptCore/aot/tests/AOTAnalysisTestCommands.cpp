/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAnalysisTestCommands.h"

#if ENABLE(AOT)

#include "AOTCallGraph.h"
#include "AOTEffectsDescription.h"
#include "AOTFlowApply.h"
#include "AOTHeatGraph.h"
#include "AOTIRPrinter.h"
#include "AOTRecordUsers.h"
#include "AOTRegionCloner.h"
#include "AOTResultRules.h"
#include "AOTSSARepair.h"
#include "AOTValueFlowResults.h"
#include "AOTValueSubstitution.h"
#include "AOTVerifier.h"
#include <algorithm>
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

using FunctionCommand = void (*)(TestCommandContext&, CommandWords&, Session&);
using ArgumentOnEdge = std::pair<Edge, ValueID>;

FlowPreparation prepareFlow(TestCommandContext& context, CommandWords& words)
{
    FlowPreparation preparation;
    if (!words.parseFlag("flow"_s))
        return preparation;
    if (context.form != IRForm::AsRead) {
        words.fail("flow"_s);
        return preparation;
    }

    Program& program = *context.program;
    preparation.flow.emplace(analyzeValueFlow(program, program.types, context.build));
    applyFlowResults(program, *preparation.flow, context.build);
    program.setForm(IRForm::Prepared);
    context.form = program.form();
    preparation.effects.emplace(computeEffectSummaries(program, *preparation.flow, context.build));
    return preparation;
}

static void computeAnalyses(FunctionAnalyses& analyses, AnalysisSet set)
{
    for (FunctionAnalysis analysis : set) {
        switch (analysis) {
        case FunctionAnalysis::ControlFlow:
            analyses.cfg();
            break;
        case FunctionAnalysis::Dominators:
            analyses.dominators();
            break;
        case FunctionAnalysis::NaturalLoops:
            analyses.naturalLoops();
            break;
        case FunctionAnalysis::Cycles:
            analyses.blockCycles();
            break;
        case FunctionAnalysis::UseLists:
            analyses.useLists();
            break;
        case FunctionAnalysis::Identities:
            analyses.identities();
            break;
        case FunctionAnalysis::Liveness:
            analyses.liveness();
            break;
        case FunctionAnalysis::Effects:
            analyses.effectsOfTerminator(entryBlock);
            break;
        case FunctionAnalysis::Escape:
            analyses.escape();
            break;
        case FunctionAnalysis::EscapeHistory:
            analyses.escapeHistory();
            break;
        case FunctionAnalysis::Frequencies:
            analyses.blockFrequencies();
            break;
        case FunctionAnalysis::Guards:
            analyses.guards();
            break;
        case FunctionAnalysis::FailedTests:
            analyses.findEarlierTest(Value(), entryBlock);
            break;
        case FunctionAnalysis::Induction:
            analyses.inductionVariables();
            break;
        }
    }
}

template<FunctionCommand run>
static void runOnFunction(TestCommandContext& context)
{
    CommandWords words(context);
    FunctionID id = words.parseRequired("function"_s, functionParser(*context.program));
    if (!id)
        return;
    bool printsUseLists = words.parseFlag("use-lists"_s);
    Session session(context, id);
    if (printsUseLists)
        session.analyses.setRunningPass(PassID::CompactBlocks);
    run(context, words, session);
    if (printsUseLists) {
        context.out.println("use-lists:");
        dumpUseLists(context.out, session.function, session.analyses.useLists());
    }
}

static bool beginRewrite(CommandWords& words, Session& session)
{
    if (!words.finish())
        return false;
    computeAnalyses(session.analyses, allAnalyses);
    return true;
}

static void printAndVerify(TestCommandContext& context, FunctionID id, const Function& function)
{
    printFunction(context.out, *context.program, id, function);
    verifyFunction(*context.program, id, function, context.form, context.build);
}

static void rewriteAndPrint(TestCommandContext& context, CommandWords& words, Session& session, NOESCAPE const auto& edit)
{
    if (!beginRewrite(words, session))
        return;
    if constexpr (std::is_void_v<decltype(edit())>)
        edit();
    else if (!edit())
        return;
    printAndVerify(context, session.id, session.function);
}

static bool printCloneCheck(PrintStream& out, CloneCheck check)
{
    out.println("check: ", enumName(check));
    return check == CloneCheck::CanClone;
}

static auto valueInBlockParser(Session& session)
{
    return filterParser(valueParser(session.function), [&session](ValueID value) {
        return session.analyses.useLists().locationOf(value).placement == ValuePlacement::InBlock;
    });
}

static ProgramPoint pointBefore(Session& session, ValueID value)
{
    ValueLocation location = session.analyses.useLists().locationOf(value);
    return { location.block, location.index };
}

static void expandValue(TestCommandContext& context, CommandWords& words, Session& session)
{
    constexpr uint32_t maxNumberOfArms = 2;
    auto parseNumberOfArms = filterParser(indexParser(maxNumberOfArms + 1), [](uint32_t numberOfArms) { return !!numberOfArms; });
    auto isAfterCaughtValues = [&](ValueID id) {
        ValueLocation location = session.analyses.useLists().locationOf(id);
        return location.index >= indexAfterCaughtValues(session.function, session.function.at(location.block));
    };
    ValueID value = words.parseRequired("value"_s, filterParser(valueInBlockParser(session), isAfterCaughtValues));
    uint32_t numberOfArms = words.parseOptional("arms"_s, parseNumberOfArms).value_or(maxNumberOfArms);
    auto parseRead = filterParser(valueParser(session.function), [&](ValueID read) {
        const Value& candidate = session.function.at(read);
        return candidate.opcode == Opcode::ResultAt && candidate.operand(0) == value;
    });
    std::optional<Vector<ValueID>> reads = words.parseOptional("reads"_s, listParser(parseRead));

    rewriteAndPrint(context, words, session, [&] {
        IRBuilder& builder = session.builder;
        const Value original = session.function.at(value);
        bool isReadByResultAt = numberOfResultsOf(*context.program, session.function, original).has_value();
        ProgramPoint point = pointBefore(session, value);
        IRBuilder::Expansion expansion = reads ? builder.beginExpansion(point, reads->span()) : builder.beginExpansion(point);
        auto appendArm = [&] {
            ValueID copy = builder.append(Value(original));
            Vector<ValueID, 2> results;
            if (isReadByResultAt) {
                for (uint32_t index = 0; index < expansion.results.size(); ++index) {
                    Value result = IRBuilder::make(Opcode::ResultAt, { copy });
                    result.setIndex(index);
                    results.append(builder.append(WTF::move(result)));
                }
            } else if (!expansion.results.isEmpty())
                results.append(copy);
            builder.endExpansion(expansion, results.span());
        };
        if (numberOfArms == maxNumberOfArms) {
            IRBuilder::BranchBlocks blocks = builder.emitBranch(builder.appendBoolean(true), BlockFrequency::Normal, BlockFrequency::Unlikely);
            appendArm();
            builder.moveToEndOf(blocks.notTaken);
        }
        appendArm();
    });
}

static void coverBlocks(TestCommandContext& context, CommandWords& words, Session& session)
{
    BlockID handler = words.parseRequired("handler"_s, edgeTargetParser(session.function));
    Vector<BlockID> blocks = words.parseRequired("blocks"_s, listParser(blockParser(session.function)));
    rewriteAndPrint(context, words, session, [&] { session.builder.coverWithHandler(blocks.span(), Target { handler, { }, { } }); });
}

static Vector<Vector<ValueID>> parseVariables(CommandWords& words, const Function& function)
{
    BitVector isDefinition;
    auto parseDefinition = filterParser(valueParser(function), [&](ValueID value) { return !isDefinition.set(value.index()); });
    return words.parseAll("variable"_s, listParser(parseDefinition));
}

static void repairVariables(Session& session, const Vector<Vector<ValueID>>& variables, RegionCloner* cloner = nullptr)
{
    ValueSubstitution substitution(session.function);
    for (const Vector<ValueID>& definitions : variables) {
        for (ValueID definition : definitions.subspan(1))
            substitution.replace(definition, definitions.first());
    }
    substitution.apply(session.function, &session.analyses);

    SSARepair repair(session.builder, session.analyses);
    for (const Vector<ValueID>& definitions : variables) {
        SSARepair::Variable variable = repair.addVariable(definitions.first());
        for (ValueID definition : definitions.subspan(1))
            repair.addDefinition(variable, definition);
    }
    if (cloner)
        cloner->addVariablesTo(repair, session.analyses);
    repair.run();
}

static void duplicateRegion(TestCommandContext& context, CommandWords& words, Session& session)
{
    const Function& function = session.function;
    Vector<BlockID> region = words.parseRequired("region"_s, listParser(blockParser(function)));
    auto entersRegion = [&](Edge edge) { return region.contains(targetOf(function, edge).block); };
    Vector<Edge> redirectedEdges = words.parseAll("redirect"_s, filterParser(edgeParser(function), entersRegion));
    Vector<Vector<ValueID>> variables = parseVariables(words, function);

    rewriteAndPrint(context, words, session, [&] {
        Vector<BlockID> entries;
        for (Edge edge : redirectedEdges) {
            BlockID target = targetOf(function, edge).block;
            if (region.contains(edge.from) && !entries.contains(target))
                entries.append(target);
        }
        if (!printCloneCheck(context.out, RegionCloner::checkCanDuplicate(session.analyses, region.span(), entries.span())))
            return false;

        RegionCloner cloner(session.builder, function);
        cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
        for (Edge edge : redirectedEdges) {
            if (entersRegion(edge))
                session.builder.redirectEdge(edge, cloner.cloneOf(targetOf(function, edge).block), Arguments(targetOf(function, edge).arguments));
        }
        if (variables.isEmpty())
            cloner.repairSSA(session.analyses);
        else
            repairVariables(session, variables, &cloner);
        return true;
    });
}

static Function outlineRegion(Session& session, const Vector<BlockID>& region, const Vector<ValueID>& liveIns)
{
    const Function& origin = session.function;
    Function outlined;
    outlined.name = origin.name;
    outlined.module = origin.module;
    outlined.position = origin.position;
    outlined.kind = origin.kind;
    outlined.role = FunctionRole::OutlinedBody;
    outlined.flags = origin.flags & OptionSet<FunctionFlags> { FunctionFlags::IsSloppy, FunctionFlags::IsChecked, FunctionFlags::IsHiddenInStackTraces };
    outlined.thisType = DeclaredType { .type = TypeTable::anything };
    outlined.results = origin.results;
    outlined.capabilities = initialCapabilitiesOf(outlined);
    outlined.inlineFrames = origin.inlineFrames;

    IRBuilder builder(session.analysisContext, outlined, session.types);
    RegionCloner cloner(builder, origin);
    BlockID entry = builder.addBlock();
    auto addEntryParameterLike = [&](ValueID inOrigin) {
        const Value& value = origin.at(inOrigin);
        outlined.parameters.append(DeclaredType { .type = value.type });
        ValueID parameter = builder.addParameter(entry, value.type);
        outlined.edit(parameter).setExpected(value.expected());
        return parameter;
    };
    for (ValueID liveIn : liveIns)
        cloner.mapValue(liveIn, addEntryParameterLike(liveIn));
    Arguments headArguments;
    for (ValueID parameter : origin.at(region.first()).parameters)
        headArguments.append(addEntryParameterLike(parameter));
    cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
    builder.moveToEndOf(entry);
    builder.emitJump(cloner.cloneOf(region.first()), WTF::move(headArguments));
    return outlined;
}

static void extractRegion(TestCommandContext& context, CommandWords& words, Session& session)
{
    Vector<BlockID> region = words.parseRequired("region"_s, listParser(blockParser(session.function)));
    if (!beginRewrite(words, session) || !printCloneCheck(context.out, RegionCloner::checkCanExtract(session.analyses, region.span())))
        return;
    bool hasEdgeLeavingRegion = std::ranges::any_of(region, [&](BlockID block) {
        return std::ranges::any_of(session.function.at(block).terminator.targets, [&](const Target& target) { return !region.contains(target.block); });
    });
    if (hasEdgeLeavingRegion) {
        words.fail("region"_s);
        return;
    }

    Vector<ValueID> liveIns = RegionCloner::findLiveIns(session.function, region.span());
    dumpLine(context.out, "live-ins: "_s, liveIns);
    Function outlined = outlineRegion(session, region, liveIns);
    FunctionID outlinedID = ProgramEditor(*context.program).addFunctionLike(session.id, WTF::move(outlined));
    printAndVerify(context, outlinedID, context.program->at(outlinedID));
}

static void inlineCall(TestCommandContext& context, CommandWords& words, Session& session)
{
    const Program& program = *context.program;
    Function& function = session.function;
    ValueID callID = words.parseRequired("value"_s, filterParser(valueInBlockParser(session), [&](ValueID id) {
        const Value& value = function.at(id);
        return value.opcode == Opcode::CallDirect && program.at(value.function()).hasBody() && !numberOfResultsOf(program, function, value);
    }));

    rewriteAndPrint(context, words, session, [&] {
        IRBuilder& builder = session.builder;
        const Value call = function.at(callID);
        if (!printCloneCheck(context.out, RegionCloner::checkCanInline(program.at(call.function()))))
            return false;

        Function callee = program.at(call.function());
        {
            const AnalysisContext& caller = session.analysisContext;
            IRBuilder calleeBuilder(AnalysisContext { caller.build, caller.program, caller.flow, caller.effects, call.function() }, callee, session.types);
            for (uint32_t index = 0; index < callee.blocks.size(); ++index) {
                if (callee.blocks[index].terminator.kind == TerminatorKind::ReturnCall)
                    calleeBuilder.demoteReturnCall(BlockID(index));
            }
        }
        IRBuilder::Expansion expansion = builder.beginExpansion(pointBefore(session, callID));
        builder.setHandler(expansion.handler);
        RegionCloner cloner(builder, callee);
        for (uint32_t index = 0; index < callee.blocks[0].parameters.size(); ++index)
            cloner.mapValue(callee.blocks[0].parameters[index], call.operand(index));
        cloner.cloneBlocks(idsMatching<BlockID>(callee.blocks.size(), [](BlockID) { return true; }).span(), [](ValueID, Value&) { });
        builder.emitJump(cloner.cloneOf(entryBlock));
        for (BlockID block : cloner.clonedBlocks()) {
            Terminator& terminator = function.edit(block).terminator;
            if (terminator.kind != TerminatorKind::Return)
                continue;
            ValueID result = std::exchange(terminator, Terminator { }).valueOperand();
            builder.moveToEndOf(block);
            builder.endExpansion(expansion, result ? result : builder.appendUndefined());
        }
        return true;
    });
}

static void repairSSA(TestCommandContext& context, CommandWords& words, Session& session)
{
    const Function& function = session.function;
    auto redirects = words.parseAll("redirect"_s, pairParser(':', edgeParser(function), edgeTargetParser(function)));
    Vector<Vector<ValueID>> variables = parseVariables(words, function);
    rewriteAndPrint(context, words, session, [&] {
        for (auto [edge, target] : redirects)
            session.builder.redirectEdge(edge, target, { });
        repairVariables(session, variables);
    });
}

static void editBlock(TestCommandContext& context, CommandWords& words, Session& session, bool canEditEntry, size_t (*numberOfIndexes)(const Block&), NOESCAPE const auto& edit)
{
    auto canBeEdited = [&](BlockID block) { return canEditEntry || block != entryBlock; };
    BlockID block = words.parseRequired("block"_s, filterParser(blockParser(session.function), canBeEdited));
    BitVector indexes = words.parseRequired("indexes"_s, collectionParser<BitVector>(indexParser(block ? numberOfIndexes(session.function.at(block)) : 0)));
    rewriteAndPrint(context, words, session, [&] { edit(block, indexes); });
}

static void splitBlock(TestCommandContext& context, CommandWords& words, Session& session)
{
    editBlock(context, words, session, true, [](const Block& block) -> size_t { return block.values.size() + 1; }, [&](BlockID block, const BitVector& indexes) {
        session.builder.splitBlock(block, idsIn<uint32_t>(indexes).span());
    });
}

static void removeParameters(TestCommandContext& context, CommandWords& words, Session& session)
{
    editBlock(context, words, session, false, [](const Block& block) -> size_t { return block.parameters.size(); }, [&](BlockID block, const BitVector& indexes) {
        session.builder.removeParameters(block, indexes);
    });
}

static void eraseValues(TestCommandContext& context, CommandWords& words, Session& session)
{
    editBlock(context, words, session, true, [](const Block& block) -> size_t { return block.values.size(); }, [&](BlockID block, const BitVector& indexes) {
        session.builder.eraseValues(block, indexes);
    });
}

static void convertBranch(TestCommandContext& context, CommandWords& words, Session& session)
{
    constexpr size_t numberOfBranchTargets = 2;
    auto endsInBranch = [&](BlockID block) { return session.function.at(block).terminator.isConditionalBranch(); };
    BlockID block = words.parseRequired("block"_s, filterParser(blockParser(session.function), endsInBranch));
    uint32_t targetIndex = words.parseRequired("target"_s, indexParser(numberOfBranchTargets));
    rewriteAndPrint(context, words, session, [&] { session.builder.convertBranchToJump(block, targetIndex); });
}

static void emptyUnreachableBlocks(TestCommandContext& context, CommandWords& words, Session& session)
{
    rewriteAndPrint(context, words, session, [&] { context.out.println("emptied: ", session.builder.emptyUnreachableBlocks()); });
}

static void insertValues(TestCommandContext& context, CommandWords& words, Session& session)
{
    Vector<ProgramPoint> points = words.parseRequired("points"_s, listParser(insertionPointParser(session.function)));
    rewriteAndPrint(context, words, session, [&] {
        for (ProgramPoint point : points)
            session.builder.insertBefore(point, IRBuilder::make(Opcode::Undefined));
        session.builder.applyInsertions();
    });
}

static void substituteValues(TestCommandContext& context, CommandWords& words, Session& session)
{
    auto parseValue = valueParser(session.function);
    ValueSubstitution substitution(session.function);
    for (auto [from, to] : words.parseAll("replace"_s, pairParser(':', parseValue, parseValue))) {
        if (substitution.resolve(from) != from || substitution.resolve(to) == from) {
            words.fail("replace"_s);
            break;
        }
        substitution.replace(from, to);
    }
    bool printsFrequencies = words.parseFlag("frequencies"_s);
    rewriteAndPrint(context, words, session, [&] { substitution.apply(session.function, &session.analyses); });
    if (printsFrequencies && !words.hasFailed())
        session.analyses.blockFrequencies().dump(context.out, session.function);
}

static std::optional<FunctionAnalysis> parseSectionName(StringView name)
{
    for (FunctionAnalysis analysis : allAnalyses) {
        if (name == sectionNameOf(analysis))
            return analysis;
    }
    return std::nullopt;
}

static void dumpComputedAnalyses(PrintStream& out, const FunctionAnalyses& analyses)
{
    Vector<ASCIILiteral> names;
    for (FunctionAnalysis analysis : analyses.computed())
        names.append(sectionNameOf(analysis));
    dumpLine(out, "computed: "_s, names);
}

static void invalidateAnalyses(TestCommandContext& context, CommandWords& words, Session& session)
{
    AnalysisSet analysesToCompute = words.parseRequired("compute"_s, collectionParser<AnalysisSet>(parseSectionName));
    AnalysisSet analysesToInvalidate = words.parseRequired("invalidate"_s, collectionParser<AnalysisSet>(parseSectionName));
    if (!words.finish())
        return;

    computeAnalyses(session.analyses, analysesToCompute);
    dumpComputedAnalyses(context.out, session.analyses);
    session.analyses.invalidate(analysesToInvalidate);
    dumpComputedAnalyses(context.out, session.analyses);
}

static void addParameters(TestCommandContext& context, CommandWords& words, Session& session)
{
    const Program& program = *context.program;
    const Function& function = session.function;
    BlockID block = words.parseRequired("block"_s, edgeTargetParser(function));
    Vector<TypeID> types = words.parseAll("type"_s, printedIDParser<TypeID>(program.types.size(), [&](PrintStream& out, TypeID type) { printType(out, program, type); }));
    auto entersBlock = [&](const ArgumentOnEdge& argument) { return targetOf(function, argument.first).block == block; };
    auto parseArgument = filterParser(pairParser(':', edgeParser(function), valueParser(function)), entersBlock);
    auto fillsEveryEdgeOnce = [&](const Vector<ArgumentOnEdge>& arguments) {
        size_t numberOfEdgesFilledOnce = 0;
        size_t numberOfEdges = 0;
        for (size_t index = 0; index < function.blocks.size(); ++index) {
            forEachOutgoingEdge(function, BlockID(index), [&](Edge edge, const Target& target) {
                if (target.block != block)
                    return;
                ++numberOfEdges;
                numberOfEdgesFilledOnce += std::ranges::count(arguments, edge, &ArgumentOnEdge::first) == 1;
            });
        }
        return numberOfEdgesFilledOnce == numberOfEdges && arguments.size() == numberOfEdges;
    };
    Vector<Vector<ArgumentOnEdge>> argumentLists = words.parseAll("arguments"_s, filterParser(listParser(parseArgument), fillsEveryEdgeOnce));
    if (types.isEmpty() || argumentLists.isEmpty())
        words.failMissingArgument();
    else if (types.size() != argumentLists.size())
        words.fail("arguments"_s);

    rewriteAndPrint(context, words, session, [&] {
        for (TypeID type : types)
            session.builder.addParameter(block, type);
        for (const Vector<ArgumentOnEdge>& arguments : argumentLists) {
            for (auto [edge, value] : arguments)
                session.builder.appendArgument(edge, value);
        }
    });
}

static bool canProveOperand(const Value& test, uint32_t operandIndex)
{
    if (provesForStretch(provesOf(test.opcode)))
        return !operandIndex;
    return operandIndex < test.numberOfOperands() && test.operand(operandIndex) && test.operandKind(operandIndex) != OperandKind::Guard;
}

static void branchWithProof(TestCommandContext& context, CommandWords& words, Session& session)
{
    const Function& function = session.function;
    auto endsInUnreachable = [&](BlockID block) { return function.at(block).terminator.kind == TerminatorKind::Unreachable; };
    BlockID block = words.parseRequired("block"_s, filterParser(blockParser(function), endsInUnreachable));
    ValueID test = words.parseRequired("test"_s, filterParser(valueParser(function), [&](ValueID value) {
        return block && function.at(block).values.contains(value) && provesOf(function.at(value).opcode) != Proof::None;
    }));
    uint32_t provenOperand = words.parseOptional("operand"_s, indexParser(UINT32_MAX)).value_or(0);
    if (test && !canProveOperand(function.at(test), provenOperand))
        words.fail("operand"_s);

    rewriteAndPrint(context, words, session, [&] {
        IRBuilder& builder = session.builder;
        builder.moveToEndOf(block);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, provenOperand);
        builder.emitReturn(isGuardValue(function, branch.provenOnTaken) ? std::nullopt : std::optional(branch.provenOnTaken));
        builder.moveToEndOf(branch.notTaken);
        builder.emitUnreachable();
    });
}

static void splitEdge(TestCommandContext& context, CommandWords& words, Session& session)
{
    Edge edge = words.parseRequired("edge"_s, filterParser(edgeParser(session.function), [](Edge edge) { return !edge.isHandlerEdge(); }));
    rewriteAndPrint(context, words, session, [&] { session.builder.splitEdge(edge); });
}

static void demoteReturnCall(TestCommandContext& context, CommandWords& words, Session& session)
{
    auto endsInReturnCall = [&](BlockID block) { return session.function.at(block).terminator.kind == TerminatorKind::ReturnCall; };
    BlockID block = words.parseRequired("block"_s, filterParser(blockParser(session.function), endsInReturnCall));
    rewriteAndPrint(context, words, session, [&] { session.builder.demoteReturnCall(block); });
}

static void removeHandler(TestCommandContext& context, CommandWords& words, Session& session)
{
    auto hasHandler = [&](BlockID block) { return session.function.at(block).handler.has_value(); };
    BlockID block = words.parseRequired("block"_s, filterParser(blockParser(session.function), hasHandler));
    rewriteAndPrint(context, words, session, [&] { session.builder.removeHandler(block); });
}

static void runHeatGraph(TestCommandContext& context)
{
    Program& program = *context.program;
    CommandWords words(context);
    FlowPreparation preparation = prepareFlow(context, words);
    if (!words.finish())
        return;

    FlowResults unknownFlow;
    EffectSummaries unknownEffects;
    const FlowResults& flow = preparation.flow ? *preparation.flow : unknownFlow;
    const EffectSummaries& effects = preparation.effects ? *preparation.effects : unknownEffects;

    CallGraph callGraph = buildCallGraph(program, flow, context.build);
    HeatGraph graph = buildHeatGraph(program, callGraph, flow, effects, context.build);
    graph.dump(context.out);
    Vector<double> activations = estimateActivations(graph);
    context.out.println("activations:");
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (program.functions[index].hasBody())
            context.out.printf("f%u %.6f\n", index, activations[index]);
    }
}

static void runFunctionReferences(TestCommandContext& context)
{
    const Program& program = *context.program;
    CommandWords words(context);
    FunctionID id = words.parseRequired("function"_s, functionParser(program));
    if (!words.finish())
        return;

    forEachFunctionReference(program, program.at(id), [&](FunctionID target, FunctionReferenceKind kind) {
        context.out.println("f", target.index(), " ", nameOf(kind));
    });
}

static void runFunctionsSeeing(TestCommandContext& context)
{
    const Program& program = *context.program;
    CommandWords words(context);
    auto parseLayout = idParser<LayoutID>('L', program.layouts.size());
    auto parseField = [&](StringView text) -> std::optional<FieldReference> {
        auto field = pairParser(':', parseLayout, indexParser(UINT32_MAX))(text);
        return field && field->second < program.at(field->first).fields.size() ? std::optional(FieldReference { field->first, field->second }) : std::nullopt;
    };
    Vector<LayoutID> layouts = words.parseOptional("layouts"_s, listParser(parseLayout)).value_or(Vector<LayoutID> { });
    Vector<FieldReference> fields = words.parseOptional("fields"_s, listParser(parseField)).value_or(Vector<FieldReference> { });
    Vector<VariableID> variables = words.parseOptional("variables"_s, listParser(idParser<VariableID>('g', program.variables.size()))).value_or(Vector<VariableID> { });
    if (!words.finish())
        return;

    Vector<uint8_t> followsFieldsOfOperands(program.functions.size(), [&](size_t index) -> uint8_t {
        const Function& function = program.functions[index];
        return std::ranges::any_of(function.blocks, [&](const Block& block) {
            return describeEffects(program, function, block.terminator).followsFieldsOfOperands || std::ranges::any_of(block.values, [&](ValueID value) {
                return describeEffects(program, function, function.at(value)).followsFieldsOfOperands;
            });
        });
    });
    Vector<uint8_t> isAsked(FillWith { }, program.functions.size(), static_cast<uint8_t>(1));
    for (FunctionID id : findFunctionsSeeing(program, { layouts.span(), fields.span(), variables.span() }, followsFieldsOfOperands.span(), isAsked.span(), context.build.parallelLoops()))
        context.out.println("f", id.index());
}

std::span<const TestCommand> analysisTestCommands()
{
    static constexpr TestCommand commands[] {
        { "analyses"_s, IRForm::Prepared, runAnalyses, ProgramAsRead::TypedAndVerified },
        { "expand"_s, IRForm::Prepared, runOnFunction<expandValue>, ProgramAsRead::TypedAndVerified },
        { "cover"_s, IRForm::Prepared, runOnFunction<coverBlocks>, ProgramAsRead::TypedAndVerified },
        { "duplicate"_s, IRForm::Prepared, runOnFunction<duplicateRegion>, ProgramAsRead::TypedAndVerified },
        { "extract"_s, IRForm::Prepared, runOnFunction<extractRegion>, ProgramAsRead::TypedAndVerified },
        { "inline"_s, IRForm::Prepared, runOnFunction<inlineCall>, ProgramAsRead::TypedAndVerified },
        { "repair"_s, IRForm::Prepared, runOnFunction<repairSSA>, ProgramAsRead::TypedAndVerified },
        { "split-block"_s, IRForm::Prepared, runOnFunction<splitBlock>, ProgramAsRead::TypedAndVerified },
        { "remove-parameters"_s, IRForm::Prepared, runOnFunction<removeParameters>, ProgramAsRead::TypedAndVerified },
        { "erase-values"_s, IRForm::Prepared, runOnFunction<eraseValues>, ProgramAsRead::TypedAndVerified },
        { "convert-branch"_s, IRForm::Prepared, runOnFunction<convertBranch>, ProgramAsRead::TypedAndVerified },
        { "empty-unreachable-blocks"_s, IRForm::Prepared, runOnFunction<emptyUnreachableBlocks>, ProgramAsRead::TypedAndVerified },
        { "insert"_s, IRForm::Prepared, runOnFunction<insertValues>, ProgramAsRead::TypedAndVerified },
        { "substitute"_s, IRForm::Prepared, runOnFunction<substituteValues>, ProgramAsRead::TypedAndVerified },
        { "invalidate"_s, IRForm::Prepared, runOnFunction<invalidateAnalyses>, ProgramAsRead::TypedAndVerified },
        { "add-parameter"_s, IRForm::Prepared, runOnFunction<addParameters>, ProgramAsRead::TypedAndVerified },
        { "branch-with-proof"_s, IRForm::Prepared, runOnFunction<branchWithProof>, ProgramAsRead::TypedAndVerified },
        { "split-edge"_s, IRForm::Prepared, runOnFunction<splitEdge>, ProgramAsRead::TypedAndVerified },
        { "demote-return-call"_s, IRForm::Prepared, runOnFunction<demoteReturnCall>, ProgramAsRead::TypedAndVerified },
        { "remove-handler"_s, IRForm::Prepared, runOnFunction<removeHandler>, ProgramAsRead::TypedAndVerified },
        { "heat-graph"_s, IRForm::Prepared, runHeatGraph, ProgramAsRead::TypedAndVerified },
        { "function-references"_s, IRForm::Prepared, runFunctionReferences, ProgramAsRead::TypedAndVerified },
        { "functions-seeing"_s, IRForm::Prepared, runFunctionsSeeing, ProgramAsRead::TypedAndVerified },
        { "guards"_s, IRForm::Prepared, runGuardAvailability, ProgramAsRead::TypedAndVerified },
        { "failed-tests"_s, IRForm::Prepared, runFailedTests, ProgramAsRead::TypedAndVerified },
        { "induction"_s, IRForm::Prepared, runInductionVariables, ProgramAsRead::TypedAndVerified },
        { "frequencies"_s, IRForm::Prepared, runBlockFrequencies, ProgramAsRead::TypedAndVerified },
        { "forward-dataflow"_s, IRForm::Prepared, runForwardDataflow, ProgramAsRead::TypedAndVerified },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
