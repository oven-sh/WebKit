/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVirtualObjectParameters.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTExactBuffer.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTInlineCopy.h"
#include "AOTRegionCloner.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTThreads.h"
#include "AOTVirtualObjectShapes.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FreshArgument {
    FunctionID callee;
    uint32_t parameter { 0 };
};

struct Promotion {
    FunctionID id;
    uint32_t parameter { 0 };
    Function split;
    Function body;
    BlockID head;
    Vector<ValueID, 6> promoted;
    Origin callOrigin;
};

enum class AliasUse : uint8_t { Unknown, Cast, Read, Test };

struct Aliases {
    BitVector values;
    BitVector narrowingBlocks;
    Origin firstRead;
};

struct EarlyBlocks {
    BitVector blocks;
    Vector<ProgramPoint, 2> cuts;
};

struct BodyRegion {
    BlockID head;
    Vector<BlockID> blocks;
};

using AliasWorklist = Vector<ValueID, 8>;

class FreshArgumentCollector {
public:
    FreshArgumentCollector(const AnalysisContext&, Vector<FreshArgument>&);

    void run();

private:
    bool isFreshObject(ValueID);
    void visitCall(const Value& call);

    const AnalysisContext m_context;
    const Function& m_caller;
    Vector<FreshArgument>& m_freshArguments;
    std::optional<FunctionAnalyses> m_analyses;
};

class ParameterPromoter {
public:
    ParameterPromoter(const AnalysisContext&, TypeArena&);

    std::optional<Promotion> run(const BitVector& freshParameters);

private:
    AliasUse classifyUse(const Use&) const;
    AliasUse classifyTest(const Value& test, uint32_t operandIndex) const;
    void addNarrowedAliases(ValueID test, Aliases&, AliasWorklist&);
    void addNarrowedAliasesOnEdge(Edge, Aliases&, AliasWorklist&) const;
    std::optional<Aliases> findAliases(ValueID parameter);
    std::optional<EarlyBlocks> findEarlyBlocks(const Aliases&, const Subject&);
    std::optional<Promotion> tryPromote(uint32_t parameter);

    const AnalysisContext m_context;
    const Function& m_function;
    TypeArena& m_types;
    FunctionAnalyses m_analyses;
};

} // anonymous namespace

static constexpr OpcodeSet objectTests { Opcode::ToBoolean, Opcode::TypeTest, Opcode::ShapeTest, Opcode::IsSlotFilled, Opcode::PropHas, Opcode::PropHasOwn };
static constexpr OptionSet<FunctionCapabilities> inheritedCapabilities { FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost };

static_assert((evaluatedTests | objectTests) == evaluatedTests);
static_assert(evaluatedTests.contains(Opcode::StrictEq));

FreshArgumentCollector::FreshArgumentCollector(const AnalysisContext& context, Vector<FreshArgument>& freshArguments)
    : m_context(context)
    , m_caller(context.program.at(context.id))
    , m_freshArguments(freshArguments)
{
}

bool FreshArgumentCollector::isFreshObject(ValueID id)
{
    ValueID definition = sameObjectThroughCasts(m_caller, id);
    if (interpretedOf(m_caller.at(definition).opcode) != Interpretation::Allocation)
        return false;
    if (!m_analyses)
        m_analyses.emplace(m_context, m_caller);
    DescribedObject described = describeVirtualObject(*m_analyses, definition);
    return described.support == VirtualObjectSupport::Supported && described.description.kind == VirtualObjectKind::Object;
}

void FreshArgumentCollector::visitCall(const Value& call)
{
    Vector<uint32_t, 4> freshOperands;
    call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (isFreshObject(operand))
            freshOperands.append(operandIndex);
    });
    if (freshOperands.isEmpty())
        return;

    const Program& program = m_context.program;
    CalleeList callees = calleesOfCall(program, call);
    for (FunctionID id : callees.functions) {
        const Function& callee = program.at(id);
        for (uint32_t operandIndex : freshOperands) {
            std::optional<uint32_t> entryParameter = entryParameterOf(callee, call, operandIndex);
            if (entryParameter && callee.entryParameterKind(*entryParameter) == EntryParameter::Parameter)
                m_freshArguments.append(FreshArgument { id, *entryParameter - callee.firstParameterIndex() });
        }
    }
}

