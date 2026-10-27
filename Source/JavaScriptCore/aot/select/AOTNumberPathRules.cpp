/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTRegionCloner.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NumberPath {
    SelectionGroup group;
    BlockID block;
    uint32_t firstIndex { 0 };
    uint32_t lastIndex { 0 };
    Vector<ValueID, 2> tested;
    bool includesTerminator { false };
};

struct SplitPath {
    const NumberPath* path { nullptr };
    BlockID middle;
    BlockID head;
    BlockID integerArm;
    Vector<ValueID, 2> narrowed;
};

} // anonymous namespace

static constexpr OpcodeSet pathOperators {
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Negate, Opcode::Increment, Opcode::Decrement, Opcode::BitNot, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq,
    Opcode::StrictEq, Opcode::ToInt32, Opcode::ToUint32
};

static bool hasTaggedResult(SelectionContext& context, const Value& definition)
{
    if (definition.opcode == Opcode::GetSlot) {
        const SlotAgreement& agreement = context.slots().agreementOf(context.program(), definition.key(), definition.receivers());
        return !agreement.groups.isEmpty() && std::ranges::all_of(agreement.groups, [](const SlotGroup& group) {
            return group.slot.representation == SlotRepresentation::JSValue;
        });
    }
    if (definition.opcode == Opcode::GetArrayElement)
        return elementRepresentationOf(definition.arrayStorage()) == Representation::JSValue;
    return definition.opcode == Opcode::CallBuiltin || resultRuleOf(definition.opcode) == ResultRule::Unknown;
}

static bool isTaggedNumber(SelectionContext& context, ValueID id)
{
    const Type& type = context.typeOf(id);
    if (!type.isNumber() || type.isInteger() || !(type.primitives & Primitive::Integer))
        return false;
    if (type.integers.intersectWith(Range::int32()).isEmpty())
        return false;

    const Function& function = context.function();
    return hasTaggedResult(context, function.at(sameObjectThroughCasts(function, id)));
}

static bool isPathOperator(SelectionContext& context, const Value& value)
{
    if (!pathOperators.contains(value.opcode))
        return false;

    bool hasOnlyNumbers = true;
    value.forEachChild([&](ValueID operand) {
        hasOnlyNumbers &= context.typeOf(operand).isNumber();
    });
    return hasOnlyNumbers;
}

static Vector<ValueID, 2> findTestedOperands(SelectionContext& context, const Value& seed)
{
    Vector<ValueID, 2> tested;
    bool hasOtherNumber = false;
    seed.forEachChild([&](ValueID operand) {
        if (context.typeOf(operand).isInteger())
            return;
        if (!isTaggedNumber(context, operand))
            hasOtherNumber = true;
        else if (!tested.contains(operand))
            tested.append(operand);
    });
    if (hasOtherNumber)
        return { };
    return tested;
}

static bool isDependentValue(const UseLists& uses, const NumberPath& path, const BitVector& isDependent, ValueID id)
{
    ValueLocation location = uses.locationOf(id);
    return location.placement == ValuePlacement::InBlock && location.block == path.block && location.index >= path.firstIndex
        && isDependent.get(location.index - path.firstIndex);
}

static bool extendsPath(SelectionContext& context, const NumberPath& path, const BitVector& isDependent, const Value& value)
{
    if (!isPathOperator(context, value))
        return false;

    const UseLists& uses = context.analyses().useLists();
    bool hasPathOperand = false;
    bool hasOtherNumber = false;
    value.forEachChild([&](ValueID operand) {
        if (path.tested.contains(operand) || isDependentValue(uses, path, isDependent, operand))
            hasPathOperand = true;
        else if (!context.typeOf(operand).isInteger())
            hasOtherNumber = true;
    });
    return hasPathOperand && !hasOtherNumber;
}

