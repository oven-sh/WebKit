/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeRelations.h"

#if ENABLE(AOT)

#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using RegExpLiteralSet = SortedSet<RegExpLiteralInType>;

} // namespace

void copyObjectParts(Type& target, const Type& source)
{
    target.anyObject = source.anyObject;
    target.anyFunction = source.anyFunction;
    target.functions = source.functions;
    target.builtins = source.builtins;
    target.classes = source.classes;
    target.layouts = source.layouts;
    target.regExpLiteralSet = source.regExpLiteralSet;
}

static bool isRegExpPart(const ClassPart& part)
{
    return part.builtinClass == BuiltinClasses::RegExp;
}

template<typename T>
static void intersectLiterals(Type& result, const Type& a, const Type& b, LiteralPart<T> part)
{
    if (a.primitives & b.primitives & part.widenedBit) {
        result.primitives |= part.widenedBit;
        return;
    }
    SortedSet<T>& literals = result.*part.literals;
    if (a.primitives & part.widenedBit)
        literals = b.*part.literals;
    else {
        literals = a.*part.literals;
        if (!(b.primitives & part.widenedBit))
            literals.intersect(b.*part.literals);
    }
    if (!literals.isEmpty())
        result.primitives |= part.literalBit;
}

static Type intersectPrimitives(const Type& a, const Type& b)
{
    Type result;
    result.primitives = a.primitives & b.primitives & plainPrimitives;
    result.integers = a.integers.intersectWith(b.integers);
    if (!result.integers.isEmpty())
        result.primitives |= Primitive::Integer;
    intersectLiterals(result, a, b, stringPart);
    intersectLiterals(result, a, b, symbolPart);
    if (a.opaque == b.opaque)
        result.opaque = a.opaque;
    if (a.environment == b.environment)
        result.environment = a.environment;
    return result;
}

template<typename T>
static SortedSet<T> intersectLists(const SortedSet<T>& a, bool includesAllOfA, const SortedSet<T>& b, bool includesAllOfB)
{
    SortedSet<T> result = a;
    if (!includesAllOfA)
        result.intersect(b);
    if (includesAllOfB)
        result.unite(b);
    return result;
}

// A function object is an instance of Function. Of an entry that the table does not call a function, a binding or the value of a property, nothing is known.
static bool mayBeInstanceOf(const BuiltinTable& builtins, BuiltinID builtin, BuiltinClassID builtinClass)
{
    if (!isCallableBuiltin(builtins, builtin) || isFunctionClass(builtins, builtinClass))
        return true;
    return relate(builtins, BuiltinClasses::Function, builtinClass) == ClassRelation::Unknown;
}

// The built-ins of the first type that the second type does not name, and may hold under a class.
static bool holdsBuiltinListedUnder(const BuiltinTable& builtins, const Type& a, const Type& b, BuiltinClassID builtinClass)
{
    return std::ranges::any_of(a.builtins, [&](BuiltinID builtin) {
        return !includesBuiltin(b, builtin, builtins) && mayBeInstanceOf(builtins, builtin, builtinClass);
    });
}

// Where one type names a built-in and the other the function it is a closure of, or a class it may be an instance of, the result names the object as the second type does.
static void intersectBuiltins(Type& result, const Type& a, const Type& b, const TypeTable& types)
{
    const BuiltinTable& builtins = types.builtins();
    for (BuiltinID builtin : b.builtins) {
        if (mayContainBuiltin(types, a, builtin))
            result.builtins.add(builtin);
    }
    for (BuiltinID builtin : a.builtins) {
        if (b.builtins.contains(builtin))
            continue;
        if (includesBuiltin(b, builtin, builtins)) {
            result.builtins.add(builtin);
            continue;
        }
        if (b.anyFunction)
            result.anyFunction = true;
        for (FunctionID function : b.functions) {
            if (mayBeClosureOf(types, builtin, function))
                result.functions.add(function);
        }
    }
}

static ClassPart withoutProofs(ClassPart part)
{
    part.contentsAreProven = false;
    part.arrayStorage = ArrayStorage::Other;
    part.provenPristine = { };
    return part;
}