void FreshArgumentCollector::run()
{
    static const OpcodeSet calls = [] {
        OpcodeSet result;
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (effectRuleOf(opcode) == EffectRule::Call)
                result.add(opcode);
        }
        return result;
    }();
    static const OpcodeSet allocations = [] {
        OpcodeSet result;
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (interpretedOf(opcode) == Interpretation::Allocation)
                result.add(opcode);
        }
        return result;
    }();
    // A fresh object is one that the caller allocates.
    if (!mayHoldValueWith(m_context.program, m_context.id, allocations))
        return;
    forEachListedValue(m_context.program, m_context.id, calls, [&](BlockID, uint32_t, ValueID, const Value& call) {
        visitCall(call);
    });
}

static Vector<BitVector> findFreshParameters(const Program& program, const OptimizationInputs& inputs, BuildContext& build)
{
    unsigned count = program.functions.size();
    Vector<FreshArgument> freshArguments = collectInParallel<FreshArgument>(build.parallelLoops(), count, [&](unsigned index, Vector<FreshArgument>& found) {
        FunctionID id(index);
        if (hasBody(program, id))
            FreshArgumentCollector(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, found).run();
    });

    Vector<BitVector> freshParameters(count);
    for (const FreshArgument& argument : freshArguments)
        freshParameters[argument.callee.index()].set(argument.parameter);
    return freshParameters;
}

