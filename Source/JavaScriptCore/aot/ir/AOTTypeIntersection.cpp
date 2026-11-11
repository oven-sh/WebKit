/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeRelations.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

using IntersectionClassParts = Vector<ClassPart, 8>;

static IntersectionClassParts copyClassPartsForIntersection(const TypeArena& arena, ClassPartsID classes)
{
    IntersectionClassParts parts;
    if (classes)
        parts.append(arena.at(classes));
    return parts;
}

template<typename Sets, typename SetID>
static void intersectLiteralSets(TypeParts& result, SetID TypeParts::* literals, Sets& sets, const Type& a, const Type& b, uint16_t widenedBit, uint16_t literalBit)
{
    if (a.primitives & b.primitives & widenedBit) {
        result.primitives |= widenedBit;
        return;
    }
    if (a.primitives & widenedBit)
        result.*literals = b.*literals;
    else if (b.primitives & widenedBit)
        result.*literals = a.*literals;
    else
        result.*literals = sets.intersectionOf(a.*literals, b.*literals);
    if (result.*literals)
        result.primitives |= literalBit;
}

static void intersectBoundsOfFractions(TypeParts& result, const Type& a, const Type& b)
{
    if (!(result.primitives & Primitive::FractionalNumber))
        return;
    if (a.boundsOfFractions.isEmpty() || b.boundsOfFractions.isEmpty()) {
        result.boundsOfFractions = a.boundsOfFractions.unionWith(b.boundsOfFractions);
        return;
    }
    Range bounds = a.boundsOfFractions.intersectWith(b.boundsOfFractions);
    if (bounds.minimum >= bounds.maximum)
        result.primitives &= ~Primitive::FractionalNumber;
    else
        result.boundsOfFractions = bounds;
}

static void intersectPrimitiveParts(TypeParts& result, TypeArena& arena, const Type& a, const Type& b)
{
    result.primitives = a.primitives & b.primitives & plainPrimitives;
    intersectBoundsOfFractions(result, a, b);
    Range integers = a.integers.intersectWith(b.integers);
    if (!integers.isEmpty()) {
        result.integers = integers;
        result.primitives |= Primitive::Integer;
    }
    intersectLiteralSets(result, &TypeParts::stringLiterals, arena.stringSets(), a, b, Primitive::String, Primitive::LiteralString);
    intersectLiteralSets(result, &TypeParts::symbolLiterals, arena.keySets(), a, b, Primitive::Symbol, Primitive::LiteralSymbol);
    if (a.opaque == b.opaque)
        result.opaque = a.opaque;
    if (a.environment == b.environment)
        result.environment = a.environment;
}

static bool mayIntersectUnknownOf(TypeReader reader, ObjectID object, const Type& other, bool otherMayHoldAnyClosure, LayoutsFoundForCellsNobodyDescribes& found)
{
    if (mayFitUnknownOf(reader, object, other, found))
        return true;

    const BuiltinTable& builtins = reader.builtins();
    std::span<const ClassPart> classes;
    if (other.unnamedClasses)
        classes = reader.at(other.unnamedClasses);

    if (BuiltinID builtin = reader.objects().builtinOf(object)) {
        return other.anyFunction || std::ranges::any_of(classes, [&](const ClassPart& part) {
            return mayBeInstanceOf(builtins, builtin, part.builtinClass);
        });
    }

    const ObjectRecord& record = reader.at(object);
    if (otherMayHoldAnyClosure && record.traits.containsAny({ ObjectTrait::IsClosure, ObjectTrait::IsCallable }))
        return true;
    return record.has(ObjectTrait::HasNoShape) && std::ranges::any_of(classes, [&](const ClassPart& part) {
        return !hasInstanceTest(builtins, part.builtinClass);
    });
}

ObjectSetID TypeArena::membersWithAliasIn(ObjectSetID set, ObjectSetID namedByOther)
{
    if (!set || !namedByOther || set == namedByOther)
        return { };
    auto members = at(namedByOther);
    bool isRemembered = !members.isList() || !at(set).isList();
    if (isRemembered) {
        if (std::optional<uint32_t> known = remembered(Operation::MembersWithAlias, set.bits(), namedByOther.bits()))
            return ObjectSetID::fromBits(*known);
    }

    ObjectSetAliasIndex index(*this, members);
    Vector<ObjectID, 32> withAlias;
    m_objectSets.forEachAdded(set, namedByOther, [&](ObjectID object) {
        if (index.namesAliasOf(object))
            withAlias.append(object);
    });
    ObjectSetID result = m_objectSets.ofSorted(withAlias.span());
    if (isRemembered)
        remember(Operation::MembersWithAlias, set.bits(), namedByOther.bits(), result.bits());
    return result;
}

