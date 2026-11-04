/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeArena.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTObjects.h"
#include "AOTTaggedLayouts.h"
#include "AOTTypeRelations.h"
#include "JSType.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t noSecondOperandInShapeMemo = TypeID().bits();

namespace {

enum class ShapeDescription : uint8_t {
    OfExpectation,
    // Where a test passes, the value is cut by the type that the test was made for. So that type names only what the test tells from everything else.
    OfTest,
};

// What stands in place of the objects that lose their names and of the layouts that no test finds.
struct ShapesOfDescribedObjects {
    Vector<LayoutID, 32> layouts;
    // In ascending order of class, one part a class.
    Vector<ClassPart, 8> classes;
    bool classesDifferFromType { false };
    bool anyObject { false };
    bool anyFunction { false };
    LayoutID layoutOfPreviousObject;
    bool previousObjectMayHaveLayoutBelow { false };
};

} // anonymous namespace

// Neither an object nor a layout says anything of contents, so a class that is listed keeps what is written of them.
static void addClassToDescribedShapes(ShapesOfDescribedObjects& shapes, BuiltinClassID builtinClass)
{
    auto place = std::ranges::lower_bound(shapes.classes, builtinClass, { }, &ClassPart::builtinClass);
    if (place != shapes.classes.end() && place->builtinClass == builtinClass)
        return;
    shapes.classes.insert(place - shapes.classes.begin(), ClassPart { builtinClass, { } });
    shapes.classesDifferFromType = true;
}

static void addLayoutToDescribedShapes(TypeReader reader, ShapesOfDescribedObjects& shapes, LayoutID layout)
{
    BuiltinClassID builtinClass = builtinClassWrittenAbove(reader, layout);
    if (!builtinClass) {
        shapes.layouts.append(layout);
        return;
    }
    // An ordinary object or a cell of the class: no test finds both and nothing else.
    if (reader.program()->at(layout).mayBeOrdinaryObject)
        shapes.anyObject = true;
    else
        addClassToDescribedShapes(shapes, builtinClass);
}

// The constructor of a tagged family makes an object of the member that its argument names, and the record of the object names the layout of the maker.
static void addLayoutWithTaggedMembersToDescribedShapes(TypeReader reader, const Program& program, ShapesOfDescribedObjects& shapes, LayoutID layout)
{
    addLayoutToDescribedShapes(reader, shapes, layout);
    for (const TaggedFamily& family : std::ranges::equal_range(program.taggedFamilies, layout, { }, &TaggedFamily::maker)) {
        for (const TaggedLayout& member : family.members)
            addLayoutToDescribedShapes(reader, shapes, member.layout);
    }
}

// Whatever escapes merges into the form of the layout that its record names, with whatever layout `new.target` gave it.
static bool mayHaveLayoutBelowTheOneDescribed(const ObjectTable& objects, const ObjectRecord& record)
{
    if (record.has(ObjectTrait::MayHaveLayoutBelow))
        return true;
    return record.kind == ObjectKind::EscapedOfLayout && objects.at(objects.instanceOf(record.layout)).has(ObjectTrait::MayHaveLayoutBelow);
}

static void addObjectToDescribedShapes(TypeReader reader, ShapesOfDescribedObjects& shapes, const ObjectRecord& record)
{
    if (shapes.anyObject)
        return;
    if (record.has(ObjectTrait::HasClass)) {
        addClassToDescribedShapes(shapes, record.builtinClass);
        return;
    }
    if (!record.has(ObjectTrait::HasLayout)) {
        (record.has(ObjectTrait::IsCallable) ? shapes.anyFunction : shapes.anyObject) = true;
        return;
    }

    bool mayHaveLayoutBelow = mayHaveLayoutBelowTheOneDescribed(reader.objects(), record);
    if (record.layout == shapes.layoutOfPreviousObject && mayHaveLayoutBelow == shapes.previousObjectMayHaveLayoutBelow)
        return;
    shapes.layoutOfPreviousObject = record.layout;
    shapes.previousObjectMayHaveLayoutBelow = mayHaveLayoutBelow;

    const Program* program = reader.program();
    if (!program) {
        // Nobody can list the layouts below.
        shapes.anyObject = mayHaveLayoutBelow;
        shapes.layouts.append(record.layout);
        return;
    }
    addLayoutWithTaggedMembersToDescribedShapes(reader, *program, shapes, record.layout);
    if (!mayHaveLayoutBelow)
        return;
    for (LayoutID below : program->layoutsExpectedBelow(record.layout))
        addLayoutWithTaggedMembersToDescribedShapes(reader, *program, shapes, below);
}