static bool mayPromoteParameters(const Function& function)
{
    constexpr OptionSet<FunctionFlags> excludedFlags { FunctionFlags::HasRestParameter, FunctionFlags::IsSloppy, FunctionFlags::TakesCallee, FunctionFlags::HasNewTarget };
    if (!function.hasBody() || !function.mayChangeSignature() || !function.mayBeCloned() || !function.hasOrdinaryFrame())
        return false;
    if (function.results.size() != 1 || function.constructedClass || function.hasAny(excludedFlags) || function.isHiddenInStackTraces())
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
        return true;
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ParameterPromoter::ParameterPromoter(const AnalysisContext& context, TypeArena& types)
    : m_context(context)
    , m_function(context.program.at(context.id))
    , m_types(types)
    , m_analyses(context, m_function)
{
}

AliasUse ParameterPromoter::classifyTest(const Value& test, uint32_t operandIndex) const
{
    if (test.opcode == Opcode::StrictEq) {
        uint32_t otherIndex = operandIndex == OperandIndex::StrictEq::left ? OperandIndex::StrictEq::right : OperandIndex::StrictEq::left;
        std::optional<VirtualConstant> other = virtualConstantOf(m_context.program, m_function, test.operand(otherIndex));
        bool isComparedWithNullish = other == VirtualConstant::Undefined || other == VirtualConstant::Null;
        return isComparedWithNullish ? AliasUse::Test : AliasUse::Unknown;
    }
    return !operandIndex && objectTests.contains(test.opcode) ? AliasUse::Test : AliasUse::Unknown;
}

AliasUse ParameterPromoter::classifyUse(const Use& use) const
{
    if (use.kind != Use::Kind::Operand)
        return AliasUse::Unknown;
    const Value& user = m_function.at(use.user);
    switch (interpretedOf(user.opcode)) {
    case Interpretation::Cast:
        return use.operandIndex ? AliasUse::Unknown : AliasUse::Cast;
    case Interpretation::Read:
        return accessOf(user.opcode) == Access::Read && receiverOf(user.opcode) == use.operandIndex ? AliasUse::Read : AliasUse::Unknown;
    case Interpretation::Test:
        return classifyTest(user, use.operandIndex);
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Store:
    case Interpretation::List:
    case Interpretation::Guards:
        return AliasUse::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ParameterPromoter::addNarrowedAliasesOnEdge(Edge edge, Aliases& aliases, AliasWorklist& worklist) const
{
    auto& parameters = m_function.at(targetOf(m_function, edge).block).parameters;
    uint32_t firstParameter = firstEdgeResultParameter(m_function, edge.from, edge.targetIndex);
    uint32_t numberOfResults = numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
        EdgeResult result = edgeResultAt(m_function, { edge.from, edge.targetIndex, resultIndex });
        if (!result.subject || !aliases.values.get(result.subject.index()))
            continue;

        ValueID parameter = parameters[firstParameter + resultIndex];
        switch (result.kind) {
        case EdgeResultKind::NarrowedOperand:
            worklist.append(parameter);
            break;
        case EdgeResultKind::Guard:
            aliases.values.quickSet(parameter.index());
            break;
        case EdgeResultKind::DeclaredResult:
            continue;
        }
        aliases.narrowingBlocks.quickSet(edge.from.index());
    }
}

void ParameterPromoter::addNarrowedAliases(ValueID test, Aliases& aliases, AliasWorklist& worklist)
{
    for (const Use& use : m_analyses.useLists().usesOf(test)) {
        if (use.kind != Use::Kind::TerminatorOperand)
            continue;
        const Terminator& terminator = m_function.at(use.block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex)
            addNarrowedAliasesOnEdge(Edge { use.block, targetIndex }, aliases, worklist);
    }
}

std::optional<Aliases> ParameterPromoter::findAliases(ValueID parameter)
{
    const UseLists& useLists = m_analyses.useLists();
    Aliases aliases { BitVector(m_function.values.size()), BitVector(m_function.blocks.size()), { } };
    std::optional<Use> firstRead;
    AliasWorklist worklist { parameter };
    while (!worklist.isEmpty()) {
        ValueID alias = worklist.takeLast();
        if (aliases.values.quickSet(alias.index()))
            continue;
        for (const Use& use : useLists.usesOf(alias)) {
            switch (classifyUse(use)) {
            case AliasUse::Unknown:
                return std::nullopt;
            case AliasUse::Cast:
                worklist.append(use.user);
                break;
            case AliasUse::Read:
                if (!firstRead || useLists.isBefore(use, *firstRead))
                    firstRead = use;
                break;
            case AliasUse::Test:
                addNarrowedAliases(use.user, aliases, worklist);
                break;
            }
        }
    }
    if (!firstRead)
        return std::nullopt;
    aliases.firstRead = m_function.at(firstRead->user).origin();
    return aliases;
}

std::optional<EarlyBlocks> ParameterPromoter::findEarlyBlocks(const Aliases& aliases, const Subject& subject)
{
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();
    const BlockCycles& cycles = m_analyses.blockCycles();
    LivenessOptions livenessOptions;
    livenessOptions.trackedValues = &aliases.values;
    Liveness liveness(m_function, cfg, useLists, livenessOptions);

    EarlyBlocks early { aliases.narrowingBlocks, { } };
    BitVector hasTerminatorUse = aliases.narrowingBlocks;
    Vector<uint32_t> indexAfterLastUse(FillWith { }, m_function.blocks.size(), 0u);
    for (size_t alias : aliases.values) {
        BlockID definingBlock = useLists.blockOf(ValueID(alias));
        if (definingBlock != entryBlock)
            early.blocks.quickSet(definingBlock.index());
        for (const Use& use : useLists.usesOf(ValueID(alias))) {
            early.blocks.quickSet(use.block.index());
            switch (use.kind) {
            case Use::Kind::Operand: {
                uint32_t& index = indexAfterLastUse[use.block.index()];
                index = std::max(index, useLists.locationOf(use.user).index + 1);
                break;
            }
            case Use::Kind::TerminatorOperand:
            case Use::Kind::EdgeArgument:
                hasTerminatorUse.quickSet(use.block.index());
                break;
            case Use::Kind::HandlerArgument:
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    for (BlockID block : cfg.reversePostOrder()) {
        if (!liveness.liveAtHead(block).empty())
            early.blocks.quickSet(block.index());
    }

    auto isAliasLiveOut = [&](BlockID block) {
        return std::ranges::any_of(cfg.successors(block), [&](BlockID successor) {
            return !liveness.liveAtHead(successor).empty();
        });
    };
    auto castsEarlyValue = [&](ValueID id) {
        const Value& value = m_function.at(id);
        return isCast(value.opcode) && early.blocks.get(useLists.blockOf(value.operand(0)).index());
    };
    auto hasTailCallBefore = [&](BlockID block, uint32_t cut) {
        const Terminator& terminator = m_function.at(block).terminator;
        return terminator.kind == TerminatorKind::ReturnCall && useLists.locationOf(terminator.callOperand()).index < cut;
    };

    size_t numberOfEarlyValues = 0;
    for (size_t index : early.blocks) {
        BlockID block(index);
        if (cycles.isInCycle(block) || m_function.at(block).handler)
            return std::nullopt;

        const BlockValues& values = m_function.at(block).values;
        uint32_t cut = indexAfterLastUse[index];
        while (cut < values.size() && castsEarlyValue(values[cut]))
            ++cut;
        if (hasTerminatorUse.quickGet(index) || isAliasLiveOut(block) || hasTailCallBefore(block, cut))
            cut = values.size();
        if (cut < values.size())
            early.cuts.append(ProgramPoint { block, cut });
        numberOfEarlyValues += cut;
    }

    if (m_context.build.exceeds(LimitID::MaxParameterWrapperValues, numberOfEarlyValues, subject))
        return std::nullopt;
    return early;
}

static void cutBlocks(const AnalysisContext& context, TypeArena& types, Function& function, std::span<const ProgramPoint> cuts)
{
    IRBuilder builder(context, function, types);
    for (ProgramPoint cut : cuts) {
        builder.setOrigin(function.at(function.at(cut.block).values[cut.index]).origin());
        builder.splitBlock(cut);
    }
}

static size_t countValues(const Function& function, const BitVector& blocks)
{
    size_t count = 0;
    for (size_t block : blocks)
        count += function.blocks[block].values.size();
    return count;
}

static std::optional<BodyRegion> findBody(FunctionAnalyses& analyses, const BitVector& earlyBlocks, const Subject& subject)
{
    const Function& function = analyses.function();
    const CFG& cfg = analyses.cfg();

    BitVector exits(function.blocks.size());
    for (size_t early : earlyBlocks) {
        for (BlockID successor : cfg.successors(BlockID(early))) {
            if (!earlyBlocks.get(successor.index()))
                exits.quickSet(successor.index());
        }
    }

    BlockID head;
    BitVector isInBody;
    size_t bodySize = 0;
    for (size_t index : exits) {
        BlockID candidate(index);
        BitVector reachableBlocks = cfg.blocksReachableFrom(candidate, { });
        size_t size = countValues(function, reachableBlocks);
        if (head && (size < bodySize || (size == bodySize && cfg.reversePostOrderIndex(candidate) > cfg.reversePostOrderIndex(head))))
            continue;
        head = candidate;
        isInBody = WTF::move(reachableBlocks);
        bodySize = size;
    }
    if (!head)
        return std::nullopt;

    for (size_t early : earlyBlocks) {
        if (isInBody.get(early))
            return std::nullopt;
    }

    BodyRegion body { head, { } };
    size_t startSize = 0;
    for (BlockID block : cfg.reversePostOrder()) {
        if (!isInBody.get(block.index())) {
            startSize += function.at(block).values.size();
            continue;
        }
        body.blocks.append(block);
        if (block == head)
            continue;
        for (Edge edge : cfg.incomingEdges(block)) {
            if (!isInBody.get(edge.from.index()))
                return std::nullopt;
        }
    }
    if (analyses.context().build.exceeds(LimitID::MaxParameterWrapperValues, startSize, subject))
        return std::nullopt;
    return body;
}

static std::optional<Vector<ValueID, 6>> findPromotedValues(FunctionAnalyses& analyses, const BodyRegion& body, const BitVector& aliases, const Subject& subject)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    if (RegionCloner::checkCanExtract(analyses, body.blocks.span()) != CloneCheck::CanClone)
        return std::nullopt;

    const UseLists& useLists = analyses.useLists();
    Vector<ValueID, 6> promoted;
    for (ValueID liveIn : RegionCloner::findLiveIns(function, body.blocks.span())) {
        RELEASE_ASSERT(!aliases.get(liveIn.index()));
        ValueLocation location = useLists.locationOf(liveIn);
        if (location.placement != ValuePlacement::Parameter || location.block != entryBlock)
            promoted.append(liveIn);
    }
    promoted.append(function.at(body.head).parameters.span());

    for (ValueID id : promoted) {
        if (!program.typeOf(function, id).isJavaScriptValue())
            return std::nullopt;
    }
    if (analyses.context().build.exceeds(LimitID::MaxPromotedParameters, promoted.size(), subject))
        return std::nullopt;
    return promoted;
}

static DeclaredType promotedParameterType(const Value& value)
{
    if (value.expected() && value.type == TypeTable::anything)
        return { .type = value.expected(), .flags = PositionFlags::IsExpected };
    return { .type = value.type };
}

static Function createBodySignature(const Promotion& promotion)
{
    const Function& function = promotion.split;
    Function body;
    body.name = function.name;
    body.module = function.module;
    body.position = function.position;
    body.text = function.text;
    body.kind = function.kind;
    body.role = FunctionRole::Source;
    body.flags = function.flags;
    body.length = function.length;
    body.enclosingRuntimeFunction = function.enclosingRuntimeFunction;
    body.captures = function.captures;
    body.thisType = function.thisType;
    body.parameters = function.parameters;
    body.results = function.results;
    body.capabilities = initialCapabilitiesOf(body) | FunctionCapabilities::HasOnlyDirectCallers;
    body.derivation.source = promotion.id;
    body.inlineFrames = function.inlineFrames;

    body.parameters.removeAt(promotion.parameter);
    for (ValueID id : promotion.promoted)
        body.parameters.append(promotedParameterType(function.at(id)));
    return body;
}

static Function createBody(const AnalysisContext& context, TypeArena& types, const Promotion& promotion, const BodyRegion& region)
{
    const Function& split = promotion.split;
    Function body = createBodySignature(promotion);
    IRBuilder builder(context, body, types);
    RegionCloner cloner(builder, split);
    BlockID entry = builder.addBlock();
    auto addEntryParameterLike = [&](ValueID inSplit, std::optional<uint32_t> original) {
        const Value& value = split.at(inSplit);
        builder.setOrigin(value.origin());
        ValueID added = builder.addParameter(entry, value.type);
        body.edit(added, ValueEdit::Type).setExpected(value.expected());
        body.derivation.parameterMap.append(ParameterOrigin { original });
        return added;
    };

    auto& entryParameters = split.at(entryBlock).parameters;
    auto copyEntryParameter = [&](uint32_t index) {
        cloner.mapValue(entryParameters[index], addEntryParameterLike(entryParameters[index], index));
    };
    uint32_t aggregate = split.firstParameterIndex() + promotion.parameter;
    std::optional<unsigned> hiddenRest = split.hiddenRestParameterIndex();
    for (uint32_t index = 0; index < entryParameters.size(); ++index) {
        if (index != aggregate && index != hiddenRest)
            copyEntryParameter(index);
    }

    auto& headParameters = split.at(region.head).parameters;
    Arguments headArguments;
    for (ValueID value : promotion.promoted) {
        ValueID added = addEntryParameterLike(value, std::nullopt);
        if (headParameters.contains(value))
            headArguments.append(added);
        else
            cloner.mapValue(value, added);
    }
    if (hiddenRest)
        copyEntryParameter(*hiddenRest);
    RELEASE_ASSERT(body.at(entryBlock).parameters.size() == body.numberOfEntryParameters());

    cloner.cloneBlocks(region.blocks.span(), [](ValueID, Value&) { });
    builder.setOrigin({ body.position, 0 });
    builder.moveToEndOf(entry);
    builder.emitJump(cloner.cloneOf(region.head), WTF::move(headArguments));
    return body;
}

std::optional<Promotion> ParameterPromoter::tryPromote(uint32_t parameter)
{
    ValueID aggregate = m_function.at(entryBlock).parameters[m_function.firstParameterIndex() + parameter];
    Subject subject = m_analyses.subject(aggregate);
    std::optional<Aliases> aliases = findAliases(aggregate);
    if (!aliases)
        return std::nullopt;
    std::optional<EarlyBlocks> early = findEarlyBlocks(*aliases, subject);
    if (!early)
        return std::nullopt;

    Promotion promotion;
    promotion.id = m_context.id;
    promotion.parameter = parameter;
    promotion.split = m_function;
    promotion.callOrigin = { aliases->firstRead.inlineFrame ? m_function.position : aliases->firstRead.position, 0 };
    cutBlocks(m_context, m_types, promotion.split, early->cuts.span());

    FunctionAnalyses analyses(m_context, promotion.split);
    std::optional<BodyRegion> region = findBody(analyses, early->blocks, subject);
    if (!region)
        return std::nullopt;
    auto promoted = findPromotedValues(analyses, *region, aliases->values, subject);
    if (!promoted)
        return std::nullopt;

    promotion.head = region->head;
    promotion.promoted = WTF::move(*promoted);
    promotion.body = createBody(m_context, m_types, promotion, *region);
    return promotion;
}

std::optional<Promotion> ParameterPromoter::run(const BitVector& freshParameters)
{
    for (size_t parameter : freshParameters) {
        if (auto promotion = tryPromote(parameter))
            return promotion;
    }
    return std::nullopt;
}

static Vector<Promotion> findPromotions(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build, std::span<const FunctionID> candidates, const Vector<BitVector>& freshParameters)
{
    const Program& program = editor.program();
    Vector<std::optional<Promotion>> found(candidates.size());
    InterningPhases phases(editor, candidates.size());
    forEachIndexInParallel(build.parallelLoops(), candidates.size(), [&](unsigned task) {
        FunctionID id = candidates[task];
        TaskArenas arenas(phases, task);
        AnalysisContext context { build, program, &inputs.flow, &inputs.effects, id };
        found[task] = ParameterPromoter(context, arenas.types).run(freshParameters[id.index()]);
    });
    BodyRemaps remaps = phases.finish();

    Vector<Promotion> promotions;
    for (std::optional<Promotion>& promotion : found) {
        if (!promotion)
            continue;
        applyRemaps(remaps, promotion->split);
        applyRemaps(remaps, promotion->body);
        promotions.append(WTF::move(*promotion));
    }
    return promotions;
}

static void emitReturnOfBodyCall(IRBuilder& builder, FunctionID body, ValueID call)
{
    const Program& program = builder.context().program;
    if (program.typeOf(builder.function(), call).isNever()) {
        builder.emitUnreachable();
        return;
    }
    Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { call });
    terminator.setRequirement(tailCallRequirementOf(program.at(body)));
    builder.emitTerminator(WTF::move(terminator));
}

// A read in the wrapper may throw, or run a getter or a trap. A stack that is captured there shows the function: the wrapper is hidden, so all it does but the call of the body lies in a frame of the body.
static void showOperationsInFrameOfBody(Function& wrapper, FunctionID body)
{
    auto isCallOfBody = [&](ValueID id) {
        const Value& value = wrapper.at(id);
        return value.opcode == Opcode::CallDirect && value.function() == body;
    };
    bool mayCaptureStack = std::ranges::any_of(wrapper.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return mayThrow(wrapper.at(id).opcode) && !isCallOfBody(id);
        });
    });
    if (!mayCaptureStack)
        return;

    Vector<InlineFrame> frames;
    frames.reserveInitialCapacity(inlineFrameCountOf(wrapper) + 1);
    frames.append(InlineFrame { });
    frames.append(InlineFrame { .callee = body, .callPosition = wrapper.position, .module = wrapper.module });
    for (size_t index = 1; index < wrapper.inlineFrames.size(); ++index) {
        InlineFrame frame = wrapper.inlineFrames[index];
        ++frame.parent;
        frames.append(frame);
    }
    wrapper.inlineFrames = WTF::move(frames);

    for (Block& block : wrapper.editAllBlocks()) {
        for (ValueID id : block.values) {
            if (!isCallOfBody(id))
                ++wrapper.edit(id).inlineFrame;
        }
        if (block.terminator.kind != TerminatorKind::ReturnCall || !isCallOfBody(block.terminator.callOperand()))
            ++block.terminator.inlineFrame;
    }
}

