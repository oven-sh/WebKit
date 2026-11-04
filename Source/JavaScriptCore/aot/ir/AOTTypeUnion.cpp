/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeArena.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

using ClassPartsToUnite = Vector<ClassPart, 8>;
using UnitedClassParts = Vector<ClassPart, 16>;

// Null means that nothing is written, which reads as anything, so it swallows the other side. internClassParts() stores a null with written contents after it as TypeTable::anything.
// A union that isAnything() may differ from TypeTable::anything by seeds, Absent, Unspecified or named objects. It becomes null too, or the union of three types would depend on their order.
static TypeID uniteWrittenContents(TypeArena& arena, TypeID first, TypeID second)
{
    if (first == second)
        return first;
    if (!first || !second)
        return { };
    TypeID united = arena.unionOf(first, second);
    if (!holdsOnlyPrimitives(united) && arena.at(united).isAnything())
        return { };
    return united;
}

static ClassPart unitePartsOfOneClass(TypeArena& arena, const ClassPart& first, const ClassPart& second)
{
    ASSERT(first.builtinClass == second.builtinClass);
    ClassPart united;
    united.builtinClass = first.builtinClass;
    for (unsigned position = 0; position < mostContentsOfClass; ++position)
        united.writtenContents[position] = uniteWrittenContents(arena, first.writtenContents[position], second.writtenContents[position]);
    return united;
}

static ClassPartsID uniteClassParts(TypeArena& arena, ClassPartsID classesOfFirst, ClassPartsID classesOfSecond)
{
    if (classesOfFirst == classesOfSecond || !classesOfSecond)
        return classesOfFirst;
    if (!classesOfFirst)
        return classesOfSecond;

    // Copies, because uniting contents interns, and interning may move the parts that an id names.
    ClassPartsToUnite first(arena.at(classesOfFirst));
    ClassPartsToUnite second(arena.at(classesOfSecond));

    UnitedClassParts united;
    size_t indexInFirst = 0;
    size_t indexInSecond = 0;
    while (indexInFirst < first.size() && indexInSecond < second.size()) {
        const ClassPart& partOfFirst = first[indexInFirst];
        const ClassPart& partOfSecond = second[indexInSecond];
        if (partOfFirst.builtinClass < partOfSecond.builtinClass) {
            united.append(partOfFirst);
            ++indexInFirst;
        } else if (partOfSecond.builtinClass < partOfFirst.builtinClass) {
            united.append(partOfSecond);
            ++indexInSecond;
        } else {
            united.append(unitePartsOfOneClass(arena, partOfFirst, partOfSecond));
            ++indexInFirst;
            ++indexInSecond;
        }
    }
    united.append(first.subspan(indexInFirst));
    united.append(second.subspan(indexInSecond));
    return arena.internClassParts(united.span());
}

static bool unionHoldsTwoInternalValues(const Type& first, const Type& second)
{
    return (first.opaque && second.opaque && first.opaque != second.opaque)
        || (first.environment && second.environment && first.environment != second.environment);
}

// intern() decides which part swallows which. Here each part only grows with either side.
// The parts for (a, b) intern to the same type as the parts for (b, a): TypeArena::unionOf() remembers one answer for both.
TypeParts uniteTypes(TypeArena& arena, const Type& first, const Type& second)
{
    TypeParts united;
    united.primitives = first.primitives | second.primitives;
    united.seeds = first.seeds | second.seeds;
    united.anyObject = first.anyObject || second.anyObject;
    united.anyFunction = first.anyFunction || second.anyFunction;
    united.integers = first.integers.unionWith(second.integers);
    if (!first.holdsFractionsWithoutBounds() && !second.holdsFractionsWithoutBounds())
        united.boundsOfFractions = first.boundsOfFractions.unionWith(second.boundsOfFractions);

    // intern() turns an opaque value or an environment into anything once it is mixed with something else.
    // The parts hold only one of each, so anyObject stands in for a second, different one.
    united.opaque = first.opaque ? first.opaque : second.opaque;
    united.environment = first.environment ? first.environment : second.environment;
    if (unionHoldsTwoInternalValues(first, second))
        united.anyObject = true;

    united.stringLiterals = arena.stringSets().unionOf(first.stringLiterals, second.stringLiterals);
    united.symbolLiterals = arena.keySets().unionOf(first.symbolLiterals, second.symbolLiterals);
    united.objects = arena.objectSets().unionOf(first.objects, second.objects);
    united.unnamedLayouts = arena.layoutSets().unionOf(first.unnamedLayouts, second.unnamedLayouts);
    united.unnamedClasses = uniteClassParts(arena, first.unnamedClasses, second.unnamedClasses);
    return united;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
