/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTResultRules.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

struct DeliveredField {
    uint32_t resultIndex { 0 };
    uint32_t field { 0 };
    bool isDeliveredAsBounds { false };
};

static uint64_t numberOfElementsInUse(const VirtualFamily& family)
{
    uint64_t numberOfElements = 0;
    for (uint32_t field = 0; field < family.numberOfFields(); ++field) {
        const VirtualField& virtualField = family.fieldAt(field);
        if (virtualField.kind == VirtualFieldKind::Element)
            numberOfElements = std::max(numberOfElements, static_cast<uint64_t>(virtualField.index) + 1);
    }
    return numberOfElements;
}

static const VirtualShape* findDeliveredShape(const VirtualFamily& family, uint32_t descriptionIndex)
{
    for (uint32_t index = 0; index < family.numberOfShapes(); ++index) {
        const VirtualShape& shape = family.at(VirtualShapeID(index));
        if (shape.kind == VirtualShapeKind::Fields && shape.descriptionIndex == descriptionIndex)
            return &shape;
    }
    return nullptr;
}

static bool deliversEveryField(const Program& program, LayoutID layout, uint32_t numberOfParts)
{
    BuiltinLayoutID builtinLayout = program.at(layout).builtinLayout;
    return builtinLayout && program.builtins().at(builtinLayout).fields.size() == numberOfParts;
}

static void retypeProducer(const Program& program, IRBuilder& builder, ValueID producer)
{
    Function& function = builder.function();
    Value& value = function.edit(producer);
    value.type = resultTypeOf(program, function, value, builder.types());
    value.setExpected(expectedTypeOf(program, function, value, builder.types()));
}

static ValueID emitResult(IRBuilder& builder, ValueID producer, uint32_t resultIndex)
{
    Value result = IRBuilder::make(Opcode::ResultAt, { producer });
    result.setIndex(resultIndex);
    return builder.append(WTF::move(result));
}

static ValueID emitSubstringInBounds(IRBuilder& builder, ValueID string, ValueID producer, uint32_t resultIndexOfStart)
{
    ValueID start = emitResult(builder, producer, resultIndexOfStart);
    ValueID end = emitResult(builder, producer, resultIndexOfStart + 1);
    return builder.append(Opcode::SubstringOrUndefined, { string, start, end });
}

ValueID ObjectInterpreter::emitPresenceTag(ValueID isPresent, uint8_t tagBitWhenAbsent)
{
    IRBuilder& builder = cursor().builder();
    ValueID tagWhenAbsent = builder.appendInteger(int64_t(1) << tagBitWhenAbsent);
    ValueID shift = builder.append(Opcode::ToNumber, { isPresent });
    return builder.append(Opcode::LShift, { tagWhenAbsent, shift });
}

