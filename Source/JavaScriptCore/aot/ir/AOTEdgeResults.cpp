/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEdgeResults.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTResultRulesInternal.h"
#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

const TargetInfo* infoOfTarget(TerminatorKind kind, uint32_t targetIndex, uint32_t numberOfTargets)
{
    std::span<const TargetInfo> infos = targetInfosOf(kind);
    if (targetIndex >= numberOfTargets || infos.empty())
        return nullptr;
    if (!infos.front().isVariadic)
        return targetIndex < infos.size() ? &infos[targetIndex] : nullptr;

    ASSERT(std::ranges::none_of(infos.subspan(1), &TargetInfo::isVariadic));
    uint32_t numberOfFixedTargets = static_cast<uint32_t>(infos.size()) - 1;
    if (numberOfTargets < numberOfFixedTargets)
        return nullptr;
    uint32_t numberOfVariadicTargets = numberOfTargets - numberOfFixedTargets;
    if (targetIndex < numberOfVariadicTargets)
        return &infos.front();
    return &infos[targetIndex - numberOfVariadicTargets + 1];
}

static const TargetInfo* infoOfTarget(const Terminator& terminator, uint32_t targetIndex)
{
    return infoOfTarget(terminator.kind, targetIndex, static_cast<uint32_t>(terminator.targets.size()));
}

TargetResults targetResultsOf(TerminatorKind kind, uint32_t targetIndex, uint32_t numberOfTargets)
{
    const TargetInfo* info = infoOfTarget(kind, targetIndex, numberOfTargets);
    if (!info)
        return TargetResults::None;
    if (info->takesProofs)
        return TargetResults::Proofs;
    return info->numberOfResults ? TargetResults::DeclaredResults : TargetResults::None;
}

uint32_t numberOfEdgeResults(const Function& function, BlockID from, uint32_t targetIndex)
{
    const Terminator& terminator = function.at(from).terminator;
    const TargetInfo* info = infoOfTarget(terminator, targetIndex);
    if (!info)
        return 0;
    if (info->takesProofs)
        return static_cast<uint32_t>(terminator.targets[targetIndex].proofs.size());
    return info->numberOfResults;
}

uint32_t firstEdgeResultParameter(const Function& function, BlockID from, uint32_t targetIndex)
{
    return static_cast<uint32_t>(function.at(from).terminator.targets[targetIndex].arguments.size());
}

std::optional<Proof> proofOfTerminator(const Function& function, const Block& block)
{
    const Terminator& terminator = block.terminator;
    Proof proof = terminator.isConditionalBranch() ? provesOf(function.at(terminator.conditionOperand()).opcode) : provesOf(terminator.kind);
    if (proof == Proof::None)
        return std::nullopt;
    return proof;
}

template<typename Test>
static std::optional<EdgeResult> edgeResultOfProofEntry(EdgeResult result, const Test& test, uint32_t proofEntry)
{
    if (provesForStretch(result.proof)) {
        if (proofEntry || result.side != EdgeSide::Taken)
            return std::nullopt;
        result.kind = EdgeResultKind::Guard;
        if (test.numberOfOperands())
            result.subject = test.operand(0);
        return result;
    }

    if (proofEntry >= test.numberOfOperands() || !test.operand(proofEntry) || test.operandKind(proofEntry) == OperandKind::Guard)
        return std::nullopt;
    result.kind = EdgeResultKind::NarrowedOperand;
    result.subject = test.operand(proofEntry);
    result.operandIndex = proofEntry;
    return result;
}

std::optional<EdgeResult> tryGetEdgeResult(const Function& function, EdgeResultPosition position)
{
    const Block& block = function.at(position.from);
    const Terminator& terminator = block.terminator;
    const TargetInfo* info = infoOfTarget(terminator, position.targetIndex);
    if (!info)
        return std::nullopt;
    if (!info->takesProofs) {
        if (position.resultIndex >= info->numberOfResults)
            return std::nullopt;
        return EdgeResult { .kind = EdgeResultKind::DeclaredResult };
    }

    const Target& target = terminator.targets[position.targetIndex];
    if (position.resultIndex >= target.proofs.size())
        return std::nullopt;
    std::optional<Proof> proof = proofOfTerminator(function, block);
    if (!proof)
        return std::nullopt;

    uint32_t proofEntry = target.proofs[position.resultIndex];
    EdgeResult result { .proof = *proof };
    if (terminator.isConditionalBranch()) {
        result.side = position.targetIndex ? EdgeSide::NotTaken : EdgeSide::Taken;
        result.test = terminator.conditionOperand();
        return edgeResultOfProofEntry(result, function.at(result.test), proofEntry);
    }
    RELEASE_ASSERT(terminator.hasCases());
    result.caseIndex = position.targetIndex;
    return edgeResultOfProofEntry(result, terminator, proofEntry);
}

