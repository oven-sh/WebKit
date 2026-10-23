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
#include "AOTVector.h"
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
        , m_addedBlocks(m_function.blocks.size())
        , m_isInRegion(m_addedBlocks)
        , m_separatesFunctions(separatesFunctions)
    {
    }

    RegionBorder(FunctionAnalyses& analyses, const BitVector& isInRegion)
        : m_analyses(analyses)
        , m_program(analyses.program())
        , m_function(analyses.function())
        , m_cfg(analyses.cfg())
        , m_useLists(analyses.useLists())
        , m_isInRegion(isInRegion)
        , m_separatesFunctions(false)
    {
    }

    bool add(BlockID block)
    {
        return block != m_cfg.root() && m_cfg.isReachable(block) && !m_addedBlocks.set(block.index());
    }

    bool setEntries(std::span<const BlockID>, const RegionCloner::LivenessSource&);

    CloneCheck check(BlockID) const;
    bool canMergeAt(ValueID definition, BlockID join) const;
    bool canMergeAt(BlockID join) const;

private:
    bool contains(BlockID block) const { return m_isInRegion.get(block.index()); }
    bool isDefinedOutside(ValueID) const;
    bool isLiveAtEntry(ValueID definition) const;

    bool namesOnlyOutsideValues(const GuardOrigin&) const;
    bool isAboutOutsideValues(const GuardFact&) const;
    bool provesSameOnceMerged(ValueID guard) const;
    bool acceptsParameter(OperandKind, ValueID used) const;
    bool acceptsParameter(const Use&, ValueID used) const;
    bool canBecomeParameter(ValueID) const;
    bool passesProofsOf(BlockID from, ValueID test) const;

    bool isSeparatedFromDefinition(const Use&, ValueID definition) const;
    bool canBeSeparatedFromUses(ValueID definition) const;
    bool canMergeWithClone(ValueID definition) const;
    bool canFillParameters(Edge, const Target&) const;
    bool canFillOutsideParameters(BlockID from) const;
    bool canUseOutsideValues(BlockID) const;
    bool getsNewNameInCopy(ValueID) const;
    bool isAboutValueWithNewNameInCopy(const GuardFact&) const;
    bool joinsGuardsOfValuesWithNewNames(BlockID) const;
    CloneCheck checkIncomingEdges(BlockID) const;

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    BitVector m_addedBlocks;
    const BitVector& m_isInRegion;
    std::span<const BlockID> m_entries;
    const RegionCloner::LivenessSource* m_liveness { nullptr };
    mutable std::optional<BitVector> m_isLiveAtEntry;
    bool m_separatesFunctions;
};

bool RegionBorder::setEntries(std::span<const BlockID> entries, const RegionCloner::LivenessSource& liveness)
{
    m_entries = entries;
    m_liveness = &liveness;
    return std::ranges::all_of(entries, [&](BlockID entry) {
        return contains(entry);
    });
}

