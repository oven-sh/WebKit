/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFieldMoves.h"

#if ENABLE(AOT)

#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPrimitives.h"
#include "AOTProgramStrings.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "PureNaN.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr OptionSet<PrimitiveSet> numberPrimitives = OptionSet<PrimitiveSet>::fromRaw(Typed::Primitive::Number);
static constexpr OptionSet<PrimitiveSet> allPrimitives = OptionSet<PrimitiveSet>::fromRaw(Typed::Primitive::All);

OptionSet<ValueKind> valueKindOf(JSValue value)
{
    if (value.isInt32())
        return ValueKind::Int32;
    if (value.isString())
        return ValueKind::String;
    if (value.isTrue())
        return ValueKind::True;
    if (value.isFalse())
        return ValueKind::False;
    if (value.isUndefined())
        return ValueKind::Undefined;
    if (value.isNull())
        return ValueKind::Null;
    if (value.isDouble())
        return ValueKind::Double;
    return { };
}

static bool integerRangeCovers(const RuntimeType& type, int64_t minimum, int64_t maximum)
{
    auto range = type.integerRange();
    return !range || (range->first <= minimum && range->second >= maximum);
}

OptionSet<ValueKind> kindsAlwaysAccepted(const RuntimeType& type)
{
    OptionSet<PrimitiveSet> primitives = type.primitiveSet();
    OptionSet<ValueKind> kinds;
    if (primitives.contains(PrimitiveSet::Integer) && integerRangeCovers(type, INT32_MIN, INT32_MAX))
        kinds.add(ValueKind::Int32);
    if (primitives.containsAll(numberPrimitives) && integerRangeCovers(type, -maxSlotInteger, maxSlotInteger))
        kinds.add(ValueKind::Double);
    if (primitives.contains(PrimitiveSet::String))
        kinds.add(ValueKind::String);
    if (primitives.contains(PrimitiveSet::True))
        kinds.add(ValueKind::True);
    if (primitives.contains(PrimitiveSet::False))
        kinds.add(ValueKind::False);
    if (primitives.contains(PrimitiveSet::Undefined))
        kinds.add(ValueKind::Undefined);
    if (primitives.contains(PrimitiveSet::Null))
        kinds.add(ValueKind::Null);
    return kinds;
}

bool alwaysFits(const RuntimeLayoutTable& layouts, const FieldData& from, const FieldData& to)
{
    if (from.type == to.type)
        return true;
    const RuntimeType& fromType = layouts.typeOf(from);
    const RuntimeType& toType = layouts.typeOf(to);
    if (!fromType.cellTests().empty() || !fromType.literalStrings().empty() || !toType.literalStrings().empty())
        return false;
    if (!(fromType.primitiveSet() - toType.primitiveSet() - PrimitiveSet::Absent).isEmpty())
        return false;
    auto toRange = toType.integerRange();
    if (!fromType.primitiveSet().contains(PrimitiveSet::Integer) || !toRange)
        return true;
    auto fromRange = fromType.integerRange();
    return fromRange && fromRange->first >= toRange->first && fromRange->second <= toRange->second;
}

static bool acceptsEveryValue(const RuntimeType& type)
{
    if (!type.primitiveSet().containsAll(allPrimitives) || type.integerRange() || !type.literalStrings().empty())
        return false;
    return type.acceptsEveryObject();
}

