/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

    // Goes on where the value is an object.
    CellPointer objectOrElse(ValueID, LBasicBlock otherwise);
    // Every object of the type is one of the program.
    bool hasOnlyObjectsOfProgram(const Type&) const;
    Operand yieldAsTableSays(Operand, const Type& guarantee, const Value& call, const Type&);
    // From an allocator that is there and whose cells have that size. Nothing has been stored to the cell.
    CellPointer allocateCell(LValue allocator, size_t cellSize, LBasicBlock slowPath);
    CellPointer newCopyOnWriteArray(LValue list, LBasicBlock slowPath);
    CellPointer lowerOwnPropertyKeys(InlineBuiltin, ValueID object);
    CellPointer listKeysForIn(Boxed object, const Type&);
    enum class OwnField : uint8_t { InAllMembers, InNoMember, Varies };
    OwnField ownFieldOfMembers(LayoutID view, StringID) const;
    Word32 lowerHasOwnProperty(ValueID object, ValueID key);
    // Goes on where Instance::ownPropertyPlaces says where the object has the property, or that it has none, and yields what it says.
    Word32 placeOfOwnPropertyOrElse(CellPointer object, ValueID key, LBasicBlock otherwise);
    // Whether the object has the property, by what that has yielded.
    Word32 hasOwnPropertyAt(CellPointer object, Word32 place);
    Operand lowerGetOwnPropertyDescriptor(ValueID result, const Value&);
    Operand lowerCreate(ValueID result, const Value&);
    TypedPointer inlineProperty(CellPointer object, PropertyOffset);
    Operand lowerGetPrototypeOf(ValueID result, const Value&);
    Word32 lowerSameValue(ValueID, ValueID);
    // What an object of the layout has for keys while it has the order word: the fields that are properties, as indexes into Layout::fields, in the order of the keys.
    struct KeyOrder {
        LayoutID layout;
        uint32_t orderWord;
        Vector<uint16_t, 8> fields;
    };
    Vector<KeyOrder, 2> keyOrdersExpectedOf(ValueID) const;
    template<typename InPlace, typename Generally> void byKeyOrder(ValueID object, const Vector<KeyOrder, 2>&, const InPlace&, const Generally&);
    CellPointer cloneAsPlainObject(ValueID source);
    CellPointer newArrayOf(std::span<const Boxed>);
    CellPointer lowerValuesOrEntries(InlineBuiltin, const Value&);
    bool isNewPlainObject(ValueID) const;
    Operand lowerAssign(ValueID result, const Value&);