bool RegionBorder::isLiveAtEntry(ValueID definition) const
{
    if (m_entries.empty())
        return false;
    if (!m_isLiveAtEntry) {
        m_isLiveAtEntry.emplace();
        const Liveness& liveness = (*m_liveness)();
        for (BlockID entry : m_entries) {
            for (ValueID value : liveness.liveAtHead(entry)) {
                if (!isDefinedOutside(value))
                    m_isLiveAtEntry->set(value.index());
            }
        }
    }
    return m_isLiveAtEntry->get(definition.index());
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
    return use.block != m_useLists.blockOf(definition) && isLiveAtEntry(definition);
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

bool RegionBorder::canMergeWithClone(ValueID definition) const
{
    if (isDefinedOutside(definition))
        return true;
    BlockID block = m_useLists.blockOf(definition);
    for (const Use& use : m_useLists.usesOf(definition)) {
        if (use.block != block && !acceptsParameter(use, definition))
            return false;
    }
    return canBecomeParameter(definition);
}

bool RegionBorder::canMergeAt(ValueID definition, BlockID join) const
{
    return !m_analyses.liveness().isLiveAtHead(definition, join) || canMergeWithClone(definition);
}

bool RegionBorder::canMergeAt(BlockID join) const
{
    for (ValueID definition : m_analyses.liveness().liveAtHead(join)) {
        if (!canMergeWithClone(definition))
            return false;
    }
    for (Edge edge : m_cfg.incomingEdges(join)) {
        if (contains(edge.from) && !canFillParameters(edge, targetOf(m_function, edge)))
            return false;
    }
    return true;
}

bool RegionBorder::canFillParameters(Edge edge, const Target& target) const
{
    auto& parameters = m_function.at(target.block).parameters;
    for (uint32_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
        IncomingValue incoming = incomingValueOf(m_function, edge, parameterIndex);
        switch (incoming.kind) {
        case IncomingValue::Kind::Argument:
            if (incoming.argument && !isDefinedOutside(incoming.argument) && !canBecomeParameter(incoming.argument))
                return false;
            break;
        case IncomingValue::Kind::EdgeResult:
            if (!canBecomeParameter(parameters[parameterIndex]))
                return false;
            break;
        }
    }
    return true;
}

bool RegionBorder::canFillOutsideParameters(BlockID from) const
{
    bool canFill = true;
    forEachOutgoingEdge(m_function, from, [&](Edge edge, const Target& target) {
        if (!contains(target.block))
            canFill &= canFillParameters(edge, target);
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

bool RegionBorder::getsNewNameInCopy(ValueID value) const
{
    auto isJoinedWithCopy = [&](ValueID id) {
        return !isDefinedOutside(id) && isLiveAtEntry(id);
    };
    return isJoinedWithCopy(value) || isJoinedWithCopy(m_analyses.identityOf(value));
}

bool RegionBorder::isAboutValueWithNewNameInCopy(const GuardFact& fact) const
{
    if (fact.subject && getsNewNameInCopy(fact.subject))
        return true;
    return std::ranges::any_of(fact.origins, [&](const GuardOrigin& origin) {
        switch (origin.source) {
        case GuardSource::SwitchCase:
            return false;
        case GuardSource::Allocation:
            return getsNewNameInCopy(origin.test);
        case GuardSource::Test:
            break;
        }
        const Value& test = m_function.at(origin.test);
        bool readsSuchValue = false;
        test.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (test.operandKind(operandIndex) != OperandKind::Guard)
                readsSuchValue |= getsNewNameInCopy(operand);
        });
        return readsSuchValue;
    });
}

// `for (..) { .. odd[k] .. }` behind tests of `odd` and `k` in front of the loop, and again behind a call in it: the head of the loop takes the guards as arguments, and they join because all the tests are about the same two values.
bool RegionBorder::joinsGuardsOfValuesWithNewNames(BlockID blockID) const
{
    if (m_entries.empty())
        return false;
    const CompactList<ValueID>& parameters = m_function.at(blockID).parameters;
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        if (!isGuardValue(m_function, parameters[index]))
            continue;
        bool isArgument = std::ranges::any_of(m_cfg.incomingEdges(blockID), [&](Edge edge) {
            return !edge.isHandlerEdge() && incomingValueOf(m_function, edge, index).kind == IncomingValue::Kind::Argument;
        });
        if (!isArgument)
            continue;
        const GuardFacts& facts = m_analyses.guardFacts(parameters[index]);
        bool staysAboutSameNames = facts.isKnown() && std::ranges::none_of(facts.facts, [&](const GuardFact& fact) {
            return isAboutValueWithNewNameInCopy(fact);
        });
        if (!staysAboutSameNames)
            return true;
    }
    return false;
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
    if (!canFillOutsideParameters(blockID) || joinsGuardsOfValuesWithNewNames(blockID))
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

static CloneCheck checkRegionBorder(FunctionAnalyses& analyses, std::span<const BlockID> region, std::span<const BlockID> entries, const RegionCloner::LivenessSource& liveness, bool separatesFunctions)
{
    if (region.empty())
        return CloneCheck::Unknown;
    RegionBorder border(analyses, separatesFunctions);
    for (BlockID block : region) {
        if (!border.add(block))
            return CloneCheck::Unknown;
    }
    if (!border.setEntries(entries, liveness))
        return CloneCheck::Unknown;
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
    default:
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
    return checkRegionBorder(analyses, region, { }, [&]() -> const Liveness& {
        return analyses.liveness();
    }, border == Border::BothWays);
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
    return checkCanDuplicate(analyses, region, entries, [&]() -> const Liveness& {
        return analyses.liveness();
    });
}

CloneCheck RegionCloner::checkCanDuplicate(FunctionAnalyses& analyses, std::span<const BlockID> region, std::span<const BlockID> entries, NOESCAPE const LivenessSource& liveness)
{
    constexpr bool separatesFunctions = false;
    CloneCheck check = checkRegionBorder(analyses, region, entries, liveness, separatesFunctions);
    if (check != CloneCheck::CanClone)
        return check;
    return checkBindings(analyses.function(), region, CloneDestination::SameFunction);
}

std::unique_ptr<Liveness> RegionCloner::computeLivenessInside(FunctionAnalyses& analyses, std::span<const BlockID> blocks, std::span<const BlockID> entries)
{
    const Function& function = analyses.function();
    BitVector isDefinedInBlocks(function.values.size());
    for (BlockID block : blocks) {
        for (ValueID parameter : function.at(block).parameters)
            isDefinedInBlocks.quickSet(parameter.index());
        for (ValueID value : function.at(block).values)
            isDefinedInBlocks.quickSet(value.index());
    }
    BitVector isEntry(function.blocks.size());
    for (BlockID entry : entries)
        isEntry.quickSet(entry.index());

    LivenessOptions options;
    options.trackedValues = &isDefinedInBlocks;
    options.trackedBlocks = &isEntry;
    return makeUnique<Liveness>(function, analyses.cfg(), analyses.useLists(), options);
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

bool RegionCloner::canMergeAt(FunctionAnalyses& analyses, ValueID definition, BlockID join)
{
    size_t numberOfBlocks = analyses.function().blocks.size();
    BitVector isInRegion(numberOfBlocks);
    for (size_t index = 0; index < numberOfBlocks; ++index)
        isInRegion.quickSet(index);
    return canMergeAt(analyses, isInRegion, definition, join);
}

bool RegionCloner::canMergeAt(FunctionAnalyses& analyses, const BitVector& isInRegion, ValueID definition, BlockID join)
{
    return RegionBorder(analyses, isInRegion).canMergeAt(definition, join);
}

bool RegionCloner::canMergeAt(FunctionAnalyses& analyses, const BitVector& isInRegion, BlockID join)
{
    return RegionBorder(analyses, isInRegion).canMergeAt(join);
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
        Block& added = m_destination.function().edit(clone, BlockEdit::Rest);
        added.role = m_source.at(block).role;
        added.shape = m_source.at(block).shape;
        m_blockMap[block.index()] = clone;
        m_isInRegion.set(block.index());
        m_clonedBlocks.append(clone);
    }
    for (BlockID block : m_region)
        addClones(block, didCloneValue);
    for (BlockID block : m_region)
        cloneContents(block, didCloneValue);
    m_destination.didChangeControlFlow(m_clonedBlocks.span());
}

void RegionCloner::addClones(BlockID regionBlock, NOESCAPE const DidCloneValue& didCloneValue)
{
    const Block& source = m_source.at(regionBlock);
    Block& clone = m_destination.function().edit(m_blockMap[regionBlock.index()], BlockEdit::Parameters);
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
            function.edit(cloneBlock, BlockEdit::Values).values.append(clone);
        }
    };

    // A constant that the terminator names is copied in front of the other values: a tested condition stays the last value of its block.
    uint32_t numberOfCaughtValues = indexAfterCaughtValues(m_source, source);
    cloneValues(source.values.span().first(numberOfCaughtValues));
    Terminator terminator = cloneTerminator(regionBlock, cloneBlock, didCloneValue);
    cloneValues(source.values.span().subspan(numberOfCaughtValues));
    function.edit(cloneBlock, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = WTF::move(terminator);

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

    Block& clone = function.edit(cloneBlock, { BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest });
    if (handler || isSameFunction())
        clone.handler = WTF::move(handler);
    clone.subject = remap(source.subject, regionBlock, cloneBlock, didCloneValue);
}

ValueID RegionCloner::cloneOperands(ValueID inSource, BlockID regionBlock, BlockID cloneBlock, NOESCAPE const DidCloneValue& didCloneValue)
{
    Function& function = m_destination.function();
    ValueID clone = m_valueMap[inSource.index()];
    Value value = WTF::move(function.edit(clone, ValueEdit::Definition));
    value.forEachChild([&](ValueID& child) {
        child = remap(child, regionBlock, cloneBlock, didCloneValue);
    });
    function.edit(clone, ValueEdit::Definition) = WTF::move(value);
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
    m_destination.function().edit(cloneBlock, BlockEdit::Values).values.append(clone);
    m_rematerializedIn[inSource.index()] = regionBlock;
    m_rematerializedClone[inSource.index()] = clone;
    return clone;
}

void RegionCloner::callDidCloneValue(ValueID inSource, ValueID clone, NOESCAPE const DidCloneValue& didCloneValue)
{
    Function& function = m_destination.function();
    size_t numberOfBlocks = function.blocks.size();
    size_t numberOfValues = function.values.size();
    didCloneValue(inSource, function.edit(clone));
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