EdgeResult edgeResultAt(const Function& function, EdgeResultPosition position)
{
    std::optional<EdgeResult> result = tryGetEdgeResult(function, position);
    RELEASE_ASSERT(result);
    return *result;
}

ValueID operandNarrowedOnEdge(const Function& function, EdgeResultPosition position)
{
    EdgeResult result = edgeResultAt(function, position);
    return result.kind == EdgeResultKind::NarrowedOperand ? result.subject : ValueID();
}

Type truthyPartOf(const Type& type, TypeArena&)
{
    if (!type.isJavaScriptValue())
        return type;

    Type result = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::False);
    if (!(result.primitives & Primitive::Integer))
        return result;
    if (result.integers == Range::one(0))
        return result.withoutPrimitives(Primitive::Integer);
    if (!result.integers.minimum)
        result.integers.minimum = 1;
    else if (!result.integers.maximum)
        result.integers.maximum = -1;
    return result;
}

Type falsyPartOf(const Type& type, TypeArena&)
{
    if (!type.isJavaScriptValue())
        return type;

    Type result = type.withoutObjects().withoutPrimitives(Primitive::True | Primitive::FractionalNumber | Primitive::AnySymbol);
    if (!(result.primitives & Primitive::Integer))
        return result;
    if (!result.integers.contains(0))
        return result.withoutPrimitives(Primitive::Integer);
    result.integers = Range::one(0);
    return result;
}

static Type narrowByTruthiness(const TypingContext& context, const Type& source, EdgeSide side)
{
    StringID emptyString = context.program.strings.emptyString();
    if (side == EdgeSide::Taken) {
        Type truthy = truthyPartOf(source, context.types);
        if (truthy.stringLiterals.remove(emptyString) && truthy.stringLiterals.isEmpty())
            return truthy.withoutPrimitives(Primitive::LiteralString);
        return truthy;
    }

    Type falsy = falsyPartOf(source, context.types);
    if (!falsy.hasLiteralStrings())
        return falsy;
    if (!falsy.stringLiterals.contains(emptyString))
        return falsy.withoutPrimitives(Primitive::LiteralString);
    falsy.stringLiterals = { emptyString };
    return falsy;
}

static bool mayBeMemberWithoutLayout(const TypingContext& context, const Type& type)
{
    return type.layouts && context.types.at(type.layouts).hasMembersWithoutLayout;
}

static Type withMembersWithoutLayout(const TypingContext& context, Type&& type)
{
    if (mayBeMemberWithoutLayout(context, type))
        return WTF::move(type);
    LayoutSet layouts { .isComplete = true, .hasMembersWithoutLayout = true };
    if (type.layouts)
        layouts.members = context.types.at(type.layouts).members;
    type.layouts = context.types.intern(WTF::move(layouts));
    return WTF::move(type);
}

static Type narrowByFailedTest(const TypingContext& context, const Type& source, const Type& tested)
{
    const BuiltinTable& builtins = context.program.builtins();
    auto isUntestable = [&](BuiltinID builtin) {
        return !isTestableBuiltin(builtins, builtin);
    };
    bool mayHaveNoLayout = mayBeMemberWithoutLayout(context, source);
    if (!mayHaveNoLayout && std::ranges::none_of(tested.builtins, isUntestable))
        return source.without(tested, context.types);
    if (source.withOnlyObjects().isSubsetOf(tested, context.program))
        return source.without(tested, context.types);

    Type recognized = tested;
    removeAllMatching(recognized.builtins, isUntestable);
    Type failing = source.without(recognized, context.types);
    if (!mayHaveNoLayout)
        return failing;
    return withMembersWithoutLayout(context, WTF::move(failing));
}

static Type withoutMembersWithoutLayout(const TypingContext& context, const Type& type)
{
    if (!mayBeMemberWithoutLayout(context, type))
        return type;
    Type listed = type;
    const Vector<LayoutID>& members = context.types.at(type.layouts).members;
    listed.layouts = members.isEmpty() ? LayoutSetID() : context.types.intern(LayoutSet { Vector<LayoutID>(members), true });
    return listed;
}

static Type narrowByTest(const TypingContext& context, const Type& source, const Type& tested, EdgeSide side)
{
    return side == EdgeSide::Taken ? source.castTo(withoutMembersWithoutLayout(context, tested), context.types) : narrowByFailedTest(context, source, tested);
}

static Type zeroesJoined(const Type& other)
{
    Type result = other;
    if (other.primitives & Primitive::WholeNumber) {
        result.primitives |= Primitive::Integer;
        result.integers = other.integers.unionWith(Range::one(0));
    }
    if (other.integers.contains(0))
        result.primitives |= Primitive::WholeNumber;
    return result;
}

