/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"

namespace JSC {

class JSCell;
class JSCellButterfly;
class Symbol;
class VM;

namespace AOT {

struct State;

class ClassToken {
public:
    static constexpr unsigned classIndexSlot = 0;
    static constexpr unsigned firstNameSlot = 1;

    static JSCellButterfly* create(VM&, const LayoutData& classLayout);
    static uint32_t classIndexOf(JSCell* token);
    static const LayoutData& classLayoutOf(State&, JSCell* token);
    static Symbol* privateNameOf(VM&, State&, JSCell* token, uint32_t field);
    static Symbol* privateNameIfCreated(JSCell* token, uint32_t field);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
