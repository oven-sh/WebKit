/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTType.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static Type uniteInternalValues(const Type& a, const Type& b)
{
    bool usesFirst = !a.isJavaScriptValue();
    Type result = usesFirst ? a : b;
    const Type& other = usesFirst ? b : a;
    result.primitives |= other.primitives;
    bool isSameValue = a.opaque == b.opaque && a.environment == b.environment;
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
    return result;
}

static ClassParts uniteClasses(const ClassParts& a, const ClassParts& b, TypeArena& arena)
{
    ClassParts result;
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
    result.layouts = arena.unionOf(layouts, other.layouts);
    result.canonicalize();
    return result;
}

static bool liesInsideDeclaredFamily(const WideningContext& context, const Type& type)
{
    const Program& program = context.program;
    std::span<const LayoutID> members = membersOf(context.types.table(), type);
    if (members.empty())
        return false;

    Vector<LayoutID, 16> visited { declaredLayoutOf(program, members[0]) };
    for (size_t index = 0; index < visited.size(); ++index) {
        for (LayoutID supertype : program.at(visited[index]).supertypes) {
            if (visited.contains(supertype))
                continue;
            visited.append(supertype);
            if (program.at(supertype).kind != LayoutKind::Record)
                continue;
            const LayoutSet& family = program.at(program.declaredMembersOf(supertype));
            if (family.members.size() >= members.size() && std::ranges::includes(family.members, members))
                return true;
        }
    }
    return false;
}

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
    auto numberOfLayouts = [&](const Type& listed) -> size_t {
        return membersOf(context.types.table(), listed).size();
    };
    bool widensLayouts = numberOfLayouts(type) > context.build.limit(LimitID::MaxLayoutsInType) && !liesInsideDeclaredFamily(context, type) && exceeds(LimitID::MaxLayoutsInType, numberOfLayouts);
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
    if (widensFunctions) {
        type.anyFunction = true;
        type.functions.clear();
    }
    if (widensRegExpLiterals)
        type.regExpLiteralSet.clear();
    return widensStrings || widensSymbols || widensLayouts || widensClasses || widensFunctions || widensBuiltins || widensRegExpLiterals;
}

Type Type::unionWideningWith(const Type& other, const WideningContext& context) const
{
    Type result = unionWith(other, context.types);
    if (widenGrownLists(result, *this, other, context)) {
        result.canonicalize();
        if (context.didWiden)
            *context.didWiden = true;
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