template<typename Fits>
static ObjectSetID membersNamedByOtherOrFitting(TypeArena& arena, ObjectSetID set, ObjectSetID other, bool someMemberMayFit, const Fits& fits)
{
    ObjectSets& sets = arena.objectSets();
    if (arena.at(set).isList() && arena.at(other).isList()) {
        auto namedByOther = arena.at(other);
        ObjectSetAliasIndex index(arena, namedByOther);
        return sets.filtered(set, [&](ObjectID object) {
            if (other && (namedByOther.contains(object) || index.namesAliasOf(object)))
                return true;
            return someMemberMayFit && fits(object);
        });
    }

    ObjectSetID withAlias = arena.membersWithAliasIn(set, other);
    if (!someMemberMayFit)
        return sets.unionOf(sets.intersectionOf(set, other), withAlias);
    auto namedByOther = arena.at(other);
    auto namedUnderAnotherNumber = arena.at(withAlias);
    return sets.filtered(set, [&](ObjectID object) {
        return namedByOther.contains(object) || namedUnderAnotherNumber.contains(object) || fits(object);
    });
}

static ObjectSetID askEachNamedObjectOfIntersection(TypeArena& arena, ObjectSetID namedObjects, const Type& other, bool wholeUnknownOfOtherStays, bool otherMayHoldAnyClosure)
{
    TypeReader reader(arena);
    const ObjectTable& objects = arena.objects();
    LayoutsFoundForCellsNobodyDescribes found;
    return membersNamedByOtherOrFitting(arena, namedObjects, other.objects, other.hasUnknown() || otherMayHoldAnyClosure, [&](ObjectID object) {
        if (wholeUnknownOfOtherStays && objects.at(object).has(ObjectTrait::IsMerged))
            return surelyFitsUnknownOf(reader, object, other);
        return mayIntersectUnknownOf(reader, object, other, otherMayHoldAnyClosure, found);
    });
}

namespace {

class SharedNamedObjects final : public SharedAnswers::Answer {
public:
    enum class Form : uint8_t { NoObject, EveryObjectOfSet, List, EveryObjectButList };

    SharedNamedObjects(Form form, uint32_t list)
        : form(form)
        , list(list)
    {
    }

    size_t sizeInBytes() const final { return sizeof(SharedNamedObjects); }

    const Form form;
    const uint32_t list;
};

} // anonymous namespace

static ObjectSetID objectSetOfSharedList(TypeArena& arena, SharedAnswers& answers, uint32_t number)
{
    constexpr uint8_t question = static_cast<uint8_t>(AnswerKeptInArena::ObjectSetOfList);
    if (std::optional<uint32_t> kept = arena.rememberedAnswer(question, number, 0))
        return ObjectSetID::fromBits(*kept);
    std::span<const uint32_t> members = answers.listAt(number);
    RELEASE_ASSERT(!members.empty());
    static_assert(sizeof(ObjectID) == sizeof(uint32_t));
    ObjectSetID set = members.size() == 1 ? setOfOne<ObjectSetID>(ObjectID(members[0])) : arena.objectSets().ofSorted(unsafeMakeSpan(reinterpret_cast<const ObjectID*>(members.data()), members.size()));
    arena.rememberAnswer(question, number, 0, set.bits());
    return set;
}

