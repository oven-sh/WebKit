/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVirtualObject.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet stringRangeHeaps = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage };

static DescribedObject unsupported(VirtualObjectSupport support, LimitID limit = LimitID::None)
{
    RELEASE_ASSERT(support != VirtualObjectSupport::Supported);
    return { support, limit, { } };
}

static DescribedObject supported(const VirtualObjectDescription& description)
{
    return { VirtualObjectSupport::Supported, LimitID::None, description };
}

static bool onlyReadsAndCreatesStrings(const Effects& effects)
{
    return !effects.mayRunOtherCode() && effects.writes.containsOnly(stringRangeHeaps) && effects.reads.containsOnly(stringRangeHeaps);
}

static bool canBeDeferred(const EffectsOnEdges& effects, VirtualObjectKind kind)
{
    const Effects& normal = effects.normal;
    if (kind == VirtualObjectKind::StringRange)
        return onlyReadsAndCreatesStrings(normal);
    return normal.isUnobservable() && normal.reads.containsOnly(bookkeepingHeaps);
}

static bool writesOnlyWhatSlotTypesAdmit(const Program& program, const Function& function, const Value& allocation)
{
    const Layout& layout = program.at(allocation.layout());
    std::span<const ValueID> values = allocation.variadicOperands();
    for (uint32_t index = 0; index < values.size(); ++index) {
        if (!program.typeOf(function, values[index]).isSubsetOf(program.at(slotTypeOf(program, layout.fields[allocation.fields()[index]])), program))
            return false;
    }
    return true;
}