static void copyContents(ClassPart& target, const ClassPart& source)
{
    target.contents = source.contents;
    target.arrayStorage = source.arrayStorage;
}

static TypeID intersectKnownContents(TypeID first, TypeID second, TypeArena& arena)
{
    if (first == TypeTable::anything)
        return second;
    if (second == TypeTable::anything)
        return first;
    return arena.intersectionOf(first, second);
}

static void intersectContents(ClassPart& result, const ClassPart& a, const ClassPart& b, TypeArena& arena)
{
    result.arrayStorage = a.arrayStorage != ArrayStorage::Other ? a.arrayStorage : b.arrayStorage;
    size_t size = std::max(a.contents.size(), b.contents.size());
    result.contents.resize(size);
    for (unsigned index = 0; index < size; ++index)
        result.contents[index] = intersectKnownContents(a.contentsAt(index), b.contentsAt(index), arena);
}

static ClassPart intersectParts(const ClassPart& a, const ClassPart& b, TypeArena& arena, DeclaredParts declared)
{
    ClassPart result = a;
    result.contentsAreProven = a.contentsAreProven || b.contentsAreProven;
    result.provenPristine = a.provenPristine | b.provenPristine;
    if (a.contentsAreProven != b.contentsAreProven)
        copyContents(result, a.contentsAreProven ? a : b);
    else if (a.contentsAreProven || declared == DeclaredParts::FromEither)
        intersectContents(result, a, b, arena);
    else
        copyContents(result, b.contents.isEmpty() ? a : b);
    return result;
}

static std::optional<RegExpLiteralSet> intersectRegExpLiterals(const RegExpLiteralSet& a, const RegExpLiteralSet& b)
{
    if (a.isEmpty() || b.isEmpty())
        return a.isEmpty() ? b : a;
    RegExpLiteralSet common = a;
    common.intersect(b);
    if (common.isEmpty())
        return std::nullopt;
    return common;
}

static void intersectClasses(Type& result, const Type& a, const Type& b, TypeArena& arena, DeclaredParts declared)
{
    const BuiltinTable& builtins = arena.builtins();
    const RegExpLiteralSet anyRegExp { };
    // A listed function of the other type is in the result itself, and its closures need no class part.
    auto overlapsAnyFunction = [&](const ClassPart& part, const Type& type) {
        return !(a.anyFunction && b.anyFunction) && type.anyFunction && isFunctionClass(builtins, part.builtinClass);
    };
    // An object may be in the two types through parts of two classes.
    auto isListedUnder = [&](const ClassPart& part, const Type& other) {
        return std::ranges::any_of(other.classes, [&](const ClassPart& otherPart) {
            return otherPart.builtinClass != part.builtinClass
                && relate(builtins, part.builtinClass, otherPart.builtinClass) == ClassRelation::Unknown
                && !builtins.isSameOrSubclassOf(otherPart.builtinClass, part.builtinClass);
        });
    };
    auto extendsClassOf = [&](const ClassPart& part, const Type& other) {
        return std::ranges::any_of(other.classes, [&](const ClassPart& otherPart) {
            return otherPart.builtinClass != part.builtinClass && builtins.isSameOrSubclassOf(part.builtinClass, otherPart.builtinClass);
        });
    };
    for (const ClassPart& part : a.classes) {
        const ClassPart* partner = b.findClassPart(part.builtinClass);
        bool includesPart = overlapsAnyFunction(part, b);
        // What the part proves does not cover an instance that is in a through another part and is listed under this class.
        bool includesPartner = partner && (overlapsAnyFunction(*partner, a) || isListedUnder(part, a) || holdsBuiltinListedUnder(builtins, a, b, part.builtinClass));
        if (!partner && !includesPart && !extendsClassOf(part, b))
            continue;
        if (isRegExpPart(part)) {
            auto literals = intersectRegExpLiterals(includesPartner ? anyRegExp : a.regExpLiteralSet, includesPart ? anyRegExp : b.regExpLiteralSet);
            if (!literals)
                continue;
            result.regExpLiteralSet = WTF::move(*literals);
        }
        if (!partner && includesPart && !extendsClassOf(part, b)) {
            result.anyFunction = true;
            continue;
        }
        if (!partner) {
            result.classes.append(part);
            continue;
        }
        // With includesPart, an instance may be in b as a function, so partner's proofs do not cover it.
        result.classes.append(intersectParts(includesPartner ? withoutProofs(part) : part, includesPart ? withoutProofs(*partner) : *partner, arena, declared));
    }
    for (const ClassPart& part : b.classes) {
        if (a.findClassPart(part.builtinClass))
            continue;
        if (!overlapsAnyFunction(part, a) && !isListedUnder(part, a) && !holdsBuiltinListedUnder(builtins, a, b, part.builtinClass))
            continue;
        if (isRegExpPart(part))
            result.regExpLiteralSet = b.regExpLiteralSet;
        result.classes.append(part);
    }
}

