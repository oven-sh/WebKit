/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeArena.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <bit>
#include <tuple>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static NEVER_INLINE TypeID internTypeOfPrimitivesInArena(TypeArena& arena, uint16_t primitives, Range integers)
{
    bool isKept = primitives == Primitive::Integer && integers.isSubsetOf(Range::int32());
    uint32_t minimum = static_cast<uint32_t>(integers.minimum);
    uint32_t maximum = static_cast<uint32_t>(integers.maximum);
    if (isKept) {
        if (TypeID made = arena.typeMadeBefore(MakerOfType::Integers, minimum, maximum))
            return made;
    }

    TypeParts parts;
    parts.primitives = primitives;
    parts.integers = integers;
    if (!isKept)
        return arena.intern(parts);
    TypeID result = arena.internPartsThatAreCanonical(parts);
    arena.noteTypeMade(MakerOfType::Integers, minimum, maximum, result, !liesInThread(result));
    return result;
}

static ALWAYS_INLINE TypeID typeOfPrimitivesInArena(TypeArena& arena, uint16_t primitives, Range integers)
{
    primitives &= Primitive::Defined;
    integers = primitives & Primitive::Integer ? integers.intersectWith(Range::full()) : Range::empty();
    if (integers.isEmpty()) {
        primitives &= ~Primitive::Integer;
        integers = Range::empty();
    }
    if (std::optional<TypeID> type = tryMakeTypeOfPrimitives(primitives, integers))
        return *type;
    return internTypeOfPrimitivesInArena(arena, primitives, integers);
}

static constexpr Range integersLeftWhereTestFailed(Range integers, Range removed)
{
    if (integers.isEmpty() || removed.isEmpty())
        return integers;
    if (integers.isSubsetOf(removed))
        return Range::empty();
    if (removed.contains(integers.minimum))
        return { removed.maximum + 1, integers.maximum };
    if (removed.contains(integers.maximum))
        return { integers.minimum, removed.minimum - 1 };
    return integers;
}

static_assert(integersLeftWhereTestFailed({ 0, 9 }, { 0, 3 }) == Range { 4, 9 });
static_assert(integersLeftWhereTestFailed({ 0, 9 }, { 7, 20 }) == Range { 0, 6 });
static_assert(integersLeftWhereTestFailed({ 0, 9 }, { 3, 5 }) == Range { 0, 9 });
static_assert(integersLeftWhereTestFailed({ 0, 9 }, { -1, 9 }) == Range::empty());
static_assert(integersLeftWhereTestFailed({ 0, 9 }, Range::empty()) == Range { 0, 9 });

static ALWAYS_INLINE std::pair<uint32_t, uint32_t> memoKeyOfSymmetricOperation(TypeID a, TypeID b)
{
    return a.bits() < b.bits() ? std::pair { a.bits(), b.bits() } : std::pair { b.bits(), a.bits() };
}

TypeID TypeArena::integer(Range range)
{
    return typeOfPrimitivesInArena(*this, Primitive::Integer, range);
}

TypeID TypeArena::number(double value)
{
    PrimitiveSet primitive = primitiveOfNumber(value);
    if (primitive == PrimitiveSet::Integer)
        return integer(Range::one(static_cast<int64_t>(value)));
    if (primitive != PrimitiveSet::FractionalNumber)
        return fromPrimitives(static_cast<uint16_t>(primitive));

    int64_t towardZero = static_cast<int64_t>(value);
    TypeParts parts;
    parts.primitives = Primitive::FractionalNumber;
    parts.boundsOfFractions = value < 0 ? Range { towardZero - 1, towardZero } : Range { towardZero, towardZero + 1 };
    return intern(parts);
}

TypeID TypeArena::numbersWithin(Range bounds)
{
    TypeParts parts;
    parts.primitives = Primitive::Integer | Primitive::FractionalNumber;
    parts.integers = bounds;
    parts.boundsOfFractions = bounds;
    return intern(parts);
}

TypeID TypeArena::stringLiteral(StringID string)
{
    if (liesInThread(string))
        return TypeTable::string;
    if (TypeID kept = m_table.typeOfLiteralString(string))
        return kept;
    if (TypeID made = typeMadeBefore(MakerOfType::StringLiteral, string.bits(), 0))
        return made;

    TypeParts parts;
    parts.primitives = Primitive::LiteralString;
    parts.stringLiterals = StringSets::of(string);
    TypeID result = internPartsThatAreCanonical(parts);
    noteTypeMade(MakerOfType::StringLiteral, string.bits(), 0, result, !liesInThread(result));
    return result;
}