// A TypeTest compares a cell with the cell of a built-in and asks a closure for its function. It finds no other object by itself.
static bool isFoundByTestOfIdentity(const BuiltinTable& builtins, const ObjectRecord& record)
{
    switch (record.kind) {
    case ObjectKind::Closure:
        return true;
    case ObjectKind::Builtin:
    case ObjectKind::ModuleBuiltin:
        return record.has(ObjectTrait::IsFoundByIdentity) && isTestableBuiltin(builtins, BuiltinID(record.payload));
    case ObjectKind::Unknown:
    case ObjectKind::ResultOfBuiltin:
    case ObjectKind::HostObject:
    case ObjectKind::EscapedFunctionsOfHolder:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::FunctionPrototype:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Namespace:
    case ObjectKind::Instance:
    case ObjectKind::Prototype:
    case ObjectKind::EscapedOfLayout:
    case ObjectKind::Site:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool keepsNameInShapeDescription(const BuiltinTable& builtins, const ObjectRecord& record, ShapeDescription description)
{
    if (record.has(ObjectTrait::IsMerged))
        return false;
    switch (description) {
    case ShapeDescription::OfExpectation:
        return record.traits.containsAny({ ObjectTrait::IsClosure, ObjectTrait::IsFoundByIdentity });
    case ShapeDescription::OfTest:
        return isFoundByTestOfIdentity(builtins, record);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void widenClassesWithoutInstanceTestInDescribedShapes(const BuiltinTable& builtins, ShapesOfDescribedObjects& shapes)
{
    shapes.classes.removeAllMatching([&](const ClassPart& part) {
        if (hasInstanceTest(builtins, part.builtinClass))
            return false;
        (holdsOnlyCallables(builtins, part.builtinClass) ? shapes.anyFunction : shapes.anyObject) = true;
        shapes.classesDifferFromType = true;
        return true;
    });
}

static TypeParts describeObjectsByShape(TypeArena& arena, const Type& type, ShapeDescription description)
{
    TypeReader reader(arena);
    const BuiltinTable& builtins = arena.builtins();
    const ObjectTable& objects = arena.objects();
    ASSERT(!arena.program() || std::ranges::is_sorted(arena.program()->taggedFamilies, { }, &TaggedFamily::maker));

    TypeParts parts = type;
    ShapesOfDescribedObjects shapes;
    shapes.anyObject = type.anyObject;
    shapes.anyFunction = type.anyFunction;
    if (type.unnamedClasses)
        shapes.classes.append(arena.at(type.unnamedClasses));

    if (type.objects) {
        parts.objects = arena.objectSets().filtered(type.objects, [&](ObjectID object) {
            const ObjectRecord& record = objects.at(object);
            if (keepsNameInShapeDescription(builtins, record, description))
                return true;
            addObjectToDescribedShapes(reader, shapes, record);
            return false;
        });
        // No test tells through which call the creator of an object was entered.
        if (parts.objects && type.traitsOfNamed.contains(ObjectTrait::IsOfInstance)) {
            parts.objects = arena.objectSets().replaced(parts.objects, [&](ObjectID object) {
                return objects.sharedFormOf(object);
            });
        }
    }

    if (type.unnamedLayouts) {
        parts.unnamedLayouts = arena.layoutSets().filtered(type.unnamedLayouts, [&](LayoutID layout) {
            if (!builtinClassWrittenAbove(reader, layout))
                return true;
            addLayoutToDescribedShapes(reader, shapes, layout);
            return false;
        });
    }

    if (description == ShapeDescription::OfTest)
        widenClassesWithoutInstanceTestInDescribedShapes(builtins, shapes);

    parts.anyObject = shapes.anyObject;
    parts.anyFunction = shapes.anyFunction || shapes.anyObject;
    if (shapes.anyObject) {
        parts.unnamedLayouts = { };
        parts.unnamedClasses = { };
    } else {
        if (!shapes.layouts.isEmpty())
            parts.unnamedLayouts = arena.layoutSets().unionOf(parts.unnamedLayouts, arena.layoutSets().ofUnsorted(shapes.layouts.span()));
        if (shapes.classesDifferFromType)
            parts.unnamedClasses = arena.internClassParts(shapes.classes.span());
    }

    // A test that asks for any object or for any callable asks for nothing beside it.
    if (description == ShapeDescription::OfTest && parts.objects) {
        if (parts.anyObject)
            parts.objects = { };
        else if (parts.anyFunction) {
            parts.objects = arena.objectSets().filtered(parts.objects, [&](ObjectID object) {
                return !objects.at(object).has(ObjectTrait::IsCallable);
            });
        }
    }
    return parts;
}

static bool holdsNothingToDescribeByShape(const Type& type, ShapeDescription description)
{
    return !type.objects && !type.unnamedLayouts && (description == ShapeDescription::OfExpectation || !type.unnamedClasses);
}

static TypeID internDescriptionByShape(TypeArena& arena, TypeID type, const Type& record, ShapeDescription description)
{
    TypeParts parts = describeObjectsByShape(arena, record, description);
    if (parts == static_cast<const TypeParts&>(record))
        return type;
    return arena.intern(parts);
}

TypeID TypeArena::describedByShape(TypeID type)
{
    ASSERT(type);
    if (holdsOnlyPrimitives(type))
        return type;
    Type record = at(type);
    if (holdsNothingToDescribeByShape(record, ShapeDescription::OfExpectation))
        return type;

    if (std::optional<uint32_t> known = remembered(Operation::DescribedByShape, type.bits(), noSecondOperandInShapeMemo))
        return TypeID::fromBits(*known);
    TypeID result = internDescriptionByShape(*this, type, record, ShapeDescription::OfExpectation);
    remember(Operation::DescribedByShape, type.bits(), noSecondOperandInShapeMemo, result.bits());
    return result;
}

TypeID TypeArena::withOnlyNamedObjects(TypeID type)
{
    ASSERT(type);
    if (holdsOnlyPrimitives(type))
        return type;

    Type record = at(type);
    if (!record.hasUnknown())
        return type;
    TypeParts parts = record;
    parts.seeds = { };
    parts.anyObject = false;
    parts.anyFunction = false;
    parts.unnamedLayouts = { };
    parts.unnamedClasses = { };
    return intern(parts);
}

// A merged member goes only where the `unknown` beside it stands for the same objects.
static bool unknownStandsForMergedObject(const TypeArena& arena, const Type& holder, ObjectID object)
{
    const ObjectRecord& record = arena.at(object);
    if (!record.has(ObjectTrait::IsMerged))
        return false;
    if (holder.anyObject)
        return true;
    if (holder.anyFunction && record.has(ObjectTrait::IsCallable))
        return true;
    if (record.layout && holder.unnamedLayouts && arena.at(holder.unnamedLayouts).contains(record.layout))
        return true;
    if (!record.builtinClass || !holder.unnamedClasses)
        return false;
    return std::ranges::any_of(arena.at(holder.unnamedClasses), [&](const ClassPart& part) {
        return part.builtinClass == record.builtinClass;
    });
}

// Many nodes hold one value: n nodes that hold n objects cost n + n.
TypeID TypeArena::withoutMergedObjectsInsideUnknown(TypeID type)
{
    ASSERT(type);
    if (holdsOnlyPrimitives(type))
        return type;
    Type record = at(type);
    if (!record.hasUnknown() || !record.namesMergedObjects())
        return type;

    if (std::optional<uint32_t> known = remembered(Operation::WithoutMergedObjectsInsideUnknown, type.bits(), noSecondOperandInShapeMemo))
        return TypeID::fromBits(*known);
    TypeID result = type;
    ObjectSetID standingByRight = m_objectSets.filtered(record.objects, [&](ObjectID object) {
        return !unknownStandsForMergedObject(*this, record, object);
    });
    if (standingByRight != record.objects) {
        TypeParts parts = record;
        parts.objects = standingByRight;
        result = intern(parts);
    }
    remember(Operation::WithoutMergedObjectsInsideUnknown, type.bits(), noSecondOperandInShapeMemo, result.bits());
    return result;
}

TypeID TypeArena::withoutCopiesBesideSharedForms(TypeID type)
{
    if (!type || holdsOnlyPrimitives(type))
        return type;
    Type record = at(type);
    if (!record.traitsOfNamed.contains(ObjectTrait::IsOfInstance))
        return type;

    if (std::optional<uint32_t> known = remembered(Operation::WithoutCopiesBesideSharedForms, type.bits(), noSecondOperandInShapeMemo))
        return TypeID::fromBits(*known);
    TypeID result = type;
    SetView<ObjectID, ObjectSetID> named = at(record.objects);
    ObjectSetID kept = m_objectSets.filtered(record.objects, [&](ObjectID object) {
        return !named.contains(at(object).sharedForm);
    });
    if (kept != record.objects) {
        TypeParts parts = record;
        parts.objects = kept;
        result = intern(parts);
    }
    remember(Operation::WithoutCopiesBesideSharedForms, type.bits(), noSecondOperandInShapeMemo, result.bits());
    return result;
}

TypeID TypeArena::testablePartOf(TypeID type)
{
    ASSERT(type);
    type = withoutBoundsOfFractions(type);
    if (holdsOnlyPrimitives(type))
        return type;
    Type record = at(type);
    if (holdsNothingToDescribeByShape(record, ShapeDescription::OfTest))
        return type;

    if (std::optional<uint32_t> known = remembered(Operation::TestablePart, type.bits(), noSecondOperandInShapeMemo))
        return TypeID::fromBits(*known);
    TypeID result = internDescriptionByShape(*this, type, record, ShapeDescription::OfTest);
    remember(Operation::TestablePart, type.bits(), noSecondOperandInShapeMemo, result.bits());
    return result;
}

TypeID TypeArena::withObjectsReplaced(TypeID type, NOESCAPE const ScopedLambda<ObjectID(ObjectID)>& replacement)
{
    ASSERT(type);
    if (holdsOnlyPrimitives(type))
        return type;

    Type record = at(type);
    if (!record.objects)
        return type;
    ObjectSetID replaced = m_objectSets.replaced(record.objects, [&](ObjectID object) {
        ObjectID objectInItsPlace = replacement(object);
        RELEASE_ASSERT(objectInItsPlace && objectInItsPlace != ObjectTable::unknown());
        return objectInItsPlace;
    });
    if (replaced == record.objects)
        return type;
    TypeParts parts = record;
    parts.objects = replaced;
    return intern(parts);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
