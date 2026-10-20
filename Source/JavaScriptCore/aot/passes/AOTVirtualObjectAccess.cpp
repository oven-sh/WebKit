/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVirtualObject.h"

#if ENABLE(AOT)

#include "AOTInlineBuiltinForms.h"
#include "AOTPropertyAccess.h"
#include "AOTValueIdentity.h"
#include "ArrayConventions.h"
#include <wtf/EnumSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr EnumSet<VirtualObjectKind> layoutObjectKinds { VirtualObjectKind::Object, VirtualObjectKind::BuiltinFields };
static constexpr EnumSet<VirtualObjectKind> arrayKinds { VirtualObjectKind::Array, VirtualObjectKind::ArgumentList, VirtualObjectKind::BuiltinPrefix };
static constexpr EnumSet<VirtualObjectKind> kindsWithElements = arrayKinds | VirtualObjectKind::Arguments;
static constexpr EnumSet<VirtualObjectKind> kindsWithLength = kindsWithElements | VirtualObjectKind::StringRange;

static_assert(!OperandIndex::CallBuiltin::receiver, "A read names its object in operand 0.");

static constexpr ProofSet proofsOfLayoutObject {
    Proof::Shape,
    Proof::NoExtraProperties,
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
    case Opcode::GetSlotOrUndefined:
    case Opcode::IsSlotFilled:
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
    case Opcode::ElemGet:
    case Opcode::ElemSet:
    case Opcode::ElemDefine:
    case Opcode::ElemHas:
        return lookUpElement(context, description, access, access.propertyOperand());
    case Opcode::GetArrayElement:
    case Opcode::PutArrayElement:
    case Opcode::HasArrayElement:
        return lookUpElement(context, description, access, access.positionOperand());
    case Opcode::GetArrayLength:
        return arrayKinds.contains(description.kind) ? createLengthAccess() : VirtualFieldAccess { };
    case Opcode::GetStringLength:
        return description.kind == VirtualObjectKind::StringRange ? createLengthAccess() : VirtualFieldAccess { };
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
        // A store to a data property that the object has looks at nothing above the object. What the plan asks of the prototypes is for the store that creates the field.
        if (plan.form != AccessForm::Slot || query->access != Access::Store)
            return { };
        VirtualFieldAccess stored = lookUpSlot(program, description, Opcode::PutSlot, query->key);
        stored.whenAbsent = AbsentField::Unknown;
        return stored;
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
    case AccessForm::Split:
        return { };
    }

    VirtualFieldAccess result = lookUpSlot(program, description, plan.operation, query->key);
    if (plan.requiredProofs.contains(Proof::SlotFilled))
        result.whenAbsent = AbsentField::Unknown;
    return result;
}

VirtualFieldAccess lookUpElement(const VirtualObjectContext& context, const VirtualObjectDescription& description, const Value& access, ValueID position)
{
    if (!kindsWithElements.contains(description.kind))
        return { };

    bool isArray = description.kind == VirtualObjectKind::Array;
    Interpretation interpretation = interpretedOf(access.opcode);
    AbsentField whenAbsent = AbsentField::Unknown;
    switch (interpretation) {
    case Interpretation::Read:
        if (description.kind != VirtualObjectKind::BuiltinPrefix)
            whenAbsent = AbsentField::FindsNothing;
        break;
    case Interpretation::Test:
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
        if (*index < 0 || *index > static_cast<int64_t>(MAX_ARRAY_INDEX))
            return { };
        return createFieldAccess(VirtualField::element(static_cast<uint32_t>(*index)), PresentField::IsOwnData, whenAbsent);
    }
    if (interpretation == Interpretation::Read && program.typeOf(context.function, position).isNumber())
        return createDynamicElementAccess(position);
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
