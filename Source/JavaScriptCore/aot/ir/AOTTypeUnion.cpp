/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTType.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static Type uniteInternalValues(const Type& a, const Type& b)
{
    bool usesFirst = !a.isJavaScriptValue();
    Type result = usesFirst ? a : b;
    const Type& other = usesFirst ? b : a;
    result.primitives |= other.primitives;
    bool isSameValue = a.opaque == b.opaque && a.environment == b.environment;
    // No type includes both values: canonicalize() answers anything for an internal value mixed with objects.
    if (!isSameValue && !other.hasOnlyPrimitives(Primitive::Absent))
        result.anyObject = true;
    result.canonicalize();
    return result;
}

static ClassPart uniteParts(const ClassPart& a, const ClassPart& b, TypeArena& arena)
{
    ClassPart result = a;
    result.contentsAreProven = a.contentsAreProven && b.contentsAreProven;
    result.provenPristine = a.provenPristine & b.provenPristine;
    if (a.arrayStorage != b.arrayStorage)
        result.arrayStorage = ArrayStorage::Other;
    size_t numberOfContents = std::max(a.contents.size(), b.contents.size());
    result.contents.grow(numberOfContents);
    for (unsigned index = 0; index < numberOfContents; ++index)
        result.contents[index] = arena.unionOf(a.contentsAt(index), b.contentsAt(index));
    size_t numberOfPredictions = std::max(a.predictedContents.size(), b.predictedContents.size());
    result.predictedContents.grow(numberOfPredictions);
    for (unsigned index = 0; index < numberOfPredictions; ++index)
        result.predictedContents[index] = arena.unionOf(a.predictedContentsAt(index), b.predictedContentsAt(index));
    return result;
}

static Vector<ClassPart, 1> uniteClasses(const Vector<ClassPart, 1>& a, const Vector<ClassPart, 1>& b, TypeArena& arena)
{
    Vector<ClassPart, 1> result;
    result.reserveInitialCapacity(a.size() + b.size());
    size_t indexInA = 0;
    size_t indexInB = 0;
    while (indexInA < a.size() && indexInB < b.size()) {
        if (a[indexInA].builtinClass < b[indexInB].builtinClass)
            result.append(a[indexInA++]);
        else if (b[indexInB].builtinClass < a[indexInA].builtinClass)
            result.append(b[indexInB++]);
        else
            result.append(uniteParts(a[indexInA++], b[indexInB++], arena));
    }
    result.append(a.subspan(indexInA));
    result.append(b.subspan(indexInB));
    return result;
}

static SortedSet<RegExpLiteralInType> uniteRegExpLiterals(const Type& a, const Type& b)
{
    const SortedSet<RegExpLiteralInType>& first = a.regExpLiteralSet;
    const SortedSet<RegExpLiteralInType>& second = b.regExpLiteralSet;
    if (first.isEmpty() && second.isEmpty())
        return { };
    if (!a.findClassPart(BuiltinClasses::RegExp))
        return second;
    if (!b.findClassPart(BuiltinClasses::RegExp))
        return first;
    if (first.isEmpty() || second.isEmpty())
        return { };
    SortedSet<RegExpLiteralInType> united = first;
    united.unite(second);
    return united;
}

static LayoutSetID uniteLayouts(LayoutSetID a, LayoutSetID b, TypeArena& arena)
{
    if (!b || a == b)
        return a;
    if (!a)
        return b;
    const Vector<LayoutID>& first = arena.at(a).members;
    const Vector<LayoutID>& second = arena.at(b).members;
    Vector<LayoutID, 16> members(first.size() + second.size());
    members.shrink(std::ranges::set_union(first, second, members.begin()).out - members.begin());
    if (members.size() == first.size())
        return a;
    if (members.size() == second.size())
        return b;
    return arena.intern(LayoutSet { Vector<LayoutID>(members.span()), true });
}

Type Type::unionWith(const Type& other, TypeArena& arena) const
{
    if (*this == other || other.isNever())
        return *this;
    if (isNever())
        return other;
    if (!isJavaScriptValue() || !other.isJavaScriptValue())
        return uniteInternalValues(*this, other);

    Type result = *this;
    result.primitives |= other.primitives;
    result.integers = integers.unionWith(other.integers);
    result.stringLiterals.unite(other.stringLiterals);
    result.symbolLiterals.unite(other.symbolLiterals);
    result.anyObject |= other.anyObject;
    result.anyFunction |= other.anyFunction;
    result.functions.unite(other.functions);
    result.builtins.unite(other.builtins);
    if (!other.classes.isEmpty())
        result.classes = uniteClasses(classes, other.classes, arena);
    result.regExpLiteralSet = uniteRegExpLiterals(*this, other);
    result.layouts = uniteLayouts(layouts, other.layouts, arena);
    result.canonicalize();
    return result;
}

// A union that is as long as its longer operand is that operand: the join has built nothing, however long the list is.
static bool widenGrownLists(Type& type, const Type& first, const Type& second, const WideningContext& context)
{
    auto exceeds = [&](LimitID limit, const Invocable<size_t(const Type&)> auto& sizeIn) {
        size_t size = sizeIn(type);
        return size > std::max(sizeIn(first), sizeIn(second)) && context.build.exceeds(limit, size, context.subject);
    };
    auto sizeOf = [](auto Type::* list) {
        return [list](const Type& listed) -> size_t {
            return (listed.*list).size();
        };
    };
    bool widensStrings = exceeds(LimitID::MaxLiteralsInType, sizeOf(&Type::stringLiterals));
    bool widensSymbols = exceeds(LimitID::MaxSymbolLiteralsInType, sizeOf(&Type::symbolLiterals));
    bool widensLayouts = exceeds(LimitID::MaxLayoutsInType, [&](const Type& listed) -> size_t {
        return membersOf(context.types.table(), listed).size();
    });
    bool widensClasses = exceeds(LimitID::MaxClassesInType, sizeOf(&Type::classes));
    bool widensFunctions = exceeds(LimitID::MaxFunctionsInType, sizeOf(&Type::functions));
    bool widensBuiltins = exceeds(LimitID::MaxBuiltinsInType, sizeOf(&Type::builtins));
    bool widensRegExpLiterals = exceeds(LimitID::MaxRegExpLiteralsInType, sizeOf(&Type::regExpLiteralSet));

    if (widensStrings)
        type.primitives |= Primitive::String;
    if (widensSymbols)
        type.primitives |= Primitive::Symbol;
    if (widensLayouts || widensClasses || widensBuiltins)
        type.anyObject = true;
    if (widensFunctions)
        type.anyFunction = true;
    if (widensRegExpLiterals)
        type.regExpLiteralSet.clear();
    return widensStrings || widensSymbols || widensLayouts || widensClasses || widensFunctions || widensBuiltins || widensRegExpLiterals;
}

Type Type::unionWideningWith(const Type& other, const WideningContext& context) const
{
    Type result = unionWith(other, context.types);
    if (widenGrownLists(result, *this, other, context))
        result.canonicalize();
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