static ObjectSetID filterSetOfProgramOncePerBuild(TypeArena& arena, SharedAnswers& answers, SharedQuestion kind, ObjectSetID set, std::array<uint32_t, 4> words, NOESCAPE const ScopedLambda<ObjectSetID(ObjectSetID)>& askEachMemberOf)
{
    RELEASE_ASSERT(liesInProgram(set));
    SharedAnswers::Question question { kind, set.bits(), words[0], words[1], words[2], words[3] };
    if (const SharedAnswers::Answer* found = answers.find(question)) {
        const auto& shared = static_cast<const SharedNamedObjects&>(*found);
        ObjectSetID picked;
        switch (shared.form) {
        case SharedNamedObjects::Form::NoObject:
            break;
        case SharedNamedObjects::Form::EveryObjectOfSet:
            picked = set;
            break;
        case SharedNamedObjects::Form::List:
            picked = objectSetOfSharedList(arena, answers, shared.list);
            break;
        case SharedNamedObjects::Form::EveryObjectButList:
            picked = arena.objectSets().differenceOf(set, objectSetOfSharedList(arena, answers, shared.list));
            break;
        }
        if constexpr (SharedAnswers::worksFoundAnswersOutAgain)
            RELEASE_ASSERT(askEachMemberOf(set) == picked);
        return picked;
    }

    ObjectSetID picked = askEachMemberOf(set);
    if (!picked)
        answers.add(question, makeUniqueWithoutFastMallocCheck<SharedNamedObjects>(SharedNamedObjects::Form::NoObject, 0u));
    else if (picked == set)
        answers.add(question, makeUniqueWithoutFastMallocCheck<SharedNamedObjects>(SharedNamedObjects::Form::EveryObjectOfSet, 0u));
    else {
        ObjectSetID leftOut = arena.objectSets().differenceOf(set, picked);
        bool listsWhatIsLeftOut = arena.objectSets().sizeOf(leftOut) < arena.objectSets().sizeOf(picked);
        ObjectSetID listed = listsWhatIsLeftOut ? leftOut : picked;
        Vector<uint32_t> members;
        members.reserveInitialCapacity(arena.objectSets().sizeOf(listed));
        arena.at(listed).forEach([&](ObjectID object) {
            members.append(object.index());
        });
        SharedNamedObjects::Form form = listsWhatIsLeftOut ? SharedNamedObjects::Form::EveryObjectButList : SharedNamedObjects::Form::List;
        answers.add(question, makeUniqueWithoutFastMallocCheck<SharedNamedObjects>(form, answers.internList(members.span())));
    }
    return picked;
}

ObjectSetID filterMembersOncePerBuild(TypeArena& arena, SharedQuestion kind, ObjectSetID set, std::array<uint32_t, 4> words, NOESCAPE const ScopedLambda<ObjectSetID(ObjectSetID)>& askEachMemberOf)
{
    ObjectSets& sets = arena.objectSets();
    SharedAnswers* answers = arena.keeps() == ArenaKeeps::UntilReset && sets.sizeOf(set) > membersPerSetChunk ? arena.table().sharedAnswers() : nullptr;
    ObjectSetID ofProgram = answers ? sets.setOfProgramBehind(set) : ObjectSetID();
    if (!ofProgram)
        return askEachMemberOf(set);
    if (sets.sizeOf(set) < sets.sizeOf(ofProgram) / 2 && !answers->find({ kind, ofProgram.bits(), words[0], words[1], words[2], words[3] }))
        return askEachMemberOf(set);

    ObjectSetID picked = filterSetOfProgramOncePerBuild(arena, *answers, kind, ofProgram, words, askEachMemberOf);
    if (set != ofProgram) {
        picked = sets.intersectionOf(picked, set);
        if (ObjectSetID fromElsewhere = sets.differenceOf(set, ofProgram))
            picked = sets.unionOf(picked, askEachMemberOf(fromElsewhere));
        if constexpr (SharedAnswers::worksFoundAnswersOutAgain)
            RELEASE_ASSERT(askEachMemberOf(set) == picked);
    }
    sets.noteSetOfProgramBehind(picked, ofProgram);
    return picked;
}

static ALWAYS_INLINE ObjectSetID intersectNamedObjects(TypeArena& arena, const Type& type, const Type& other, bool wholeUnknownOfOtherStays)
{
    if (!type.objects || other.anyObject || type.objects == other.objects)
        return type.objects;

    bool otherMayHoldAnyClosure = mayContainAnyClosure(TypeReader(arena), other);
    bool isWide = arena.objectSets().sizeOf(type.objects) > membersPerSetChunk;
    uint32_t objectsOfOther = isWide ? arena.setOfRound(other.objects).word : other.objects.bits();
    uint32_t layoutsOfOther = isWide ? arena.setOfRound(other.unnamedLayouts).word : other.unnamedLayouts.bits();
    if (liesInThread(ObjectSetID::fromBits(objectsOfOther)) || liesInThread(LayoutSetID::fromBits(layoutsOfOther)) || liesInThread(other.unnamedClasses))
        return askEachNamedObjectOfIntersection(arena, type.objects, other, wholeUnknownOfOtherStays, otherMayHoldAnyClosure);

    uint32_t flagsOfOther = static_cast<uint32_t>(other.anyFunction) | static_cast<uint32_t>(wholeUnknownOfOtherStays) << 1 | static_cast<uint32_t>(otherMayHoldAnyClosure) << 2;
    return filterMembersOncePerBuild(arena, SharedQuestion::NamedObjectsOfIntersection, type.objects, { objectsOfOther, layoutsOfOther, other.unnamedClasses.bits(), flagsOfOther }, scopedLambda<ObjectSetID(ObjectSetID)>([&](ObjectSetID namedObjects) {
        return askEachNamedObjectOfIntersection(arena, namedObjects, other, wholeUnknownOfOtherStays, otherMayHoldAnyClosure);
    }));
}