static std::optional<FieldMoveKind> rawMoveKind(SlotRepresentation from, SlotRepresentation to)
{
    auto isCell32 = [](SlotRepresentation representation) {
        return representation == SlotRepresentation::Cell32 || representation == SlotRepresentation::OptionalCell32;
    };
    auto isBoxedCell = [](SlotRepresentation representation) {
        return representation == SlotRepresentation::Cell || representation == SlotRepresentation::JSValue;
    };
    if (from == to)
        return from == SlotRepresentation::Int32OrDouble ? FieldMoveKind::Int32OrDouble : FieldMoveKind::CopyBits;
    switch (from) {
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::Float:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::OptionalInt32:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::Int32OrDouble:
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
        return std::nullopt;
    case SlotRepresentation::Int32:
        if (to == SlotRepresentation::Double || to == SlotRepresentation::OptionalDouble)
            return FieldMoveKind::Int32ToDouble;
        return std::nullopt;
    case SlotRepresentation::Double:
        if (to == SlotRepresentation::OptionalDouble)
            return FieldMoveKind::PurifyDouble;
        return std::nullopt;
    case SlotRepresentation::OptionalDouble:
        if (to == SlotRepresentation::Double)
            return FieldMoveKind::CopyBits;
        return std::nullopt;
    case SlotRepresentation::Cell:
    case SlotRepresentation::JSValue:
        if (isBoxedCell(to))
            return FieldMoveKind::CopyBits;
        if (isCell32(to))
            return FieldMoveKind::UnboxToCell32;
        return std::nullopt;
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::DeferredClosure:
        if (isCell32(to) || to == SlotRepresentation::DeferredClosure)
            return FieldMoveKind::CopyBits;
        if (isBoxedCell(to))
            return FieldMoveKind::BoxCell32;
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FieldMoveKind classifyMove(const RuntimeLayoutTable& layouts, const FieldData* from, const FieldData* to)
{
    ASSERT(from || to);
    constexpr uint32_t mayLieOutsideCell = fieldIsOutOfLine | fieldMayExistWithoutStorage;
    if (!to)
        return from->flags & mayLieOutsideCell ? FieldMoveKind::SlotToBoxed : rawMoveKind(from->slot.representation(), SlotRepresentation::JSValue).value_or(FieldMoveKind::SlotToBoxed);
    if (!from)
        return to->slot.representation() == SlotRepresentation::JSValue && acceptsEveryValue(layouts.typeOf(*to)) ? FieldMoveKind::CopyBits : FieldMoveKind::BoxedToSlot;
    bool fits = alwaysFits(layouts, *from, *to);
    FieldMoveKind valueMove = to->slot.representation() == SlotRepresentation::JSValue && fits ? FieldMoveKind::SlotToBoxed : FieldMoveKind::BoxedToSlot;
    if (from->flags & mayLieOutsideCell || !fits)
        return valueMove;
    return rawMoveKind(from->slot.representation(), to->slot.representation()).value_or(valueMove);
}

static FieldMove moveToField(const FieldData& to)
{
    FieldMove move;
    move.to = to.slot.offset;
    move.width = traitsOf(to.slot.representation()).width;
    move.flags = to.isOutOfLine() ? FieldMove::toIsOutOfLine : 0;
    move.toField = &to;
    return move;
}

static void addGranuleOf(FieldMovePlan& plan, const FieldData& to)
{
    if (to.isOutOfLine())
        plan.granuleBits |= granuleBitOf(to.slot.offset);
}

static void setMoveSource(FieldMove& move, const FieldData& from)
{
    move.from = from.slot.offset;
    if (from.isOutOfLine())
        move.flags |= FieldMove::fromIsOutOfLine;
    move.fromField = &from;
}

static FieldMove moveToInlineProperty(PropertyOffset to)
{
    ASSERT(isInlineOffset(to));
    FieldMove move;
    move.to = JSObject::offsetOfInlineStorage() + offsetInInlineStorage(to) * sizeof(EncodedJSValue);
    move.width = sizeof(EncodedJSValue);
    return move;
}

static void setMoveSource(FieldMove& move, PropertyOffset from)
{
    if (isInlineOffset(from)) {
        move.from = JSObject::offsetOfInlineStorage() + offsetInInlineStorage(from) * sizeof(EncodedJSValue);
        return;
    }
    move.from = -offsetInButterfly(from) * sizeof(EncodedJSValue);
    move.flags |= FieldMove::fromIsOutOfLine;
}

void FieldMovePlan::append(const FieldMove& move)
{
    bool isAbsentAsZero = !move.fromField || representsAbsentAsZero(move.fromField->slot.representation());
    if (move.kind == FieldMoveKind::CopyBits && move.width == sizeof(EncodedJSValue) && isAbsentAsZero && !move.flags && move.from <= UINT16_MAX && move.to <= UINT16_MAX) {
        boxedWordMoves.append(BoxedWordMove { static_cast<uint16_t>(move.from), static_cast<uint16_t>(move.to) });
        return;
    }
    moves.append(move);
}

void FieldMovePlan::appendFieldToField(const RuntimeLayoutTable& layouts, const FieldData& from, const FieldData& to)
{
    FieldMove move = moveToField(to);
    setMoveSource(move, from);
    move.kind = classifyMove(layouts, &from, &to);
    append(move);
    addGranuleOf(*this, to);
    if (from.slot.representation() == SlotRepresentation::DeferredClosure)
        sourceHasDeferredClosures = true;
}

void FieldMovePlan::appendPropertyToField(const RuntimeLayoutTable& layouts, PropertyOffset from, const FieldData& to)
{
    FieldMove move = moveToField(to);
    setMoveSource(move, from);
    move.kind = classifyMove(layouts, nullptr, &to);
    append(move);
    addGranuleOf(*this, to);
}

void FieldMovePlan::appendPropertyToInlineProperty(PropertyOffset from, PropertyOffset to)
{
    FieldMove move = moveToInlineProperty(to);
    setMoveSource(move, from);
    move.kind = FieldMoveKind::CopyBits;
    append(move);
}

void FieldMovePlan::appendFieldToInlineProperty(const RuntimeLayoutTable& layouts, const FieldData& from, PropertyOffset to)
{
    FieldMove move = moveToInlineProperty(to);
    setMoveSource(move, from);
    move.kind = classifyMove(layouts, &from, nullptr);
    append(move);
    if (from.slot.representation() == SlotRepresentation::DeferredClosure)
        sourceHasDeferredClosures = true;
}

void FieldMovePlan::appendSetAbsent(const FieldData& to)
{
    FieldMove move = moveToField(to);
    move.kind = FieldMoveKind::SetAbsent;
    moves.append(move);
}

void FieldMovePlan::finish(Structure* targetStructure, uint32_t targetSize, uint32_t targetOrderWord)
{
    status = FieldMovePlanStatus::Usable;
    size = targetSize;
    orderWord = targetOrderWord;
    structure = targetStructure;
}

static const uint8_t* sourceAddress(JSObject* source, const FieldMove& move)
{
    ASSERT(!move.fromField || !(move.flags & FieldMove::fromIsOutOfLine));
    if (move.flags & FieldMove::fromIsOutOfLine)
        return reinterpret_cast<const uint8_t*>(source->butterfly()) - move.from;
    return reinterpret_cast<const uint8_t*>(source) + move.from;
}

static uint8_t* targetAddress(JSObject* target, const FieldMove& move)
{
    if (!(move.flags & FieldMove::toIsOutOfLine))
        return reinterpret_cast<uint8_t*>(target) + move.to;
    return static_cast<uint8_t*>(uncheckedDowncast<Object>(target)->extension()->slot(move.to));
}

static JSValue readBoxedValue(JSObject* source, const FieldMove& move)
{
    if (!move.fromField)
        return JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(sourceAddress(source, move)));
    Object* object = uncheckedDowncast<Object>(source);
    if (JSValue value = object->readInline(*move.fromField))
        return value;
    return object->read(*move.fromField);
}

static void copyBits(unsigned width, const uint8_t* from, uint8_t* to)
{
    switch (width) {
    case 1:
        *to = *from;
        return;
    case 2:
        *reinterpret_cast<uint16_t*>(to) = *reinterpret_cast<const uint16_t*>(from);
        return;
    case 4:
        *reinterpret_cast<uint32_t*>(to) = *reinterpret_cast<const uint32_t*>(from);
        return;
    case 8:
        *reinterpret_cast<uint64_t*>(to) = *reinterpret_cast<const uint64_t*>(from);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool canStoreInField(Object& target, const FieldData& to, JSValue value)
{
    if (!value)
        return false;
    State& state = target.state();
    if (!state.objects->layouts.fieldAccepts(to, value))
        return false;
    uint64_t scratch = 0;
    SlotRepresentation representation = to.slot.representation();
    return tryWriteSlot(*state.strings, representation, &scratch, value) || (representation == SlotRepresentation::Int32OrDouble && value.isNumber());
}

static bool canApplyMove(const FieldMove& move, JSObject* source, JSObject* target)
{
    switch (move.kind) {
    case FieldMoveKind::SetAbsent:
        return true;
    case FieldMoveKind::SlotToBoxed:
        return !!readBoxedValue(source, move);
    case FieldMoveKind::BoxedToSlot:
        return canStoreInField(*uncheckedDowncast<Object>(target), *move.toField, readBoxedValue(source, move));
    case FieldMoveKind::CopyBits:
    case FieldMoveKind::PurifyDouble:
    case FieldMoveKind::Int32ToDouble:
    case FieldMoveKind::BoxCell32:
    case FieldMoveKind::UnboxToCell32:
    case FieldMoveKind::Int32OrDouble:
        return !move.fromField || !slotHoldsAbsent(move.fromField->slot.representation(), sourceAddress(source, move));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void moveInt32OrDouble(VM& vm, const FieldMove& move, Object& source, Object& target)
{
    int32_t word = *static_cast<const int32_t*>(source.slotOf(*move.fromField));
    if (word == doubleInExtensionInt32) {
        double number = *static_cast<const double*>(source.extension()->slot(source.layout().overflowOffsetOf(*move.fromField)));
        target.storeDoubleInExtension(vm, target.layout().overflowOffsetOf(*move.toField), number);
    }
    *static_cast<int32_t*>(target.slotOf(*move.toField)) = word;
}

static void applyMove(VM& vm, const FieldMove& move, JSObject* source, JSObject* target)
{
    auto from = [&] {
        return sourceAddress(source, move);
    };
    auto to = [&] {
        return targetAddress(target, move);
    };
    switch (move.kind) {
    case FieldMoveKind::CopyBits:
        copyBits(move.width, from(), to());
        return;
    case FieldMoveKind::PurifyDouble:
        *reinterpret_cast<double*>(to()) = purifyNaN(*reinterpret_cast<const double*>(from()));
        return;
    case FieldMoveKind::Int32ToDouble:
        *reinterpret_cast<double*>(to()) = *reinterpret_cast<const int32_t*>(from());
        return;
    case FieldMoveKind::BoxCell32: {
        uint32_t word = *reinterpret_cast<const uint32_t*>(from());
        JSValue value = word >= minCell32 ? JSValue(decodeCell32(source, word)) : JSValue::decode(word);
        *reinterpret_cast<EncodedJSValue*>(to()) = JSValue::encode(value);
        return;
    }
    case FieldMoveKind::UnboxToCell32: {
        JSValue value = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(from()));
        *reinterpret_cast<uint32_t*>(to()) = value.isCell() ? encodeCell32(target, value.asCell()) : static_cast<uint32_t>(JSValue::encode(value));
        return;
    }
    case FieldMoveKind::BoxedToSlot: {
        bool didStore = uncheckedDowncast<Object>(target)->store(vm, *move.toField, readBoxedValue(source, move));
        ASSERT_UNUSED(didStore, didStore);
        return;
    }
    case FieldMoveKind::SlotToBoxed:
        *reinterpret_cast<EncodedJSValue*>(to()) = JSValue::encode(readBoxedValue(source, move));
        return;
    case FieldMoveKind::Int32OrDouble:
        moveInt32OrDouble(vm, move, *uncheckedDowncast<Object>(source), *uncheckedDowncast<Object>(target));
        return;
    case FieldMoveKind::SetAbsent:
        uncheckedDowncast<Object>(target)->clear(*move.toField);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool applyMoves(VM& vm, const FieldMovePlan& plan, JSObject* target, JSObject* source)
{
    ASSERT(plan.isUsable());
    auto wordOf = [](auto* object, uint16_t offset) -> auto& {
        return *reinterpret_cast<EncodedJSValue*>(reinterpret_cast<uint8_t*>(object) + offset);
    };
    for (BoxedWordMove move : plan.boxedWordMoves) {
        if (!wordOf(source, move.from))
            return false;
    }
    for (const FieldMove& move : plan.moves) {
        if (!canApplyMove(move, source, target))
            return false;
    }
    for (BoxedWordMove move : plan.boxedWordMoves)
        wordOf(target, move.to) = wordOf(source, move.from);
    for (const FieldMove& move : plan.moves)
        applyMove(vm, move, source, target);
    vm.writeBarrier(target);
    vm.mutatorFence();
    return true;
}

static ALWAYS_INLINE bool tryApplyMove(VM& vm, const FieldMove& move, JSObject* source, JSObject* target)
{
    switch (move.kind) {
    case FieldMoveKind::SlotToBoxed: {
        ASSERT(move.fromField && !(move.flags & FieldMove::toIsOutOfLine));
        Object& object = *uncheckedDowncast<Object>(source);
        JSValue value = object.readInline(*move.fromField);
        if (!value) [[unlikely]] {
            value = object.read(*move.fromField);
            if (!value)
                return false;
        }
        *reinterpret_cast<EncodedJSValue*>(reinterpret_cast<uint8_t*>(target) + move.to) = JSValue::encode(value);
        return true;
    }
    case FieldMoveKind::BoxedToSlot: {
        JSValue value = readBoxedValue(source, move);
        Object& object = *uncheckedDowncast<Object>(target);
        return canStoreInField(object, *move.toField, value) && object.store(vm, *move.toField, value);
    }
    case FieldMoveKind::CopyBits:
    case FieldMoveKind::PurifyDouble:
    case FieldMoveKind::Int32ToDouble:
    case FieldMoveKind::BoxCell32:
    case FieldMoveKind::UnboxToCell32:
    case FieldMoveKind::Int32OrDouble:
    case FieldMoveKind::SetAbsent:
        if (!canApplyMove(move, source, target))
            return false;
        applyMove(vm, move, source, target);
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool applyMovesToNewObject(VM& vm, const FieldMovePlan& plan, JSFinalObject* target, JSObject* source)
{
    ASSERT(plan.isUsable());
    auto* wordsOfSource = reinterpret_cast<const uint8_t*>(source);
    auto* wordsOfTarget = reinterpret_cast<uint8_t*>(target);
    for (BoxedWordMove move : plan.boxedWordMoves) {
        EncodedJSValue word = *reinterpret_cast<const EncodedJSValue*>(wordsOfSource + move.from);
        if (!word)
            return false;
        *reinterpret_cast<EncodedJSValue*>(wordsOfTarget + move.to) = word;
    }
    for (const FieldMove& move : plan.moves) {
        if (!tryApplyMove(vm, move, source, target))
            return false;
    }
    vm.writeBarrier(target);
    vm.mutatorFence();
    return true;
}

JSObject* tryCreateByPlan(VM& vm, const FieldMovePlan& plan, JSObject* source)
{
    ASSERT(plan.isUsable());
    DeferGC deferGC(vm);
    if (plan.sourceHasDeferredClosures)
        uncheckedDowncast<Object>(source)->materializeDeferredClosures(vm);
    Object* target = Object::create(vm, plan.structure, uncheckedDowncast<CompiledStructure>(plan.structure)->layout(), plan.size, plan.orderWord);
    if (plan.granuleBits)
        target->ensureExtensionGranules(vm, plan.granuleBits);
    return applyMoves(vm, plan, target, source) ? target : nullptr;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
