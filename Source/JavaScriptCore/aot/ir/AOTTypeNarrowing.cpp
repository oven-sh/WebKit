/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeNarrowing.h"

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTCode.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <type_traits>

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

TargetResults targetResultsOf(TerminatorKind kind, uint32_t targetIndex, uint32_t numberOfTargets)
{
    const TargetInfo* info = infoOfTarget(kind, targetIndex, numberOfTargets);
    if (!info)
        return TargetResults::None;
    if (info->takesProofs)
        return TargetResults::Proofs;
    return info->numberOfResults ? TargetResults::DeclaredResults : TargetResults::None;
}

static TypeID narrowedWithin(TypeArena& types, TypeID source, TypeID narrowed)
{
    if (types.isSubset(narrowed, source))
        return narrowed;
    if (!types.at(narrowed).hasObjects())
        return source;
    TypeID withObjectsOfSource = types.unionOf(types.withoutObjects(narrowed), types.withOnlyObjects(source));
    return types.isSubset(withObjectsOfSource, source) ? withObjectsOfSource : source;
}

static void setNarrowedIntegers(TypeParts& parts, Range integers)
{
    if (integers.isEmpty()) {
        parts.primitives &= ~Primitive::Integer;
        parts.integers = Range::empty();
        return;
    }
    parts.integers = integers;
}

static void setNarrowedStringLiterals(TypeParts& parts, StringSetID literals)
{
    parts.stringLiterals = literals;
    if (!literals)
        parts.primitives &= ~Primitive::LiteralString;
}

static TypeID withIntegersNarrowedTo(TypeArena& types, TypeID type, Range integers)
{
    TypeParts parts = types.at(type);
    setNarrowedIntegers(parts, integers);
    return types.intern(parts);
}

static StringID emptyStringKnownTo(const TypeArena& types)
{
    const Program* program = types.program();
    return program ? program->strings.emptyString() : StringID();
}

static Range truthyIntegersOf(Range integers)
{
    if (!integers.minimum)
        return { 1, integers.maximum };
    if (!integers.maximum)
        return { integers.minimum, -1 };
    return integers;
}

static NEVER_INLINE TypeID makeTruthyPart(TypeArena& types, TypeID type, StringID emptyString)
{
    Type whole = types.at(type);
    if (!whole.isJavaScriptValue())
        return type;

    TypeParts parts = whole;
    parts.primitives &= ~(Primitive::UndefinedOrNull | Primitive::False);
    setNarrowedIntegers(parts, truthyIntegersOf(whole.integers));
    bool takesLiteralAway = emptyString && whole.hasLiteralStrings() && types.stringSets().contains(whole.stringLiterals, emptyString);
    if (takesLiteralAway)
        setNarrowedStringLiterals(parts, types.stringSets().without(whole.stringLiterals, emptyString));
    TypeID result = types.intern(parts);
    if (!takesLiteralAway)
        types.noteTypeMade(MakerOfType::TruthyPart, type.bits(), emptyString.bits(), result, !liesInThread(type) && !liesInThread(result));
    return result;
}

static ALWAYS_INLINE TypeID truthyPartWith(TypeArena& types, TypeID type, StringID emptyString)
{
    if (TypeID made = types.typeMadeBefore(MakerOfType::TruthyPart, type.bits(), emptyString.bits()))
        return made;
    return makeTruthyPart(types, type, emptyString);
}

static NEVER_INLINE TypeID makeFalsyPart(TypeArena& types, TypeID type, StringID emptyString)
{
    if (!types.at(type).isJavaScriptValue())
        return type;

    TypeID withoutObjects = types.withoutObjects(type);
    Type whole = types.at(withoutObjects);
    TypeParts parts = whole;
    parts.primitives &= ~(Primitive::True | Primitive::FractionalNumber | Primitive::AnySymbol);
    parts.symbolLiterals = KeySetID();
    setNarrowedIntegers(parts, whole.integers.contains(0) ? Range::one(0) : Range::empty());
    if (emptyString && whole.hasLiteralStrings()) {
        bool mayBeEmpty = types.stringSets().contains(whole.stringLiterals, emptyString);
        setNarrowedStringLiterals(parts, mayBeEmpty ? StringSets::of(emptyString) : StringSetID());
    }
    TypeID result = types.intern(parts);
    types.noteTypeMade(MakerOfType::FalsyPart, type.bits(), emptyString.bits(), result, !liesInThread(type) && !liesInThread(withoutObjects) && !liesInThread(result));
    return result;
}

