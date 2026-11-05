/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3Value.h"
#include "B3ValueKey.h"
#include <span>
#include <wtf/HashMap.h>
#include <wtf/IndexMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class Procedure;

struct ConstantUse {
    BasicBlock* block;
    Value* user;
    Value** child;
    unsigned userIndex;
};

struct ConstantPlacement {
    explicit ConstantPlacement(Procedure&);

    void add(Value*, BasicBlock*);

    IndexMap<BasicBlock*, Vector<Value*>> materializations;
    UncheckedKeyHashMap<Value*, BasicBlock*> blockOf;
    UncheckedKeyHashMap<ValueKey, Vector<Value*, 1>> valuesByKey;
};

inline bool isPlacedByFrequency(const Value* value)
{
    return value->opcode() != ArgumentReg;
}

void placeConstantsByFrequency(Procedure&, std::span<const ConstantUse>, ConstantPlacement&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