static TypeID intersectKnownContents(TypeArena& arena, TypeID first, TypeID second)
{
    if (first == TypeTable::anything)
        return second;
    if (second == TypeTable::anything)
        return first;
    return arena.intersectionOf(first, second);
}

static ClassPart intersectPartsOfOneClass(TypeArena& arena, const ClassPart& first, const ClassPart& second, DeclaredParts declared)
{
    if (!second.hasWrittenContents())
        return first;
    if (!first.hasWrittenContents() || declared == DeclaredParts::FromSecond)
        return second;

    ClassPart result = first;
    for (unsigned position = 0; position < mostContentsOfClass; ++position)
        result.writtenContents[position] = intersectKnownContents(arena, first.writtenContentsAt(position), second.writtenContentsAt(position));
    return result;
}

static bool classMayIntersectUnknownOf(const BuiltinTable& builtins, BuiltinClassID builtinClass, std::span<const ClassPart> otherParts, bool otherHoldsAnyFunction)
{
    if (otherHoldsAnyFunction && isFunctionClass(builtins, builtinClass))
        return true;
    return std::ranges::any_of(otherParts, [&](const ClassPart& other) {
        return other.builtinClass != builtinClass
            && relate(builtins, builtinClass, other.builtinClass) == ClassRelation::Unknown
            && !builtins.isSameOrSubclassOf(other.builtinClass, builtinClass);
    });
}

static ClassPartsID intersectClassParts(TypeArena& arena, std::span<const ClassPart> first, bool firstHoldsAnyFunction, std::span<const ClassPart> second, bool secondHoldsAnyFunction, DeclaredParts declared)
{
    const BuiltinTable& builtins = arena.builtins();
    IntersectionClassParts kept;
    auto keepIfMayBeBoth = [&](const ClassPart& part, std::span<const ClassPart> otherParts, bool otherHoldsAnyFunction) {
        if (classMayIntersectUnknownOf(builtins, part.builtinClass, otherParts, otherHoldsAnyFunction))
            kept.append(part);
    };

    size_t indexInFirst = 0;
    size_t indexInSecond = 0;
    while (indexInFirst < first.size() && indexInSecond < second.size()) {
        const ClassPart& partOfFirst = first[indexInFirst];
        const ClassPart& partOfSecond = second[indexInSecond];
        if (partOfFirst.builtinClass < partOfSecond.builtinClass) {
            keepIfMayBeBoth(partOfFirst, second, secondHoldsAnyFunction);
            ++indexInFirst;
        } else if (partOfSecond.builtinClass < partOfFirst.builtinClass) {
            keepIfMayBeBoth(partOfSecond, first, firstHoldsAnyFunction);
            ++indexInSecond;
        } else {
            kept.append(intersectPartsOfOneClass(arena, partOfFirst, partOfSecond, declared));
            ++indexInFirst;
            ++indexInSecond;
        }
    }
    for (const ClassPart& part : first.subspan(indexInFirst))
        keepIfMayBeBoth(part, second, secondHoldsAnyFunction);
    for (const ClassPart& part : second.subspan(indexInSecond))
        keepIfMayBeBoth(part, first, firstHoldsAnyFunction);

    if (kept.isEmpty())
        return { };
    return arena.internClassParts(kept.span());
}

static LayoutSetID intersectLayoutsWithClasses(TypeArena& arena, LayoutSetID layouts, std::span<const ClassPart> classes)
{
    if (!layouts || classes.empty())
        return { };
    TypeReader reader(arena);
    return arena.layoutSets().filtered(layouts, [&](LayoutID layout) {
        return mayBeInsideSomeClassPart(reader, layout, classes);
    });
}

