/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3BlockRarity.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3Procedure.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace B3 {

WTF_MAKE_TZONE_ALLOCATED_IMPL(BlockRarity);

BlockRarity::BlockRarity(Procedure& proc)
    : m_classes(proc.size(), FrequencyClass::Rare)
{
    Vector<BasicBlock*, 32> worklist { proc[0] };
    m_classes[proc[0]] = FrequencyClass::Normal;
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        for (const FrequentedBlock& successor : block->successors()) {
            FrequencyClass candidate = rarerOf(m_classes[block], successor.frequency());
            if (!B3::isRarerThan(m_classes[successor.block()], candidate))
                continue;
            m_classes[successor.block()] = candidate;
            worklist.append(successor.block());
        }
    }
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
