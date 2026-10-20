/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegionCloner.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTSSARepair.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class RegionBorder {
    WTF_MAKE_NONCOPYABLE(RegionBorder);
public:
    RegionBorder(FunctionAnalyses& analyses, bool separatesFunctions)
        : m_analyses(analyses)
        , m_program(analyses.program())
        , m_function(analyses.function())
        , m_cfg(analyses.cfg())
        , m_useLists(analyses.useLists())
        , m_isInRegion(m_function.blocks.size())
        , m_separatesFunctions(separatesFunctions)
    {
    }

    bool add(BlockID block)
    {
        return block != m_cfg.root() && m_cfg.isReachable(block) && !m_isInRegion.set(block.index());
    }

    bool addEntry(const Liveness&, BlockID);

    CloneCheck check(BlockID) const;

private:
    bool contains(BlockID block) const { return m_isInRegion.get(block.index()); }
    bool isDefinedOutside(ValueID) const;

    bool namesOnlyOutsideValues(const GuardOrigin&) const;
    bool isAboutOutsideValues(const GuardFact&) const;
    bool provesSameOnceMerged(ValueID guard) const;
    bool acceptsParameter(OperandKind, ValueID used) const;
    bool acceptsParameter(const Use&, ValueID used) const;
    bool canBecomeParameter(ValueID) const;
    bool passesProofsOf(BlockID from, ValueID test) const;

    bool isSeparatedFromDefinition(const Use&, ValueID definition) const;
    bool canBeSeparatedFromUses(ValueID definition) const;
    bool canFillOutsideParameters(BlockID from) const;
    bool canUseOutsideValues(BlockID) const;
    CloneCheck checkIncomingEdges(BlockID) const;

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    BitVector m_isInRegion;
    BitVector m_isLiveAtEntry;
    bool m_separatesFunctions;
};

bool RegionBorder::addEntry(const Liveness& liveness, BlockID entry)
{
    if (!contains(entry))
        return false;
    for (ValueID value : liveness.liveAtHead(entry)) {
        if (!isDefinedOutside(value))
            m_isLiveAtEntry.set(value.index());
    }
    return true;
}

bool RegionBorder::isDefinedOutside(ValueID value) const
{
    BlockID block = m_useLists.blockOf(value);
    return !block || !contains(block);
}

bool RegionBorder::isAboutOutsideValues(const GuardFact& fact) const
{
    if (fact.subject && !isDefinedOutside(fact.subject))
        return false;
    return std::ranges::all_of(fact.origins, [&](const GuardOrigin& origin) {
        return namesOnlyOutsideValues(origin);
    });
}

bool RegionBorder::namesOnlyOutsideValues(const GuardOrigin& origin) const
{
    switch (origin.source) {
    case GuardSource::SwitchCase:
        return true;
    case GuardSource::Allocation:
        return isDefinedOutside(origin.test);
    case GuardSource::Test:
        break;
    }

    const Value& test = m_function.at(origin.test);
    bool readsOnlyOutsideValues = true;
    test.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (test.operandKind(operandIndex) != OperandKind::Guard)
            readsOnlyOutsideValues &= isDefinedOutside(m_analyses.identityOf(operand));
    });
    return readsOnlyOutsideValues;
}

// A test and its copy have one condition when both are about the same values. What is about a value of the region is about another value in the copy.
bool RegionBorder::provesSameOnceMerged(ValueID guard) const
{
    if (m_separatesFunctions)
        return false;
    const GuardFacts& facts = m_analyses.guardFacts(guard);
    return facts.isKnown() && std::ranges::all_of(facts.facts, [&](const GuardFact& fact) {
        return isAboutOutsideValues(fact);
    });
}

bool RegionBorder::acceptsParameter(OperandKind kind, ValueID used) const
{
    if (mustBeDefinition(kind))
        return false;
    if (kind == OperandKind::Guard)
        return provesSameOnceMerged(used);
    if (!m_separatesFunctions)
        return true;
    std::optional<OpaqueKind> opaque = opaqueKindOf(kind);
    return !opaque || !isFunctionLocal(*opaque);
}

