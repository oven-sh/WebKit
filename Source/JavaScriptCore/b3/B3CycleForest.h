/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3CFG.h"
#include <wtf/CycleForest.h>

namespace JSC { namespace B3 {

class Procedure;

class CycleForest final : public WTF::CycleForest<CFG> {
public:
    JS_EXPORT_PRIVATE explicit CycleForest(Procedure&);

    const Vector<BasicBlock*>& blocksInPostOrder() const LIFETIME_BOUND { return nodesInPostOrder(); }
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
