/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

    bool isInlineBuiltin(ValueID, InlineBuiltin) const;
    CellPointer arrayOperand(ValueID);
    Integer indexOperand(ValueID);
    IndexingType indexingTypeAtCreation(ValueID newArray) const;
    CellPointer newArrayWithHoles(ValueID result, const Value&, bool isCapacity);