bool RegionBorder::acceptsParameter(const Use& use, ValueID used) const
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return acceptsParameter(m_function.at(use.user).operandKind(use.operandIndex), used);
    case Use::Kind::TerminatorOperand:
        return acceptsParameter(m_function.at(use.block).terminator.operandKind(use.operandIndex), used) && !passesProofsOf(use.block, used);
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool RegionBorder::canBecomeParameter(ValueID id) const
{
    const Value& value = m_function.at(id);
    if (!value.type || numberOfResultsOf(m_program, m_function, value))
        return false;
    std::optional<OpaqueKind> opaque = m_program.at(value.type).opaque;
    return !opaque || acceptsParameter(operandKindOf(*opaque), id);
}

bool RegionBorder::passesProofsOf(BlockID from, ValueID test) const
{
    const Terminator& terminator = m_function.at(from).terminator;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        uint32_t numberOfResults = numberOfEdgeResults(m_function, from, targetIndex);
        for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
            if (edgeResultAt(m_function, { from, targetIndex, resultIndex }).test == test)
                return true;
        }
    }
    return false;
}

bool RegionBorder::isSeparatedFromDefinition(const Use& use, ValueID definition) const
{
    if (!contains(use.block))
        return true;
    return m_isLiveAtEntry.get(definition.index()) && use.block != m_useLists.blockOf(definition);
}

bool RegionBorder::canBeSeparatedFromUses(ValueID definition) const
{
    bool isSeparated = false;
    for (const Use& use : m_useLists.usesOf(definition)) {
        if (!isSeparatedFromDefinition(use, definition))
            continue;
        if (!acceptsParameter(use, definition))
            return false;
        isSeparated = true;
    }
    return !isSeparated || canBecomeParameter(definition);
}

bool RegionBorder::canFillOutsideParameters(BlockID from) const
{
    bool canFill = true;
    forEachOutgoingEdge(m_function, from, [&](Edge edge, const Target& target) {
        if (contains(target.block))
            return;
        auto& parameters = m_function.at(target.block).parameters;
        for (uint32_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
            IncomingValue incoming = incomingValueOf(m_function, edge, parameterIndex);
            switch (incoming.kind) {
            case IncomingValue::Kind::Argument:
                if (incoming.argument && !isDefinedOutside(incoming.argument) && !canBecomeParameter(incoming.argument))
                    canFill = false;
                break;
            case IncomingValue::Kind::EdgeResult:
                if (!canBecomeParameter(parameters[parameterIndex]))
                    canFill = false;
                break;
            }
        }
    });
    return canFill;
}

bool RegionBorder::canUseOutsideValues(BlockID blockID) const
{
    const Block& block = m_function.at(blockID);
    bool canUse = true;
    auto visitChild = [&](ValueID child) {
        if (child && isDefinedOutside(child) && !canBecomeParameter(child))
            canUse = false;
    };
    auto visitOperand = [&](ValueID operand, const Use& use) {
        if (isDefinedOutside(operand) && !acceptsParameter(use, operand))
            canUse = false;
    };
    for (ValueID user : block.values) {
        m_function.at(user).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            visitOperand(operand, Use { Use::Kind::Operand, blockID, user, operandIndex, 0 });
            visitChild(operand);
        });
    }
    block.terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        visitOperand(operand, Use { Use::Kind::TerminatorOperand, blockID, ValueID(), operandIndex, 0 });
    });
    block.forEachChildOutsideValues(visitChild);
    return canUse;
}

CloneCheck RegionBorder::checkIncomingEdges(BlockID blockID) const
{
    bool isEnteredFromOutside = false;
    for (Edge edge : m_cfg.incomingEdges(blockID)) {
        if (contains(edge.from))
            continue;
        if (edge.isHandlerEdge())
            return CloneCheck::HandlerOutsideRegion;
        isEnteredFromOutside = true;
    }
    if (!isEnteredFromOutside)
        return CloneCheck::CanClone;
    for (ValueID parameter : m_function.at(blockID).parameters) {
        if (!canBecomeParameter(parameter))
            return CloneCheck::OperandMustBeDefinition;
    }
    return CloneCheck::CanClone;
}

