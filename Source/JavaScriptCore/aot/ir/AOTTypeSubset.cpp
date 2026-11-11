/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeRelations.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static std::span<const ClassPart> classPartsToCompare(TypeReader reader, ClassPartsID parts)
{
    if (!parts)
        return { };
    return reader.at(parts);
}

template<typename SetID>
static auto membersToCompare(TypeReader reader, SetID set) -> decltype(reader.at(set))
{
    if (!set)
        return { };
    return reader.at(set);
}

static bool functionsOverlapClasses(const BuiltinTable& builtins, std::span<const ClassPart> parts)
{
    return std::ranges::any_of(parts, [&](const ClassPart& part) {
        return isFunctionClass(builtins, part.builtinClass);
    });
}

static bool builtinOverlapsClasses(const BuiltinTable& builtins, BuiltinID builtin, std::span<const ClassPart> parts)
{
    return std::ranges::any_of(parts, [&](const ClassPart& part) {
        return mayBeInstanceOf(builtins, builtin, part.builtinClass);
    });
}

template<typename SetID>
static bool isInternedSetSubset(TypeReader reader, SetID set, SetID of)
{
    if (!set || set == of)
        return true;
    return of && isSubset(reader.at(set).untyped(), reader.at(of).untyped());
}

template<typename SetID>
static bool internedSetsOverlap(TypeReader reader, SetID a, SetID b)
{
    if (!a || !b)
        return false;
    return a == b || overlap(reader.at(a).untyped(), reader.at(b).untyped());
}

bool mayContainAnyCallable(TypeReader reader, const Type& type)
{
    return type.mayBeFunction() || functionsOverlapClasses(reader.builtins(), classPartsToCompare(reader, type.classes));
}

bool mayContainAnyClosure(TypeReader reader, const Type& type)
{
    const BuiltinTable& builtins = reader.builtins();
    return type.anyFunction || std::ranges::any_of(classPartsToCompare(reader, type.classes), [&](const ClassPart& part) {
        return holdsOnlyCallables(builtins, part.builtinClass);
    });
}

bool mayContainBuiltin(TypeReader reader, const Type& type, BuiltinID builtin)
{
    if (type.anyFunction)
        return true;

    const BuiltinTable& builtins = reader.builtins();
    if (builtinOverlapsClasses(builtins, builtin, classPartsToCompare(reader, type.unnamedClasses)))
        return true;

    const ObjectTable& objects = reader.objects();
    ObjectID object = objects.objectOf(builtin);
    if (mayFitUnknownOf(reader, object, type))
        return true;

    BuiltinID canonical = builtins.canonicalID(builtin);
    return membersToCompare(reader, type.objects).anyOf([&](ObjectID named) {
        return objects.builtinOf(named) == canonical || mayBeSameObject(reader, object, named);
    });
}

bool overlapsAnotherClass(TypeReader reader, BuiltinClassID builtinClass, const Type& type)
{
    const BuiltinTable& builtins = reader.builtins();
    return std::ranges::any_of(classPartsToCompare(reader, type.classes), [&](const ClassPart& part) {
        return part.builtinClass != builtinClass && relate(builtins, builtinClass, part.builtinClass) == ClassRelation::Unknown;
    });
}

static bool areClassesSubset(TypeReader reader, ClassPartsID classes, const Type& of)
{
    if (classes == of.unnamedClasses)
        return true;
    std::span<const ClassPart> holders = classPartsToCompare(reader, of.unnamedClasses);
    return std::ranges::all_of(classPartsToCompare(reader, classes), [&](const ClassPart& part) {
        return (of.anyFunction && holdsOnlyCallables(reader.builtins(), part.builtinClass)) || findPartHolding(reader, holders, part.builtinClass);
    });
}

static bool areLayoutsSubset(TypeReader reader, LayoutSetID layouts, LayoutSetID of, std::span<const ClassPart> classesOfHolder)
{
    if (isInternedSetSubset(reader, layouts, of))
        return true;
    if (classesOfHolder.empty())
        return false;
    auto listed = membersToCompare(reader, of);
    return reader.at(layouts).allOf([&](LayoutID layout) {
        return listed.contains(layout) || isInsideSomeClassPart(reader, layout, classesOfHolder);
    });
}