static Type objectsWithoutLayoutOf(const Type& type)
{
    Type objects = type.withOnlyObjects();
    objects.layouts = { };
    return objects;
}

// A set does not say which objects its members without a layout are, so each object without a layout that the other type lists may be one of them.
static void intersectMembersWithoutLayout(Type& result, const Type& a, const Type& b, TypeArena& arena)
{
    bool areInFirst = hasMembersWithoutLayout(arena.table(), a);
    bool areInSecond = hasMembersWithoutLayout(arena.table(), b);
    if (areInFirst && areInSecond)
        copyObjectParts(result, objectsWithoutLayoutOf(a).unionWith(objectsWithoutLayoutOf(b), arena));
    else
        copyObjectParts(result, areInFirst ? b : a);
    result.layouts = arena.intersectionOf(a.layouts, b.layouts);
}

static void intersectListedObjects(Type& result, const Type& a, const Type& b, TypeArena& arena, DeclaredParts declared)
{
    const BuiltinTable& builtins = arena.builtins();
    result.anyFunction = a.anyFunction && b.anyFunction;
    result.functions = intersectLists(a.functions, mayContainAnyCallable(builtins, b), b.functions, mayContainAnyCallable(builtins, a));
    // Beside any function a list says what to expect, and a cast says it anew: behind the failed test for a function that function is expected no more.
    if (result.anyFunction && declared == DeclaredParts::FromSecond)
        result.functions = b.functions;
    intersectBuiltins(result, a, b, arena.table());
    intersectClasses(result, a, b, arena, declared);
    result.layouts = arena.intersectionOf(a.layouts, b.layouts);
}

// What a cast gives lies inside the cast type.
static void castToFamilyWithUnlistedMembers(Type& result, const Type& source, const Type& family, TypeArena& arena)
{
    if (hasMembersWithoutLayout(arena.table(), source)) {
        copyObjectParts(result, family);
        result.layouts = arena.intersectionOf(source.layouts, family.layouts);
        return;
    }

    intersectListedObjects(result, source, family, arena, DeclaredParts::FromSecond);
    if (isSubset(objectsWithoutLayoutOf(source), objectsWithoutLayoutOf(result), arena.table()))
        return;
    LayoutSet layouts { .isComplete = true };
    if (result.layouts)
        layouts = arena.at(result.layouts);
    layouts.hasMembersWithoutLayout = true;
    result.layouts = arena.intern(WTF::move(layouts));
}

static void intersectKnownObjects(Type& result, const Type& a, const Type& b, TypeArena& arena, DeclaredParts declared)
{
    if (declared == DeclaredParts::FromSecond && hasMembersWithoutLayout(arena.table(), b)) {
        castToFamilyWithUnlistedMembers(result, a, b, arena);
        return;
    }
    if (hasMembersWithoutLayout(arena.table(), a) || hasMembersWithoutLayout(arena.table(), b)) {
        intersectMembersWithoutLayout(result, a, b, arena);
        return;
    }
    intersectListedObjects(result, a, b, arena, declared);
}

