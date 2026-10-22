/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTExpandOwnKeysOrders.h"
#include "AOTJSONEmission.h"
#include "AOTJSONSupport.h"
#include "AOTStringBuilderCursor.h"
#include <climits>
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class JSONSerializerBuilder final : public Emission {
public:
    JSONSerializerBuilder(IRBuilder&, const EmissionInputs&, ValueID stringBuilder);

    // False: appendRoot() would write nothing and leave for whenThereIsNoText.
    bool hasPlanFor(ValueID value) const { return planValue(value).kind != ValueKind::Generic && !typeOf(value).hasOnlyPrimitives(Primitive::Undefined); }
    void appendRoot(ValueID value, BlockID whenThereIsNoText);
    void appendObjectOfLayout(ValueID object, LayoutID, BlockID whenShapeDiffers);
    void commit();

    static DerivedSignature signatureOf(Emission&, LayoutID);

private:
    enum class TypeCertainty : uint8_t { Proven, Expected };

    enum class ValueKind : uint8_t {
        Generic,
        Nullish,
        Boolean,
        Integer,
        Number,
        String,
        Date,
        Array,
        Objects,
        ObjectParts,
    };

    struct ValuePlan {
        ValueKind kind { ValueKind::Generic };
        TypeCertainty certainty { TypeCertainty::Expected };
        TypeID tested;
        TypeID element;
        Vector<LayoutID, 4> layouts;
        // The plans of the kinds of object in a union, each behind its test. The layouts come last: what fails their tests is written by the generic code.
        Vector<ValuePlan> parts;
        bool mayBeNullish { false };
    };

    struct Cursor {
        ValueID value;
        uint32_t reservedBytes { 0 };
    };

    struct MergePoint {
        BlockFrequency frequency { BlockFrequency::Normal };
        BlockID block;
        ValueID cursor;
        // Set where each arm brings the guard that the code behind the merge relies on.
        ValueID guard;
        uint32_t reservedBytes { UINT_MAX };
    };

    // Where the next member starts, the text of the object is empty, ends in a member, or ends in "{" or ",".
    enum class MemberListState : uint8_t { NotOpened, AfterMember, AfterSeparator };

    struct Position {
        // A member that starts after a separator writes the separator of the next member.
        bool endsWithComma() const { return kind == JSONPosition::Property && memberList == MemberListState::AfterSeparator; }

        JSONPosition kind { JSONPosition::Root };
        PropertyKeyID key;
        ValueID index;
        MemberListState memberList { MemberListState::AfterSeparator };
    };

    struct Member {
        PropertyKeyID key;
        uint32_t field { 0 };
        uint32_t positionInOrder { 0 };
        ValuePlan plan;
    };

    // The key orders that one arm writes: an order and the expected orders that are prefixes of it. An object with fewer keys has empty slots for the rest.
    struct KeyOrderFamily {
        uint32_t smallestCount() const { return descendingCounts.last(); }

        KeyOrderFields longestOrder;
        Vector<uint32_t, 4> descendingCounts;
    };

    struct TestedKeyOrder {
        ValueID guard;
        ValueID numberOfOwnMembers;
    };

    struct ObjectArm {
        TestedObject object;
        LayoutID layout;
        const KeyOrderFamily& orders;
        // Set where the orders of the arm differ in it.
        ValueID numberOfOwnMembers;
        Vector<Member, 8> members;
        BlockID remainder;
        BlockID done;
    };

    struct ArrayLoop {
        ValueID array;
        ValueID guard;
        ValueID length;
        TypeID elementType;
        MergePoint& closing;
    };

    void reserve(uint32_t bytes);
    void ensureCursor(uint32_t bytes);
    void append(Value&& cursorRow);
    void appendToken(JSONToken);
    void appendText(ValueID string);
    bool hasCursor() const { return !!m_cursor.value; }

    // An arm that has left for another merge point leaves the builder in no block. From there a jump is nothing, and so is going on at a merge point that no arm has reached.
    bool isInBlock() { return !!builder().currentBlock(); }
    MergePoint createMergePoint(BlockFrequency = BlockFrequency::Normal);
    void jumpTo(MergePoint&, ValueID guard = { });
    void continueAt(const MergePoint&);
    void leaveAfterUnknownCode();
    void leaveIfWritingRunsCode(ValueID written);

    ValuePlan planValue(ValueID) const;
    ValuePlan planType(const Type& proven, const Type* expected) const;
    ValuePlan planDefinedType(const Type& defined, TypeCertainty) const;
    ValuePlan planBuiltinClass(const Type& instances, TypeCertainty) const;
    ValuePlan planLayouts(const Type& objects, TypeCertainty) const;
    bool removePartsThatCannotPass(ValuePlan&, const Type& defined) const;

    void appendValue(ValueID, const ValuePlan&, const Position&);
    void appendPlannedValue(ValueID, const ValuePlan&, const Position&);
    void appendScalar(ValueID, const ValuePlan&);
    void appendNullish(const Position&);
    void appendGeneric(ValueID, const Position&);
    void appendByHost(ValueID, const Position&);
    void appendKeyOf(const Position&);

    void appendArray(ValueID, const ValuePlan&, const Position&);
    enum class ElementCode : uint8_t { RunsNoUnknownCode, MayRunUnknownCode };
    ElementCode appendArrayLoop(const ArrayLoop&, ArrayStorage);
    void appendArrayElement(const ArrayLoop&, ValueID element, ValueID index);
    void appendRemainingElements(const ArrayLoop&, ValueID firstIndex);
    void appendObjects(ValueID, const ValuePlan&, const Position&);
    void appendObjectParts(ValueID, const ValuePlan&, const Position&);
    void appendCallOfSerializer(ValueID object, LayoutID, BlockID whenUnknown);

    void appendKeyOrderArms(const TestedObject&, LayoutID, BlockID whenNoOrderMatches, BlockID done);
    Vector<KeyOrderFamily, 4> groupKeyOrders(LayoutID, std::span<const KeyOrderFields>, uint64_t maxArms) const;
    TestedKeyOrder emitKeyOrderTests(const TestedObject&, const ObjectArm&, BlockID notTaken);
    void appendMembers(ObjectArm&);
    void appendMember(ObjectArm&, size_t index, MemberListState&);
    void renewGuards(ObjectArm&, size_t nextMember, MemberListState);
    static std::optional<JSONToken> separatorAfter(MemberListState);
    static JSONToken closingTokenOf(MemberListState);
    void appendRemainder(const ObjectArm&);

    ValueID m_stringBuilder;
    Cursor m_cursor;
    // Where an arm goes on that has run unknown code: the object or the array around the value renews its guards there, and the other arms keep theirs.
    MergePoint* m_afterUnknownCode { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