static ALWAYS_INLINE TypeID falsyPartWith(TypeArena& types, TypeID type, StringID emptyString)
{
    if (TypeID made = types.typeMadeBefore(MakerOfType::FalsyPart, type.bits(), emptyString.bits()))
        return made;
    return makeFalsyPart(types, type, emptyString);
}

TypeID truthyPartOf(TypeArena& types, TypeID type)
{
    return truthyPartWith(types, type, emptyStringKnownTo(types));
}

TypeID falsyPartOf(TypeArena& types, TypeID type)
{
    return falsyPartWith(types, type, emptyStringKnownTo(types));
}

TypeID narrowByTruthiness(const TypingContext& context, TypeID source, EdgeSide side)
{
    StringID emptyString = context.program.strings.emptyString();
    if (side == EdgeSide::Taken)
        return truthyPartWith(context.types, source, emptyString);
    return falsyPartWith(context.types, source, emptyString);
}

bool mayRefuseUntestableBuiltins(const TypingContext& context, TypeID source, TypeID tested)
{
    if (holdsOnlyPrimitives(tested))
        return false;

    TypeArena& types = context.types;
    Type testedType = types.at(tested);
    if (!testedType.hasNamedObjects())
        return false;

    bool namesUntestableBuiltin = false;
    forEachBuiltinOf(types, testedType, [&](BuiltinID builtin) {
        namesUntestableBuiltin |= !isTestableBuiltin(types.builtins(), builtin);
    });
    return namesUntestableBuiltin && !types.isSubset(types.withOnlyObjects(source), tested);
}

static TypeID withoutBuiltinsThatNoTestFinds(TypeArena& types, TypeID tested)
{
    const ObjectTable& objects = types.objects();
    Type whole = types.at(tested);
    TypeParts parts = whole;
    parts.objects = types.objectSets().filtered(whole.objects, [&](ObjectID object) {
        BuiltinID builtin = objects.builtinOf(object);
        return !builtin || isTestableBuiltin(types.builtins(), builtin);
    });
    return types.intern(parts);
}

static bool testFindsEveryObjectOfLayout(const TypingContext& context, LayoutID layout)
{
    return isObjectLayoutOf(context.program.at(layout).kind) && !builtinClassWrittenAbove(context.types, layout);
}

static TypeID withoutLayoutsThatTestMayRefuse(const TypingContext& context, TypeID tested)
{
    if (holdsOnlyPrimitives(tested))
        return tested;

    TypeArena& types = context.types;
    if (TypeID made = types.typeMadeBefore(MakerOfType::WithoutLayoutsThatTestMayRefuse, tested.bits(), 0))
        return made;

    Type whole = types.at(tested);
    if (!whole.unnamedLayouts)
        return tested;
    LayoutSetID layoutsFound = types.layoutSets().filtered(whole.unnamedLayouts, [&](LayoutID layout) {
        return testFindsEveryObjectOfLayout(context, layout);
    });
    TypeID result = tested;
    if (layoutsFound != whole.unnamedLayouts) {
        TypeParts parts = whole;
        parts.unnamedLayouts = layoutsFound;
        result = types.intern(parts);
    }
    types.noteTypeMade(MakerOfType::WithoutLayoutsThatTestMayRefuse, tested.bits(), 0, result, !liesInThread(tested) && !liesInThread(layoutsFound) && !liesInThread(result));
    return result;
}

