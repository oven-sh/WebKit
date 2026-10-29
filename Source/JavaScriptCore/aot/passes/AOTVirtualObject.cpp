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
#include "AOTInlineBuiltinForms.h"
#include "AOTMemoryLocation.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccess.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "ArrayConventions.h"
#include <algorithm>
#include <wtf/EnumSet.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

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
    return !effects.mayRunOtherCode() && !effects.mayThrowByRule() && effects.writes.containsOnly(stringRangeHeaps) && effects.reads.containsOnly(stringRangeHeaps);
}

// A view over the memory of another. Its bounds are taken, and the other view keeps its length and its place in the buffer, so it does not matter when the view is made.
static bool onlyMakesView(const Effects& effects)
{
    constexpr AbstractHeapSet viewHeaps = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::TypedArrayStorage };
    return !effects.mayRunOtherCode() && !effects.mayThrowByRule() && effects.writes.containsOnly(viewHeaps) && effects.reads.containsOnly(viewHeaps);
}

static bool canBeDeferred(const EffectsOnEdges& effects, VirtualObjectKind kind)
{
    const Effects& normal = effects.normal;
    if (kind == VirtualObjectKind::StringRange)
        return onlyReadsAndCreatesStrings(normal);
    if (kind == VirtualObjectKind::ViewRange)
        return onlyMakesView(normal);
    return normal.isUnobservable() && normal.reads.containsOnly(bookkeepingHeaps);
}

static bool writesOnlyWhatSlotsAdmit(const Program& program, const Function& function, const Value& allocation, ListedValues listedValues)
{
    const Layout& layout = program.at(allocation.layout());
    std::span<const ValueID> values = allocation.variadicOperands();
    for (uint32_t index = 0; index < values.size(); ++index) {
        uint32_t field = allocation.fields()[index];
        const Type& listed = program.typeOf(function, values[index]);
        const Type& slotType = program.at(slotTypeOf(program, layout.fields[field]));
        if (listed.isSubsetOf(slotType, program))
            continue;
        if (listedValues == ListedValues::HaveTypesOfFields || !slotHoldsUndefinedAndNull(program, { allocation.layout(), field }) || !listed.withoutNullish().isSubsetOf(slotType, program))
            return false;
    }
    return true;
}

static DescribedObject describeObject(const Program& program, const Function& function, const Value& value, VirtualObjectDescription description, ListedValues listedValues)
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
        if (!writesOnlyWhatSlotsAdmit(program, function, value, listedValues))
            return unsupported(VirtualObjectSupport::Unknown);
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

    // The object copies the list when it is created, and the list may be the function's own rest parameter.
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
    TypeID contents = array.writtenContentsAt(program.builtins(), ContentsSlot::Element);
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