TypeID TypeArena::symbolLiteral(PropertyKeyID symbol)
{
    if (liesInThread(symbol))
        return TypeTable::symbol;

    TypeParts parts;
    parts.primitives = Primitive::LiteralSymbol;
    parts.symbolLiterals = KeySets::of(symbol);
    return intern(parts);
}

TypeID TypeArena::forObject(ObjectID object)
{
    RELEASE_ASSERT(object != ObjectTable::unknown());
    if (TypeID made = typeMadeBefore(MakerOfType::ForObject, object.bits(), 0))
        return made;

    TypeParts parts;
    parts.objects = ObjectSets::of(object);
    TypeID result = internPartsThatAreCanonical(parts);
    noteTypeMade(MakerOfType::ForObject, object.bits(), 0, result, !liesInThread(result));
    return result;
}

TypeID TypeArena::forObjects(ObjectSetID set)
{
    ASSERT(!at(set).contains(ObjectTable::unknown()));
    TypeParts parts;
    parts.objects = set;
    return intern(parts);
}

TypeID TypeArena::forLayout(LayoutID layout)
{
    return forLayoutSet(LayoutSets::of(layout));
}

TypeID TypeArena::forLayouts(std::span<const LayoutID> unsorted)
{
    if (unsorted.empty())
        return TypeTable::never;
    return forLayoutSet(m_layoutSets.ofUnsorted(unsorted));
}

TypeID TypeArena::forLayoutSet(LayoutSetID set)
{
    if (TypeID made = typeMadeBefore(MakerOfType::ForLayoutSet, set.bits(), 0))
        return made;

    TypeParts parts;
    parts.unnamedLayouts = set;
    TypeID result = set ? internPartsThatAreCanonical(parts) : intern(parts);
    noteTypeMade(MakerOfType::ForLayoutSet, set.bits(), 0, result, !liesInThread(set) && !liesInThread(result));
    return result;
}

TypeID TypeArena::builtinClass(BuiltinClassID classID, std::span<const TypeID> writtenContents)
{
    TypeParts parts;
    if (writtenContents.empty()) {
        if (TypeID made = typeMadeBefore(MakerOfType::BuiltinClass, classID.index(), 0))
            return made;
        parts.unnamedClasses = partsOf(classID);
        TypeID result = intern(parts);
        noteTypeMade(MakerOfType::BuiltinClass, classID.index(), 0, result, !liesInThread(result));
        return result;
    }

    RELEASE_ASSERT(writtenContents.size() <= mostContentsOfClass);
    bool isKept = writtenContents.size() == 1;
    TypeID firstContent = writtenContents.front();
    if (isKept) {
        if (TypeID made = typeMadeBefore(MakerOfType::BuiltinClassWithOneContent, classID.index(), firstContent.bits()))
            return made;
    }

    ClassPart part;
    part.builtinClass = classID;
    std::ranges::copy(writtenContents, part.writtenContents.begin());
    parts.unnamedClasses = internClassParts(singleElementSpan(part));
    TypeID result = intern(parts);
    if (isKept)
        noteTypeMade(MakerOfType::BuiltinClassWithOneContent, classID.index(), firstContent.bits(), result, !liesInThread(firstContent) && !liesInThread(result));
    return result;
}

TypeID TypeArena::forEnvironment(LayoutID layout)
{
    TypeParts parts;
    parts.environment = layout;
    return intern(parts);
}

TypeID TypeArena::forOpaque(OpaqueKind kind)
{
    TypeParts parts;
    parts.opaque = kind;
    return intern(parts);
}

TypeID TypeArena::makeAnything(UnknownSeed seed)
{
    TypeParts parts;
    parts.primitives = Primitive::All;
    parts.integers = Range::full();
    parts.seeds = seed;
    parts.anyObject = true;
    parts.anyFunction = true;
    TypeID made = internPartsThatAreCanonical(parts);
    m_anythingOfSeed[std::countr_zero(static_cast<uint16_t>(seed))] = made;
    m_someAnythingOfSeedLiesInThread |= liesInThread(made);
    return made;
}

TypeID TypeArena::object(UnknownSeed seed)
{
    TypeParts parts;
    parts.seeds = seed;
    parts.anyObject = true;
    parts.anyFunction = true;
    return intern(parts);
}