static TypeID describeLooselyFittingObjectsByShape(TypeArena& types, TypeID passing, TypeID tested)
{
    if (types.keeps() != ArenaKeeps::UntilReset || holdsOnlyPrimitives(passing) || types.isSubset(passing, tested))
        return { };

    Type whole = types.at(passing);
    if (!whole.traitsOfNamed.contains(ObjectTrait::HasLayout))
        return { };

    Type testedType = types.at(tested);
    auto namedByTest = types.at(testedType.objects);
    ObjectSetID objectsKept = types.objectSets().filtered(whole.objects, [&](ObjectID object) {
        return !types.at(object).has(ObjectTrait::HasLayout) || namedByTest.contains(object) || surelyFitsUnknownOf(types, object, testedType);
    });
    if (objectsKept == whole.objects)
        return { };

    TypeID replaced = types.forObjects(types.objectSets().differenceOf(whole.objects, objectsKept));
    TypeParts parts = whole;
    parts.objects = objectsKept;
    TypeID described = types.unionOf(types.intern(parts), types.intersectionOf(types.describedByShape(replaced), tested));
    if (isNeverType(described) || !types.isSubset(described, tested))
        return { };
    return described;
}

ALWAYS_INLINE_INTO_OTHER_FILES TypeID withUndescribedObjectsInside(TypeArena& types, TypeID passing, TypeID holder)
{
    if (holdsOnlyPrimitives(passing) || holdsOnlyPrimitives(holder))
        return passing;
    Type whole = types.at(passing);
    Type holderType = types.at(holder);
    if (!whole.traitsOfNamed.contains(ObjectTrait::HasNoShape) || holderType.anyObject)
        return passing;

    auto namedByHolder = types.at(holderType.objects);
    ObjectSetID objectsKept = types.objectSets().filtered(whole.objects, [&](ObjectID object) {
        const ObjectRecord& record = types.at(object);
        if (!record.has(ObjectTrait::HasNoShape) || record.traits.containsAny({ ObjectTrait::IsCallable, ObjectTrait::IsClosure }))
            return true;
        return namedByHolder.contains(object) || surelyFitsUnknownOf(types, object, holderType);
    });
    if (objectsKept == whole.objects)
        return passing;

    TypeParts parts = whole;
    parts.objects = objectsKept;
    return types.unionOf(types.intern(parts), types.withOnlyObjects(holder));
}

static TypeID withLooselyFittingClassesInside(TypeArena& types, TypeID passing, TypeID tested)
{
    if (holdsOnlyPrimitives(passing) || holdsOnlyPrimitives(tested) || types.isSubset(passing, tested))
        return passing;
    Type whole = types.at(passing);
    Type testedType = types.at(tested);
    if (!testedType.unnamedClasses || testedType.anyFunction)
        return passing;

    const BuiltinTable& builtins = types.builtins();
    Vector<ClassPart, 8> testedClasses;
    testedClasses.append(types.at(testedType.unnamedClasses));
    auto liesInTestedClass = [&](BuiltinClassID builtinClass) {
        return std::ranges::any_of(testedClasses, [&](const ClassPart& part) {
            return builtins.isSameOrSubclassOf(builtinClass, part.builtinClass);
        });
    };

    ObjectSetID objectsKept = whole.objects;
    if (whole.traitsOfNamed.contains(ObjectTrait::HasClass)) {
        auto namedByTest = types.at(testedType.objects);
        objectsKept = types.objectSets().filtered(whole.objects, [&](ObjectID object) {
            const ObjectRecord& record = types.at(object);
            if (!record.has(ObjectTrait::HasClass) || namedByTest.contains(object))
                return true;
            return surelyFitsUnknownOf(types, object, testedType) || liesInTestedClass(record.builtinClass);
        });
    }

    Vector<ClassPart, 8> classesKept;
    size_t numberOfClasses = 0;
    if (whole.unnamedClasses) {
        std::span<const ClassPart> classes = types.at(whole.unnamedClasses);
        numberOfClasses = classes.size();
        for (const ClassPart& part : classes) {
            if (liesInTestedClass(part.builtinClass))
                classesKept.append(part);
        }
    }
    if (objectsKept == whole.objects && classesKept.size() == numberOfClasses)
        return passing;

    TypeParts parts = whole;
    parts.objects = objectsKept;
    if (classesKept.size() != numberOfClasses)
        parts.unnamedClasses = classesKept.isEmpty() ? ClassPartsID() : types.internClassParts(classesKept.span());
    return types.unionOf(types.intern(parts), types.withOnlyObjects(tested));
}

