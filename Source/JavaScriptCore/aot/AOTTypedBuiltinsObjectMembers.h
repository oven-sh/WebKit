/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

    CellPointer objectOrElse(ValueID, LBasicBlock otherwise);
    bool hasOnlyProgramObjects(const Type&) const;
    Operand convertToDeclaredResult(Operand, const Type& guarantee, const Value& call, const Type&);
    CellPointer allocateCell(LValue allocator, size_t cellSize, LBasicBlock slowPath);
    CellPointer newCopyOnWriteArray(LValue list, LBasicBlock slowPath);
    LValue layoutKeyListOrElse(CellPointer object, LBasicBlock otherwise);
    CellPointer lowerOwnPropertyKeys(InlineBuiltin, ValueID object);
    CellPointer listKeysForIn(Boxed object, const Type&);
    enum class OwnField : uint8_t { InAllMembers, InNoMember, Varies };
    OwnField ownFieldOfSubtypes(LayoutID view, StringID) const;
    Word32 lowerHasOwnProperty(ValueID object, ValueID key);
    Word32 ownPropertyLocationOrElse(CellPointer object, ValueID key, LBasicBlock otherwise);
    Word32 hasOwnPropertyAt(CellPointer object, Word32 place);
    Operand lowerGetOwnPropertyDescriptor(ValueID result, const Value&);
    Operand lowerCreate(ValueID result, const Value&);
    Operand createWithPrototype(ValueID result, const Value&);
    std::optional<size_t> indexOfAssignThatClones(ValueID created);
    TypedPointer inlineProperty(CellPointer object, PropertyOffset);
    Operand lowerGetPrototypeOf(ValueID result, const Value&);
    Operand lowerSetPrototypeOf(ValueID result, const Value&, Writes);
    Operand lowerAdoptPrototype(const Value&, Writes);
    Word32 lowerSameValue(ValueID, ValueID);
    struct KeyOrder {
        LayoutID layout;
        uint32_t orderWord;
        Vector<uint16_t, 8> fields;
    };
    Vector<KeyOrder, 2> keyOrdersExpectedOf(ValueID) const;
    template<typename InlinePath, typename GenericPath> void byKeyOrder(ValueID object, const Vector<KeyOrder, 2>&, const InlinePath&, const GenericPath&);
    struct AssignedField {
        const Field* from;
        const Field* to;
    };
    std::optional<Vector<AssignedField, 8>> fieldsAssignedWithoutRunningCode(ValueID target, const KeyOrder&);
    void assignFields(CellPointer target, ValueID targetID, CellPointer source, std::span<const AssignedField>, LBasicBlock otherwise);
    CellPointer cloneAsPlainObject(ValueID source, const CloneDefinitions&);
    CellPointer newArrayOf(std::span<const Boxed>);
    CellPointer lowerValuesOrEntries(InlineBuiltin, const Value&);
    bool isNewPlainObject(ValueID) const;
    Operand lowerAssign(ValueID result, const Value&);
