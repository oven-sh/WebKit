/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

    bool isInlineBuiltin(ValueID, InlineBuiltin) const;
    CellPointer arrayOperand(ValueID);
    Integer indexOperand(ValueID);
    IndexingType indexingTypeAtBirth(ValueID newArray) const;
    CellPointer newArrayWithHoles(ValueID result, const Value&, bool isCapacity);
