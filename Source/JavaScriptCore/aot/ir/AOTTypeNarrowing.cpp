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

// intersectionOf() and differenceOf() may overapproximate, so the result is not always a subset of `source`.
// It is among the objects that they do: one that only may fit stays named, as builtin(Math) does beside any function. So the objects of `source` stand in for those, and what is said of the rest holds:
// a value that may be undefined and has passed a test for functions is not undefined.
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

// `type` holds integers.
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

// A range has no holes, so zero can be removed only at one of its ends.
static Range truthyIntegersOf(Range integers)
{
    if (!integers.minimum)
        return { 1, integers.maximum };
    if (!integers.maximum)
        return { integers.minimum, -1 };
    return integers;
}

// With a null `emptyString` nobody knows which literal is empty, and the literals stay on both sides.
// WholeNumber holds -0 and NaN beside the infinities, String the empty string, BigInt 0n: each stays on both sides.
static TypeID truthyPartWith(TypeArena& types, TypeID type, StringID emptyString)
{
    Type whole = types.at(type);
    if (!whole.isJavaScriptValue())
        return type;

    TypeParts parts = whole;
    parts.primitives &= ~(Primitive::UndefinedOrNull | Primitive::False);
    setNarrowedIntegers(parts, truthyIntegersOf(whole.integers));
    if (emptyString && whole.hasLiteralStrings() && types.stringSets().contains(whole.stringLiterals, emptyString))
        setNarrowedStringLiterals(parts, types.stringSets().without(whole.stringLiterals, emptyString));
    return types.intern(parts);
}