TypeID TypeArena::function(UnknownSeed seed)
{
    TypeParts parts;
    parts.seeds = seed;
    parts.anyFunction = true;
    return intern(parts);
}

TypeID TypeArena::unionOfDifferentTypes(TypeID a, TypeID b)
{
    if (holdsOnlyPrimitives(a) && holdsOnlyPrimitives(b))
        return typeOfPrimitivesInArena(*this, primitivesInID(a) | primitivesInID(b), integersInID(a).unionWith(integersInID(b)));

    auto [first, second] = memoKeyOfSymmetricOperation(a, b);
    if (std::optional<uint32_t> known = rememberedAtOnce(Operation::Union, first, second))
        return TypeID::fromBits(*known);
    return uniteAndRemember(a, b);
}

NEVER_INLINE TypeID TypeArena::uniteAndRemember(TypeID a, TypeID b)
{
    auto [first, second] = memoKeyOfSymmetricOperation(a, b);
    if (std::optional<uint32_t> known = rememberedElsewhere(Operation::Union, first, second))
        return TypeID::fromBits(*known);
    TypeID result = intern(uniteTypes(*this, at(a), at(b)));
    remember(Operation::Union, first, second, result.bits());
    return result;
}

TypeID TypeArena::intersectionOfDifferentTypes(TypeID a, TypeID b, DeclaredParts declared)
{
    if (holdsOnlyPrimitives(a) && holdsOnlyPrimitives(b))
        return typeOfPrimitivesInArena(*this, primitivesInID(a) & primitivesInID(b), integersInID(a).intersectWith(integersInID(b)));

    bool isCast = declared == DeclaredParts::FromSecond;
    Operation operation = isCast ? Operation::Cast : Operation::Intersection;
    auto [first, second] = isCast ? std::pair { a.bits(), b.bits() } : memoKeyOfSymmetricOperation(a, b);
    if (std::optional<uint32_t> known = rememberedAtOnce(operation, first, second))
        return TypeID::fromBits(*known);
    return intersectAndRemember(a, b, declared);
}

NEVER_INLINE TypeID TypeArena::intersectAndRemember(TypeID a, TypeID b, DeclaredParts declared)
{
    bool isCast = declared == DeclaredParts::FromSecond;
    Operation operation = isCast ? Operation::Cast : Operation::Intersection;
    auto [first, second] = isCast ? std::pair { a.bits(), b.bits() } : memoKeyOfSymmetricOperation(a, b);
    if (std::optional<uint32_t> known = rememberedElsewhere(operation, first, second))
        return TypeID::fromBits(*known);
    TypeID result = intern(intersectTypes(*this, at(a), at(b), declared));
    remember(operation, first, second, result.bits());
    return result;
}

TypeID TypeArena::differenceOfTypesWithValues(TypeID value, TypeID removed)
{
    if (holdsOnlyPrimitives(value) && holdsOnlyPrimitives(removed)) {
        uint16_t primitives = primitivesInID(value) & ~(primitivesInID(removed) & ~Primitive::Integer);
        return typeOfPrimitivesInArena(*this, primitives, integersLeftWhereTestFailed(integersInID(value), integersInID(removed)));
    }

    if (std::optional<uint32_t> known = rememberedAtOnce(Operation::Difference, value.bits(), removed.bits()))
        return TypeID::fromBits(*known);
    return subtractAndRemember(value, removed);
}

NEVER_INLINE TypeID TypeArena::subtractAndRemember(TypeID value, TypeID removed)
{
    if (std::optional<uint32_t> known = rememberedElsewhere(Operation::Difference, value.bits(), removed.bits()))
        return TypeID::fromBits(*known);
    TypeID result = intern(subtractTypes(*this, at(value), at(removed)));
    remember(Operation::Difference, value.bits(), removed.bits(), result.bits());
    return result;
}

bool TypeArena::isSubsetOfDifferentType(TypeID type, TypeID of)
{
    if (holdsOnlyPrimitives(type) && holdsOnlyPrimitives(of))
        return !(primitivesInID(type) & ~primitivesInID(of)) && integersInID(type).isSubsetOf(integersInID(of));

    if (std::optional<uint32_t> known = rememberedAtOnce(Operation::Subset, type.bits(), of.bits()))
        return !!*known;
    return workOutSubsetAndRemember(type, of);
}

