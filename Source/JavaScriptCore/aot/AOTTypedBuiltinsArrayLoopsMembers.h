/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

    bool isInlineBuiltin(ValueID, InlineBuiltin) const;
    CellPointer arrayOperand(ValueID);
    Integer indexOperand(ValueID);
    IndexingType indexingTypeAtBirth(ValueID newArray) const;
    bool leavesArraysAsTheyAre(const Block&, size_t index) const;
    void findShapeTestsThatHold();
    bool hasPassedShapeTest(ValueID array, bool isDouble);
    Vector<std::pair<ValueID, bool>, 4> m_shapeTests;
    Vector<uint64_t> m_shapeTestsThatHoldAtHead;
    bool m_hasFoundShapeTests { false };
    CellPointer newArrayWithHoles(ValueID result, const Value&, bool isCapacity);