CloneCheck RegionBorder::check(BlockID blockID) const
{
    const Block& block = m_function.at(blockID);
    for (ValueID parameter : block.parameters) {
        if (!canBeSeparatedFromUses(parameter))
            return CloneCheck::OperandMustBeDefinition;
    }
    for (ValueID value : block.values) {
        if (!canBeSeparatedFromUses(value))
            return CloneCheck::OperandMustBeDefinition;
    }
    if (!canFillOutsideParameters(blockID))
        return CloneCheck::OperandMustBeDefinition;
    if (!m_separatesFunctions)
        return CloneCheck::CanClone;

    if (!canUseOutsideValues(blockID))
        return CloneCheck::OperandMustBeDefinition;
    if (block.handler && !contains(block.handler->block))
        return CloneCheck::HandlerOutsideRegion;
    return checkIncomingEdges(blockID);
}

} // anonymous namespace

static CloneCheck checkRegionBorder(FunctionAnalyses& analyses, std::span<const BlockID> region, std::span<const BlockID> entries, bool separatesFunctions)
{
    if (region.empty())
        return CloneCheck::Unknown;
    RegionBorder border(analyses, separatesFunctions);
    for (BlockID block : region) {
        if (!border.add(block))
            return CloneCheck::Unknown;
    }
    if (!entries.empty()) {
        const Liveness& liveness = analyses.liveness();
        for (BlockID entry : entries) {
            if (!border.addEntry(liveness, entry))
                return CloneCheck::Unknown;
        }
    }
    for (BlockID block : region) {
        CloneCheck check = border.check(block);
        if (check != CloneCheck::CanClone)
            return check;
    }
    return CloneCheck::CanClone;
}