NEVER_INLINE bool TypeArena::workOutSubsetAndRemember(TypeID type, TypeID of)
{
    if (std::optional<uint32_t> known = rememberedElsewhere(Operation::Subset, type.bits(), of.bits()))
        return !!*known;
    bool result = Typed::isSubset(*this, at(type), at(of));
    remember(Operation::Subset, type.bits(), of.bits(), result);
    return result;
}

bool TypeArena::overlapOfDifferentTypes(TypeID a, TypeID b)
{
    if (holdsOnlyPrimitives(a) && holdsOnlyPrimitives(b))
        return (primitivesInID(a) & primitivesInID(b) & ~Primitive::Integer) || !integersInID(a).intersectWith(integersInID(b)).isEmpty();

    auto [first, second] = memoKeyOfSymmetricOperation(a, b);
    if (std::optional<uint32_t> known = rememberedAtOnce(Operation::Overlap, first, second))
        return !!*known;
    return workOutOverlapAndRemember(a, b);
}

NEVER_INLINE bool TypeArena::workOutOverlapAndRemember(TypeID a, TypeID b)
{
    auto [first, second] = memoKeyOfSymmetricOperation(a, b);
    if (std::optional<uint32_t> known = rememberedElsewhere(Operation::Overlap, first, second))
        return !!*known;
    bool result = Typed::overlap(*this, at(a), at(b));
    remember(Operation::Overlap, first, second, result);
    return result;
}

bool TypeArena::mayBeSameObject(TypeID a, TypeID b)
{
    ASSERT(a && b);
    if (holdsOnlyPrimitives(a) || holdsOnlyPrimitives(b))
        return false;
    if (a == b) {
        Type type = at(a);
        return type.hasObjects() || !type.isJavaScriptValue();
    }

    auto [first, second] = memoKeyOfSymmetricOperation(a, b);
    if (std::optional<uint32_t> known = remembered(Operation::SameObject, first, second))
        return !!*known;
    bool result = Typed::mayBeSameObject(*this, at(a), at(b));
    remember(Operation::SameObject, first, second, result);
    return result;
}

TypeID TypeArena::numbersAskedBehindCallOfBuiltin(TypeID id)
{
    TypeID numbers = intersectionOf(id, TypeTable::number);
    if (isNeverType(numbers))
        return numbers;
    if (isSubset(numbers, TypeTable::int32))
        return TypeTable::int32;
    TypeID int32OrLength = integer(Range::int32OrLength());
    if (isSubset(numbers, int32OrLength))
        return int32OrLength;
    return TypeTable::number;
}

TypeID TypeArena::wholeKindsAndBuiltinClassesOf(TypeID id)
{
    static constexpr std::array<uint16_t, 6> kinds { Primitive::Undefined, Primitive::Null, Primitive::Boolean, Primitive::AnyString, Primitive::AnySymbol, Primitive::BigInt };
    Type type = at(id);
    uint16_t primitives = 0;
    for (uint16_t kind : kinds) {
        if (type.primitives & kind)
            primitives |= kind & Primitive::All;
    }
    TypeID numbers = numbersAskedBehindCallOfBuiltin(id);
    if (!type.hasObjects())
        return unionOf(fromPrimitives(primitives), numbers);
    TypeID classes = type.hasOnlyClassObjects() ? intersectionOf(testablePartOf(id), TypeTable::object) : TypeID();
    bool canBeAsked = classes && !isNeverType(classes) && testablePartOf(classes) == classes;
    return unionOf(withPrimitives(canBeAsked ? classes : TypeID(TypeTable::object), primitives), numbers);
}

TypeID TypeArena::withPrimitives(TypeID type, uint16_t added)
{
    RELEASE_ASSERT(!(added & primitivesWithLists));
    added &= Primitive::Defined;
    bool addsIntegers = added & Primitive::Integer;
    if (holdsOnlyPrimitives(type))
        return typeOfPrimitivesInArena(*this, primitivesInID(type) | added, addsIntegers ? Range::full() : integersInID(type));

    if (TypeID made = typeMadeBefore(MakerOfType::WithPrimitives, type.bits(), added))
        return made;

    TypeParts parts = recordOf(type);
    bool addsFractions = added & Primitive::FractionalNumber;
    if (!(added & ~parts.primitives) && (!addsIntegers || parts.integers == Range::full()) && (!addsFractions || parts.boundsOfFractions.isEmpty()))
        return type;
    parts.primitives |= added;
    if (addsIntegers)
        parts.integers = Range::full();
    if (addsFractions)
        parts.boundsOfFractions = Range::empty();
    TypeID result = intern(parts);
    noteTypeMade(MakerOfType::WithPrimitives, type.bits(), added, result, !liesInThread(type) && !liesInThread(result));
    return result;
}

