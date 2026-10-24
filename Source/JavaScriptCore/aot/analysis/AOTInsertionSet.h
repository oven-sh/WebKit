/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTVector.h"
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class InsertionSet {
public:
    void insert(ProgramPoint, ValueID);
    bool isEmpty() const { return m_insertions.isEmpty(); }
    // Tells of each block what it has put into the list of the block, in the order of the list.
    void execute(Function&, NOESCAPE const ScopedLambda<void(BlockID, std::span<const ValueID>)>& didInsert);

private:
    struct QueuedInsertion {
        BlockID block;
        uint32_t index;
        ValueID value;
    };

    Vector<QueuedInsertion, 8> m_insertions;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