static void intersectUnknownObjects(TypeParts& result, TypeArena& arena, const Type& a, const Type& b, DeclaredParts declared)
{
    if (a.anyObject || b.anyObject) {
        const Type& bounded = a.anyObject ? b : a;
        result.anyObject = bounded.anyObject;
        result.anyFunction = bounded.anyFunction;
        result.unnamedLayouts = bounded.unnamedLayouts;
        result.unnamedClasses = bounded.unnamedClasses;
        return;
    }

    result.anyFunction = a.anyFunction && b.anyFunction;

    IntersectionClassParts classesOfFirst = copyClassPartsForIntersection(arena, a.unnamedClasses);
    IntersectionClassParts classesOfSecond = copyClassPartsForIntersection(arena, b.unnamedClasses);

    LayoutSets& layoutSets = arena.layoutSets();
    LayoutSetID layouts = layoutSets.intersectionOf(a.unnamedLayouts, b.unnamedLayouts);
    layouts = layoutSets.unionOf(layouts, intersectLayoutsWithClasses(arena, a.unnamedLayouts, classesOfSecond.span()));
    layouts = layoutSets.unionOf(layouts, intersectLayoutsWithClasses(arena, b.unnamedLayouts, classesOfFirst.span()));
    result.unnamedLayouts = layouts;

    if (a.unnamedClasses == b.unnamedClasses)
        result.unnamedClasses = a.unnamedClasses;
    else
        result.unnamedClasses = intersectClassParts(arena, classesOfFirst.span(), a.anyFunction, classesOfSecond.span(), b.anyFunction, declared);
}

TypeParts intersectTypes(TypeArena& arena, const Type& a, const Type& b, DeclaredParts declared)
{
    TypeParts result;
    intersectPrimitiveParts(result, arena, a, b);
    if (!a.hasObjects() || !b.hasObjects())
        return result;

    intersectUnknownObjects(result, arena, a, b, declared);
    auto holdsWholeUnknownOf = [&](const Type& side) {
        return side.hasUnknown() && result.anyObject == side.anyObject && result.anyFunction == side.anyFunction
            && result.unnamedLayouts == side.unnamedLayouts && result.unnamedClasses == side.unnamedClasses;
    };
    ObjectSetID namedByFirst = intersectNamedObjects(arena, a, b, holdsWholeUnknownOf(b));
    ObjectSetID namedBySecond = intersectNamedObjects(arena, b, a, holdsWholeUnknownOf(a));
    result.objects = arena.objectSets().unionOf(namedByFirst, namedBySecond);
    if (result.anyObject || result.anyFunction || result.unnamedLayouts || result.unnamedClasses)
        result.seeds = a.seeds | b.seeds;
    return result;
}

static void subtractIntegersFromParts(TypeParts& result, Range removed)
{
    Range integers = result.integers;
    if (integers.isEmpty() || removed.isEmpty())
        return;
    if (integers.isSubsetOf(removed)) {
        result.primitives &= ~Primitive::Integer;
        result.integers = Range::empty();
    } else if (removed.contains(integers.minimum))
        result.integers.minimum = removed.maximum + 1;
    else if (removed.contains(integers.maximum))
        result.integers.maximum = removed.minimum - 1;
}

template<typename Sets, typename SetID>
static void subtractLiteralSet(TypeParts& result, SetID TypeParts::* literals, Sets& sets, const Type& removed, uint16_t widenedBit, uint16_t literalBit)
{
    if (removed.primitives & widenedBit) {
        result.primitives &= ~(widenedBit | literalBit);
        result.*literals = { };
        return;
    }
    result.*literals = sets.differenceOf(result.*literals, removed.*literals);
    if (!(result.*literals))
        result.primitives &= ~literalBit;
}