static bool areNamedObjectsSubset(TypeReader reader, const Type& type, const Type& of)
{
    if (isInternedSetSubset(reader, type.objects, of.objects))
        return true;
    if (!of.hasUnknown())
        return false;

    auto named = reader.at(type.objects);
    auto namedByHolder = membersToCompare(reader, of.objects);
    bool allAreInside = true;
    forEachAddedMember(named.untyped(), namedByHolder.untyped(), [&](uint32_t member) {
        allAreInside = allAreInside && surelyFitsUnknownOf(reader, ObjectID(member), of);
    });
    return allAreInside;
}

static bool areObjectsSubset(TypeReader reader, const Type& type, const Type& of)
{
    if (!type.hasObjects() || of.anyObject)
        return true;
    if (type.anyObject || (type.anyFunction && !of.anyFunction))
        return false;

    return areClassesSubset(reader, type.unnamedClasses, of)
        && areLayoutsSubset(reader, type.unnamedLayouts, of.unnamedLayouts, classPartsToCompare(reader, of.unnamedClasses))
        && areNamedObjectsSubset(reader, type, of);
}

bool isSubset(TypeReader reader, const Type& type, const Type& of)
{
    uint16_t missingPrimitives = type.primitives & ~of.primitives;
    if (of.primitives & Primitive::String)
        missingPrimitives &= ~Primitive::LiteralString;
    if (of.primitives & Primitive::Symbol)
        missingPrimitives &= ~Primitive::LiteralSymbol;
    if (missingPrimitives)
        return false;
    if (!type.integers.isSubsetOf(of.integers))
        return false;
    if (type.primitives & Primitive::FractionalNumber && !of.boundsOfFractions.isEmpty() && (type.boundsOfFractions.isEmpty() || !type.boundsOfFractions.isSubsetOf(of.boundsOfFractions)))
        return false;
    if (!(of.primitives & Primitive::String) && !isInternedSetSubset(reader, type.stringLiterals, of.stringLiterals))
        return false;
    if (!(of.primitives & Primitive::Symbol) && !isInternedSetSubset(reader, type.symbolLiterals, of.symbolLiterals))
        return false;
    if (type.opaque && type.opaque != of.opaque)
        return false;
    if (type.environment && type.environment != of.environment)
        return false;
    return areObjectsSubset(reader, type, of);
}

template<typename SetID>
static bool literalsOverlap(TypeReader reader, const Type& a, const Type& b, SetID TypeParts::* literals, uint16_t widenedBit, uint16_t literalBit)
{
    uint16_t bits = widenedBit | literalBit;
    if (!(a.primitives & bits) || !(b.primitives & bits))
        return false;
    return (a.primitives | b.primitives) & widenedBit || internedSetsOverlap(reader, a.*literals, b.*literals);
}

static bool layoutsOverlapClasses(TypeReader reader, LayoutSetID layouts, std::span<const ClassPart> classes)
{
    if (!layouts || classes.empty())
        return false;
    return reader.at(layouts).anyOf([&](LayoutID layout) {
        return mayBeInsideSomeClassPart(reader, layout, classes);
    });
}

static bool classesOverlap(const BuiltinTable& builtins, std::span<const ClassPart> a, std::span<const ClassPart> b)
{
    return std::ranges::any_of(a, [&](const ClassPart& first) {
        return std::ranges::any_of(b, [&](const ClassPart& second) {
            return first.builtinClass == second.builtinClass || relate(builtins, first.builtinClass, second.builtinClass) == ClassRelation::Unknown;
        });
    });
}

static bool unknownObjectsOverlap(TypeReader reader, const Type& a, const Type& b)
{
    if (a.anyFunction && b.anyFunction)
        return true;

    const BuiltinTable& builtins = reader.builtins();
    std::span<const ClassPart> classesOfFirst = classPartsToCompare(reader, a.unnamedClasses);
    std::span<const ClassPart> classesOfSecond = classPartsToCompare(reader, b.unnamedClasses);
    if (a.anyFunction && functionsOverlapClasses(builtins, classesOfSecond))
        return true;
    if (b.anyFunction && functionsOverlapClasses(builtins, classesOfFirst))
        return true;
    return internedSetsOverlap(reader, a.unnamedLayouts, b.unnamedLayouts)
        || layoutsOverlapClasses(reader, a.unnamedLayouts, classesOfSecond)
        || layoutsOverlapClasses(reader, b.unnamedLayouts, classesOfFirst)
        || classesOverlap(builtins, classesOfFirst, classesOfSecond);
}

