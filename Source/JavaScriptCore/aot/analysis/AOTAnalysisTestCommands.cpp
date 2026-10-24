/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAnalysisTestCommands.h"

#if ENABLE(AOT)

#include "AOTCallGraph.h"
#include "AOTEffectSummaries.h"
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
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <algorithm>
#include <wtf/EnumTraits.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class TestedBlockEdit : uint8_t { Split, RemoveParameters, EraseValues };

using FunctionCommand = void (*)(TestCommandContext&, CommandWords&, Session&);
using ArgumentOnEdge = std::pair<Edge, ValueID>;

} // anonymous namespace

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
    if (printsUseLists)
        dumpAnalyses(context.out, session.analyses, FunctionAnalysis::UseLists);
}

static bool beginRewrite(CommandWords& words, Session& session)
{
    if (!words.finish())
        return false;
    computeAnalyses(session.analyses, allAnalyses);
    return true;
}

static void finishRewrite(TestCommandContext& context, Session& session)
{
    printFunction(context.out, *context.program, session.id, session.function);
    verifyFunction(*context.program, session.id, session.function, context.form, context.build);
}

static auto valueInBlockParser(Session& session)
{
    return filterParser(valueParser(session.function), [&session](ValueID value) {
        return session.analyses.useLists().locationOf(value).placement == ValuePlacement::InBlock;
    });
}

static auto typeParser(const Program& program)
{
    return [&program](StringView text) -> std::optional<TypeID> {
        for (uint32_t index = 0; index < program.types.size(); ++index) {
            StringPrintStream printed;
            printType(printed, program, TypeID(index));
            String printedText = printed.toString();
            if (printedText == text)
                return TypeID(index);
        }
        return std::nullopt;
    };
}

static ProgramPoint pointBefore(Session& session, ValueID value)
{
    ValueLocation location = session.analyses.useLists().locationOf(value);
    return { location.block, location.index };
}

static void appendExpansionArm(IRBuilder& builder, const IRBuilder::Expansion& expansion, const Value& original, bool isReadByResultAt)
{
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
    if (!beginRewrite(words, session))
        return;

    IRBuilder& builder = session.builder;
    const Value original = session.function.at(value);
    bool isReadByResultAt = numberOfResultsOf(*context.program, session.function, original).has_value();
    ProgramPoint point = pointBefore(session, value);
    IRBuilder::Expansion expansion = reads ? builder.beginExpansion(point, reads->span()) : builder.beginExpansion(point);
    if (numberOfArms == maxNumberOfArms) {
        IRBuilder::BranchBlocks blocks = builder.emitBranch(builder.appendBoolean(true), BlockFrequency::Normal, BlockFrequency::Unlikely);
        appendExpansionArm(builder, expansion, original, isReadByResultAt);
        builder.moveToEndOf(blocks.notTaken);
    }
    appendExpansionArm(builder, expansion, original, isReadByResultAt);
    finishRewrite(context, session);
}

static void coverBlocks(TestCommandContext& context, CommandWords& words, Session& session)
{
    BlockID handler = words.parseRequired("handler"_s, edgeTargetParser(session.function));
    Vector<BlockID> blocks = words.parseRequired("blocks"_s, listParser(blockParser(session.function)));
    if (!beginRewrite(words, session))
        return;

    session.builder.coverWithHandler(blocks.span(), Target { handler, { }, { } });
    finishRewrite(context, session);
}

static void addVariables(SSARepair& repair, const Vector<Vector<ValueID>>& variables)
{
    for (const Vector<ValueID>& definitions : variables) {
        SSARepair::Variable variable = repair.addVariable(definitions.first());
        for (ValueID definition : definitions.subspan(1))
            repair.addDefinition(variable, definition);
    }
}

static void nameFirstDefinitions(Session& session, const Vector<Vector<ValueID>>& variables)
{
    ValueSubstitution substitution(session.function);
    for (const Vector<ValueID>& definitions : variables) {
        for (ValueID definition : definitions.subspan(1))
            substitution.replace(definition, definitions.first());
    }
    substitution.apply(session.function, &session.analyses);
}

