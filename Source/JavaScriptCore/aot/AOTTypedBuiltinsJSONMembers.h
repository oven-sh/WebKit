/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

    struct JSONOutput {
        B3::Variable* cursor { nullptr };
        LBasicBlock givesUp { nullptr };
        Vector<uint8_t, 64> pendingText;
        unsigned knownCapacity { 0 };
        unsigned fieldsLeft { 0 };
        LBasicBlock handsOverRestOfList { nullptr };
    };
    struct JSONExit {
        LBasicBlock block { nullptr };
        unsigned knownCapacity { UINT_MAX };
    };
    struct JSONKeyOrder {
        LayoutID layout;
        uint32_t order;
        uint32_t count;
        uint32_t leastCount;
        Vector<uint16_t, 8> fields;
    };
    enum class JSONPosition : uint8_t { Property, Element };
    struct JSONMember {
        std::span<const uint8_t> name;
        bool mayBeLeftOut;
    };
    template<typename Describe, typename Append> void appendMembers(JSONOutput&, unsigned count, const Describe&, const Append&);
    struct JSONLiteralMember {
        StringID name;
        ValueID value;
        ValueID definition;
    };
    std::optional<Vector<JSONLiteralMember, 8>> membersOfFreshLiteral(ValueID, ValueID holder) const;
    void appendFreshLiteral(JSONOutput&, const Vector<JSONLiteralMember, 8>&, unsigned depth);
    Vector<JSONKeyOrder, 4> keyOrdersForJSON(const Type&) const;
    Vector<JSONKeyOrder, 4> keyOrdersForJSON(std::span<const LayoutID>) const;
    Vector<std::pair<LayoutID, FunctionID>, 4> serializersFor(const Type&) const;
    bool hasCodeForJSON(const Type&) const;
    bool hasCodeForObjectsIn(const Type&) const;
    LValue cursorOf(JSONOutput&);
    void setCursor(JSONOutput&, LValue);
    LBasicBlock exitThrough(JSONOutput&, JSONExit&);
    void reserveCapacity(JSONOutput&, unsigned size);
    void appendText(JSONOutput&, std::span<const uint8_t>);
    void appendText(JSONOutput& output, ASCIILiteral text) { appendText(output, asBytes(text.span8())); }
    void flushText(JSONOutput&);
    void didAppendWithRoomToSpare(JSONOutput&, Pointer cursor);
    void appendInteger(JSONOutput&, Integer, unsigned maxLength);
    void appendNumber(JSONOutput&, Number);
    void appendBoolean(JSONOutput&, Word32);
    void appendString(JSONOutput&, CellPointer);
    void appendGenerally(JSONOutput&, Boxed, JSONPosition, JSONExit& nothing);
    void appendNilOrLeaveOut(JSONOutput&, Word32 isNull, const Type&, JSONPosition, JSONExit& nothing, JSONExit& continuation);
    void appendCell(JSONOutput&, CellPointer, const Type&, bool typeIsFact, unsigned depth, JSONPosition, JSONExit& nothing);
    void appendBySerializer(JSONOutput&, CellPointer, FunctionID);
    void appendObjectBeyondPlace(JSONOutput&, CellPointer, const Type&, JSONPosition, JSONExit& nothing);
    void appendObject(JSONOutput&, CellPointer, const Type&, unsigned depth, JSONPosition, JSONExit& nothing);
    void appendObjectInPlace(JSONOutput&, CellPointer, const Vector<JSONKeyOrder, 4>&, unsigned depth, LBasicBlock otherwise, JSONExit& continuation);
    Operand lowerAppendJSON(const Value&);
    void appendField(JSONOutput&, CellPointer, const Field&, unsigned depth, JSONExit& nothing);
    void appendArray(JSONOutput&, CellPointer, const Type&, unsigned depth, JSONPosition, JSONExit& nothing);
    void appendValue(JSONOutput&, Operand, const Type&, bool typeIsFact, unsigned depth, JSONPosition, JSONExit& nothing);
    bool appendArgument(JSONOutput&, ValueID);
    Operand lowerJSONStringify(ValueID result, const Value&);
    Operand lowerResponseJSON(ValueID result, const Value&);