static Type narrowByStrictEquality(const TypingContext& context, const Type& source, const Type& other, EdgeSide side)
{
    if (side == EdgeSide::Taken)
        return source.intersectWith(zeroesJoined(other), context.types);
    return other.isSingleValue() ? source.without(other, context.types) : source;
}

static Type narrowByLooseEquality(const TypingContext& context, const Type& source, const Type& other, EdgeSide side)
{
    if (other.isUndefinedOrNull()) {
        if (side == EdgeSide::Taken)
            return source.intersectWith(context.types.at(TypeTable::nullish), context.types);
        return source.withoutNullish();
    }
    if (side == EdgeSide::NotTaken)
        return source;

    bool comparesWithoutConversion = (source.isNumber() && other.isNumber()) || (source.isAnyString() && other.isAnyString()) || (source.isBoolean() && other.isBoolean());
    return comparesWithoutConversion ? narrowByStrictEquality(context, source, other, side) : source;
}

static Opcode mirroredRelation(Opcode comparison)
{
    switch (comparison) {
    case Opcode::Less:
        return Opcode::Greater;
    case Opcode::LessEq:
        return Opcode::GreaterEq;
    case Opcode::Greater:
        return Opcode::Less;
    case Opcode::GreaterEq:
        return Opcode::LessEq;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static Opcode negatedRelation(Opcode comparison)
{
    switch (comparison) {
    case Opcode::Less:
        return Opcode::GreaterEq;
    case Opcode::LessEq:
        return Opcode::Greater;
    case Opcode::Greater:
        return Opcode::LessEq;
    case Opcode::GreaterEq:
        return Opcode::Less;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static Range boundsImpliedByComparison(Opcode comparison, bool valueIsLeft, bool isOnTakenEdge, Range other)
{
    Opcode relation = valueIsLeft ? comparison : mirroredRelation(comparison);
    if (!isOnTakenEdge)
        relation = negatedRelation(relation);

    switch (relation) {
    case Opcode::Less:
        return { Range::full().minimum, other.maximum - 1 };
    case Opcode::LessEq:
        return { Range::full().minimum, other.maximum };
    case Opcode::Greater:
        return { other.minimum + 1, Range::full().maximum };
    case Opcode::GreaterEq:
        return { other.minimum, Range::full().maximum };
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static Type narrowByIntegerBound(const Value& comparison, uint32_t operandIndex, const Type& source, const Type& other, EdgeSide side)
{
    if (!source.isInteger() || !other.isInteger())
        return source;

    Range implied = boundsImpliedByComparison(comparison.opcode, !operandIndex, side == EdgeSide::Taken, other.integers);
    Range range = source.integers.intersectWith(implied);
    if (range.isEmpty())
        return Type::never();
    Type result = source;
    result.integers = range;
    return result;
}

static Type narrowByComparison(const TypingContext& context, const Value& comparison, uint32_t operandIndex, const Type& source, EdgeSide side)
{
    RELEASE_ASSERT(operandIndex < 2);
    const Type& other = context.typeOf(comparison.operand(operandIndex ? 0 : 1));
    if (other.isNever())
        return Type::never();

    switch (comparison.opcode) {
    case Opcode::StrictEq:
        return narrowByStrictEquality(context, source, other, side);
    case Opcode::Eq:
        return narrowByLooseEquality(context, source, other, side);
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        return narrowByIntegerBound(comparison, operandIndex, source, other, side);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Type narrowOperandByTest(const TypingContext& context, const Value& test, uint32_t operandIndex, EdgeSide side)
{
    return narrowTypeByTest(context, context.typeOf(test.operand(operandIndex)), test, operandIndex, side);
}

Type narrowTypeByTest(const TypingContext& context, const Type& source, const Value& test, uint32_t operandIndex, EdgeSide side)
{
    Proof proof = provesOf(test.opcode);
    RELEASE_ASSERT(provesForever(proof));
    switch (proof) {
    case Proof::TestedType:
        if (Value::opcodesWithTestedType.contains(test.opcode))
            return narrowByTest(context, source, context.types.at(test.testedType()), side);
        return narrowByTest(context, source, Type::forLayoutSet(test.layouts()), side);
    case Proof::Truthiness:
        return narrowByTruthiness(context, source, side);
    case Proof::Equality:
    case Proof::IntegerBound:
        return narrowByComparison(context, test, operandIndex, source, side);
    case Proof::ExactFunction:
        if (test.opcode == Opcode::IsFunctionOfBuiltin)
            return narrowByTest(context, source, Type::forBuiltin(test.member()), side);
        return narrowByTest(context, source, Type::forFunction(test.function()), side);
    case Proof::None:
    case Proof::CaseValue:
    case Proof::NotNullish:
    case Proof::Shape:
    case Proof::SlotFilled:
    case Proof::NoExtraProperties:
    case Proof::NoFieldOutsideSlot:
    case Proof::BirthFieldsFilled:
    case Proof::NotFrozen:
    case Proof::OwnInstance:
    case Proof::Pristine:
    case Proof::ArrayStorage:
    case Proof::InBounds:
    case Proof::NotThenable:
    case Proof::MemberUnchanged:
    case Proof::PrototypeLinkUnchanged:
    case Proof::EvaluatedOnce:
    case Proof::KeyOrder:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::SourceOpen:
    case Proof::TextMatches:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type typeOfIntegerCase(int64_t value)
{
    if (!Range::full().contains(value))
        return Type::fromPrimitives(Primitive::WholeNumber);
    return zeroesJoined(Type::integer(Range::one(value)));
}

Type narrowSubjectByCase(const TypingContext& context, const Terminator& terminator, uint32_t caseIndex)
{
    return narrowTypeByCase(context, context.typeOf(terminator.subjectOperand()), terminator, caseIndex);
}

Type narrowTypeByCase(const TypingContext& context, const Type& source, const Terminator& terminator, uint32_t caseIndex)
{
    switch (terminator.kind) {
    case TerminatorKind::Switch:
        return source.intersectWith(typeOfIntegerCase(terminator.cases()[caseIndex]), context.types);
    case TerminatorKind::SwitchString:
        return source.intersectWith(Type::stringLiteral(terminator.strings()[caseIndex]), context.types);
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type withoutIntegerCases(const Type& source, PayloadListView<int64_t> cases)
{
    Vector<int64_t, 16> ascendingCases;
    for (int64_t value : cases)
        ascendingCases.append(value);
    AOT::sort(ascendingCases);

    Range integers = source.integers;
    for (int64_t value : ascendingCases) {
        if (!integers.isEmpty() && value == integers.minimum)
            ++integers.minimum;
    }
    for (int64_t value : ascendingCases | std::views::reverse) {
        if (!integers.isEmpty() && value == integers.maximum)
            --integers.maximum;
    }
    if (integers.isEmpty())
        return source.withoutPrimitives(Primitive::Integer);
    Type result = source;
    result.integers = integers;
    return result;
}

static Type withoutStringCases(const Type& source, std::span<const StringID> cases)
{
    Type result = source;
    for (StringID string : cases)
        result.stringLiterals.remove(string);
    if (result.stringLiterals.isEmpty())
        return result.withoutPrimitives(Primitive::LiteralString);
    return result;
}

Type narrowTypeByOtherwise(const Type& source, const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Switch:
        return withoutIntegerCases(source, terminator.cases());
    case TerminatorKind::SwitchString:
        return withoutStringCases(source, terminator.strings());
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type typeOfDeclaredResult(const TypingContext& context, const Terminator& terminator, EdgeResultPosition position)
{
    ResultRules rules { context };
    Type proven = applyEdgeResultRule(rules, terminator, position.targetIndex, position.resultIndex, ResultSide::Proven);
    if (!rules.wasUnproven)
        return proven;
    return applyEdgeResultRule(rules, terminator, position.targetIndex, position.resultIndex, ResultSide::Unproven);
}

Type computeTypeOnEdge(const TypingContext& context, EdgeResultPosition position)
{
    const Function& function = context.function;
    const Terminator& terminator = function.at(position.from).terminator;
    EdgeResult result = edgeResultAt(function, position);
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        if (result.test)
            return narrowOperandByTest(context, function.at(result.test), result.operandIndex, result.side);
        return narrowSubjectByCase(context, terminator, *result.caseIndex);
    case EdgeResultKind::Guard:
        return Type::forOpaque(OpaqueKind::Guard);
    case EdgeResultKind::DeclaredResult:
        return typeOfDeclaredResult(context, terminator, position);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID typeOnEdge(const Program& program, const Function& function, EdgeResultPosition position, TypeArena& arena)
{
    return arena.intern(computeTypeOnEdge({ program, function, arena, TypeLevel::Proven }, position));
}

TypeID expectedTypeOnEdge(const Program& program, const Function& function, EdgeResultPosition position, TypeID provenType, TypeArena& arena)
{
    const Type& type = arena.at(provenType);
    Type met = type.intersectWith(computeTypeOnEdge({ program, function, arena, TypeLevel::Expected }, position), arena);
    if (met.isNever() || met == type)
        return { };
    return arena.intern(met);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