static void convertToWrapper(const AnalysisContext& context, TypeArena& types, Promotion& promotion, FunctionID body)
{
    Function& wrapper = promotion.split;
    Block& head = wrapper.edit(promotion.head);
    head.values.clear();
    head.terminator = Terminator { };
    head.handler = std::nullopt;

    Vector<ValueID, 8> entryParameters(wrapper.at(entryBlock).parameters.span());
    auto entryParameterAt = [&](std::optional<unsigned> index) {
        return index ? entryParameters[*index] : ValueID();
    };
    Vector<ValueID, 8> arguments(entryParameters.subspan(wrapper.firstParameterIndex(), wrapper.parameters.size()));
    arguments.removeAt(promotion.parameter);
    arguments.appendVector(promotion.promoted);

    IRBuilder builder(context, wrapper, types);
    builder.setOrigin(promotion.callOrigin);
    builder.moveToEndOf(promotion.head);
    ValueID call = appendDirectCall(builder, {
        .callee = body,
        .captures = entryParameters.span().first(wrapper.captures.size()),
        .thisValue = entryParameterAt(wrapper.thisParameterIndex()),
        .calleeValue = entryParameterAt(wrapper.calleeParameterIndex()),
        .newTarget = entryParameterAt(wrapper.newTargetParameterIndex()),
        .arguments = arguments.span(),
        .argumentCount = entryParameterAt(wrapper.argumentCountParameterIndex()),
        .hiddenRest = entryParameterAt(wrapper.hiddenRestParameterIndex()),
    });
    emitReturnOfBodyCall(builder, body, call);

    wrapper.role = FunctionRole::ParameterWrapper;
    wrapper.creator = CreatorRecord::ParameterWrapper { body, promotion.parameter };
    wrapper.capabilities = (initialCapabilitiesOf(wrapper) - inheritedCapabilities) | (wrapper.capabilities & inheritedCapabilities);
    compactFunction(wrapper);
    moveToExactBuffer(wrapper.storageOfBlocks());
    showOperationsInFrameOfBody(wrapper, body);
}