static void growPath(SelectionContext& context, NumberPath& path, BitVector& isDependent)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();
    const BlockValues& values = function.at(path.block).values;
    for (uint32_t index = path.firstIndex + 1; index < values.size(); ++index) {
        ValueID id = values[index];
        if (!analyses.hasNoEffects(id))
            return;
        if (context.pass().exceeds(LimitID::MaxNumberPathValues, index - path.firstIndex + 1, id))
            return;
        if (!extendsPath(context, path, isDependent, function.at(id)))
            continue;
        isDependent.set(index - path.firstIndex);
        path.lastIndex = index;
    }
}

static bool isUseInsidePath(const UseLists& uses, const NumberPath& path, const Use& use)
{
    if (use.block != path.block)
        return false;

    switch (use.kind) {
    case Use::Kind::Operand: {
        uint32_t index = uses.locationOf(use.user).index;
        return index >= path.firstIndex && index <= path.lastIndex;
    }
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return path.includesTerminator;
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool acceptsParameter(OperandKind kind)
{
    return kind != OperandKind::Guard && !mustBeDefinition(kind);
}

static bool acceptsParameter(SelectionContext& context, const Use& use)
{
    const Function& function = context.function();
    switch (use.kind) {
    case Use::Kind::Operand:
        return acceptsParameter(function.at(use.user).operandKind(use.operandIndex));
    case Use::Kind::TerminatorOperand: {
        const Terminator& terminator = function.at(use.block).terminator;
        bool isTestWithProofs = terminator.isConditionalBranch() && context.passesProofs(use.block);
        return !isTestWithProofs && acceptsParameter(terminator.operandKind(use.operandIndex));
    }
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canJoinAfterPath(SelectionContext& context, const NumberPath& path)
{
    const UseLists& uses = context.analyses().useLists();
    const BlockValues& values = context.function().at(path.block).values;
    for (uint32_t index = path.firstIndex; index <= path.lastIndex; ++index) {
        for (const Use& use : uses.usesOf(values[index])) {
            if (!isUseInsidePath(uses, path, use) && !acceptsParameter(context, use))
                return false;
        }
    }
    return true;
}

static std::optional<NumberPath> findPathFrom(SelectionContext& context, BlockID block, uint32_t firstIndex)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();
    const Block& contents = function.at(block);
    ValueID seed = contents.values[firstIndex];
    if (!isPathOperator(context, function.at(seed)) || context.isClaimed(seed) || context.isOnSlowPath(seed))
        return std::nullopt;
    if (!analyses.hasNoEffects(seed))
        return std::nullopt;

    NumberPath path {
        .group = groupOfOne(seed),
        .block = block,
        .firstIndex = firstIndex,
        .lastIndex = firstIndex,
        .tested = findTestedOperands(context, function.at(seed)),
        .includesTerminator = false,
    };
    if (path.tested.isEmpty() || context.pass().exceeds(LimitID::MaxNumberPathValues, 1, seed))
        return std::nullopt;

    BitVector isDependent;
    isDependent.set(0);
    growPath(context, path, isDependent);
    if (isDependent.bitCount() < 2 && context.shapeOf(block) != CodeShape::Fast)
        return std::nullopt;

    const Terminator& terminator = contents.terminator;
    path.includesTerminator = path.lastIndex + 1 == contents.values.size() && terminator.kind == TerminatorKind::Branch
        && isDependentValue(analyses.useLists(), path, isDependent, terminator.conditionOperand());

    if (!canJoinAfterPath(context, path))
        return std::nullopt;
    return path;
}

static BlockID splitAroundPath(IRBuilder& builder, const NumberPath& path)
{
    if (path.includesTerminator)
        return builder.splitBlock(ProgramPoint { path.block, path.firstIndex });

    std::array<uint32_t, 2> cuts { path.firstIndex, path.lastIndex + 1 };
    return builder.splitBlock(path.block, cuts).first();
}

static void emitIntegerTests(IRBuilder& builder, FunctionAnalyses& analyses, SplitPath& split)
{
    Function& function = builder.function();
    builder.setOrigin(function.at(split.path->group.root).origin());
    builder.setHandler(function.at(split.head).handler);
    function.edit(split.head, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
    analyses.didChangeControlFlow();
    builder.moveToEndOf(split.head);

    Expected<BlockFrequency> frequency = function.at(split.head).frequency;
    for (ValueID tested : split.path->tested) {
        ValueID test = builder.appendTypeTest(tested, TypeTable::int32);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, frequency);
        split.narrowed.append(branch.provenOnTaken);
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(split.middle);
        builder.moveToEndOf(branch.taken);
    }
    split.integerArm = builder.currentBlock();
}

static void replaceOperandInBlock(Function& function, BlockID block, ValueID operand, ValueID replacement)
{
    auto replace = [&](ValueID& child) {
        if (child == operand)
            child = replacement;
    };
    Block& contents = function.edit(block);
    for (ValueID id : contents.values) {
        if (std::ranges::contains(function.at(id).operands(), operand))
            function.edit(id, ValueEdit::Definition).forEachChild(replace);
    }
    contents.terminator.forEachChild(replace);
}

static void copySplitPaths(FunctionAnalyses& analyses, IRBuilder& builder, Vector<SplitPath>& splitPaths)
{
    splitPaths.removeAllMatching([&](const SplitPath& split) {
        return RegionCloner::checkCanDuplicate(analyses, singleElementSpan(split.middle)) != CloneCheck::CanClone;
    });
    if (splitPaths.isEmpty())
        return;

    const CFG& cfg = analyses.cfg();
    for (SplitPath& split : splitPaths) {
        std::optional<Edge> entry = cfg.singleIncomingEdge(split.middle);
        RELEASE_ASSERT(entry);
        split.head = entry->from;
    }
    for (SplitPath& split : splitPaths)
        emitIntegerTests(builder, analyses, split);

    Function& function = builder.function();
    Vector<BlockID> region = WTF::map(splitPaths, [](const SplitPath& split) {
        return split.middle;
    });
    RegionCloner cloner(builder, function);
    cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
    for (const SplitPath& split : splitPaths) {
        BlockID copy = cloner.cloneOf(split.middle);
        for (size_t index = 0; index < split.narrowed.size(); ++index) {
            ValueID tested = split.path->tested[index];
            ValueID clone = cloner.cloneOf(tested);
            replaceOperandInBlock(function, copy, clone ? clone : tested, split.narrowed[index]);
        }
        builder.setOrigin(function.at(split.path->group.root).origin());
        builder.moveToEndOf(split.integerArm);
        builder.emitJump(copy);
    }
    cloner.repairSSA(analyses);
}

void selectNumberPaths(SelectionContext& context)
{
    const Function& function = context.function();
    Vector<NumberPath> paths;
    for (BlockID block : context.analyses().cfg().reversePostOrder()) {
        if (context.shapeOf(block) != CodeShape::Fast)
            continue;

        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues;) {
            std::optional<NumberPath> path = findPathFrom(context, block, index);
            if (!path || context.tryClaim(SelectionRuleID::NumberPaths, path->group) != ClaimResult::Claimed) {
                ++index;
                continue;
            }
            index = path->lastIndex + 1;
            paths.append(WTF::move(*path));
        }
    }

    // Every path is split before any is copied, so the analyses are computed once and one cloner and one SSA repair serve the function.
    Vector<SplitPath> splitPaths;
    splitPaths.reserveInitialCapacity(paths.size());
    context.rewriteGroups(SelectionRuleID::NumberPaths, paths, [&](GroupRewriter& rewriter, const NumberPath& path) {
        IRBuilder& builder = rewriter.builder();
        builder.applyInsertions();
        splitPaths.append(SplitPath { .path = &path, .middle = splitAroundPath(builder, path), .head = { }, .integerArm = { }, .narrowed = { } });
        if (splitPaths.size() == paths.size())
            copySplitPaths(context.analyses(), builder, splitPaths);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
