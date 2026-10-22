/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFreshResults.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlattenInnerObjects.h"
#include "AOTSinkAllocationsInternal.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <iterator>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxVirtualObjectVariants, Architecture::ARM64) <= maxTagBitsInInt32);
static_assert(defaultLimitValue(LimitID::MaxVirtualObjectVariants, Architecture::X86_64) <= maxTagBitsInInt32);

static bool isWrapperOrWorker(FunctionRole role)
{
    switch (role) {
    case FunctionRole::AsyncWorker:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ResultWorker:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
        return true;
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::CoroutineBody:
    case FunctionRole::BoundFunction:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool canReturnParts(const Function& function)
{
    if (!function.hasBody() || !function.mayChangeSignature() || !function.mayBeCloned() || !function.hasOrdinaryFrame())
        return false;
    if (function.results.size() != 1 || function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
        return false;
    if (isWrapperOrWorker(function.role))
        return false;
    // A construction by value names the function itself, and is resolved only while that is no wrapper. The body that a resolved construction calls is split like any other.
    if (function.role == FunctionRole::Source && function.constructedClass)
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
        return true;
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool returnsWithoutValue(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Return:
        return !terminator.valueOperand();
    case TerminatorKind::ReturnCall:
        return true;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isAllocation(const Function& function, ValueID value)
{
    return interpretedOf(function.at(value).opcode) == Interpretation::Allocation;
}

static bool mayBeAllocation(FunctionAnalyses& analyses, ValueID value)
{
    const Function& function = analyses.function();
    if (isAllocation(function, analyses.sameObjectThroughCasts(value)))
        return true;
    if (!mayBeReference(analyses.program().typeOf(function, value)))
        return false;

    const EscapeAnalysis& escape = analyses.escape();
    PointsToSet pointsTo = escape.pointsTo(value);
    for (ObjectID object : pointsTo.objects) {
        if (escape.at(object).kind == ObjectKind::Allocation)
            return true;
    }
    return false;
}

static FreshResultShape computeShapeOfReturns(FunctionAnalyses& analyses, ObjectsThatBecomeOneCell objectsThatBecomeOneCell)
{
    const Function& function = analyses.function();
    bool hasAllocation = false;
    Vector<ValueID, 4> returnedValues;
    for (BlockID id : analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        if (returnsWithoutValue(block.terminator))
            return { .state = FreshResultsState::Unknown };
        if (block.terminator.kind == TerminatorKind::Return)
            returnedValues.append(block.terminator.valueOperand());
        hasAllocation = hasAllocation || block.values.containsIf([&](ValueID value) {
            return isAllocation(function, value);
        });
    }
    bool mayReturnAllocation = hasAllocation && std::ranges::any_of(returnedValues, [&](ValueID value) {
        return mayBeAllocation(analyses, value);
    });
    if (!mayReturnAllocation)
        return { .state = FreshResultsState::NotFresh };

    AllocationSinking sinking(analyses, { ReturnedObjects::StayVirtual, false });
    if (!sinking.collectCandidates())
        return { .state = FreshResultsState::NotFresh };
    sinking.analyze();
    return shapeOfReturns(sinking, objectsThatBecomeOneCell);
}

FreshResultShape computeFreshResults(FunctionAnalyses& analyses)
{
    if (!canReturnParts(analyses.function()))
        return { .state = FreshResultsState::NotFresh };
    return computeShapeOfReturns(analyses, ObjectsThatBecomeOneCell::StayWhole);
}

bool returnsFreshAggregate(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    return function.hasBody() && function.results.size() == 1 && computeShapeOfReturns(analyses, ObjectsThatBecomeOneCell::AreFreshLikeOthers).isFresh();
}

FreshResultVariant variantOfShape(const AllocationSinking& sinking, VirtualObjectID object, const VirtualShape& shape)
{
    RELEASE_ASSERT(shape.kind == VirtualShapeKind::Fields);
    const VirtualFamily& family = sinking.familyOf(object);
    const VirtualObjectDescription& description = family.descriptionOf(shape);

    FreshResultVariant variant {
        .kind = VirtualShapeKind::Fields,
        .description = description,
        .prototype = sinking.function().at(description.allocation),
        .length = shape.length,
    };

    if (description.hasKeyOrder) {
        for (uint32_t field : sinking.orders().fieldsOf(shape.order))
            variant.properties.append(FreshResultField { family.fieldAt(hasStored(shape, field) ? field : family.findCounterpart(field).value_or(field)), 0 });
    }
    for (size_t storedField : shape.stored) {
        const VirtualField& field = family.fieldAt(static_cast<uint32_t>(storedField));
        if (field.isProperty()) {
            if (!description.hasKeyOrder)
                variant.properties.append(FreshResultField { field, 0 });
            continue;
        }
        if (field.kind != VirtualFieldKind::Length)
            variant.parts.append(FreshResultField { field, 0 });
    }
    return variant;
}

static bool hasReturnWithoutValue(const AllocationSinking& sinking)
{
    const Function& function = sinking.function();
    for (size_t index = 0; index < function.blocks.size(); ++index) {
        BlockID block(index);
        if (sinking.headOf(block) && returnsWithoutValue(function.at(block).terminator))
            return true;
    }
    return false;
}

static void addVariant(FreshResultShape& shape, FreshResultVariant&& variant)
{
    bool isPresent = shape.variants.containsIf([&](const FreshResultVariant& other) {
        return other.buildsSameValueAs(variant);
    });
    if (!isPresent)
        shape.variants.append(WTF::move(variant));
}

static void addVariantsOfReturn(const AllocationSinking& sinking, const VirtualReturn& returned, FreshResultShape& shape)
{
    if (!returned.object) {
        if (std::optional<VirtualConstant> constant = virtualConstantOf(sinking.program(), sinking.function(), returned.value))
            addVariant(shape, { .kind = VirtualShapeKind::Constant, .constant = *constant });
        else
            addVariant(shape, { .kind = VirtualShapeKind::Cell });
        return;
    }

    const VirtualFamily& family = sinking.familyOf(returned.object);
    for (const ObjectVariant& objectVariant : returned.state.variants) {
        const VirtualShape& objectShape = family.at(objectVariant.shape);
        switch (objectShape.kind) {
        case VirtualShapeKind::Cell:
            addVariant(shape, { .kind = VirtualShapeKind::Cell });
            break;
        case VirtualShapeKind::Constant:
            addVariant(shape, { .kind = VirtualShapeKind::Constant, .constant = objectShape.constant });
            break;
        case VirtualShapeKind::Fields: {
            FreshResultVariant variant = variantOfShape(sinking, returned.object, objectShape);
            // Only a rare arm stores a value that nothing proves to fit its field, whatever merges with it on the way to the return.
            if (hasUnprovenField(family, objectShape))
                variant.frequency = BlockFrequency::Rare;
            addVariant(shape, WTF::move(variant));
            break;
        }
        }
    }
}

static bool hasVariantOfKind(const FreshResultShape& shape, VirtualShapeKind kind)
{
    return shape.variants.containsIf([&](const FreshResultVariant& variant) {
        return variant.kind == kind;
    });
}

static uint32_t ensurePart(FreshResultShape& shape, const VirtualField& field, TypeID type)
{
    for (uint32_t part = 0; part < shape.partTypes.size(); ++part) {
        if (part != shape.cellPart && shape.fieldOfPart[part] == field && shape.partTypes[part] == type)
            return part;
    }
    uint32_t part = shape.partTypes.size();
    shape.fieldOfPart.append(field);
    shape.partTypes.append(type);
    return part;
}

static void assignParts(const AllocationSinking& sinking, FreshResultShape& shape)
{
    if (hasVariantOfKind(shape, VirtualShapeKind::Cell)) {
        shape.cellPart = 0;
        shape.fieldOfPart.append(VirtualField { });
        const DeclaredType& result = sinking.function().result();
        shape.partTypes.append(result.flags.contains(PositionFlags::IsExpected) ? TypeTable::anything : result.type);
    }

    for (FreshResultVariant& variant : shape.variants) {
        if (variant.kind != VirtualShapeKind::Fields)
            continue;
        auto assign = [&](FreshResultField& entry) {
            entry.part = ensurePart(shape, entry.field, typeOfField(sinking.objectContext(), variant.description, entry.field));
        };
        std::ranges::for_each(variant.properties, assign);
        std::ranges::for_each(variant.parts, assign);
    }
}

// The parts of such an object would leave the function with the inner object made, and the caller would build the outer one around it: two cells for ever.
// Left in one piece the two become one cell, which is no more than the function allocates with the parts.
static bool listsObjectThatMayLieInIt(const AllocationSinking& sinking, const FreshResultVariant& variant)
{
    if (variant.kind != VirtualShapeKind::Fields)
        return false;
    const Function& function = sinking.function();
    const Value& allocation = function.at(variant.description.allocation);
    if (allocation.opcode != Opcode::NewObject)
        return false;
    auto fields = allocation.fields();
    auto values = allocation.valuesOperands();
    for (size_t index = 0; index < fields.size(); ++index) {
        const Value& listed = function.at(sameObjectThroughCasts(function, values[index]));
        if (listed.opcode == Opcode::NewObject && mayLieInOuterObject(sinking.program(), { allocation.layout(), fields[index] }, listed.layout()))
            return true;
    }
    return false;
}

FreshResultShape shapeOfReturns(const AllocationSinking& sinking, ObjectsThatBecomeOneCell objectsThatBecomeOneCell)
{
    std::span<const VirtualReturn> returns = sinking.virtualReturns();
    if (returns.empty())
        return { .state = FreshResultsState::NotFresh };
    if (hasReturnWithoutValue(sinking))
        return { .state = FreshResultsState::Unknown };

    FreshResultShape shape;
    for (const VirtualReturn& returned : returns)
        addVariantsOfReturn(sinking, returned, shape);

    if (!hasVariantOfKind(shape, VirtualShapeKind::Fields))
        return { .state = FreshResultsState::NotFresh };
    bool mayBecomeOneObject = objectsThatBecomeOneCell == ObjectsThatBecomeOneCell::StayWhole && sinking.context().build.isEnabled(PassID::FlattenInnerObjects, FunctionID()) && std::ranges::any_of(shape.variants, [&](const FreshResultVariant& variant) {
        return listsObjectThatMayLieInIt(sinking, variant);
    });
    if (mayBecomeOneObject)
        return { .state = FreshResultsState::NotFresh };
    if (shape.variants.size() > sinking.limit(LimitID::MaxVirtualObjectVariants)) {
        shape.state = FreshResultsState::Unknown;
        return shape;
    }

    assignParts(sinking, shape);
    // Nothing can be stored in such a field, so the allocation is never reached, and no function has a result of the type never beside others.
    if (shape.partTypes.contains(TypeTable::never))
        return { .state = FreshResultsState::Unknown };
    shape.state = FreshResultsState::Fresh;
    return shape;
}

static bool haveSameFields(std::span<const FreshResultField> a, std::span<const FreshResultField> b)
{
    return std::ranges::equal(a, b, { }, &FreshResultField::field, &FreshResultField::field);
}

// The place of an allocation is what reports and counts go by: two places that say the same otherwise build the same value.
static bool haveSamePayloadButForPlace(const Value& a, const Value& b)
{
    if (!Value::opcodesWithAllocationSite.contains(a.opcode) || a.allocationSite() == b.allocationSite())
        return a.hasSamePayloadAs(b);
    Value atSamePlace = b;
    atSamePlace.setAllocationSite(a.allocationSite());
    return atSamePlace.hasSamePayloadAs(a);
}

bool FreshResultVariant::buildsSameValueAs(const FreshResultVariant& other) const
{
    return kind == other.kind
        && constant == other.constant
        && description.kind == other.description.kind
        && prototype.opcode == other.prototype.opcode
        && haveSamePayloadButForPlace(prototype, other.prototype)
        && length == other.length
        && haveSameFields(properties.span(), other.properties.span())
        && haveSameFields(parts.span(), other.parts.span());
}

static ASCIILiteral sourceTextOf(VirtualConstant constant)
{
    switch (constant) {
    case VirtualConstant::Undefined:
        return "undefined"_s;
    case VirtualConstant::Null:
        return "null"_s;
    case VirtualConstant::False:
        return "false"_s;
    case VirtualConstant::True:
        return "true"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpType(PrintStream& out, TypeID type)
{
    using NamedType = std::pair<TypeID, ASCIILiteral>;
    static constexpr NamedType fixedTypes[] {
        { TypeTable::never, "never"_s },
        { TypeTable::undefined, "undefined"_s },
        { TypeTable::boolean, "boolean"_s },
        { TypeTable::int32, "int32"_s },
        { TypeTable::integer, "integer"_s },
        { TypeTable::number, "number"_s },
        { TypeTable::string, "string"_s },
        { TypeTable::function, "function"_s },
        { TypeTable::object, "object"_s },
        { TypeTable::anything, "anything"_s },
        { TypeTable::null, "null"_s },
        { TypeTable::nullish, "nullish"_s },
        { TypeTable::absent, "absent"_s },
        { TypeTable::guard, "guard"_s },
    };
    auto fixedType = std::ranges::find(fixedTypes, type, &NamedType::first);
    if (fixedType != std::end(fixedTypes)) {
        out.print(fixedType->second);
        return;
    }
    out.print("t", type.index());
}

static void dumpField(PrintStream& out, const FreshResultField& entry)
{
    if (entry.field.kind == VirtualFieldKind::Named)
        out.print("k", entry.field.key.index());
    else
        out.print(entry.field);
    out.print(":p", entry.part);
}

static void dumpVariant(PrintStream& out, const FreshResultVariant& variant)
{
    switch (variant.kind) {
    case VirtualShapeKind::Cell:
        out.print("cell");
        return;
    case VirtualShapeKind::Constant:
        out.print(sourceTextOf(variant.constant));
        return;
    case VirtualShapeKind::Fields:
        break;
    }

    if (variant.description.layout)
        out.print("L", variant.description.layout.index());
    else
        out.print(nameOf(variant.description.kind));
    if (variant.description.kind == VirtualObjectKind::Array)
        out.print("[", variant.length, "]");

    out.print("{");
    CommaPrinter space(" "_s);
    for (const FreshResultField& property : variant.properties) {
        out.print(space);
        dumpField(out, property);
    }
    for (const FreshResultField& part : variant.parts) {
        out.print(space);
        dumpField(out, part);
    }
    out.print("}");
}

void FreshResultShape::dump(PrintStream& out) const
{
    switch (state) {
    case FreshResultsState::Unknown:
        out.print("unknown");
        return;
    case FreshResultsState::NotFresh:
        out.print("not fresh");
        return;
    case FreshResultsState::Fresh:
        break;
    }

    out.print("fresh");
    if (hasTag())
        out.print(" tag");

    out.print(" variants=[");
    CommaPrinter variantComma;
    for (const FreshResultVariant& variant : variants) {
        out.print(variantComma);
        dumpVariant(out, variant);
    }

    out.print("] parts=[");
    CommaPrinter partComma;
    for (TypeID type : partTypes) {
        out.print(partComma);
        dumpType(out, type);
    }
    out.print("]");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
