/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTRuntimeType.h"
#include "AOTVector.h"
#include "JSCJSValue.h"
#include "PropertyOffset.h"
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/OptionSet.h>

namespace JSC {

class JSObject;
class Structure;
class VM;

namespace AOT {

class AcceptedValue;
class Object;
class RuntimeLayoutTable;

enum class FieldMoveKind : uint8_t {
    CopyBits,
    PurifyDouble,
    Int32ToDouble,
    BoxCell32,
    UnboxToCell32,
    BoxedToSlot,
    SlotToBoxed,
    SetAbsent,
};

struct FieldMove {
    static constexpr uint8_t fromIsOutOfLine = 1;
    static constexpr uint8_t toIsOutOfLine = 2;
    // The slot of the source has a marker for undefined or null that the slot of the target lacks.
    static constexpr uint8_t refusesUndefinedAndNull = 4;

    uint32_t from { 0 };
    uint32_t to { 0 };
    FieldMoveKind kind { FieldMoveKind::CopyBits };
    uint8_t width { 0 };
    uint8_t flags { 0 };
    const FieldData* fromField { nullptr };
    const FieldData* toField { nullptr };
};

// From a slot in the cell of the source that holds a boxed value, or zero where the field is absent, to such a slot or an inline property of the target.
struct BoxedWordMove {
    uint16_t from { 0 };
    uint16_t to { 0 };
};

enum class FieldMovePlanStatus : uint8_t { NeverApplies, Usable };

struct FieldMovePlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FieldMovePlan);

    bool isUsable() const { return status == FieldMovePlanStatus::Usable; }
    bool movesNothing() const { return moves.isEmpty() && boxedWordMoves.isEmpty(); }

    void appendFieldToField(const RuntimeLayoutTable&, const FieldData& from, const FieldData& to);
    void appendPropertyToField(const RuntimeLayoutTable&, PropertyOffset from, const FieldData& to);
    void appendFieldToInlineProperty(const RuntimeLayoutTable&, const FieldData& from, PropertyOffset to);
    void appendPropertyToInlineProperty(PropertyOffset from, PropertyOffset to);
    void appendSetAbsent(const FieldData& to);
    void finish(Structure* targetStructure, uint32_t targetSize, uint32_t targetOrderWord);

    FieldMovePlanStatus status { FieldMovePlanStatus::NeverApplies };
    bool sourceHasDeferredClosures { false };
    // A move makes one data property that can be written, listed and deleted out of another. No plan applies to a field that its layout gives other attributes.
    bool namesFieldWithAttributes { false };
    uint32_t size { 0 };
    uint32_t orderWord { 0 };
    uint64_t granuleBits { 0 };
    Structure* structure { nullptr };
    Vector<FieldMove, 8> moves;
    Vector<BoxedWordMove, 8> boxedWordMoves;

private:
    void append(const FieldMove&);
};

bool applyMoves(VM&, const FieldMovePlan&, JSObject* target, JSObject* source);
// Nothing else has seen the target, an ordinary object whose properties are still empty, so it may be left half filled where a move does not apply, and each value is read once.
bool applyMovesToNewObject(VM&, const FieldMovePlan&, JSFinalObject* target, JSObject* source);
JSObject* tryCreateByPlan(VM&, const FieldMovePlan&, JSObject* source);

enum class ValueKind : uint8_t {
    Int32 = 1,
    String = 2,
    True = 4,
    False = 8,
    Undefined = 16,
    Null = 32,
    Double = 64,
};

OptionSet<ValueKind> valueKindOf(JSValue);
OptionSet<ValueKind> kindsAlwaysAccepted(const RuntimeType&);
bool alwaysFits(const RuntimeLayoutTable&, const FieldData& from, const FieldData& to);
// Asks the representation too, so the store of what it returns cannot fail.
std::optional<AcceptedValue> acceptInSlot(Object& target, const FieldData&, JSValue);
FieldMoveKind classifyMove(const RuntimeLayoutTable&, const FieldData* fromOrNull, const FieldData* toOrNull);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