static void duplicateRegion(TestCommandContext& context, CommandWords& words, Session& session)
{
    const Function& function = session.function;
    Vector<BlockID> region = words.parseRequired("region"_s, listParser(blockParser(function)));
    auto entersRegion = [&](Edge edge) { return region.contains(targetOf(function, edge).block); };
    Vector<Edge> redirectedEdges = words.parseAll("redirect"_s, filterParser(edgeParser(function), entersRegion));
    BitVector isDefinition;
    auto parseDefinition = filterParser(valueParser(function), [&](ValueID value) { return !isDefinition.set(value.index()); });
    Vector<Vector<ValueID>> variables = words.parseAll("variable"_s, listParser(parseDefinition));
    if (!beginRewrite(words, session))
        return;

    Vector<BlockID> entries;
    for (Edge edge : redirectedEdges) {
        BlockID target = targetOf(function, edge).block;
        if (region.contains(edge.from) && !entries.contains(target))
            entries.append(target);
    }
    CloneCheck check = RegionCloner::checkCanDuplicate(session.analyses, region.span(), entries.span());
    context.out.println("check: ", enumName(check));
    if (check != CloneCheck::CanClone)
        return;

    RegionCloner cloner(session.builder, function);
    cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
    for (Edge edge : redirectedEdges) {
        if (entersRegion(edge))
            session.builder.redirectEdge(edge, cloner.cloneOf(targetOf(function, edge).block), Arguments(targetOf(function, edge).arguments));
    }
    if (variables.isEmpty())
        cloner.repairSSA(session.analyses);
    else {
        nameFirstDefinitions(session, variables);
        SSARepair repair(session.builder, session.analyses);
        addVariables(repair, variables);
        cloner.addVariablesTo(repair, session.analyses);
        repair.run();
    }
    finishRewrite(context, session);
}

