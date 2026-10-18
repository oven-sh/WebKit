/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

    struct JSONOutput {
        B3::Variable* cursor { nullptr };
        LBasicBlock givesUp { nullptr };
        LBasicBlock givesUpBeforeWriting { nullptr };
        Vector<uint8_t, 64> pendingBytes;
        unsigned knownCapacity { 0 };
        unsigned remainingFields { 0 };
        LBasicBlock delegatesRestOfList { nullptr };
    };
    struct JSONExit {
        LBasicBlock block { nullptr };
        unsigned knownCapacity { UINT_MAX };
    };
    struct JSONKeyOrder {
        LayoutID layout;
        uint32_t order;
        uint32_t count;
        uint32_t minKeyCount;
        Vector<uint16_t, 8> fields;
    };
    enum class JSONPosition : uint8_t { Property, Element };
    struct JSONMember {
        std::span<const uint8_t> name;
        bool mayBeLeftOut;
    };
    enum class JSONSeparator : uint8_t { None, Always, Unknown };
    template<typename Append> void appendMember(JSONOutput&, JSONSeparator&, JSONMember, const Append&);
    template<typename Describe, typename Append> void appendMembers(JSONOutput&, unsigned count, const Describe&, const Append&);
    void appendObjectByOwnKeys(JSONOutput&, CellPointer, LayoutID, LBasicBlock otherwise, JSONExit& continuation);
    struct JSONLiteralMember {
        StringID name;
        ValueID value;
        ValueID definition;
    };
    std::optional<Vector<JSONLiteralMember, 8>> freshLiteralMembers(ValueID, ValueID holder) const;
    struct DeferredLiteral {
        const Block* block;
        ValueID object;
        size_t callIndex;
        Vector<size_t, 8> memberIndices;
        LBasicBlock materializedIn { nullptr };
        Operand materialized;
        bool isPlainObject { false };
        bool isBeingMaterialized { false };
    };
    DeferredLiteral* deferredLiteralFor(ValueID object);
    std::optional<DeferredLiteral> findDeferrableLiteral(ValueID object, size_t index, std::span<const size_t> plainObjectFields, unsigned depth);
    bool tryDeferLiteralAllocation(ValueID object, std::span<const size_t> plainObjectFields = { });
    bool isDeferredLiteralStore(size_t index) const;
    Operand materializeDeferredLiteral(DeferredLiteral&);
    Vector<DeferredLiteral, 2> m_deferredLiterals;
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
    void appendBytes(JSONOutput&, std::span<const uint8_t>);
    void appendBytes(JSONOutput& output, ASCIILiteral text) { appendBytes(output, asBytes(text.span8())); }
    void flushPendingBytes(JSONOutput&);
    void didAppendWithinCapacity(JSONOutput&, Pointer cursor);
    void appendInteger(JSONOutput&, Integer, unsigned maxLength);
    void appendNumber(JSONOutput&, Number);
    void appendBoolean(JSONOutput&, Word32);
    void appendString(JSONOutput&, CellPointer);
    void appendDate(JSONOutput&, CellPointer, const BuiltinClass&, JSONPosition, JSONExit& nothing);
    void appendGeneric(JSONOutput&, Boxed, JSONPosition, JSONExit& nothing);
    void appendNullishOrOmit(JSONOutput&, Word32 isNull, const Type&, JSONPosition, JSONExit& nothing, JSONExit& continuation);
    void appendCell(JSONOutput&, CellPointer, const Type&, bool typeIsProven, unsigned depth, JSONPosition, JSONExit& nothing);
    void appendBySerializer(JSONOutput&, CellPointer, FunctionID);
    void appendObjectOutOfLine(JSONOutput&, CellPointer, const Type&, JSONPosition, JSONExit& nothing);
    void appendObject(JSONOutput&, CellPointer, const Type&, unsigned depth, JSONPosition, JSONExit& nothing);
    void appendObjectInline(JSONOutput&, CellPointer, const Vector<JSONKeyOrder, 4>&, unsigned depth, LBasicBlock otherwise, JSONExit& continuation);
    Operand lowerAppendJSON(const Value&);
    void appendField(JSONOutput&, CellPointer, const Field&, unsigned depth, JSONExit& nothing);
    void appendArray(JSONOutput&, CellPointer, const Type&, unsigned depth, JSONPosition, JSONExit& nothing);
    void appendValue(JSONOutput&, Operand, const Type&, bool typeIsProven, unsigned depth, JSONPosition, JSONExit& nothing);
    bool appendArgument(JSONOutput&, ValueID);
    Operand lowerJSONStringify(ValueID result, const Value&);
    Operand lowerResponseJSON(ValueID result, const Value&);
