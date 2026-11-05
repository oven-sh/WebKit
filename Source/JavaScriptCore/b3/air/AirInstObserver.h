/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "MacroAssembler.h"

namespace JSC { namespace B3 { namespace Air {

class BasicBlock;
struct Inst;

class InstObserver {
public:
    virtual ~InstObserver() = default;
    virtual void willGenerate(const BasicBlock&) { }
    virtual void didGenerate(const Inst&, MacroAssembler::Label begin, MacroAssembler::Label end) = 0;
};

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
