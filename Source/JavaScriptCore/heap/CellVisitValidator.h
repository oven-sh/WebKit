/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace JSC {

class JSCell;
class SlotVisitor;

class CellVisitValidator {
public:
    static void validate(SlotVisitor&, const JSCell*);

private:
    [[noreturn]] static void crashOnMismatch(const JSCell*);
};

} // namespace JSC