static bool hasEdgeLeavingRegion(const Function& function, const Vector<BlockID>& region)
{
    return std::ranges::any_of(region, [&](BlockID block) {
        return std::ranges::any_of(function.at(block).terminator.targets, [&](const Target& target) { return !region.contains(target.block); });
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
    if (!beginRewrite(words, session))
        return;

    CloneCheck check = RegionCloner::checkCanExtract(session.analyses, region.span());
    context.out.println("check: ", enumName(check));
    if (check != CloneCheck::CanClone)
        return;
    if (hasEdgeLeavingRegion(session.function, region)) {
        words.fail("region"_s);
        return;
    }

    Vector<ValueID> liveIns = RegionCloner::findLiveIns(session.function, region.span());
    dumpLine(context.out, "live-ins: "_s, liveIns);
    Function outlined = outlineRegion(session, region, liveIns);

    FunctionID outlinedID = ProgramEditor(*context.program).addFunctionLike(session.id, WTF::move(outlined));
    const Function& addedFunction = context.program->at(outlinedID);
    printFunction(context.out, *context.program, outlinedID, addedFunction);
    verifyFunction(*context.program, outlinedID, addedFunction, context.form, context.build);
}

static void demoteReturnCalls(Session& session, FunctionID id, Function& function)
{
    const AnalysisContext& context = session.analysisContext;
    IRBuilder builder(AnalysisContext { context.build, context.program, context.flow, context.effects, id }, function, session.types);
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        if (function.blocks[index].terminator.kind == TerminatorKind::ReturnCall)
            builder.demoteReturnCall(BlockID(index));
    }
}

static void inlineCall(TestCommandContext& context, CommandWords& words, Session& session)
{
    const Program& program = *context.program;
    Function& function = session.function;
    ValueID callID = words.parseRequired("value"_s, filterParser(valueInBlockParser(session), [&](ValueID id) {
        const Value& value = function.at(id);
        return value.opcode == Opcode::CallDirect && program.at(value.function()).hasBody() && !numberOfResultsOf(program, function, value);
    }));
    if (!beginRewrite(words, session))
        return;

    IRBuilder& builder = session.builder;
    const Value call = function.at(callID);
    CloneCheck check = RegionCloner::checkCanInline(program.at(call.function()));
    context.out.println("check: ", enumName(check));
    if (check != CloneCheck::CanClone)
        return;

    Function callee = program.at(call.function());
    demoteReturnCalls(session, call.function(), callee);
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
    finishRewrite(context, session);
}

static void repairSSA(TestCommandContext& context, CommandWords& words, Session& session)
{
    const Function& function = session.function;
    BitVector isDefinition;
    auto parseDefinition = filterParser(valueParser(function), [&](ValueID value) { return !isDefinition.set(value.index()); });
    auto redirects = words.parseAll("redirect"_s, pairParser(':', edgeParser(function), edgeTargetParser(function)));
    Vector<Vector<ValueID>> variables = words.parseAll("variable"_s, listParser(parseDefinition));
    if (!beginRewrite(words, session))
        return;

    for (auto [edge, target] : redirects)
        session.builder.redirectEdge(edge, target, { });
    nameFirstDefinitions(session, variables);
    SSARepair repair(session.builder, session.analyses);
    addVariables(repair, variables);
    repair.run();
    finishRewrite(context, session);
}

static size_t numberOfEditableIndexes(TestedBlockEdit edit, const Block& block)
{
    switch (edit) {
    case TestedBlockEdit::Split:
        return numberOfInsertionPoints(block);
    case TestedBlockEdit::RemoveParameters:
        return block.parameters.size();
    case TestedBlockEdit::EraseValues:
        return block.values.size();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<TestedBlockEdit edit>
static void editBlock(TestCommandContext& context, CommandWords& words, Session& session)
{
    auto canBeEdited = [](BlockID block) { return edit != TestedBlockEdit::RemoveParameters || block != entryBlock; };
    BlockID block = words.parseRequired("block"_s, filterParser(blockParser(session.function), canBeEdited));
    size_t numberOfIndexes = block ? numberOfEditableIndexes(edit, session.function.at(block)) : 0;
    BitVector indexes = words.parseRequired("indexes"_s, collectionParser<BitVector>(indexParser(numberOfIndexes)));
    if (!beginRewrite(words, session))
        return;

    switch (edit) {
    case TestedBlockEdit::Split:
        session.builder.splitBlock(block, idsIn<uint32_t>(indexes).span());
        break;
    case TestedBlockEdit::RemoveParameters:
        session.builder.removeParameters(block, indexes);
        break;
    case TestedBlockEdit::EraseValues:
        session.builder.eraseValues(block, indexes);
        break;
    }
    finishRewrite(context, session);
}

static void convertBranch(TestCommandContext& context, CommandWords& words, Session& session)
{
    constexpr size_t numberOfBranchTargets = 2;
    auto endsInBranch = [&](BlockID block) { return session.function.at(block).terminator.isConditionalBranch(); };
    BlockID block = words.parseRequired("block"_s, filterParser(blockParser(session.function), endsInBranch));
    uint32_t targetIndex = words.parseRequired("target"_s, indexParser(numberOfBranchTargets));
    if (!beginRewrite(words, session))
        return;

    session.builder.convertBranchToJump(block, targetIndex);
    finishRewrite(context, session);
}

static void emptyUnreachableBlocks(TestCommandContext& context, CommandWords& words, Session& session)
{
    if (!beginRewrite(words, session))
        return;

    size_t numberOfEmptiedBlocks = session.builder.emptyUnreachableBlocks();
    context.out.println("emptied: ", numberOfEmptiedBlocks);
    finishRewrite(context, session);
}

static void insertValues(TestCommandContext& context, CommandWords& words, Session& session)
{
    Vector<ProgramPoint> points = words.parseRequired("points"_s, listParser(insertionPointParser(session.function)));
    if (!beginRewrite(words, session))
        return;

    for (ProgramPoint point : points)
        session.builder.insertBefore(point, IRBuilder::make(Opcode::Undefined));
    session.builder.applyInsertions();
    finishRewrite(context, session);
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
    if (!beginRewrite(words, session))
        return;

    substitution.apply(session.function, &session.analyses);
    finishRewrite(context, session);
    if (printsFrequencies)
        session.analyses.blockFrequencies().dump(context.out, session.function);
}

static void dumpComputedAnalyses(PrintStream& out, const FunctionAnalyses& analyses)
{
    Vector<FunctionAnalysis, 16> computedAnalyses;
    for (FunctionAnalysis analysis : analyses.computed())
        computedAnalyses.append(analysis);
    out.print("computed: ");
    dumpList(out, computedAnalyses.span(), [](PrintStream& out, FunctionAnalysis analysis) { out.print(sectionNameOf(analysis)); });
    out.print("\n");
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

static bool haveDistinctEdges(const Vector<ArgumentOnEdge>& arguments)
{
    for (size_t index = 0; index < arguments.size(); ++index) {
        for (size_t earlier = 0; earlier < index; ++earlier) {
            if (arguments[earlier].first == arguments[index].first)
                return false;
        }
    }
    return true;
}

static size_t numberOfEdgesInto(const Function& function, BlockID block)
{
    size_t numberOfEdges = 0;
    for (size_t index = 0; index < function.blocks.size(); ++index) {
        forEachOutgoingEdge(function, BlockID(index), [&](Edge, const Target& target) {
            if (target.block == block)
                ++numberOfEdges;
        });
    }
    return numberOfEdges;
}

static void addParameters(TestCommandContext& context, CommandWords& words, Session& session)
{
    const Function& function = session.function;
    BlockID block = words.parseRequired("block"_s, edgeTargetParser(function));
    Vector<TypeID> types = words.parseAll("type"_s, typeParser(*context.program));
    auto entersBlock = [&](const ArgumentOnEdge& argument) { return targetOf(function, argument.first).block == block; };
    auto parseArgument = filterParser(pairParser(':', edgeParser(function), valueParser(function)), entersBlock);
    auto fillsEveryEdgeOnce = [&](const Vector<ArgumentOnEdge>& arguments) {
        return haveDistinctEdges(arguments) && arguments.size() == numberOfEdgesInto(function, block);
    };
    Vector<Vector<ArgumentOnEdge>> argumentLists = words.parseAll("arguments"_s, filterParser(listParser(parseArgument), fillsEveryEdgeOnce));
    if (types.isEmpty() || argumentLists.isEmpty())
        words.failMissingArgument();
    else if (types.size() != argumentLists.size())
        words.fail("arguments"_s);
    if (!beginRewrite(words, session))
        return;

    for (TypeID type : types)
        session.builder.addParameter(block, type);
    for (const Vector<ArgumentOnEdge>& arguments : argumentLists) {
        for (auto [edge, value] : arguments)
            session.builder.appendArgument(edge, value);
    }
    finishRewrite(context, session);
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
    if (!beginRewrite(words, session))
        return;

    IRBuilder& builder = session.builder;
    builder.moveToEndOf(block);
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, provenOperand);
    builder.emitReturn(isGuardValue(function, branch.provenOnTaken) ? std::nullopt : std::optional(branch.provenOnTaken));
    builder.moveToEndOf(branch.notTaken);
    builder.emitUnreachable();
    finishRewrite(context, session);
}

static void splitEdge(TestCommandContext& context, CommandWords& words, Session& session)
{
    auto isNormalEdge = [](Edge edge) { return !edge.isHandlerEdge(); };
    Edge edge = words.parseRequired("edge"_s, filterParser(edgeParser(session.function), isNormalEdge));
    if (!beginRewrite(words, session))
        return;

    session.builder.splitEdge(edge);
    finishRewrite(context, session);
}

static void demoteReturnCall(TestCommandContext& context, CommandWords& words, Session& session)
{
    auto endsInReturnCall = [&](BlockID block) { return session.function.at(block).terminator.kind == TerminatorKind::ReturnCall; };
    BlockID block = words.parseRequired("block"_s, filterParser(blockParser(session.function), endsInReturnCall));
    if (!beginRewrite(words, session))
        return;

    session.builder.demoteReturnCall(block);
    finishRewrite(context, session);
}

static void removeHandler(TestCommandContext& context, CommandWords& words, Session& session)
{
    auto hasHandler = [&](BlockID block) { return session.function.at(block).handler.has_value(); };
    BlockID block = words.parseRequired("block"_s, filterParser(blockParser(session.function), hasHandler));
    if (!beginRewrite(words, session))
        return;

    session.builder.removeHandler(block);
    finishRewrite(context, session);
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
    Vector<LayoutID> layouts = words.parseOptional("layouts"_s, listParser(idParser<LayoutID>('L', program.layouts.size()))).value_or(Vector<LayoutID> { });
    auto fieldsAsParsed = words.parseOptional("fields"_s, listParser(pairParser(':', idParser<LayoutID>('L', program.layouts.size()), indexParser(UINT32_MAX))));
    Vector<VariableID> variables = words.parseOptional("variables"_s, listParser(idParser<VariableID>('g', program.variables.size()))).value_or(Vector<VariableID> { });
    if (!words.finish())
        return;

    Vector<FieldReference> fields;
    if (fieldsAsParsed) {
        for (auto [layout, field] : *fieldsAsParsed) {
            if (field >= program.at(layout).fields.size()) {
                context.out.println("no such field");
                return;
            }
            fields.append({ layout, static_cast<uint32_t>(field) });
        }
    }

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
        { "split-block"_s, IRForm::Prepared, runOnFunction<editBlock<TestedBlockEdit::Split>>, ProgramAsRead::TypedAndVerified },
        { "remove-parameters"_s, IRForm::Prepared, runOnFunction<editBlock<TestedBlockEdit::RemoveParameters>>, ProgramAsRead::TypedAndVerified },
        { "erase-values"_s, IRForm::Prepared, runOnFunction<editBlock<TestedBlockEdit::EraseValues>>, ProgramAsRead::TypedAndVerified },
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
