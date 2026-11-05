/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3FrequencyClass.h"
#include <wtf/IndexMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace B3 {

class Procedure;

class BlockRarity {
    WTF_MAKE_NONCOPYABLE(BlockRarity);
    WTF_MAKE_TZONE_ALLOCATED(BlockRarity);
public:
    JS_EXPORT_PRIVATE explicit BlockRarity(Procedure&);

    FrequencyClass of(BasicBlock* block) const { return m_classes[block]; }
    bool isRare(BasicBlock* block) const { return of(block) == FrequencyClass::Rare; }
    bool isRarerThan(BasicBlock* a, BasicBlock* b) const { return B3::isRarerThan(of(a), of(b)); }

private:
    IndexMap<BasicBlock*, FrequencyClass> m_classes;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