static bool canLeaveFrame(BoundTo boundTo)
{
    switch (boundTo) {
    case BoundTo::Nothing:
    case BoundTo::Function:
    case BoundTo::Site:
        return true;
    case BoundTo::Frame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RegionCloner::RegionCloner(IRBuilder& destination, const Function& source)
    : m_destination(destination)
    , m_source(source)
    , m_valueMap(FillWith { }, source.values.size(), ValueID())
    , m_blockMap(FillWith { }, source.blocks.size(), BlockID())
    , m_isSeeded(source.values.size())
    , m_isInRegion(source.blocks.size())
    , m_rematerializedClone(FillWith { }, isSameFunction() ? 0 : source.values.size(), ValueID())
    , m_rematerializedIn(FillWith { }, isSameFunction() ? 0 : source.values.size(), BlockID())
{
}

CloneCheck RegionCloner::checkBorder(FunctionAnalyses& analyses, std::span<const BlockID> region, Border border)
{
    return checkRegionBorder(analyses, region, { }, border == Border::BothWays);
}

CloneCheck RegionCloner::checkBindings(const Function& function, std::span<const BlockID> region, CloneDestination destination)
{
    switch (destination) {
    case CloneDestination::SameFunction:
        return CloneCheck::CanClone;
    case CloneDestination::OtherFunction:
        break;
    }
    for (BlockID blockID : region) {
        const Block& block = function.at(blockID);
        for (uint32_t index = indexAfterCaughtValues(function, block); index < block.values.size(); ++index) {
            if (!canLeaveFrame(boundToOf(function.at(block.values[index]).opcode)))
                return CloneCheck::BoundToFrame;
        }
        if (!canLeaveFrame(boundToOf(block.terminator.kind)))
            return CloneCheck::BoundToFrame;
    }
    return CloneCheck::CanClone;
}

CloneCheck RegionCloner::checkCanDuplicate(FunctionAnalyses& analyses, std::span<const BlockID> region)
{
    CloneCheck check = checkBorder(analyses, region, Border::OutwardOnly);
    if (check != CloneCheck::CanClone)
        return check;
    return checkBindings(analyses.function(), region, CloneDestination::SameFunction);
}

CloneCheck RegionCloner::checkCanDuplicate(FunctionAnalyses& analyses, std::span<const BlockID> region, std::span<const BlockID> entries)
{
    constexpr bool separatesFunctions = false;
    CloneCheck check = checkRegionBorder(analyses, region, entries, separatesFunctions);
    if (check != CloneCheck::CanClone)
        return check;
    return checkBindings(analyses.function(), region, CloneDestination::SameFunction);
}

CloneCheck RegionCloner::checkCanExtract(FunctionAnalyses& analyses, std::span<const BlockID> region)
{
    CloneCheck check = checkBorder(analyses, region, Border::BothWays);
    if (check != CloneCheck::CanClone)
        return check;
    return checkBindings(analyses.function(), region, CloneDestination::OtherFunction);
}

CloneCheck RegionCloner::checkCanInline(const Function& callee)
{
    if (!callee.hasBody())
        return CloneCheck::Unknown;
    if (!callee.mayBeInlined())
        return CloneCheck::FunctionMayNotBeCloned;
    Vector<BlockID> blocks(callee.blocks.size(), [](size_t index) {
        return BlockID(index);
    });
    return checkBindings(callee, blocks.span(), CloneDestination::OtherFunction);
}

Vector<ValueID> RegionCloner::findLiveIns(const Function& function, std::span<const BlockID> region)
{
    BitVector isDefined(function.values.size());
    for (BlockID block : region) {
        for (ValueID parameter : function.at(block).parameters)
            isDefined.set(parameter.index());
        for (ValueID value : function.at(block).values)
            isDefined.set(value.index());
    }
    BitVector isLiveIn(function.values.size());
    auto use = [&](ValueID value) {
        if (value && !isDefined.get(value.index()) && !isRematerializable(function.at(value)))
            isLiveIn.set(value.index());
    };
    auto useAtHead = [&](ValueID value) {
        if (value && !isDefined.get(value.index()))
            isLiveIn.set(value.index());
    };
    for (BlockID blockID : region) {
        const Block& block = function.at(blockID);
        for (ValueID value : block.values)
            function.at(value).forEachChild(use);
        block.terminator.forEachChild(use);
        if (block.handler) {
            for (ValueID argument : block.handler->arguments)
                useAtHead(argument);
        }
        useAtHead(block.subject);
    }

    Vector<ValueID> liveIns;
    liveIns.reserveInitialCapacity(isLiveIn.bitCount());
    for (size_t index : isLiveIn)
        liveIns.append(ValueID(index));
    return liveIns;
}

void RegionCloner::mapValue(ValueID inSource, ValueID inDestination)
{
    RELEASE_ASSERT(m_clonedBlocks.isEmpty());
    RELEASE_ASSERT(inDestination && !m_valueMap[inSource.index()]);
    m_valueMap[inSource.index()] = inDestination;
    m_isSeeded.set(inSource.index());
}

void RegionCloner::mapBlock(BlockID inSource, BlockID inDestination)
{
    RELEASE_ASSERT(m_clonedBlocks.isEmpty());
    RELEASE_ASSERT(inDestination && !m_blockMap[inSource.index()]);
    m_blockMap[inSource.index()] = inDestination;
}

ValueID RegionCloner::cloneOf(ValueID inSource) const
{
    return inSource.index() < m_valueMap.size() ? m_valueMap[inSource.index()] : ValueID();
}

BlockID RegionCloner::cloneOf(BlockID inSource) const
{
    return inSource.index() < m_blockMap.size() ? m_blockMap[inSource.index()] : BlockID();
}

void RegionCloner::cloneBlocksImpl(std::span<const BlockID> region, NOESCAPE const DidCloneValue& didCloneValue)
{
    RELEASE_ASSERT(m_clonedBlocks.isEmpty());
    m_region.append(region);
    m_clonedBlocks.reserveInitialCapacity(m_region.size());
    for (BlockID block : m_region) {
        RELEASE_ASSERT(!m_blockMap[block.index()]);
        BlockID clone = m_destination.addBlock(m_source.at(block).frequency);
        m_destination.function().at(clone).role = m_source.at(block).role;
        m_destination.function().at(clone).shape = m_source.at(block).shape;
        m_blockMap[block.index()] = clone;
        m_isInRegion.set(block.index());
        m_clonedBlocks.append(clone);
    }
    for (BlockID block : m_region)
        addClones(block, didCloneValue);
    for (BlockID block : m_region)
        cloneContents(block, didCloneValue);
    m_destination.didChangeControlFlow();
}

void RegionCloner::addClones(BlockID regionBlock, NOESCAPE const DidCloneValue& didCloneValue)
{
    const Block& source = m_source.at(regionBlock);
    Block& clone = m_destination.function().at(m_blockMap[regionBlock.index()]);
    auto addClone = [&](ValueID id) {
        RELEASE_ASSERT(!m_valueMap[id.index()]);
        ValueID added = m_destination.addValue(Value(m_source.at(id)));
        m_valueMap[id.index()] = added;
        return added;
    };
    for (ValueID id : source.parameters) {
        if (m_isSeeded.get(id.index()))
            continue;
        ValueID parameter = addClone(id);
        callDidCloneValue(id, parameter, didCloneValue);
        clone.parameters.append(parameter);
    }
    for (ValueID id : source.values)
        addClone(id);
}

void RegionCloner::cloneContents(BlockID regionBlock, NOESCAPE const DidCloneValue& didCloneValue)
{
    const Block& source = m_source.at(regionBlock);
    BlockID cloneBlock = m_blockMap[regionBlock.index()];
    Function& function = m_destination.function();

    auto cloneValues = [&](std::span<const ValueID> values) {
        for (ValueID id : values) {
            ValueID clone = cloneOperands(id, regionBlock, cloneBlock, didCloneValue);
            function.at(cloneBlock).values.append(clone);
        }
    };

    // A constant that the terminator names is copied in front of the other values: a tested condition stays the last value of its block.
    uint32_t numberOfCaughtValues = indexAfterCaughtValues(m_source, source);
    cloneValues(source.values.span().first(numberOfCaughtValues));
    Terminator terminator = cloneTerminator(regionBlock, cloneBlock, didCloneValue);
    cloneValues(source.values.span().subspan(numberOfCaughtValues));
    function.at(cloneBlock).terminator = WTF::move(terminator);

    auto hasCloneAtHead = [&](ValueID inSource) -> bool {
        return !inSource || isSameFunction() || cloneOf(inSource);
    };
    std::optional<Target> handler = source.handler;
    if (handler) {
        for (ValueID argument : handler->arguments)
            RELEASE_ASSERT(hasCloneAtHead(argument));
        remapTarget(*handler, regionBlock, cloneBlock, didCloneValue);
    }
    RELEASE_ASSERT(hasCloneAtHead(source.subject));

    Block& clone = function.at(cloneBlock);
    if (handler || isSameFunction())
        clone.handler = WTF::move(handler);
    clone.subject = remap(source.subject, regionBlock, cloneBlock, didCloneValue);
}

ValueID RegionCloner::cloneOperands(ValueID inSource, BlockID regionBlock, BlockID cloneBlock, NOESCAPE const DidCloneValue& didCloneValue)
{
    Function& function = m_destination.function();
    ValueID clone = m_valueMap[inSource.index()];
    Value value = WTF::move(function.at(clone));
    value.forEachChild([&](ValueID& child) {
        child = remap(child, regionBlock, cloneBlock, didCloneValue);
    });
    function.at(clone) = WTF::move(value);
    callDidCloneValue(inSource, clone, didCloneValue);
    return clone;
}

Terminator RegionCloner::cloneTerminator(BlockID regionBlock, BlockID cloneBlock, NOESCAPE const DidCloneValue& didCloneValue)
{
    Terminator terminator = m_source.at(regionBlock).terminator;
    terminator.Instruction::forEachChild([&](ValueID& child) {
        child = remap(child, regionBlock, cloneBlock, didCloneValue);
    });
    for (Target& target : terminator.targets)
        remapTarget(target, regionBlock, cloneBlock, didCloneValue);
    return terminator;
}

void RegionCloner::remapTarget(Target& target, BlockID regionBlock, BlockID cloneBlock, NOESCAPE const DidCloneValue& didCloneValue)
{
    for (ValueID& argument : target.arguments)
        argument = remap(argument, regionBlock, cloneBlock, didCloneValue);

    BlockID mapped = cloneOf(target.block);
    if (!mapped) {
        RELEASE_ASSERT(isSameFunction());
        return;
    }
    RELEASE_ASSERT(!m_isInRegion.get(target.block.index()) || m_source.at(target.block).parameters.size() == m_destination.function().at(mapped).parameters.size());
    target.block = mapped;
}

ValueID RegionCloner::remap(ValueID inSource, BlockID regionBlock, BlockID cloneBlock, NOESCAPE const DidCloneValue& didCloneValue)
{
    if (!inSource)
        return inSource;
    if (ValueID mapped = cloneOf(inSource))
        return mapped;
    if (isSameFunction())
        return inSource;

    RELEASE_ASSERT(isRematerializable(m_source.at(inSource)));
    if (m_rematerializedIn[inSource.index()] == regionBlock)
        return m_rematerializedClone[inSource.index()];
    ValueID clone = m_destination.addValue(Value(m_source.at(inSource)));
    callDidCloneValue(inSource, clone, didCloneValue);
    m_destination.function().at(cloneBlock).values.append(clone);
    m_rematerializedIn[inSource.index()] = regionBlock;
    m_rematerializedClone[inSource.index()] = clone;
    return clone;
}

void RegionCloner::callDidCloneValue(ValueID inSource, ValueID clone, NOESCAPE const DidCloneValue& didCloneValue)
{
    Function& function = m_destination.function();
    size_t numberOfBlocks = function.blocks.size();
    size_t numberOfValues = function.values.size();
    didCloneValue(inSource, function.at(clone));
    RELEASE_ASSERT(function.blocks.size() == numberOfBlocks && function.values.size() == numberOfValues);
}

void RegionCloner::repairSSA(FunctionAnalyses& analyses)
{
    const RegionCloner* cloner = this;
    repairClones(analyses, singleElementSpan(cloner));
}

void RegionCloner::repairSSA(FunctionAnalyses& analyses, std::span<const std::unique_ptr<RegionCloner>> clonersOfOneRegion)
{
    Vector<const RegionCloner*, 4> cloners;
    for (const std::unique_ptr<RegionCloner>& cloner : clonersOfOneRegion)
        cloners.append(cloner.get());
    repairClones(analyses, cloners.span());
}

void RegionCloner::addVariablesTo(SSARepair& repair, FunctionAnalyses& analyses) const
{
    const RegionCloner* cloner = this;
    addVariables(repair, analyses, singleElementSpan(cloner));
}

void RegionCloner::repairClones(FunctionAnalyses& analyses, std::span<const RegionCloner* const> clonersOfOneRegion)
{
    if (clonersOfOneRegion.empty())
        return;
    SSARepair repair(clonersOfOneRegion.front()->m_destination, analyses);
    addVariables(repair, analyses, clonersOfOneRegion);
    repair.run();
}

void RegionCloner::addVariables(SSARepair& repair, FunctionAnalyses& analyses, std::span<const RegionCloner* const> clonersOfOneRegion)
{
    const RegionCloner& first = *clonersOfOneRegion.front();
    const Function& function = first.m_source;
    RELEASE_ASSERT(first.isSameFunction() && &analyses.function() == &function);
    for (const RegionCloner* cloner : clonersOfOneRegion)
        RELEASE_ASSERT(&cloner->m_source == &function && cloner->m_region == first.m_region);

    const UseLists& useLists = analyses.useLists();
    auto isUsedOutsideBlock = [&](ValueID definition) {
        BlockID block = useLists.blockOf(definition);
        for (const Use& use : useLists.usesOf(definition)) {
            if (use.block != block)
                return true;
        }
        return false;
    };
    Vector<ValueID, 16> definitions;
    auto collect = [&](ValueID definition) {
        bool needsRepair = repair.findVariable(definition) || isUsedOutsideBlock(definition);
        for (const RegionCloner* cloner : clonersOfOneRegion) {
            ValueID clone = cloner->cloneOf(definition);
            if (!clone)
                return;
            RELEASE_ASSERT(!cloner->m_isSeeded.get(definition.index()));
            needsRepair |= isUsedOutsideBlock(clone);
        }
        if (needsRepair)
            definitions.append(definition);
    };
    for (BlockID block : first.m_region) {
        for (ValueID parameter : function.at(block).parameters)
            collect(parameter);
        for (ValueID value : function.at(block).values)
            collect(value);
    }

    // One variable holds a definition and all of its clones: an edge out of one copy carries no definition of a variable that knows only another copy.
    // A definition that the caller has put in a variable may be named where only another definition of that variable reaches, so its clones join that variable.
    for (ValueID definition : definitions) {
        std::optional<SSARepair::Variable> variable = repair.findVariable(definition);
        if (!variable)
            variable = repair.addVariable(definition);
        for (const RegionCloner* cloner : clonersOfOneRegion)
            repair.addDefinition(*variable, cloner->cloneOf(definition));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