static void intersectObjects(Type& result, const Type& a, const Type& b, TypeArena& arena, DeclaredParts declared)
{
    if (!a.hasObjects() || !b.hasObjects())
        return;
    if (a.anyObject)
        copyObjectParts(result, b);
    else if (b.anyObject)
        copyObjectParts(result, a);
    else
        intersectKnownObjects(result, a, b, arena, declared);
}

Type intersect(const Type& a, const Type& b, TypeArena& arena, DeclaredParts declared)
{
    if (a == b)
        return a;
    Type result = intersectPrimitives(a, b);
    intersectObjects(result, a, b, arena, declared);
    result.canonicalize();
    return result;
}

Type Type::intersectWith(const Type& other, TypeArena& arena) const
{
    return intersect(*this, other, arena, DeclaredParts::FromEither);
}

Type Type::castTo(const Type& castType, TypeArena& arena) const
{
    return intersect(*this, castType, arena, DeclaredParts::FromSecond);
}

static void subtractIntegers(Type& result, Range removed)
{
    Range integers = result.integers;
    if (integers.isEmpty() || removed.isEmpty())
        return;
    if (integers.isSubsetOf(removed))
        result.primitives &= ~Primitive::Integer;
    else if (removed.contains(integers.minimum))
        result.integers.minimum = removed.maximum + 1;
    else if (removed.contains(integers.maximum))
        result.integers.maximum = removed.minimum - 1;
}

template<typename T>
static void subtractLiterals(Type& result, const Type& removed, LiteralPart<T> part)
{
    SortedSet<T>& literals = result.*part.literals;
    if (removed.primitives & part.widenedBit) {
        result.primitives &= ~part.widenedBit;
        literals.clear();
    } else
        literals.subtract(removed.*part.literals);
    if (literals.isEmpty())
        result.primitives &= ~part.literalBit;
}

static void subtractCallables(Type& result, const Type& removed, const BuiltinTable& builtins)
{
    if (removed.anyFunction)
        result.anyFunction = false;
    removeAllMatching(result.functions, [&](FunctionID function) {
        return includesFunction(removed, function);
    });
    removeAllMatching(result.builtins, [&](BuiltinID builtin) {
        return includesBuiltin(removed, builtin, builtins);
    });
}

static void subtractClasses(Type& result, const Type& removed, const TypeTable& types)
{
    auto isInsideRemovedPart = [&](const ClassPart& part) {
        const ClassPart* partner = removed.findClassPart(part.builtinClass);
        return partner && isPartSubset(part, *partner, types);
    };
    bool removesOnlyListedRegExps = !removed.regExpLiteralSet.isEmpty();
    result.classes.removeAllMatching([&](const ClassPart& part) {
        return isInsideRemovedPart(part) && !(removesOnlyListedRegExps && isRegExpPart(part));
    });
    if (!removesOnlyListedRegExps || result.regExpLiteralSet.isEmpty())
        return;
    const ClassPart* regExpPart = result.findClassPart(BuiltinClasses::RegExp);
    if (!regExpPart || !isInsideRemovedPart(*regExpPart))
        return;
    result.regExpLiteralSet.subtract(removed.regExpLiteralSet);
    if (result.regExpLiteralSet.isEmpty())
        result.classes.removeFirstMatching(isRegExpPart);
}

static void subtractKnownObjects(Type& result, const Type& removed, TypeArena& arena)
{
    subtractCallables(result, removed, arena.builtins());
    subtractClasses(result, removed, arena.table());
    result.layouts = arena.differenceOf(result.layouts, removed.layouts);
}

Type Type::without(const Type& other, TypeArena& arena) const
{
    Type result = *this;
    result.primitives &= ~(other.primitives & plainPrimitives);
    subtractIntegers(result, other.integers);
    subtractLiterals(result, other, stringPart);
    subtractLiterals(result, other, symbolPart);
    if (opaque == other.opaque)
        result.opaque = std::nullopt;
    if (environment == other.environment)
        result.environment = { };
    if (other.anyObject)
        copyObjectParts(result, never());
    else if (other.hasObjects() && !anyObject)
        subtractKnownObjects(result, other, arena);
    result.canonicalize();
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