static DescribedObject describeObject(const Program& program, const Function& function, const Value& value, VirtualObjectDescription description)
{
    if (value.storage() != AllocationStorage::Heap)
        return unsupported(VirtualObjectSupport::HasChosenStorage);
    if (!allocatesOwnLayout(program, value)) {
        description.kind = VirtualObjectKind::Opaque;
        return supported(description);
    }

    description.layout = value.layout();
    const Layout& layout = program.at(description.layout);
    switch (layout.kind) {
    case LayoutKind::Environment:
    case LayoutKind::CoroutineFrame:
        description.kind = VirtualObjectKind::Environment;
        return supported(description);
    case LayoutKind::BuiltinResult:
        if (program.builtins().at(layout.builtinLayout).isTuple)
            return unsupported(VirtualObjectSupport::Unknown);
        [[fallthrough]];
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
        if (!writesOnlyWhatSlotTypesAdmit(program, function, value))
            return unsupported(VirtualObjectSupport::Unknown);
        // What stands in the list for an object whose fields lie in this one is no value, so the list is not taken apart.
        if (!layout.flattenedObjects.isEmpty()) {
            description.kind = VirtualObjectKind::Opaque;
            return supported(description);
        }
        description.kind = VirtualObjectKind::Object;
        description.hasKeyOrder = true;
        return supported(description);
    case LayoutKind::Closure:
    case LayoutKind::ModuleNamespace:
        return unsupported(VirtualObjectSupport::Unknown);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static DescribedObject describeClosure(const Value& value, VirtualObjectDescription description)
{
    if (value.storage() != AllocationStorage::Heap)
        return unsupported(VirtualObjectSupport::HasChosenStorage);

    description.kind = VirtualObjectKind::Closure;
    description.function = value.function();
    return supported(description);
}

static DescribedObject describeArray(FunctionAnalyses& analyses, const Value& value, VirtualObjectDescription description)
{
    std::span<const ValueID> elements = value.elementsOperands();
    if (elements.size() + value.holes().size() > analyses.context().build.limit(LimitID::MaxVirtualArrayLength))
        return unsupported(VirtualObjectSupport::ExceedsLimit, LimitID::MaxVirtualArrayLength);

    const Program& program = analyses.program();
    const Type& contents = program.at(value.contents());
    bool elementsFitContents = std::ranges::all_of(elements, [&](ValueID element) {
        return program.typeOf(analyses.function(), element).isSubsetOf(contents, program);
    });
    description.kind = VirtualObjectKind::Array;
    description.elementType = elementsFitContents ? value.contents() : TypeTable::anything;
    return supported(description);
}

static bool staysAsCreated(const EscapeAnalysis& escape, ValueID list)
{
    constexpr OperandUses usesOfContents { OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::RetainsContents };

    std::optional<ObjectID> object = escape.singleObject(list);
    if (!object)
        return false;
    const TrackedObject& tracked = escape.at(*object);
    return tracked.state == EscapeState::DoesNotEscape && tracked.uses.containsOnly(usesOfContents);
}

static DescribedObject describeArguments(FunctionAnalyses& analyses, const Value& value, VirtualObjectDescription description)
{
    description.kind = VirtualObjectKind::Arguments;
    description.isMapped = value.opcode == Opcode::CreateMappedArguments;
    description.numberOfParameters = value.numberOfVariadicOperands();
    if (description.numberOfParameters > analyses.context().build.limit(LimitID::MaxVirtualArrayLength))
        return unsupported(VirtualObjectSupport::ExceedsLimit, LimitID::MaxVirtualArrayLength);

    // The object copies the list when it is created, and the list may be the function's own rest parameter. Of a list that may have changed since,
    // nothing is read later: the count and the parameters are all that is known of the object, and it is built where it is created or not at all.
    description.restListStaysAsCreated = staysAsCreated(analyses.escape(), value.restOperand());
    description.canBeBuilt = description.restListStaysAsCreated;
    return supported(description);
}

static LayoutID resultLayoutOf(const Program& program, const BuiltinOverload& overload)
{
    const BuiltinTable& table = program.builtins();
    std::span<const BuiltinTypePart> parts = table.at(overload.result.type).parts;
    if (parts.size() != 1 || parts[0].kind != BuiltinTypePartKind::Layout || table.at(parts[0].layout).isTuple)
        return { };
    return program.layoutOf(parts[0].layout);
}

static bool hasOnlyRequiredFields(const Program& program, LayoutID layout)
{
    BuiltinLayoutID builtinLayout = program.at(layout).builtinLayout;
    return builtinLayout && std::ranges::none_of(program.builtins().at(builtinLayout).fields, &BuiltinLayoutField::isOptional);
}

static bool isOptionalFieldOfBuiltinLayout(const Program& program, LayoutID layout, uint32_t field)
{
    const auto& fields = program.builtins().at(program.at(layout).builtinLayout).fields;
    RELEASE_ASSERT(fields.size() == program.at(layout).fields.size());
    return fields[field].isOptional;
}

static TypeID prefixElementTypeOf(const Program& program, const ClassPart& array)
{
    TypeID contents = array.contentsAt(program.builtins(), ContentsSlot::Element);
    const Type& element = program.at(contents);
    if (element.primitives & Primitive::Undefined)
        return contents;
    return program.types.find(element.withPrimitives(Primitive::Undefined)).value_or(TypeTable::anything);
}

static DescribedObject describeBuiltinResult(FunctionAnalyses& analyses, const Value& call, VirtualObjectDescription description)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    if (call.builtinCallForm() != BuiltinCallForm::Whole)
        return unsupported(VirtualObjectSupport::NotFresh);

    const BuiltinOverload& overload = program.builtins().at(call.overload());
    bool isNeverBuilt = !overload.code.fieldsForm && overload.code.prefixForm;
    switch (freshnessOf(analyses.effectContext(), function, call)) {
    case ResultFreshness::Unknown:
        return unsupported(VirtualObjectSupport::NotFresh);
    case ResultFreshness::Unaliased:
        if (!isNeverBuilt)
            return unsupported(VirtualObjectSupport::NotFresh);
        break;
    case ResultFreshness::Fresh:
        break;
    }

    const Type& type = program.at(description.type);
    bool mayBeNull = type.primitives & Primitive::Null;
    bool mayBeUndefined = type.primitives & Primitive::Undefined;
    if (mayBeNull && mayBeUndefined)
        return unsupported(VirtualObjectSupport::HasNoPartsForm);
    if (mayBeNull)
        description.absentResult = VirtualConstant::Null;
    if (mayBeUndefined)
        description.absentResult = VirtualConstant::Undefined;
    description.staysInPlace = true;
    description.isDead = analyses.useLists().usesOf(description.allocation).empty() && onlyReadsAndCreatesStrings(analyses.effectsOf(description.allocation).normal);

    if (overload.code.fieldsForm) {
        description.kind = VirtualObjectKind::BuiltinFields;
        // Flow counts the result as an ordinary object that the built-in makes, so what it says about the layout does not cover an allocation here.
        description.canBeBuilt = false;
        description.layout = resultLayoutOf(program, overload);
        if (!description.layout)
            return unsupported(VirtualObjectSupport::HasNoPartsForm);
        // The call delivers `undefined` for a field that the object would lack, which is what a read of it finds: nothing above the object has one of the names. Which fields there are, and in which order, nobody can tell.
        description.hasKeyOrder = hasOnlyRequiredFields(program, description.layout);
        return supported(description);
    }
    if (!overload.code.prefixForm)
        return unsupported(VirtualObjectSupport::HasNoPartsForm);

    const ClassPart* array = type.findClassPart(BuiltinClasses::Array);
    if (!array)
        return unsupported(VirtualObjectSupport::HasNoPartsForm);
    description.kind = VirtualObjectKind::BuiltinPrefix;
    description.canBeBuilt = false;
    description.elementType = prefixElementTypeOf(program, *array);
    return supported(description);
}

static DescribedObject describeAllocation(FunctionAnalyses& analyses, const Value& value, VirtualObjectDescription description)
{
    switch (value.opcode) {
    case Opcode::NewObject:
        return describeObject(analyses.program(), analyses.function(), value, description);
    case Opcode::NewClosure:
        return describeClosure(value, description);
    case Opcode::NewArray:
        return describeArray(analyses, value, description);
    case Opcode::CreateRest:
        description.kind = VirtualObjectKind::ArgumentList;
        description.firstArgument = value.firstArgument();
        description.elementType = TypeTable::anything;
        return supported(description);
    case Opcode::CreateUnmappedArguments:
    case Opcode::CreateMappedArguments:
        return describeArguments(analyses, value, description);
    case Opcode::Substring:
        description.kind = VirtualObjectKind::StringRange;
        description.hasIdentity = false;
        return supported(description);
    case Opcode::SubstringOrUndefined:
        description.kind = VirtualObjectKind::StringRange;
        description.hasIdentity = false;
        description.absentResult = VirtualConstant::Undefined;
        return supported(description);
    case Opcode::CallBuiltin:
        return describeBuiltinResult(analyses, value, description);
    case Opcode::NewRegExp:
    case Opcode::NewPromise:
    case Opcode::Data:
    case Opcode::NewGenerator:
    case Opcode::CloneAs:
        description.kind = VirtualObjectKind::Opaque;
        return supported(description);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

DescribedObject describeVirtualObject(FunctionAnalyses& analyses, ValueID allocation)
{
    const Function& function = analyses.function();
    const Value& value = function.at(allocation);
    if (interpretedOf(value.opcode) != Interpretation::Allocation)
        return unsupported(VirtualObjectSupport::NotAnAllocation);

    DescribedObject described = describeAllocation(analyses, value, { .allocation = allocation, .type = value.type });
    if (described.support != VirtualObjectSupport::Supported || described.description.staysInPlace)
        return described;

    if (!canBeDeferred(analyses.effectsOf(allocation), described.description.kind))
        return unsupported(VirtualObjectSupport::HasEffectsAtAllocation);
    if (described.description.hasIdentity && freshnessOf(analyses.effectContext(), function, value) != ResultFreshness::Fresh)
        return unsupported(VirtualObjectSupport::NotFresh);
    return described;
}

static void forEachObjectField(const Program& program, const VirtualObjectDescription& description, const Value& object, const InitialFieldFunctor& functor)
{
    std::span<const uint32_t> fields = object.fields();
    std::span<const ValueID> values = object.valuesOperands();
    RELEASE_ASSERT(fields.size() == values.size());
    for (size_t index = 0; index < fields.size(); ++index)
        functor(fieldOfLayoutField(program, description.layout, fields[index]), values[index]);
    if (ValueID prototype = object.prototypeOperand())
        functor(VirtualField::part(VirtualFieldKind::Prototype), prototype);
}

static void forEachArrayField(const Value& array, const InitialFieldFunctor& functor)
{
    std::span<const uint32_t> holes = array.holes();
    std::span<const ValueID> elements = array.elementsOperands();
    size_t nextHole = 0;
    size_t nextElement = 0;
    for (uint32_t position = 0; nextElement < elements.size(); ++position) {
        if (nextHole < holes.size() && holes[nextHole] == position) {
            ++nextHole;
            continue;
        }
        functor(VirtualField::element(position), elements[nextElement++]);
    }
    if (ValueID capacity = array.capacityOperand())
        functor(VirtualField::part(VirtualFieldKind::Capacity), capacity);
}

static void forEachArgumentsField(const VirtualObjectDescription& description, const Value& arguments, const InitialFieldFunctor& functor)
{
    std::span<const ValueID> parameters = arguments.parametersOperands();
    for (uint32_t index = 0; index < parameters.size(); ++index)
        functor(VirtualField::element(index), parameters[index]);
    functor(VirtualField::part(VirtualFieldKind::ArgumentCount), arguments.countOperand());
    functor(VirtualField::part(VirtualFieldKind::RestList), arguments.restOperand());
    if (description.isMapped)
        functor(VirtualField::part(VirtualFieldKind::Callee), arguments.calleeOperand());
}

void forEachInitialField(const VirtualObjectContext& context, const VirtualObjectDescription& description, NOESCAPE const InitialFieldFunctor& functor)
{
    const Value& value = context.function.at(description.allocation);
    switch (description.kind) {
    case VirtualObjectKind::Object:
    case VirtualObjectKind::Environment:
        forEachObjectField(context.program, description, value, functor);
        return;
    case VirtualObjectKind::Closure: {
        std::span<const ValueID> captures = value.capturesOperands();
        for (uint32_t index = 0; index < captures.size(); ++index)
            functor(VirtualField::capture(index), captures[index]);
        return;
    }
    case VirtualObjectKind::Array:
        forEachArrayField(value, functor);
        return;
    case VirtualObjectKind::Arguments:
        forEachArgumentsField(description, value, functor);
        return;
    case VirtualObjectKind::StringRange:
        functor(VirtualField::part(VirtualFieldKind::StringBase), value.stringOperand());
        functor(VirtualField::part(VirtualFieldKind::RangeStart), value.startOperand());
        functor(VirtualField::part(VirtualFieldKind::RangeEnd), value.endOperand());
        return;
    case VirtualObjectKind::Opaque:
        // An opaque object is built as a copy of its allocation, with what the operands were at the allocation.
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            functor(VirtualField::operand(operandIndex), operand);
        });
        return;
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t initialLengthOf(const Function& function, const VirtualObjectDescription& description)
{
    if (description.kind != VirtualObjectKind::Array)
        return 0;
    const Value& array = function.at(description.allocation);
    return array.numberOfVariadicOperands() + static_cast<uint32_t>(array.holes().size());
}

VirtualField fieldOfLayoutField(const Program& program, LayoutID layout, uint32_t field)
{
    const Layout& record = program.at(layout);
    if (record.isEnvironment() || !record.fields[field].key)
        return VirtualField::layoutField(layout, field);
    return VirtualField::named(record.fields[field].key);
}

std::optional<uint32_t> layoutFieldOf(const Program& program, const VirtualObjectDescription& description, const VirtualField& field)
{
    if (!description.layout)
        return std::nullopt;

    switch (field.kind) {
    case VirtualFieldKind::Named:
    case VirtualFieldKind::UnprovenNamed:
        return program.at(description.layout).findField(field.key);
    case VirtualFieldKind::LayoutField:
        if (field.layout != description.layout || field.index >= program.at(description.layout).fields.size())
            return std::nullopt;
        return field.index;
    case VirtualFieldKind::Element:
    case VirtualFieldKind::Capture:
    case VirtualFieldKind::Length:
    case VirtualFieldKind::Capacity:
    case VirtualFieldKind::Prototype:
    case VirtualFieldKind::Callee:
    case VirtualFieldKind::ArgumentCount:
    case VirtualFieldKind::RestList:
    case VirtualFieldKind::StringBase:
    case VirtualFieldKind::RangeStart:
    case VirtualFieldKind::RangeEnd:
    case VirtualFieldKind::Operand:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint32_t> prefixPropertyOf(const VirtualObjectContext& context, const VirtualObjectDescription& description, PropertyKeyID key)
{
    if (description.kind != VirtualObjectKind::BuiltinPrefix)
        return std::nullopt;

    const Program& program = context.program;
    std::span<const BuiltinResultProperty> properties = program.builtins().at(context.function.at(description.allocation).overload()).code.prefixProperties;
    for (uint32_t index = 0; index < properties.size(); ++index) {
        if (program.keyOf(properties[index].name) == key)
            return index;
    }
    return std::nullopt;
}

static TypeID typeOfPrefixProperty(const VirtualObjectContext& context, const VirtualObjectDescription& description, uint32_t property)
{
    const Program& program = context.program;
    ScratchTypeArena scratch(program.types);
    TypingContext typing { program, context.function, scratch.arena() };
    BuiltinTypeID type = program.builtins().at(context.function.at(description.allocation).overload()).code.prefixProperties[property].type;
    return program.types.find(typeOfBuiltinType({ typing }, type)).value_or(TypeTable::anything);
}

static TypeID typeOfInitialValue(const VirtualObjectContext& context, const VirtualObjectDescription& description, const VirtualField& field)
{
    TypeID type = TypeTable::never;
    forEachInitialField(context, description, [&](const VirtualField& initialField, ValueID value) {
        if (initialField == field)
            type = context.function.at(value).type;
    });
    return type;
}

TypeID typeOfField(const VirtualObjectContext& context, const VirtualObjectDescription& description, const VirtualField& field)
{
    const Program& program = context.program;
    switch (field.kind) {
    case VirtualFieldKind::UnprovenNamed:
        return TypeTable::anything;
    case VirtualFieldKind::Named:
    case VirtualFieldKind::LayoutField: {
        if (std::optional<uint32_t> property = prefixPropertyOf(context, description, field.key))
            return typeOfPrefixProperty(context, description, *property);
        std::optional<uint32_t> layoutField = layoutFieldOf(program, description, field);
        if (!layoutField)
            return TypeTable::never;
        TypeID slotType = slotTypeOf(program, program.at(description.layout).fields[*layoutField]);
        if (description.kind != VirtualObjectKind::BuiltinFields || !isOptionalFieldOfBuiltinLayout(program, description.layout, *layoutField) || program.at(slotType).primitives & Primitive::Undefined)
            return slotType;
        // What the call delivers for a field that the object would lack.
        return program.types.find(program.at(slotType).withPrimitives(Primitive::Undefined)).value_or(TypeTable::anything);
    }
    case VirtualFieldKind::Element:
        if (description.kind == VirtualObjectKind::Arguments)
            return TypeTable::anything;
        return description.elementType ? description.elementType : TypeTable::never;
    case VirtualFieldKind::Capture: {
        if (!description.function)
            return TypeTable::never;
        const Vector<DeclaredType>& captures = program.at(description.function).captures;
        if (field.index >= captures.size())
            return TypeTable::never;
        const DeclaredType& capture = captures[field.index];
        return capture.flags.contains(PositionFlags::IsExpected) ? TypeTable::anything : capture.type;
    }
    case VirtualFieldKind::Length:
        // The type of every read of an array's length. Where the program has none, no read asks whether a carried length fits.
        return program.types.find(Type::integer(Range::uint32())).value_or(TypeTable::integer);
    case VirtualFieldKind::Capacity:
    case VirtualFieldKind::ArgumentCount:
    case VirtualFieldKind::RangeStart:
    case VirtualFieldKind::RangeEnd:
        return TypeTable::integer;
    case VirtualFieldKind::Prototype:
    case VirtualFieldKind::RestList:
    case VirtualFieldKind::Callee:
    case VirtualFieldKind::StringBase:
    case VirtualFieldKind::Operand:
        return typeOfInitialValue(context, description, field);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isPrivateName(const Program& program, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::EnginePrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isEnumerable(const Program& program, const VirtualObjectDescription& description, const VirtualField& field)
{
    if (field.kind == VirtualFieldKind::Element)
        return true;

    std::optional<uint32_t> layoutField = layoutFieldOf(program, description, field);
    if (!layoutField)
        return false;
    const Field& record = program.at(description.layout).fields[*layoutField];
    if (record.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden }))
        return false;
    return !record.key || !isPrivateName(program, record.key);
}

std::optional<VirtualConstant> virtualConstantOf(const Program& program, const Function& function, ValueID id)
{
    std::optional<ValueID> constant = constantThroughCasts(program, function, id);
    if (!constant)
        return std::nullopt;

    const Value& value = function.at(*constant);
    switch (value.opcode) {
    case Opcode::Undefined:
        return VirtualConstant::Undefined;
    case Opcode::Null:
        return VirtualConstant::Null;
    case Opcode::Boolean:
        return value.boolean() ? VirtualConstant::True : VirtualConstant::False;
    default:
        return std::nullopt;
    }
}

TypeID typeOf(VirtualConstant constant)
{
    switch (constant) {
    case VirtualConstant::Undefined:
        return TypeTable::undefined;
    case VirtualConstant::Null:
        return TypeTable::null;
    case VirtualConstant::False:
    case VirtualConstant::True:
        return TypeTable::boolean;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Value makeConstant(VirtualConstant constant)
{
    switch (constant) {
    case VirtualConstant::Undefined:
        return Value(Opcode::Undefined);
    case VirtualConstant::Null:
        return Value(Opcode::Null);
    case VirtualConstant::False:
    case VirtualConstant::True: {
        Value value(Opcode::Boolean);
        value.setBoolean(constant == VirtualConstant::True);
        return value;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(VirtualObjectKind kind)
{
    switch (kind) {
    case VirtualObjectKind::Opaque:
        return "Opaque"_s;
    case VirtualObjectKind::Object:
        return "Object"_s;
    case VirtualObjectKind::Environment:
        return "Environment"_s;
    case VirtualObjectKind::Closure:
        return "Closure"_s;
    case VirtualObjectKind::Array:
        return "Array"_s;
    case VirtualObjectKind::ArgumentList:
        return "ArgumentList"_s;
    case VirtualObjectKind::Arguments:
        return "Arguments"_s;
    case VirtualObjectKind::StringRange:
        return "StringRange"_s;
    case VirtualObjectKind::BuiltinFields:
        return "BuiltinFields"_s;
    case VirtualObjectKind::BuiltinPrefix:
        return "BuiltinPrefix"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(VirtualFieldKind kind)
{
    switch (kind) {
    case VirtualFieldKind::Named:
        return "Named"_s;
    case VirtualFieldKind::UnprovenNamed:
        return "UnprovenNamed"_s;
    case VirtualFieldKind::LayoutField:
        return "LayoutField"_s;
    case VirtualFieldKind::Element:
        return "Element"_s;
    case VirtualFieldKind::Capture:
        return "Capture"_s;
    case VirtualFieldKind::Length:
        return "Length"_s;
    case VirtualFieldKind::Capacity:
        return "Capacity"_s;
    case VirtualFieldKind::Prototype:
        return "Prototype"_s;
    case VirtualFieldKind::Callee:
        return "Callee"_s;
    case VirtualFieldKind::ArgumentCount:
        return "ArgumentCount"_s;
    case VirtualFieldKind::RestList:
        return "RestList"_s;
    case VirtualFieldKind::StringBase:
        return "StringBase"_s;
    case VirtualFieldKind::RangeStart:
        return "RangeStart"_s;
    case VirtualFieldKind::RangeEnd:
        return "RangeEnd"_s;
    case VirtualFieldKind::Operand:
        return "Operand"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(VirtualObjectSupport support)
{
    switch (support) {
    case VirtualObjectSupport::Unknown:
        return "Unknown"_s;
    case VirtualObjectSupport::NotAnAllocation:
        return "NotAnAllocation"_s;
    case VirtualObjectSupport::NotFresh:
        return "NotFresh"_s;
    case VirtualObjectSupport::HasEffectsAtAllocation:
        return "HasEffectsAtAllocation"_s;
    case VirtualObjectSupport::HasChosenStorage:
        return "HasChosenStorage"_s;
    case VirtualObjectSupport::HasNoPartsForm:
        return "HasNoPartsForm"_s;
    case VirtualObjectSupport::ExceedsLimit:
        return "ExceedsLimit"_s;
    case VirtualObjectSupport::Supported:
        return "Supported"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(VirtualConstant constant)
{
    switch (constant) {
    case VirtualConstant::Undefined:
        return "Undefined"_s;
    case VirtualConstant::Null:
        return "Null"_s;
    case VirtualConstant::False:
        return "False"_s;
    case VirtualConstant::True:
        return "True"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void VirtualField::dump(PrintStream& out) const
{
    switch (kind) {
    case VirtualFieldKind::Named:
        out.print("named(k", key.index(), ")");
        return;
    case VirtualFieldKind::UnprovenNamed:
        out.print("unproven(k", key.index(), ")");
        return;
    case VirtualFieldKind::LayoutField:
        out.print("field(L", layout.index(), ".", index, ")");
        return;
    case VirtualFieldKind::Element:
        out.print("element(", index, ")");
        return;
    case VirtualFieldKind::Capture:
        out.print("capture(", index, ")");
        return;
    case VirtualFieldKind::Operand:
        out.print("operand(", index, ")");
        return;
    case VirtualFieldKind::Length:
    case VirtualFieldKind::Capacity:
    case VirtualFieldKind::Prototype:
    case VirtualFieldKind::Callee:
    case VirtualFieldKind::ArgumentCount:
    case VirtualFieldKind::RestList:
    case VirtualFieldKind::StringBase:
    case VirtualFieldKind::RangeStart:
    case VirtualFieldKind::RangeEnd:
        out.print(nameOf(kind));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