static DescribedObject describeAllocation(FunctionAnalyses& analyses, const Value& value, VirtualObjectDescription description, ListedValues listedValues)
{
    switch (value.opcode) {
    case Opcode::NewObject:
        return describeObject(analyses.program(), analyses.function(), value, description, listedValues);
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
    case Opcode::TypedArrayRange:
        description.kind = VirtualObjectKind::ViewRange;
        return supported(description);
    case Opcode::CallBuiltin:
        return describeBuiltinResult(analyses, value, description);
    case Opcode::NewRegExp:
    case Opcode::NewPromise:
    case Opcode::Data:
    case Opcode::NewGenerator:
        description.kind = VirtualObjectKind::Opaque;
        return supported(description);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

DescribedObject describeVirtualObject(FunctionAnalyses& analyses, ValueID allocation, ListedValues listedValues)
{
    const Function& function = analyses.function();
    const Value& value = function.at(allocation);
    if (interpretedOf(value.opcode) != Interpretation::Allocation)
        return unsupported(VirtualObjectSupport::NotAnAllocation);

    DescribedObject described = describeAllocation(analyses, value, { .allocation = allocation, .type = value.type }, listedValues);
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
    case VirtualObjectKind::ViewRange:
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
        return program.provenTypeOf(captures[field.index]);
    }
    case VirtualFieldKind::Length:
        // The type of every read of an array's length. Where the program has none, no read asks whether a carried length fits.
        return program.types.find(Type::integer(Range::uint32())).value_or(TypeTable::integer);
    case VirtualFieldKind::Capacity:
    case VirtualFieldKind::ArgumentCount:
        return TypeTable::integer;
    case VirtualFieldKind::RangeStart:
    case VirtualFieldKind::RangeEnd:
        // One type for the bounds of nearly every range, so that the variants of a result share their parts.
        return program.at(typeOfInitialValue(context, description, field)).isSubsetOf(program.at(TypeTable::length), program) ? TypeTable::length : TypeTable::integer;
    case VirtualFieldKind::Prototype:
    case VirtualFieldKind::RestList:
    case VirtualFieldKind::Callee:
    case VirtualFieldKind::StringBase:
    case VirtualFieldKind::Operand:
        return typeOfInitialValue(context, description, field);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool holdsUndefinedAndNullToo(const VirtualObjectContext& context, const VirtualObjectDescription& description, const VirtualField& field)
{
    if (description.kind != VirtualObjectKind::Object || field.kind != VirtualFieldKind::Named)
        return false;
    std::optional<uint32_t> layoutField = layoutFieldOf(context.program, description, field);
    return layoutField && slotHoldsUndefinedAndNull(context.program, { description.layout, *layoutField });
}

static bool namesPrivateMember(const Program& program, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
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
    return !record.key || !namesPrivateMember(program, record.key);
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

static constexpr EnumSet<VirtualObjectKind> layoutObjectKinds { VirtualObjectKind::Object, VirtualObjectKind::BuiltinFields };
static constexpr EnumSet<VirtualObjectKind> arrayKinds { VirtualObjectKind::Array, VirtualObjectKind::ArgumentList, VirtualObjectKind::BuiltinPrefix };
static constexpr EnumSet<VirtualObjectKind> kindsWithElements = arrayKinds | VirtualObjectKind::Arguments;
static constexpr EnumSet<VirtualObjectKind> kindsWithLength = kindsWithElements | VirtualObjectKind::StringRange;

static_assert(!OperandIndex::CallBuiltin::receiver, "A read names its object in operand 0.");

static constexpr ProofSet proofsOfLayoutObject {
    Proof::Shape,
    Proof::NoExtraProperties,
    Proof::NoFieldOutsideSlot,
    Proof::BirthFieldsFilled,
    Proof::NotFrozen,
    Proof::SlotFilled,
    Proof::OwnInstance,
};

static VirtualFieldAccess createFieldAccess(const VirtualField& field, PresentField whenPresent, AbsentField whenAbsent)
{
    return { FieldLookup::Field, whenAbsent, whenPresent, field, { } };
}

static VirtualFieldAccess createLengthAccess()
{
    return createFieldAccess(VirtualField::part(VirtualFieldKind::Length), PresentField::IsOwnData, AbsentField::Unknown);
}

static VirtualFieldAccess createAbsentAccess()
{
    return { FieldLookup::AbsentEverywhere, AbsentField::FindsNothing, PresentField::Unknown, { }, { } };
}

static VirtualFieldAccess createDynamicElementAccess(ValueID index)
{
    return { FieldLookup::DynamicElement, AbsentField::Unknown, PresentField::Unknown, { }, index };
}

static VirtualFieldAccess lookUpEnvironmentField(const VirtualObjectDescription& description, const Value& access)
{
    if (description.kind != VirtualObjectKind::Environment || access.layout() != description.layout)
        return { };
    bool fillsField = access.opcode == Opcode::PutEnvironmentField;
    return createFieldAccess(VirtualField::layoutField(description.layout, access.field()), PresentField::IsOwnData, fillsField ? AbsentField::FindsNothing : AbsentField::Unknown);
}

static VirtualFieldAccess lookUpSlot(const Program& program, const VirtualObjectDescription& description, Opcode operation, PropertyKeyID key)
{
    VirtualField field = VirtualField::named(key);
    if (!layoutObjectKinds.contains(description.kind) || !layoutFieldOf(program, description, field))
        return { };

    switch (operation) {
    case Opcode::GetSlot:
        return createFieldAccess(field, PresentField::IsOwnData, AbsentField::Unknown);
    case Opcode::IsSlotFilled:
        // Of the fields that a built-in delivers some stand for fields that the object would lack.
        if (description.kind == VirtualObjectKind::BuiltinFields && !description.hasKeyOrder)
            return { };
        [[fallthrough]];
    case Opcode::GetSlotOrUndefined:
        return createFieldAccess(field, PresentField::IsOwnData, AbsentField::FindsNothing);
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        if (description.kind != VirtualObjectKind::Object)
            return { };
        return createFieldAccess(field, PresentField::IsOwnData, AbsentField::FindsNothing);
    default:
        return { };
    }
}

// A deletion looks at nothing above the object. What lies in a slot is a data property, and it can be deleted unless the form of its field says otherwise.
static VirtualFieldAccess lookUpDeletedField(const Program& program, const VirtualObjectDescription& description, PropertyKeyID key)
{
    VirtualField field = VirtualField::named(key);
    std::optional<uint32_t> layoutField = description.kind == VirtualObjectKind::Object ? layoutFieldOf(program, description, field) : std::nullopt;
    if (!layoutField || !fieldAt(program, { description.layout, *layoutField }).form.isEmpty())
        return { };
    return createFieldAccess(field, PresentField::IsOwnData, AbsentField::FindsNothing);
}

static bool isClassEvaluatedOnce(const Program& program, LayoutID declaringLayout)
{
    return program.at(declaringLayout).classFlow.evaluations == ExecutionCount::AtMostOnce;
}

static VirtualFieldAccess lookUpPrivateField(const Program& program, const VirtualObjectDescription& description, const Value& access)
{
    if (description.kind != VirtualObjectKind::Object)
        return { };
    VirtualField field = fieldOfLayoutField(program, access.layout(), access.field());
    if (!layoutFieldOf(program, description, field))
        return access.opcode == Opcode::PrivateHas ? createAbsentAccess() : VirtualFieldAccess { };
    if (!isClassEvaluatedOnce(program, access.layout()))
        return { };

    switch (access.opcode) {
    case Opcode::PrivateGet:
    case Opcode::PrivateSet:
        return createFieldAccess(field, PresentField::IsOwnData, AbsentField::Throws);
    case Opcode::PrivateDefine:
        return createFieldAccess(field, PresentField::Throws, AbsentField::FindsNothing);
    case Opcode::PrivateHas:
        return createFieldAccess(field, PresentField::IsOwnData, AbsentField::FindsNothing);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static VirtualFieldAccess lookUpMappedArgument(const VirtualObjectDescription& description, const Value& access)
{
    if (description.kind != VirtualObjectKind::Arguments || !description.isMapped || access.index() >= description.numberOfParameters)
        return { };
    return createFieldAccess(VirtualField::element(access.index()), PresentField::IsOwnData, AbsentField::Unknown);
}

static VirtualFieldAccess lookUpCapture(const VirtualObjectDescription& description, const Value& access)
{
    if (description.kind != VirtualObjectKind::Closure || access.function() != description.function)
        return { };
    return createFieldAccess(VirtualField::capture(access.index()), PresentField::IsOwnData, AbsentField::Unknown);
}

static VirtualFieldAccess lookUpPropertyByName(const VirtualObjectContext& context, const VirtualObjectDescription& description, const Value& access)
{
    if (layoutObjectKinds.contains(description.kind))
        return lookUpNamedField(context, description, access);
    if (access.opcode != Opcode::PropGet)
        return { };

    PropertyKeyID key = access.key();
    if (kindsWithLength.contains(description.kind) && key == context.program.keyOf(BuiltinNames::length))
        return createLengthAccess();
    if (description.kind == VirtualObjectKind::Arguments && description.isMapped && key == context.program.keyOf(BuiltinNames::callee))
        return createFieldAccess(VirtualField::named(key), PresentField::Unknown, AbsentField::Unknown);
    if (prefixPropertyOf(context, description, key))
        return createFieldAccess(VirtualField::named(key), PresentField::IsOwnData, AbsentField::Unknown);
    return { };
}

static bool readsLength(std::optional<InlineBuiltin> implementation)
{
    return implementation == InlineBuiltin::ArrayLength || implementation == InlineBuiltin::StringLength;
}

Interpretation interpretationOf(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::CallBuiltin && readsLength(inlineImplementationOf(program, value)))
        return Interpretation::Read;
    // No candidate itself: like a call of a built-in, it may make its array out of an arguments object that is never built.
    if (value.opcode == Opcode::NewArgumentList)
        return Interpretation::Allocation;
    return interpretedOf(value.opcode);
}

static VirtualFieldAccess lookUpPartReadByBuiltin(const Program& program, const VirtualObjectDescription& description, const Value& call)
{
    std::optional<InlineBuiltin> implementation = inlineImplementationOf(program, call);
    if (implementation == InlineBuiltin::ArrayLength && arrayKinds.contains(description.kind))
        return createLengthAccess();
    if (implementation == InlineBuiltin::StringLength && description.kind == VirtualObjectKind::StringRange)
        return createLengthAccess();
    return { };
}

VirtualFieldAccess lookUpField(const VirtualObjectContext& context, const VirtualObjectDescription& description, const Value& access)
{
    switch (access.opcode) {
    case Opcode::GetEnvironmentField:
    case Opcode::PutEnvironmentField:
        return lookUpEnvironmentField(description, access);
    case Opcode::GetSlot:
    case Opcode::GetSlotOrUndefined:
    case Opcode::IsSlotFilled:
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return lookUpSlot(context.program, description, access.opcode, access.key());
    case Opcode::PrivateGet:
    case Opcode::PrivateSet:
    case Opcode::PrivateDefine:
    case Opcode::PrivateHas:
        return lookUpPrivateField(context.program, description, access);
    case Opcode::PropGet:
    case Opcode::PropSet:
    case Opcode::PropDefine:
    case Opcode::PropHas:
    case Opcode::PropHasOwn:
        return lookUpPropertyByName(context, description, access);
    case Opcode::PropDelete:
        return lookUpDeletedField(context.program, description, access.key());
    case Opcode::ElemGet:
    case Opcode::ElemSet:
    case Opcode::ElemDefine:
    case Opcode::ElemHas:
        return lookUpElement(context, description, access, access.propertyOperand());
    case Opcode::GetArrayElement:
    case Opcode::PutArrayElement:
    case Opcode::HasArrayElement:
    case Opcode::IsElementPresent:
        return lookUpElement(context, description, access, access.positionOperand());
    case Opcode::GetArrayLength:
        return arrayKinds.contains(description.kind) ? createLengthAccess() : VirtualFieldAccess { };
    case Opcode::GetStringLength:
        return description.kind == VirtualObjectKind::StringRange ? createLengthAccess() : VirtualFieldAccess { };
    case Opcode::GetStringCodeUnit:
        if (description.kind != VirtualObjectKind::StringRange)
            return { };
        return { FieldLookup::DynamicElement, AbsentField::Unknown, PresentField::IsOwnData, { }, access.positionOperand() };
    case Opcode::GetMappedArgument:
    case Opcode::SetMappedArgument:
        return lookUpMappedArgument(description, access);
    case Opcode::GetCapture:
        return lookUpCapture(description, access);
    case Opcode::CallBuiltin:
        return lookUpPartReadByBuiltin(context.program, description, access);
    case Opcode::AppendElement:
    case Opcode::AppendHole:
    default:
        return { };
    }
}

static bool definesPlainDataField(const Program& program, const VirtualObjectDescription& description, const Value& access)
{
    if (access.opcode != Opcode::PropDefine || keyLookupOf(program, access.key()) != KeyLookup::Ordinary)
        return false;
    std::optional<uint32_t> field = layoutFieldOf(program, description, VirtualField::named(access.key()));
    return field && !fieldAt(program, { description.layout, *field }).flags.containsAny({ FieldFlags::IsMethod, FieldFlags::IsHidden });
}

static bool hasLayoutPrototype(const VirtualObjectContext& context, const VirtualObjectDescription& description)
{
    bool hasOtherPrototype = false;
    forEachInitialField(context, description, [&](const VirtualField& field, ValueID) {
        hasOtherPrototype |= field.kind == VirtualFieldKind::Prototype;
    });
    return !hasOtherPrototype;
}

static bool canRelyOn(const VirtualObjectContext& context, const VirtualObjectDescription& description, ProofSet required)
{
    ProofSet proven = proofsOfLayoutObject;
    if (required.contains(Proof::PrototypeLinkUnchanged) && hasLayoutPrototype(context, description))
        proven.add(Proof::PrototypeLinkUnchanged);
    return (proven | required) == proven;
}

// What is planned for a filled slot says nothing of an empty one, so the access is planned once more as one that finds the slot empty.
static bool looksAtNothingAboveEmptySlot(const VirtualObjectContext& context, const VirtualObjectDescription& description, PropertyAccessQuery query)
{
    if ((query.access != Access::Store && query.access != Access::Read) || query.expectsEmptySlot)
        return false;
    query.expectsEmptySlot = true;
    PropertyAccessPlan plan = planPropertyAccess({ context.program, context.flow }, query);
    if (plan.form != AccessForm::Slot || !plan.conditions.isEmpty() || plan.requiredProofs.contains(Proof::SlotFilled) || plan.requiredProofs.contains(Proof::BirthFieldsFilled))
        return false;
    if (query.access == Access::Read && plan.operation != Opcode::GetSlotOrUndefined)
        return false;
    return canRelyOn(context, description, plan.requiredProofs);
}

VirtualFieldAccess lookUpNamedField(const VirtualObjectContext& context, const VirtualObjectDescription& description, const Value& access)
{
    const Program& program = context.program;
    if (!layoutObjectKinds.contains(description.kind))
        return { };
    if (definesPlainDataField(program, description, access))
        return lookUpSlot(program, description, Opcode::PutSlotOrAdd, access.key());

    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(program, context.function, access);
    if (!query)
        return { };
    LayoutID layouts[] { description.layout };
    query->start = MemberStart::Instance;
    query->receivers = ReceiverCandidates::underGuard(layouts);

    PropertyAccessPlan plan = planPropertyAccess({ program, context.flow }, *query);
    if (!plan.conditions.isEmpty() || !canRelyOn(context, description, plan.requiredProofs)) {
        // An access to a data property that the object has looks at nothing above the object. What the plan asks of the prototypes is for an empty slot.
        bool readsValueInSlot = query->access == Access::Read && (plan.operation == Opcode::GetSlot || plan.operation == Opcode::GetSlotOrUndefined);
        if (plan.form != AccessForm::Slot || (query->access != Access::Store && !readsValueInSlot))
            return { };
        VirtualFieldAccess found = lookUpSlot(program, description, readsValueInSlot ? Opcode::GetSlot : Opcode::PutSlot, query->key);
        found.whenAbsent = AbsentField::Unknown;
        return found;
    }

    switch (plan.form) {
    case AccessForm::Slot:
        break;
    case AccessForm::Constant:
        return plan.member.kind == MemberKind::Absent ? createAbsentAccess() : VirtualFieldAccess { };
    case AccessForm::ByName:
    case AccessForm::Variable:
    case AccessForm::Namespace:
    case AccessForm::Builtin:
    case AccessForm::Method:
    case AccessForm::Call:
    case AccessForm::GetterInSlot:
    case AccessForm::SetterInSlot:
    case AccessForm::Split:
        return { };
    }

    VirtualFieldAccess result = lookUpSlot(program, description, plan.operation, query->key);
    // A field of birth may have been deleted since: what is planned for a filled slot says nothing of an empty one.
    if (plan.requiredProofs.contains(Proof::SlotFilled) || plan.requiredProofs.contains(Proof::BirthFieldsFilled))
        result.whenAbsent = result.lookup == FieldLookup::Field && looksAtNothingAboveEmptySlot(context, description, *query) ? AbsentField::FindsNothing : AbsentField::Unknown;
    return result;
}

// The guard of a read of a present element has found the element, and the users of a read that takes a hole for NaN get the marker of Double storage. Neither read gives undefined where there is no element.
static bool readsAbsentElementAsUndefined(const Value& read)
{
    return read.opcode != Opcode::GetArrayElement || (!read.isPresent() && !read.readsHoleAsNaN());
}

VirtualFieldAccess lookUpElement(const VirtualObjectContext& context, const VirtualObjectDescription& description, const Value& access, ValueID position)
{
    if (!kindsWithElements.contains(description.kind))
        return { };

    bool isArray = description.kind == VirtualObjectKind::Array;
    Interpretation interpretation = interpretedOf(access.opcode);
    bool readsUndefinedWhenAbsent = interpretation == Interpretation::Read && readsAbsentElementAsUndefined(access);
    AbsentField whenAbsent = AbsentField::Unknown;
    switch (interpretation) {
    case Interpretation::Read:
        if (readsUndefinedWhenAbsent && description.kind != VirtualObjectKind::BuiltinPrefix)
            whenAbsent = AbsentField::FindsNothing;
        break;
    case Interpretation::Test:
        // The storage of a list that is never built is not observable. For an element that the built-in returns, the test goes the expected way: a read gives what was returned either way.
        if (description.kind == VirtualObjectKind::BuiltinPrefix && access.opcode == Opcode::IsElementPresent)
            break;
        if (!isArray)
            return { };
        whenAbsent = AbsentField::FindsNothing;
        break;
    case Interpretation::Store:
        if (isArray)
            whenAbsent = AbsentField::FindsNothing;
        break;
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::List:
    case Interpretation::Guards:
        return { };
    }

    const Program& program = context.program;
    if (std::optional<int64_t> index = integerConstantThroughCasts(program, context.function, position)) {
        // The other accesses go on to the property of that name. This test asks for an element, and none is at a position that is no index.
        if (*index < 0 || *index > static_cast<int64_t>(MAX_ARRAY_INDEX))
            return access.opcode == Opcode::IsElementPresent ? createAbsentAccess() : VirtualFieldAccess { };
        return createFieldAccess(VirtualField::element(static_cast<uint32_t>(*index)), PresentField::IsOwnData, whenAbsent);
    }
    // A read at an unknown number gives undefined for every number that is no position of an element.
    if (readsUndefinedWhenAbsent && program.typeOf(context.function, position).isNumber())
        return createDynamicElementAccess(position);
    return { };
}

static ValueID findPart(const AllocationRequest& request, VirtualFieldKind kind)
{
    for (const FieldValue& part : request.parts) {
        if (part.field.kind == kind)
            return part.value;
    }
    return { };
}

static ValueID requiredPart(const AllocationRequest& request, VirtualFieldKind kind)
{
    ValueID value = findPart(request, kind);
    RELEASE_ASSERT(value);
    return value;
}

static Vector<ValueID, 8> valuesByIndex(std::span<const FieldValue> fields, VirtualFieldKind kind, size_t count)
{
    Vector<ValueID, 8> values(count);
    for (const FieldValue& entry : fields) {
        if (entry.field.kind == kind)
            values[entry.field.index] = entry.value;
    }
    return values;
}

static uint32_t requiredLayoutField(const Program& program, const VirtualObjectDescription& description, const VirtualField& field)
{
    std::optional<uint32_t> layoutField = layoutFieldOf(program, description, field);
    RELEASE_ASSERT(layoutField);
    return *layoutField;
}

static void setObjectContents(const Program& program, const AllocationRequest& request, Value& object)
{
    RELEASE_ASSERT(object.opcode == Opcode::NewObject);
    Vector<uint32_t, 8> fields;
    Vector<ValueID, 8> values;
    for (const FieldValue& property : request.properties) {
        fields.append(requiredLayoutField(program, request.description, property.field));
        values.append(property.value);
    }
    object.setFields(fields.span());
    object.setValuesOperands(values.span());
    object.setPrototypeOperand(findPart(request, VirtualFieldKind::Prototype));
    object.setStorage(AllocationStorage::Heap);
}

static Value makeObject(const Program& program, const AllocationRequest& request)
{
    Value object = request.prototype;
    setObjectContents(program, request, object);
    return object;
}

static Value makeClosure(const AllocationRequest& request)
{
    Value closure = request.prototype;
    Vector<ValueID, 8> captures = valuesByIndex(request.parts, VirtualFieldKind::Capture, closure.capturesOperands().size());
    RELEASE_ASSERT(!captures.contains(ValueID()));
    closure.setCapturesOperands(captures.span());
    return closure;
}

static Value makeArray(const AllocationRequest& request)
{
    Vector<ValueID, 8> valueAtPosition = valuesByIndex(request.properties, VirtualFieldKind::Element, request.length);
    Vector<uint32_t, 8> holes;
    Vector<ValueID, 8> elements;
    for (uint32_t position = 0; position < request.length; ++position) {
        if (ValueID element = valueAtPosition[position])
            elements.append(element);
        else
            holes.append(position);
    }

    Value array = request.prototype;
    array.setHoles(holes.span());
    array.setElementsOperands(elements.span());
    array.setCapacityOperand(findPart(request, VirtualFieldKind::Capacity));
    return array;
}

static Value makeArguments(const AllocationRequest& request)
{
    Vector<ValueID, 8> parameters = valuesByIndex(request.properties, VirtualFieldKind::Element, request.description.numberOfParameters);
    RELEASE_ASSERT(!parameters.contains(ValueID()));

    Value arguments = request.prototype;
    arguments.setParametersOperands(parameters.span());
    arguments.setCountOperand(requiredPart(request, VirtualFieldKind::ArgumentCount));
    arguments.setRestOperand(requiredPart(request, VirtualFieldKind::RestList));
    if (request.description.isMapped)
        arguments.setCalleeOperand(requiredPart(request, VirtualFieldKind::Callee));
    return arguments;
}

static Value makeStringRange(const AllocationRequest& request)
{
    Value range = request.prototype;
    if (!request.mayBeAbsent)
        range.become(Opcode::Substring);
    range.setStringOperand(requiredPart(request, VirtualFieldKind::StringBase));
    range.setStartOperand(requiredPart(request, VirtualFieldKind::RangeStart));
    range.setEndOperand(requiredPart(request, VirtualFieldKind::RangeEnd));
    return range;
}

static Value makeCopyOfAllocation(const AllocationRequest& request)
{
    Value copy = request.prototype;
    for (const FieldValue& part : request.parts) {
        RELEASE_ASSERT(part.field.kind == VirtualFieldKind::Operand);
        copy.setOperand(part.field.index, part.value);
    }
    return copy;
}

Value makeAllocation(const Program& program, const AllocationRequest& request)
{
    switch (request.description.kind) {
    case VirtualObjectKind::Object:
    case VirtualObjectKind::Environment:
        return makeObject(program, request);
    case VirtualObjectKind::Closure:
        return makeClosure(request);
    case VirtualObjectKind::Array:
        return makeArray(request);
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::ViewRange:
        return makeCopyOfAllocation(request);
    case VirtualObjectKind::ArgumentList:
        return request.prototype;
    case VirtualObjectKind::Arguments:
        return makeArguments(request);
    case VirtualObjectKind::StringRange:
        return makeStringRange(request);
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canReadSlotWithoutGuard(const Program& program, const VirtualObjectDescription& description, uint32_t layoutField)
{
    bool typeProvesShape = program.at(description.type).singleLayout(program) == description.layout;
    return typeProvesShape && flowOf(program, { description.layout, layoutField }).occupancy == Occupancy::AlwaysFilled;
}

static void setSlotStamp(Value& slotOperation, PropertyKeyID key)
{
    slotOperation.setKey(key);
    slotOperation.setLevel(KnowledgeLevel::Proven);
}

Value makeFieldRead(const Program& program, const VirtualObjectDescription& description, const VirtualField& field, ValueID cell, ValueID guard)
{
    switch (description.kind) {
    case VirtualObjectKind::Environment: {
        Value read(Opcode::GetEnvironmentField);
        read.setEnvironmentOperand(cell);
        read.setLayout(description.layout);
        read.setField(requiredLayoutField(program, description, field));
        return read;
    }
    case VirtualObjectKind::Object: {
        RELEASE_ASSERT(field.kind == VirtualFieldKind::Named);
        Value read(Opcode::GetSlot);
        read.setObjectOperand(cell);
        // The guard of a cell that other code has seen may have ended, and an operation may only name a guard that is available.
        if (!canReadSlotWithoutGuard(program, description, requiredLayoutField(program, description, field)))
            read.setGuardOperand(guard);
        setSlotStamp(read, field.key);
        return read;
    }
    case VirtualObjectKind::Closure: {
        RELEASE_ASSERT(field.kind == VirtualFieldKind::Capture);
        Value read(Opcode::GetCapture);
        read.setClosureOperand(cell);
        read.setFunction(description.function);
        read.setIndex(field.index);
        return read;
    }
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::ViewRange:
    case VirtualObjectKind::Array:
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::Arguments:
    case VirtualObjectKind::StringRange:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Value makeFieldStore(const Program& program, const VirtualObjectDescription& description, const FieldValue& stored, ValueID cell, ValueID guard)
{
    switch (description.kind) {
    case VirtualObjectKind::Environment: {
        Value store(Opcode::PutEnvironmentField);
        store.setEnvironmentOperand(cell);
        store.setValueOperand(stored.value);
        store.setLayout(description.layout);
        store.setField(requiredLayoutField(program, description, stored.field));
        return store;
    }
    case VirtualObjectKind::Object: {
        RELEASE_ASSERT(stored.field.kind == VirtualFieldKind::Named);
        Value store(Opcode::PutSlotOrAdd);
        store.setObjectOperand(cell);
        store.setGuardOperand(guard);
        store.setValueOperand(stored.value);
        setSlotStamp(store, stored.field.key);
        return store;
    }
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::ViewRange:
    case VirtualObjectKind::Closure:
    case VirtualObjectKind::Array:
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::Arguments:
    case VirtualObjectKind::StringRange:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool canHoldUnprovenValue(const Program& program, const VirtualObjectDescription& description, const VirtualField& named)
{
    if (description.kind != VirtualObjectKind::Object || named.kind != VirtualFieldKind::Named)
        return false;
    std::optional<uint32_t> layoutField = layoutFieldOf(program, description, named);
    if (!layoutField)
        return false;
    const Field& field = program.at(description.layout).fields[*layoutField];
    return canBeStoredBehindAllocation(field) && tryCreateFiller(program, program.at(slotTypeOf(program, field)));
}

Value makeFiller(const Program& program, const VirtualObjectDescription& description, const VirtualField& unprovenNamed)
{
    RELEASE_ASSERT(unprovenNamed.kind == VirtualFieldKind::UnprovenNamed);
    const Field& field = program.at(description.layout).fields[requiredLayoutField(program, description, unprovenNamed)];
    std::optional<Value> filler = tryCreateFiller(program, program.at(slotTypeOf(program, field)));
    RELEASE_ASSERT(filler);
    return WTF::move(*filler);
}

Value makeStoreByName(const FieldValue& unprovenNamed, ValueID cell)
{
    RELEASE_ASSERT(unprovenNamed.field.kind == VirtualFieldKind::UnprovenNamed);
    Value store(Opcode::PropSet);
    store.setObjectOperand(cell);
    store.setValueOperand(unprovenNamed.value);
    store.setKey(unprovenNamed.field.key);
    store.setMode(ECMAMode::Strict);
    return store;
}

void setExactReceivers(Value& slotOperation, LayoutID layout, TypeArena& types)
{
    RELEASE_ASSERT(Value::opcodesWithReceivers.contains(slotOperation.opcode));
    slotOperation.setReceivers(types.completeLayoutSet(singleElementSpan(layout)));
}

static bool isListedByAllocation(const VirtualObjectContext& context, const VirtualObjectDescription& description, const VirtualField& field)
{
    bool isListed = false;
    forEachInitialField(context, description, [&](const VirtualField& initialField, ValueID) {
        isListed |= initialField == field;
    });
    return isListed;
}

SpeculativeLoad canLoadSpeculatively(const VirtualObjectContext& context, const VirtualObjectDescription& description, const VirtualField& field)
{
    const Program& program = context.program;
    bool isSafe = false;
    switch (description.kind) {
    case VirtualObjectKind::Environment: {
        // A variable has its type where the program reads it. Where nobody reads, the field may hold any word.
        std::optional<uint32_t> layoutField = layoutFieldOf(program, description, field);
        isSafe = layoutField && !holdsDeclaredTypeOnlyBehindEntry(flowOf(program, { description.layout, *layoutField }).location);
        break;
    }
    case VirtualObjectKind::Closure:
        isSafe = field.kind == VirtualFieldKind::Capture && field.index < program.at(description.function).captures.size();
        break;
    case VirtualObjectKind::Object: {
        std::optional<uint32_t> layoutField = field.kind == VirtualFieldKind::Named ? layoutFieldOf(program, description, field) : std::nullopt;
        // The read of a slot that defers its closure creates the closure and stores it.
        // A field that is always filled is filled once the object is finished. The load may stand earlier, so the allocation itself has to fill the field.
        isSafe = layoutField && canReadSlotWithoutGuard(program, description, *layoutField)
            && flowOf(program, { description.layout, *layoutField }).location.deferral == ClosureDeferral::Unknown
            && isListedByAllocation(context, description, field);
        break;
    }
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::ViewRange:
    case VirtualObjectKind::Array:
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::Arguments:
    case VirtualObjectKind::StringRange:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        break;
    }
    return isSafe ? SpeculativeLoad::Safe : SpeculativeLoad::Unknown;
}

LateStore canStoreLate(const VirtualObjectContext& context, const VirtualObjectDescription& description, const VirtualField& field)
{
    const Program& program = context.program;
    std::optional<uint32_t> layoutField = layoutFieldOf(program, description, field);
    if (!layoutField)
        return LateStore::Unknown;

    switch (description.kind) {
    case VirtualObjectKind::Environment:
        return LateStore::NeedsNoPlaceholder;
    case VirtualObjectKind::Object: {
        if (field.kind != VirtualFieldKind::Named)
            return LateStore::Unknown;
        const Type& fieldType = program.at(typeOfField(context, description, field));
        // A placeholder is a store too, so a field whose stores flow has counted gets none.
        bool hasCountedStores = flowOf(program, { description.layout, *layoutField }).location.stores != StoreCount::Unknown;
        if (!hasCountedStores && program.at(TypeTable::undefined).isSubsetOf(fieldType, program))
            return LateStore::NeedsUndefinedPlaceholder;
        // Flow proves a field filled from the start where every allocation lists it, and placement then gives its slot no absent state for a store to find.
        return isListedByAllocation(context, description, field) ? LateStore::Unknown : LateStore::CreatesProperty;
    }
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::ViewRange:
    case VirtualObjectKind::Closure:
    case VirtualObjectKind::Array:
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::Arguments:
    case VirtualObjectKind::StringRange:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        return LateStore::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
