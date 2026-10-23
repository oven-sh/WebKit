/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace JSC {

class Heap;
class JSCell;
class SlotVisitor;

class CellVisitValidator {
public:
    static void validate(SlotVisitor&, const JSCell*);
    // At the end of marking, when no marker runs any more.
    static void validateMarksOfInnerCells(Heap&);

private:
    [[noreturn]] static void crashOnMismatch(const JSCell*);
};

} // namespace JSC