TypeID TypeArena::withoutPrimitivesHeld(TypeID type, uint16_t removed)
{
    if (holdsOnlyPrimitives(type))
        return typeOfPrimitivesInArena(*this, primitivesInID(type) & ~removed, integersInID(type));

    bool isRemembered = m_keeps == ArenaKeeps::UntilReset;
    if (std::optional<uint32_t> known = isRemembered ? rememberedAtOnce(Operation::WithoutPrimitives, type.bits(), removed) : std::nullopt)
        return TypeID::fromBits(*known);
    return takePrimitivesAwayAndRemember(type, removed);
}

NEVER_INLINE TypeID TypeArena::takePrimitivesAwayAndRemember(TypeID type, uint16_t removed)
{
    bool isRemembered = m_keeps == ArenaKeeps::UntilReset;
    if (std::optional<uint32_t> known = isRemembered ? rememberedElsewhere(Operation::WithoutPrimitives, type.bits(), removed) : std::nullopt)
        return TypeID::fromBits(*known);
    TypeParts parts = recordOf(type);
    parts.primitives &= ~removed;
    if (!(parts.primitives & Primitive::Integer))
        parts.integers = Range::empty();
    if (!(parts.primitives & Primitive::LiteralString))
        parts.stringLiterals = { };
    if (!(parts.primitives & Primitive::LiteralSymbol))
        parts.symbolLiterals = { };
    TypeID result = intern(parts);
    if (isRemembered)
        remember(Operation::WithoutPrimitives, type.bits(), removed, result.bits());
    return result;
}

TypeID TypeArena::withoutObjectsOfRecord(TypeID type)
{
    const Type& record = recordOf(type);
    if (!record.hasObjects())
        return type;
    TypeParts parts = record;
    parts.seeds = { };
    parts.anyObject = false;
    parts.anyFunction = false;
    parts.objects = { };
    parts.unnamedLayouts = { };
    parts.unnamedClasses = { };
    return intern(parts);
}

TypeID TypeArena::withOnlyObjects(TypeID type)
{
    if (holdsOnlyPrimitives(type))
        return TypeTable::never;

    TypeParts parts = at(type);
    if (parts.environment || parts.opaque)
        return TypeTable::never;
    if (!parts.primitives)
        return type;
    parts.primitives = 0;
    parts.integers = Range::empty();
    parts.stringLiterals = { };
    parts.symbolLiterals = { };
    return intern(parts);
}

TypeID TypeArena::withIntegers(TypeID type, Range integers)
{
    if (holdsOnlyPrimitives(type))
        return typeOfPrimitivesInArena(*this, primitivesInID(type) | Primitive::Integer, integers);

    TypeParts parts = at(type);
    integers = integers.intersectWith(Range::full());
    if (integers.isEmpty()) {
        parts.primitives &= ~Primitive::Integer;
        parts.integers = Range::empty();
    } else {
        parts.primitives |= Primitive::Integer;
        parts.integers = integers;
    }
    return intern(parts);
}

TypeID TypeArena::withoutBoundsOfFractionsOfRecord(TypeID type)
{
    const Type& record = recordOf(type);
    if (record.boundsOfFractions.isEmpty())
        return type;
    TypeParts parts = record;
    parts.boundsOfFractions = Range::empty();
    return intern(parts);
}

TypeID TypeArena::withLiteralStringsWidened(TypeID type)
{
    if (holdsOnlyPrimitives(type))
        return type;

    TypeParts parts = at(type);
    if (!(parts.primitives & Primitive::LiteralString))
        return type;
    parts.primitives = (parts.primitives & ~Primitive::LiteralString) | Primitive::String;
    parts.stringLiterals = { };
    return intern(parts);
}

TypeID TypeArena::withSeedsOnRecord(TypeID type, UnknownSeeds added)
{
    if (TypeID made = typeMadeBefore(MakerOfType::WithSeeds, type.bits(), added.toRaw()))
        return made;

    const Type& record = recordOf(type);
    if (!record.hasUnknown() || record.seeds.containsAll(added))
        return type;
    TypeParts parts = record;
    parts.seeds.add(added);
    TypeID result = intern(parts);
    noteTypeMade(MakerOfType::WithSeeds, type.bits(), added.toRaw(), result, !liesInThread(type) && !liesInThread(result));
    return result;
}

