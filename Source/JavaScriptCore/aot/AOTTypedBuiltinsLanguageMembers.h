/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

    // What the object of a `for..in` was when its keys were listed: see lowerForInKeys(). No structure has the ID zero.
    struct ForInStamp {
        ValueID keys;
        Word32 structureID;
        Word32 orderWord;
        Word32 finalStructureID;
        Integer inlineCapacity;
    };
    Vector<ForInStamp, 2> m_forInStamps;
    bool isForInKeys(const Value&) const;
    bool isTestOfForInStamp(const Value&) const;
    bool isAskedForFinalObject(ValueID keys) const;
    void branchOnForInStamp(const Value&, LBasicBlock isAsListed, LBasicBlock isNot);
    const ForInStamp* forInStampOf(ValueID object, ValueID key) const;
    CellPointer isAsListedOrElse(const ForInStamp&, Boxed object, LBasicBlock otherwise);
    ValueID originOf(ValueID) const;
    Operand lowerForInKeys(ValueID result, const Value&);
    Operand lowerHasEnumerableProperty(ValueID result, const Value&);
    Operand lowerStepByOne(InlineBuiltin, ValueID result, const Value&);