static ObjectSetID subtractNamedObjects(TypeArena& arena, ObjectSetID objects, const Type& removed)
{
    if (!objects)
        return objects;

    auto askEachObjectOf = [&](ObjectSetID asked) {
        TypeReader reader(arena);
        auto namedByRemoved = arena.at(removed.objects);
        return arena.objectSets().filtered(asked, [&](ObjectID object) {
            if (surelyFitsUnknownOf(reader, object, removed))
                return false;
            return !namedByRemoved.contains(object) || reader.at(object).has(ObjectTrait::IsMerged);
        });
    };
    bool isWide = arena.objectSets().sizeOf(objects) > membersPerSetChunk;
    uint32_t objectsOfRemoved = isWide ? arena.setOfRound(removed.objects).word : removed.objects.bits();
    uint32_t layoutsOfRemoved = isWide ? arena.setOfRound(removed.unnamedLayouts).word : removed.unnamedLayouts.bits();
    if (liesInThread(ObjectSetID::fromBits(objectsOfRemoved)) || liesInThread(LayoutSetID::fromBits(layoutsOfRemoved)) || liesInThread(removed.unnamedClasses))
        return askEachObjectOf(objects);
    uint32_t flagsOfRemoved = static_cast<uint32_t>(removed.anyObject) | static_cast<uint32_t>(removed.anyFunction) << 1;
    return filterMembersOncePerBuild(arena, SharedQuestion::NamedObjectsOfDifference, objects, { objectsOfRemoved, layoutsOfRemoved, removed.unnamedClasses.bits(), flagsOfRemoved }, scopedLambda<ObjectSetID(ObjectSetID)>(askEachObjectOf));
}

static LayoutSetID subtractUnnamedLayouts(TypeArena& arena, LayoutSetID layouts, const Type& removed)
{
    layouts = arena.layoutSets().differenceOf(layouts, removed.unnamedLayouts);
    if (!layouts || !removed.unnamedClasses)
        return layouts;

    TypeReader reader(arena);
    std::span<const ClassPart> removedClasses = arena.at(removed.unnamedClasses);
    return arena.layoutSets().filtered(layouts, [&](LayoutID layout) {
        return !isInsideSomeClassPart(reader, layout, removedClasses);
    });
}

static ClassPartsID subtractClassParts(TypeArena& arena, ClassPartsID classes, const Type& removed)
{
    if (!classes || (!removed.unnamedClasses && !removed.anyFunction))
        return classes;

    TypeReader reader(arena);
    std::span<const ClassPart> parts = arena.at(classes);
    std::span<const ClassPart> removedParts;
    if (removed.unnamedClasses)
        removedParts = arena.at(removed.unnamedClasses);
    IntersectionClassParts kept;
    for (const ClassPart& part : parts) {
        bool liesInRemoved = (removed.anyFunction && holdsOnlyCallables(arena.builtins(), part.builtinClass)) || findPartHolding(reader, removedParts, part.builtinClass);
        if (!liesInRemoved)
            kept.append(part);
    }
    if (kept.size() == parts.size())
        return classes;
    if (kept.isEmpty())
        return { };
    return arena.internClassParts(kept.span());
}

TypeParts subtractTypes(TypeArena& arena, const Type& value, const Type& removed)
{
    TypeParts result = value;
    result.primitives &= ~(removed.primitives & plainPrimitives);
    bool someFractionLiesOutsideRemoved = !removed.boundsOfFractions.isEmpty() && (value.boundsOfFractions.isEmpty() || !value.boundsOfFractions.isSubsetOf(removed.boundsOfFractions));
    if (someFractionLiesOutsideRemoved)
        result.primitives |= value.primitives & Primitive::FractionalNumber;
    subtractIntegersFromParts(result, removed.integers);
    subtractLiteralSet(result, &TypeParts::stringLiterals, arena.stringSets(), removed, Primitive::String, Primitive::LiteralString);
    subtractLiteralSet(result, &TypeParts::symbolLiterals, arena.keySets(), removed, Primitive::Symbol, Primitive::LiteralSymbol);
    if (value.opaque == removed.opaque)
        result.opaque = std::nullopt;
    if (value.environment == removed.environment)
        result.environment = { };

    if (removed.anyObject) {
        result.objects = { };
        result.anyObject = false;
        result.anyFunction = false;
        result.unnamedLayouts = { };
        result.unnamedClasses = { };
    } else if (removed.hasObjects()) {
        result.objects = subtractNamedObjects(arena, value.objects, removed);
        if (!value.anyObject) {
            result.anyFunction = value.anyFunction && !removed.anyFunction;
            result.unnamedLayouts = subtractUnnamedLayouts(arena, value.unnamedLayouts, removed);
            result.unnamedClasses = subtractClassParts(arena, value.unnamedClasses, removed);
        }
    }
    if (!result.anyObject && !result.anyFunction && !result.unnamedLayouts && !result.unnamedClasses)
        result.seeds = { };
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
