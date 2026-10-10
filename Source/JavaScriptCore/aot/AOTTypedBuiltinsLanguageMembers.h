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
    };
    Vector<ForInStamp, 2> m_forInStamps;
    ValueID originOf(ValueID) const;
    Operand lowerForInKeys(ValueID result, const Value&);
    Operand lowerHasEnumerableProperty(ValueID result, const Value&);
