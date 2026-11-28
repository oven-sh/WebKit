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
    OfTest,
};

struct ShapesOfDescribedObjects {
    Vector<LayoutID, 32> layouts;
    Vector<ClassPart, 8> classes;
    bool classesDifferFromType { false };
    bool anyObject { false };
    bool anyFunction { false };
    LayoutID layoutOfPreviousObject;
    bool previousObjectMayHaveLayoutBelow { false };
};

} // anonymous namespace

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
    if (reader.program()->at(layout).mayBeOrdinaryObject)
        shapes.anyObject = true;
    else
        addClassToDescribedShapes(shapes, builtinClass);
}

static void addLayoutWithTaggedMembersToDescribedShapes(TypeReader reader, const Program& program, ShapesOfDescribedObjects& shapes, LayoutID layout)
{
    addLayoutToDescribedShapes(reader, shapes, layout);
    for (const TaggedFamily& family : std::ranges::equal_range(program.taggedFamilies, layout, { }, &TaggedFamily::maker)) {
        for (const TaggedLayout& member : family.members)
            addLayoutToDescribedShapes(reader, shapes, member.layout);
    }
}

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

bool testTellsNoClassOfObjectsIn(TypeReader reader, const Type& type)
{
    if (!type.hasObjects() || type.anyObject)
        return false;
    const BuiltinTable& builtins = reader.builtins();
    ShapesOfDescribedObjects shapes;
    shapes.anyFunction = type.anyFunction;
    if (type.unnamedClasses)
        shapes.classes.append(reader.at(type.unnamedClasses));
    if (type.objects) {
        reader.at(type.objects).forEach([&](ObjectID object) {
            const ObjectRecord& record = reader.at(object);
            if (!keepsNameInShapeDescription(builtins, record, ShapeDescription::OfTest))
                addObjectToDescribedShapes(reader, shapes, record);
        });
    }
    if (type.unnamedLayouts) {
        reader.at(type.unnamedLayouts).forEach([&](LayoutID layout) {
            if (builtinClassWrittenAbove(reader, layout))
                addLayoutToDescribedShapes(reader, shapes, layout);
        });
    }
    widenClassesWithoutInstanceTestInDescribedShapes(builtins, shapes);
    return shapes.anyObject || (shapes.anyFunction && !type.anyFunction);
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
