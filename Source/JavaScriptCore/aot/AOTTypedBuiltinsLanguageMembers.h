/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

    struct ForInStamp {
        ValueID keys;
        Word32 structureID;
        Word32 orderWord;
        Word32 finalStructureID;
        Integer inlineCapacity;
    };
    Vector<ForInStamp, 2> m_forInStamps;
    bool isForInKeys(const Value&) const;
    bool isForInStampCheck(const Value&) const;
    bool isRequestedForFinalObject(ValueID keys) const;
    void branchOnForInStamp(const Value&, LBasicBlock isAsListed, LBasicBlock isNot);
    const ForInStamp* forInStampOf(ValueID object, ValueID key) const;
    CellPointer isAsListedOrElse(const ForInStamp&, Boxed object, LBasicBlock otherwise);
    ValueID underlyingValue(ValueID) const;
    Operand lowerForInKeys(ValueID result, const Value&);
    Operand lowerHasEnumerableProperty(ValueID result, const Value&);
    Operand lowerStepByOne(InlineBuiltin, ValueID result, const Value&);