static TypeID falsyPartWith(TypeArena& types, TypeID type, StringID emptyString)
{
    if (!types.at(type).isJavaScriptValue())
        return type;

    Type whole = types.at(types.withoutObjects(type));
    TypeParts parts = whole;
    parts.primitives &= ~(Primitive::True | Primitive::FractionalNumber | Primitive::AnySymbol);
    parts.symbolLiterals = KeySetID();
    setNarrowedIntegers(parts, whole.integers.contains(0) ? Range::one(0) : Range::empty());
    if (emptyString && whole.hasLiteralStrings()) {
        bool mayBeEmpty = types.stringSets().contains(whole.stringLiterals, emptyString);
        setNarrowedStringLiterals(parts, mayBeEmpty ? StringSets::of(emptyString) : StringSetID());
    }
    return types.intern(parts);
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

// `tested` names objects.
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

// A module namespace has no shape word. Neither has a cell of a built-in class: an instance of a class below one, or an object of a layout that holds dictionaries.
// So ShapeTest and SwitchOnShape refuse them. TypeTest does find a namespace, but `tested` does not say which test asks.
static bool testFindsEveryObjectOfLayout(const TypingContext& context, LayoutID layout)
{
    return isObjectLayoutOf(context.program.at(layout).kind) && !builtinClassWrittenAbove(context.types, layout);
}

static TypeID withoutLayoutsThatTestMayRefuse(const TypingContext& context, TypeID tested)
{
    if (holdsOnlyPrimitives(tested))
        return tested;

    TypeArena& types = context.types;
    Type whole = types.at(tested);
    if (!whole.unnamedLayouts)
        return tested;
    LayoutSetID layoutsFound = types.layoutSets().filtered(whole.unnamedLayouts, [&](LayoutID layout) {
        return testFindsEveryObjectOfLayout(context, layout);
    });
    if (layoutsFound == whole.unnamedLayouts)
        return tested;

    TypeParts parts = whole;
    parts.unnamedLayouts = layoutsFound;
    return types.intern(parts);
}

// intersectionOf() keeps a named object whole as soon as one of the layouts it may have fits `tested`, so `passing` is not always inside `tested`.
// Whatever passes a test has one of the tested shapes for life, and the slot operations behind the test need a type that proves their receivers.
// So Compile replaces such an object with `unknown` within the tested layouts it may have. The result is not a subset of a source that only names the object.
// Facts and Plan look up an object's cells by its name, so their arenas keep the object.
// Returns null if no object is replaced.
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

TypeID withUndescribedObjectsInside(TypeArena& types, TypeID passing, TypeID holder)
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

// No type says that an object is an instance of two classes. So of a tested class and a class that no test tells from it intersectionOf() keeps both, and it keeps an object that is named with such a class:
// `arguments` behind a test for arrays would be what it was in front of the test, and whoever takes nothing but an array needs a type that says so.
// What has passed is an instance of a tested class for life, so `unknown` within `tested` takes the place of the other class and of the object.
// A class below a tested class stays, and so does what is named with it: a TypeError is an Error, and says more.
// A test for layouts alone turns the cells of a built-in class away: what it leaves named stays named.
// So does a test for functions: a proxy that can be called stays a proxy, and the positions that Facts has seen it reach name it.
static TypeID withLooselyFittingClassesInside(TypeArena& types, TypeID passing, TypeID tested)
{
    if (holdsOnlyPrimitives(passing) || holdsOnlyPrimitives(tested) || types.isSubset(passing, tested))
        return passing;
    Type whole = types.at(passing);
    Type testedType = types.at(tested);
    if (!testedType.unnamedClasses || testedType.anyFunction)
        return passing;

    // Interning may move the parts that an id names, so they are copied before anything interns.
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

TypeID narrowByTest(const TypingContext& context, TypeID source, TypeID tested, EdgeSide side)
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

TypeID narrowByNotNullish(TypeArena& types, TypeID source, EdgeSide side)
{
    if (side == EdgeSide::Taken)
        return types.withoutNullish(source);
    return types.intersectionOf(source, TypeTable::nullish);
}

// 0 === -0: whatever may be one of them equals the other.
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

// The integers that stand in `relation` to some member of `other`.
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

// A failed comparison proves its negation only if neither side can be NaN.
// `i < n / 2`: an integer below a number that is no greater than an integer lies below that integer too.
// `while (point[depth] > i)`: undefined stands in no relation to anything, so where one has held neither side was undefined.
static TypeID narrowByIntegerBound(TypeArena& types, Opcode comparison, bool sourceIsLeft, TypeID source, TypeID other, EdgeSide side)
{
    if (side == EdgeSide::Taken) {
        other = types.withoutPrimitives(other, Primitive::Undefined);
        if (TypeID defined = types.withoutPrimitives(source, Primitive::Undefined); types.at(defined).isInteger())
            source = defined;
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

TypeID narrowByComparison(const TypingContext& context, Opcode comparison, bool sourceIsLeft, TypeID source, TypeID other, EdgeSide side)
{
    if (!other)
        return source;
    if (isNeverType(other))
        return TypeTable::never;

    switch (comparison) {
    case Opcode::StrictEq:
        return narrowByStrictEquality(context.types, source, other, side);
    case Opcode::Eq:
        return narrowByLooseEquality(context.types, source, other, side);
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        return narrowByIntegerBound(context.types, comparison, sourceIsLeft, source, other, side);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
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

// A range has no holes, so only cases at its ends can be removed. Sorts `cases`.
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

// Sorts `cases`.
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

TypeID narrowByOtherwise(TypeArena& types, TypeID source, std::span<const StringID> cases)
{
    Vector<StringID, 16> sortedCases(cases);
    return typeLeftByStringCases(types, source, sortedCases.mutableSpan());
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

// Engine-only schema entries never appear in a wire body.
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
    case Opcode::IsFunctionOfBuiltin:
        if constexpr (!isCursorOverInput<Test>)
            return types.forBuiltin(test.member());
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
        return narrowByTest(context, source, typeThatTestAsksFor(context.types, test), side);
    case Proof::Truthiness:
        return narrowByTruthiness(context, source, side);
    case Proof::Equality:
        // The other side of the comparison is the text in the operation's own words.
        if constexpr (!isCursorOverInput<Test>) {
            if (opcode == Opcode::StringEqualsLiteral || opcode == Opcode::ValueEqualsLiteral) {
                // Neither the length nor the flat string that the comparison was handed is a side of it.
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
    case Proof::NotNullish:
        return narrowByNotNullish(context.types, source, side);
    case Proof::None:
    case Proof::CaseValue:
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
