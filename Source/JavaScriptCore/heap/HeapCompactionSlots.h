/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCJSValue.h"

namespace JSC {

class AbstractSlotVisitor;

// For tables of roots that can say where their references are: what they refer to is free to move.
namespace HeapCompactionSlots {

bool areRecordedBy(const AbstractSlotVisitor&);
void record(AbstractSlotVisitor&, EncodedJSValue*);
void recordNarrow(AbstractSlotVisitor&, uint32_t*);

// While one exists, Heap::isMarked() does not keep the cell in place.
class WillBeRecorded {
    WTF_MAKE_NONCOPYABLE(WillBeRecorded);
public:
    WillBeRecorded();
    ~WillBeRecorded();
};

} // namespace HeapCompactionSlots

} // namespace JSC