static void replaceWithWrappers(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build, Vector<Promotion>& promotions)
{
    FunctionID firstBody = editor.reserveFunctions(promotions.size());
    Vector<FunctionID> wrappers;
    Vector<FunctionID> bodies;
    wrappers.reserveInitialCapacity(promotions.size());
    bodies.reserveInitialCapacity(promotions.size());
    for (size_t index = 0; index < promotions.size(); ++index) {
        wrappers.append(promotions[index].id);
        bodies.append(FunctionID(firstBody.index() + index));
    }

    const Program& program = editor.program();
    editFunctionsInParallel(editor, build, wrappers.span(), bodies.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        Promotion& promotion = promotions[index];
        editor.at(bodies[index]) = WTF::move(promotion.body);
        convertToWrapper(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, arenas.types, promotion, bodies[index]);
        function = WTF::move(promotion.split);
    });
}

void promoteAggregateParameters(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build)
{
    const Program& program = editor.program();
    Vector<BitVector> freshParameters = findFreshParameters(program, inputs, build);
    Vector<FunctionID> candidates = collectInParallel<FunctionID>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
        FunctionID id(index);
        if (!freshParameters[index].isEmpty() && mayPromoteParameters(program.at(id)) && build.isEnabled(PassID::PromoteAggregateParameters, id))
            found.append(id);
    });
    if (candidates.isEmpty())
        return;

    Vector<Promotion> promotions = findPromotions(editor, inputs, build, candidates.span(), freshParameters);
    if (promotions.isEmpty())
        return;

    replaceWithWrappers(editor, inputs, build, promotions);
    build.statistics().count(StatisticID::AggregateParametersPromoted, promotions.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