static bool namedObjectOverlapsUnknownOf(TypeReader reader, ObjectID object, const Type& other, std::span<const ClassPart> classesOfOther, bool otherMayHoldAnyClosure, LayoutsFoundForCellsNobodyDescribes& found)
{
    if (mayFitUnknownOf(reader, object, other, found))
        return true;

    const BuiltinTable& builtins = reader.builtins();
    if (BuiltinID builtin = reader.objects().builtinOf(object))
        return other.anyFunction || builtinOverlapsClasses(builtins, builtin, classesOfOther);

    const ObjectRecord& record = reader.at(object);
    if (otherMayHoldAnyClosure && record.traits.containsAny({ ObjectTrait::IsClosure, ObjectTrait::IsCallable }))
        return true;
    return record.has(ObjectTrait::HasNoShape) && std::ranges::any_of(classesOfOther, [&](const ClassPart& part) {
        return !hasInstanceTest(builtins, part.builtinClass);
    });
}

static bool namedObjectsOverlapUnknownOf(TypeReader reader, const Type& type, const Type& other)
{
    if (!type.objects)
        return false;
    bool otherMayHoldAnyClosure = mayContainAnyClosure(reader, other);
    if (!other.hasUnknown() && !otherMayHoldAnyClosure)
        return false;

    std::span<const ClassPart> classesOfOther = classPartsToCompare(reader, other.unnamedClasses);
    LayoutsFoundForCellsNobodyDescribes found;
    return reader.at(type.objects).anyOf([&](ObjectID object) {
        return namedObjectOverlapsUnknownOf(reader, object, other, classesOfOther, otherMayHoldAnyClosure, found);
    });
}

static bool namedObjectsMayBeSameObject(TypeReader reader, ObjectSetID a, ObjectSetID b)
{
    if (!a || !b)
        return false;

    ObjectSetAliasIndex index(reader, reader.at(b));
    return reader.at(a).anyOf([&](ObjectID first) {
        return index.namesAliasOf(first);
    });
}

static bool objectsOverlap(TypeReader reader, const Type& a, const Type& b)
{
    if (!a.hasObjects() || !b.hasObjects())
        return false;
    if (a.anyObject || b.anyObject)
        return true;
    return internedSetsOverlap(reader, a.objects, b.objects)
        || unknownObjectsOverlap(reader, a, b)
        || namedObjectsOverlapUnknownOf(reader, a, b)
        || namedObjectsOverlapUnknownOf(reader, b, a)
        || namedObjectsMayBeSameObject(reader, a.objects, b.objects);
}

static bool fractionsLieApart(const Type& a, const Type& b)
{
    if (a.boundsOfFractions.isEmpty() || b.boundsOfFractions.isEmpty())
        return false;
    Range bounds = a.boundsOfFractions.intersectWith(b.boundsOfFractions);
    return bounds.minimum >= bounds.maximum;
}

bool overlap(TypeReader reader, const Type& a, const Type& b)
{
    uint16_t primitivesOfBoth = a.primitives & b.primitives & plainPrimitives;
    if (fractionsLieApart(a, b))
        primitivesOfBoth &= ~Primitive::FractionalNumber;
    if (primitivesOfBoth)
        return true;
    if (!a.integers.intersectWith(b.integers).isEmpty())
        return true;
    if (literalsOverlap(reader, a, b, &TypeParts::stringLiterals, Primitive::String, Primitive::LiteralString))
        return true;
    if (literalsOverlap(reader, a, b, &TypeParts::symbolLiterals, Primitive::Symbol, Primitive::LiteralSymbol))
        return true;
    return mayBeSameObject(reader, a, b);
}

bool mayBeSameObject(TypeReader reader, const Type& a, const Type& b)
{
    if (a.opaque && a.opaque == b.opaque)
        return true;
    if (a.environment && a.environment == b.environment)
        return true;
    return objectsOverlap(reader, a, b);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