template<typename SetID>
static std::strong_ordering compareSetsOfTypes(const TypeArena& arena, SetID a, SetID b)
{
    if (a == b)
        return std::strong_ordering::equal;
    return compareByContent(arena.at(a).untyped(), arena.at(b).untyped());
}

static std::strong_ordering compareKeysOfTypes(const TypeArena& arena, PropertyKeyID a, PropertyKeyID b)
{
    if (a == b)
        return std::strong_ordering::equal;
    bool firstIsStable = arena.table().hasStableNumber(a);
    bool secondIsStable = arena.table().hasStableNumber(b);
    if (firstIsStable != secondIsStable)
        return firstIsStable ? std::strong_ordering::less : std::strong_ordering::greater;
    if (firstIsStable)
        return a <=> b;
    return arena.table().compareByContent(a, b);
}

using KeysOfSymbolLiterals = Vector<PropertyKeyID, 8>;

static KeysOfSymbolLiterals keysInOrderOfContent(const TypeArena& arena, KeySetID set)
{
    KeysOfSymbolLiterals keys;
    arena.at(set).forEach([&](PropertyKeyID key) {
        keys.append(key);
    });
    if (!keys.isEmpty() && !arena.table().hasStableNumber(keys.last())) {
        AOT::sort(keys, [&](PropertyKeyID a, PropertyKeyID b) {
            return compareKeysOfTypes(arena, a, b) < 0;
        });
    }
    return keys;
}

static std::strong_ordering compareKeySetsOfTypes(const TypeArena& arena, KeySetID a, KeySetID b)
{
    if (a == b)
        return std::strong_ordering::equal;
    KeysOfSymbolLiterals first = keysInOrderOfContent(arena, a);
    KeysOfSymbolLiterals second = keysInOrderOfContent(arena, b);
    size_t numberInBoth = std::min(first.size(), second.size());
    for (size_t index = 0; index < numberInBoth; ++index) {
        if (std::strong_ordering order = compareKeysOfTypes(arena, first[index], second[index]); order != 0)
            return order;
    }
    return first.size() <=> second.size();
}

static std::strong_ordering compareClassPartsOfTypes(const TypeArena& arena, ClassPartsID a, ClassPartsID b)
{
    if (a == b)
        return std::strong_ordering::equal;
    std::span<const ClassPart> first = a ? arena.at(a) : std::span<const ClassPart>();
    std::span<const ClassPart> second = b ? arena.at(b) : std::span<const ClassPart>();
    size_t numberInBoth = std::min(first.size(), second.size());
    for (size_t index = 0; index < numberInBoth; ++index) {
        if (std::strong_ordering order = first[index].builtinClass <=> second[index].builtinClass; order != 0)
            return order;
        for (unsigned position = 0; position < mostContentsOfClass; ++position) {
            if (std::strong_ordering order = arena.compareByContent(first[index].writtenContents[position], second[index].writtenContents[position]); order != 0)
                return order;
        }
    }
    return first.size() <=> second.size();
}

std::strong_ordering TypeArena::compareByContent(TypeID a, TypeID b) const
{
    if (a == b)
        return std::strong_ordering::equal;
    if (!a || !b)
        return a ? std::strong_ordering::greater : std::strong_ordering::less;

    Type first = at(a);
    Type second = at(b);
    auto plainPartsOf = [](const Type& type) {
        return std::tuple(type.primitives, type.integers.minimum, type.integers.maximum, type.boundsOfFractions.minimum, type.boundsOfFractions.maximum, type.anyObject, type.anyFunction, type.seeds.toRaw(), type.opaque, type.environment);
    };
    if (std::strong_ordering order = plainPartsOf(first) <=> plainPartsOf(second); order != 0)
        return order;
    if (std::strong_ordering order = compareSetsOfTypes(*this, first.stringLiterals, second.stringLiterals); order != 0)
        return order;
    if (std::strong_ordering order = compareKeySetsOfTypes(*this, first.symbolLiterals, second.symbolLiterals); order != 0)
        return order;
    if (std::strong_ordering order = compareSetsOfTypes(*this, first.objects, second.objects); order != 0)
        return order;
    if (std::strong_ordering order = compareSetsOfTypes(*this, first.unnamedLayouts, second.unnamedLayouts); order != 0)
        return order;
    return compareClassPartsOfTypes(*this, first.unnamedClasses, second.unnamedClasses);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