Execution ObjectInterpreter::executeBuiltinResult(ValueID call, VirtualObjectID object)
{
    auto leaveWhole = [&](LimitID limit = LimitID::None) {
        m_sinking.abandon(object, limit);
        return executeOther(call);
    };

    if (stateOf(object))
        return leaveWhole();

    const VirtualObjectDescription& description = m_sinking.descriptionOf(object);
    if (description.isDead)
        return Execution::Interpreted;

    VirtualFamily& family = m_sinking.familyOf(object);
    bool isPrefix = description.kind == VirtualObjectKind::BuiltinPrefix;
    RELEASE_ASSERT(isPrefix || description.kind == VirtualObjectKind::BuiltinFields);
    if (isPrefix && numberOfElementsInUse(family) > m_sinking.limit(LimitID::MaxVirtualArrayLength))
        return leaveWhole(LimitID::MaxVirtualArrayLength);

    BuiltinCallForm form = isPrefix ? BuiltinCallForm::Prefix : BuiltinCallForm::Fields;
    uint32_t prefixLength = isPrefix ? prefixLengthNeededBy(object) : 0;
    const BuiltinOverload& overload = program().builtins().at(function().at(call).overload());
    std::optional<BuiltinResultIndexes> indexes = resultIndexesOf(program().builtins(), function().at(call).overload(), form, prefixLength);
    RELEASE_ASSERT(indexes);
    if (description.absentResult && !indexes->presence)
        return leaveWhole();
    if (!isPrefix && !deliversEveryField(program(), description.layout, indexes->numberOfParts))
        return leaveWhole();

    VirtualShape present;
    present.kind = VirtualShapeKind::Fields;
    present.descriptionIndex = m_sinking.at(object).descriptionIndex;
    present.length = prefixLength;
    Vector<DeliveredField, 8> deliveredFields;
    auto deliver = [&](uint32_t resultIndex, const VirtualField& virtualField, bool isDeliveredAsBounds = false) {
        std::optional<uint32_t> field = fieldFor(object, virtualField);
        if (!field)
            return false;
        present.stored.set(*field);
        if (description.hasKeyOrder && virtualField.isProperty())
            present.order = m_sinking.orders().append(present.order, *field);
        deliveredFields.append(DeliveredField { resultIndex, *field, isDeliveredAsBounds });
        return true;
    };

    bool hasEveryField = !indexes->length || deliver(*indexes->length, VirtualField::part(VirtualFieldKind::Length));
    for (uint32_t property = 0; hasEveryField && property < indexes->numberOfProperties; ++property)
        hasEveryField = deliver(indexes->firstProperty + property, VirtualField::named(program().keyOf(overload.code.prefixProperties[property].name)));
    for (uint32_t part = 0; hasEveryField && part < indexes->numberOfParts; ++part)
        hasEveryField = deliver(indexes->resultOfPart(part), isPrefix ? VirtualField::element(part) : fieldOfLayoutField(program(), description.layout, part), indexes->partsAreBounds);
    if (!hasEveryField)
        return leaveWhole(LimitID::MaxVirtualObjectFields);

    std::optional<uint8_t> tagBitWhenAbsent = m_sinking.at(object).tagBitWhenAbsent;
    RELEASE_ASSERT(tagBitWhenAbsent.has_value() == description.absentResult.has_value());
    VirtualShapeID presentShape = family.intern(WTF::move(present));
    ObjectState state = ObjectState::withShape(presentShape);
    if (tagBitWhenAbsent) {
        VirtualShapeID absentShape = family.intern(VirtualShape::ofConstant(*description.absentResult));
        state.variants = { ObjectVariant { *tagBitWhenAbsent, absentShape }, ObjectVariant { static_cast<uint8_t>(*tagBitWhenAbsent + 1), presentShape } };
    }

    requireCellsOfOperands(call);
    forgetFieldsWrittenBy(call);
    if (!isRewriting()) {
        m_state.add(object, WTF::move(state));
        return Execution::Interpreted;
    }

    IRBuilder& builder = cursor().builder();
    builder.function().edit(call).setBuiltinCallForm(form);
    if (isPrefix)
        builder.function().edit(call).setPrefixLength(prefixLength);
    keep(call);
    retypeProducer(program(), builder, call);
    m_state.add(object, WTF::move(state));

    ValueID rangedString = indexes->partsAreBounds ? builtinCallOf(program(), builder.function().at(call)).operandAt(*overload.result.substringOf) : ValueID();
    ObjectValues& carried = values().ensure(object);
    if (tagBitWhenAbsent)
        carried.tag = emitPresenceTag(emitResult(builder, call, *indexes->presence), *tagBitWhenAbsent);
    for (const DeliveredField& delivered : deliveredFields)
        carried.setField(delivered.field, delivered.isDeliveredAsBounds ? emitSubstringInBounds(builder, rangedString, call, delivered.resultIndex) : emitResult(builder, call, delivered.resultIndex));
    return Execution::Interpreted;
}

uint32_t ObjectInterpreter::prefixLengthNeededBy(VirtualObjectID object) const
{
    const VirtualFamily& family = m_sinking.familyOf(object);
    // An append to another array of the family numbers new elements after the call ran, and every solve and mode has to deliver one prefix.
    if (const VirtualShape* delivered = findDeliveredShape(family, m_sinking.at(object).descriptionIndex))
        return delivered->length;
    return static_cast<uint32_t>(std::min(numberOfElementsInUse(family), m_sinking.limit(LimitID::MaxVirtualArrayLength)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