static NEVER_INLINE TypeID narrowByTestAnew(const TypingContext& context, TypeID source, TypeID tested, EdgeSide side)
{
    TypeArena& types = context.types;
    if (side == EdgeSide::Taken) {
        tested = types.withoutBoundsOfFractions(tested);
        TypeID passing = types.intersectionOf(source, tested);
        TypeID inside = passing;
        if (types.keeps() == ArenaKeeps::UntilReset)
            inside = withLooselyFittingClassesInside(types, withUndescribedObjectsInside(types, passing, tested), tested);
        if (TypeID described = describeLooselyFittingObjectsByShape(types, inside, tested))
            return described;
        return inside == passing ? narrowedWithin(types, source, passing) : inside;
    }
    TypeID recognized = mayRefuseUntestableBuiltins(context, source, tested) ? withoutBuiltinsThatNoTestFinds(types, tested) : tested;
    return narrowedWithin(types, source, types.differenceOf(source, withoutLayoutsThatTestMayRefuse(context, recognized)));
}

TypeID narrowByTest(const TypingContext& context, TypeID source, TypeID tested, EdgeSide side)
{
    TypeArena& types = context.types;
    MakerOfType maker = side == EdgeSide::Taken ? MakerOfType::NarrowedByTestThatPassed : MakerOfType::NarrowedByTestThatFailed;
    if (TypeID made = types.typeMadeBefore(maker, source.bits(), tested.bits()))
        return made;
    TypeID result = narrowByTestAnew(context, source, tested, side);
    types.noteTypeMade(maker, source.bits(), tested.bits(), result, !liesInThread(source) && !liesInThread(tested) && !liesInThread(result));
    return result;
}

TypeID narrowByNotNullish(TypeArena& types, TypeID source, EdgeSide side)
{
    if (side == EdgeSide::Taken)
        return types.withoutNullish(source);
    return types.intersectionOf(source, TypeTable::nullish);
}

static TypeID withZeroesJoined(TypeArena& types, TypeID type)
{
    Type whole = types.at(type);
    bool mayBeNegativeZero = whole.primitives & Primitive::WholeNumber;
    bool mayBeZero = whole.integers.contains(0);
    if (mayBeNegativeZero == mayBeZero)
        return type;

    TypeParts parts = whole;
    if (mayBeNegativeZero) {
        parts.primitives |= Primitive::Integer;
        parts.integers = whole.integers.unionWith(Range::one(0));
    } else
        parts.primitives |= Primitive::WholeNumber;
    return types.intern(parts);
}

static TypeID narrowByStrictEquality(TypeArena& types, TypeID source, TypeID other, EdgeSide side)
{
    if (side == EdgeSide::Taken)
        return narrowedWithin(types, source, types.intersectionOf(source, withZeroesJoined(types, other)));
    if (!isSingleValue(types, types.at(other)))
        return source;
    return narrowedWithin(types, source, types.differenceOf(source, other));
}

static TypeID narrowByLooseEquality(TypeArena& types, TypeID source, TypeID other, EdgeSide side)
{
    if (isNullishType(other))
        return narrowByNotNullish(types, source, side == EdgeSide::Taken ? EdgeSide::NotTaken : EdgeSide::Taken);

    Type sourceType = types.at(source);
    Type otherType = types.at(other);
    bool comparesWithoutConversion = (sourceType.isNumber() && otherType.isNumber())
        || (sourceType.isAnyString() && otherType.isAnyString())
        || (sourceType.isBoolean() && otherType.isBoolean());
    return comparesWithoutConversion ? narrowByStrictEquality(types, source, other, side) : source;
}

static Opcode mirroredBoundTest(Opcode comparison)
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

static Opcode negatedBoundTest(Opcode comparison)
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

static Range integersPassingBoundTest(Opcode relation, Range other)
{
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

static TypeID narrowByIntegerBound(TypeArena& types, Opcode comparison, bool sourceIsLeft, TypeID source, TypeID other, EdgeSide side)
{
    if (side == EdgeSide::Taken) {
        other = types.withoutPrimitives(other, Primitive::Undefined);
        if (TypeID defined = types.withoutPrimitives(source, Primitive::Undefined); types.at(defined).isInteger())
            source = defined;
    } else if (types.at(source).primitives & Primitive::Undefined) {
        TypeID defined = types.withoutPrimitives(source, Primitive::Undefined);
        TypeID narrowed = types.at(defined).isInteger() ? narrowByIntegerBound(types, comparison, sourceIsLeft, defined, other, side) : defined;
        return narrowed == defined ? source : types.unionOf(narrowed, TypeTable::undefined);
    }
    Type sourceType = types.at(source);
    Type otherType = types.at(other);
    if (!sourceType.isInteger() || !otherType.isNumberWithinBounds())
        return source;

    Opcode relation = sourceIsLeft ? comparison : mirroredBoundTest(comparison);
    if (side == EdgeSide::NotTaken)
        relation = negatedBoundTest(relation);
    Range integers = sourceType.integers.intersectWith(integersPassingBoundTest(relation, otherType.boundsOfNumbers()));
    if (integers.isEmpty())
        return TypeTable::never;
    return types.integer(integers);
}

static NEVER_INLINE TypeID narrowByComparisonAnew(TypeArena& types, Opcode comparison, bool sourceIsLeft, TypeID source, TypeID other, EdgeSide side)
{
    switch (comparison) {
    case Opcode::StrictEq:
        return narrowByStrictEquality(types, source, other, side);
    case Opcode::Eq:
        return narrowByLooseEquality(types, source, other, side);
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        return narrowByIntegerBound(types, comparison, sourceIsLeft, source, other, side);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static MakerOfType makerOfNarrowingByComparison(Opcode comparison, bool sourceIsLeft, EdgeSide side)
{
    unsigned relation = 0;
    switch (comparison) {
    case Opcode::StrictEq:
        relation = 0;
        break;
    case Opcode::Eq:
        relation = 1;
        break;
    case Opcode::Less:
        relation = sourceIsLeft ? 2 : 3;
        break;
    case Opcode::LessEq:
        relation = sourceIsLeft ? 4 : 5;
        break;
    case Opcode::Greater:
        relation = sourceIsLeft ? 6 : 7;
        break;
    case Opcode::GreaterEq:
        relation = sourceIsLeft ? 8 : 9;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return static_cast<MakerOfType>(static_cast<unsigned>(MakerOfType::FirstNarrowedByComparison) + relation * 2 + (side == EdgeSide::Taken ? 1 : 0));
}

TypeID narrowByComparison(const TypingContext& context, Opcode comparison, bool sourceIsLeft, TypeID source, TypeID other, EdgeSide side)
{
    if (!other)
        return source;
    if (isNeverType(other))
        return TypeTable::never;

    TypeArena& types = context.types;
    MakerOfType maker = makerOfNarrowingByComparison(comparison, sourceIsLeft, side);
    if (TypeID made = types.typeMadeBefore(maker, source.bits(), other.bits()))
        return made;
    TypeID result = narrowByComparisonAnew(types, comparison, sourceIsLeft, source, other, side);
    types.noteTypeMade(maker, source.bits(), other.bits(), result, !liesInThread(source) && !liesInThread(other) && !liesInThread(result));
    return result;
}

static TypeID typeEqualToIntegerCase(TypeArena& types, int64_t value)
{
    if (!Range::full().contains(value))
        return TypeArena::fromPrimitives(Primitive::WholeNumber);
    return withZeroesJoined(types, types.integer(Range::one(value)));
}

TypeID narrowByIntegerCase(const TypingContext& context, TypeID source, int64_t caseValue)
{
    TypeArena& types = context.types;
    return narrowedWithin(types, source, types.intersectionOf(source, typeEqualToIntegerCase(types, caseValue)));
}

TypeID narrowByStringCase(const TypingContext& context, TypeID source, StringID caseValue)
{
    TypeArena& types = context.types;
    return narrowedWithin(types, source, types.intersectionOf(source, types.stringLiteral(caseValue)));
}

static TypeID typeLeftByIntegerCases(TypeArena& types, TypeID source, std::span<int64_t> cases)
{
    Range integers = types.at(source).integers;
    if (integers.isEmpty())
        return source;

    AOT::sort(cases);
    for (int64_t value : cases) {
        if (value == integers.minimum)
            ++integers.minimum;
    }
    for (int64_t value : cases | std::views::reverse) {
        if (value == integers.maximum)
            --integers.maximum;
    }
    return withIntegersNarrowedTo(types, source, integers);
}

static TypeID typeLeftByStringCases(TypeArena& types, TypeID source, std::span<StringID> cases)
{
    Type whole = types.at(source);
    if (!whole.hasLiteralStrings())
        return source;

    AOT::sort(cases);
    TypeParts parts = whole;
    setNarrowedStringLiterals(parts, types.stringSets().filtered(whole.stringLiterals, [&](StringID literal) {
        return !std::ranges::binary_search(cases, literal);
    }));
    return types.intern(parts);
}

TypeID narrowByOtherwise(TypeArena& types, TypeID source, std::span<const int64_t> cases)
{
    Vector<int64_t, 16> sortedCases(cases);
    return typeLeftByIntegerCases(types, source, sortedCases.mutableSpan());
}

TypeID narrowByShapeCase(const TypingContext& context, TypeID source, LayoutSetID caseLayouts)
{
    return narrowByTest(context, source, context.types.forLayoutSet(caseLayouts), EdgeSide::Taken);
}

TypeID narrowByOtherwise(const TypingContext& context, TypeID source, std::span<const LayoutSetID> cases)
{
    TypeArena& types = context.types;
    Vector<LayoutID, 32> layoutsOfCases;
    for (LayoutSetID layouts : cases) {
        types.at(layouts).forEach([&](LayoutID layout) {
            layoutsOfCases.append(layout);
        });
    }
    if (layoutsOfCases.isEmpty())
        return source;
    return narrowByTest(context, source, types.forLayouts(layoutsOfCases.span()), EdgeSide::NotTaken);
}

static TypeID typeOfObjectsWithStructureOfClass(TypeArena& types, TypeID source, LayoutID classLayout)
{
    TypeID instances = types.forLayout(classLayout);
    TypeID passing = types.intersectionOf(source, instances);
    return types.at(passing).hasObjects() && types.isSubset(passing, instances) ? passing : instances;
}

static TypeID narrowByPositionBelowLength(TypeArena& types, TypeID position, TypeID length)
{
    Type lengthType = length ? types.at(length) : Type();
    if (!length || !types.at(position).isInteger() || !lengthType.isInteger() || lengthType.integers.maximum < 1)
        return position;
    return narrowedWithin(types, position, types.intersectionOf(position, types.integer(Range { 0, lengthType.integers.maximum - 1 })));
}

template<typename Holder> static constexpr bool isCursorOverInput = std::is_same_v<Holder, BodyValue> || std::is_same_v<Holder, BodyTerminator>;

template<typename Test>
static TypeID typeThatTestAsksFor(TypeArena& types, const Test& test)
{
    switch (opcodeOf(test)) {
    case Opcode::TypeTest:
        return test.testedType();
    case Opcode::IsClosureOf:
        return types.forFunction(test.function());
    case Opcode::ShapeTest:
        if constexpr (!isCursorOverInput<Test>)
            return types.forLayoutSet(test.layouts());
        break;
    case Opcode::IsPlainInstanceOf:
    case Opcode::FindsDeclaredMember:
        if constexpr (!isCursorOverInput<Test>)
            return types.forLayoutSet(test.receivers());
        break;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Test>
TypeID typeOfEdgeResult(const TypingContext& context, const Test& test, OperandTypes operandsOfTest, uint32_t narrowedOperandIndex, EdgeSide side)
{
    TypeID source = operandsOfTest[narrowedOperandIndex];
    RELEASE_ASSERT(source);

    Opcode opcode = opcodeOf(test);
    switch (provesOf(opcode)) {
    case Proof::TestedType:
    case Proof::Shape:
    case Proof::ExactFunction:
        if constexpr (!isCursorOverInput<Test>) {
            if (side == EdgeSide::NotTaken && canFailForMarkedObject(test))
                return source;
            if ((opcode == Opcode::IsPlainInstanceOf || opcode == Opcode::FindsDeclaredMember) && (side == EdgeSide::NotTaken || !test.receivers()))
                return source;
        } else if (opcode == Opcode::IsPlainInstanceOf || opcode == Opcode::FindsDeclaredMember)
            return source;
        return narrowByTest(context, source, typeThatTestAsksFor(context.types, test), side);
    case Proof::Truthiness:
        return narrowByTruthiness(context, source, side);
    case Proof::Equality:
        if constexpr (!isCursorOverInput<Test>) {
            if (opcode == Opcode::StringEqualsLiteral || opcode == Opcode::ValueEqualsLiteral) {
                static_assert(!OperandIndex::StringEqualsLiteral::string && !OperandIndex::ValueEqualsLiteral::value);
                if (narrowedOperandIndex)
                    return source;
                return narrowByComparison(context, Opcode::StrictEq, true, source, context.types.stringLiteral(test.literal()), side);
            }
        }
        [[fallthrough]];
    case Proof::IntegerBound:
        RELEASE_ASSERT(narrowedOperandIndex < 2);
        return narrowByComparison(context, opcode, !narrowedOperandIndex, source, operandsOfTest[narrowedOperandIndex ? 0 : 1], side);
    case Proof::PositionBelowLength:
        return side == EdgeSide::Taken && !narrowedOperandIndex ? narrowByPositionBelowLength(context.types, source, operandsOfTest[1]) : source;
    case Proof::NotNullish:
        return narrowByNotNullish(context.types, source, side);
    case Proof::StructureOfClass:
        if constexpr (!isCursorOverInput<Test>) {
            if (side == EdgeSide::Taken)
                return typeOfObjectsWithStructureOfClass(context.types, source, test.layout());
        }
        return source;
    case Proof::None:
    case Proof::CaseValue:
    case Proof::SlotFilled:
    case Proof::NoExtraProperties:
    case Proof::NoFieldOutsideSlot:
    case Proof::HeldFieldsFilled:
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
        return source;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Element, typename List>
static Vector<Element, 16> casesCopiedFrom(const List& list)
{
    Vector<Element, 16> cases;
    cases.reserveInitialCapacity(list.size());
    for (Element element : list)
        cases.append(element);
    return cases;
}

template<typename Switch>
TypeID typeOfCaseResult(const TypingContext& context, const Switch& terminator, TypeID subject, uint32_t targetIndex)
{
    RELEASE_ASSERT(subject);

    switch (terminatorKindOf(terminator)) {
    case TerminatorKind::Switch: {
        auto cases = terminator.cases();
        if (targetIndex < cases.size())
            return narrowByIntegerCase(context, subject, cases[targetIndex]);
        Vector<int64_t, 16> sortedCases = casesCopiedFrom<int64_t>(cases);
        return typeLeftByIntegerCases(context.types, subject, sortedCases.mutableSpan());
    }
    case TerminatorKind::SwitchString: {
        auto cases = terminator.strings();
        if (targetIndex < cases.size())
            return narrowByStringCase(context, subject, cases[targetIndex]);
        Vector<StringID, 16> sortedCases = casesCopiedFrom<StringID>(cases);
        return typeLeftByStringCases(context.types, subject, sortedCases.mutableSpan());
    }
    case TerminatorKind::SwitchOnShape:
        if constexpr (!isCursorOverInput<Switch>) {
            std::span<const LayoutSetID> cases = terminator.layoutSets();
            if (targetIndex < cases.size())
                return narrowByShapeCase(context, subject, cases[targetIndex]);
            return narrowByOtherwise(context, subject, cases);
        }
        break;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template TypeID typeOfEdgeResult<Draft>(const TypingContext&, const Draft&, OperandTypes, uint32_t, EdgeSide);
template TypeID typeOfEdgeResult<Operation>(const TypingContext&, const Operation&, OperandTypes, uint32_t, EdgeSide);
template TypeID typeOfEdgeResult<BodyValue>(const TypingContext&, const BodyValue&, OperandTypes, uint32_t, EdgeSide);

template TypeID typeOfCaseResult<TerminatorDraft>(const TypingContext&, const TerminatorDraft&, TypeID, uint32_t);
template TypeID typeOfCaseResult<Terminator>(const TypingContext&, const Terminator&, TypeID, uint32_t);
template TypeID typeOfCaseResult<BodyTerminator>(const TypingContext&, const BodyTerminator&, TypeID, uint32_t);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
