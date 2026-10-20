/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class InsertionSet {
public:
    void insert(ProgramPoint, ValueID);
    bool isEmpty() const { return m_insertions.isEmpty(); }
    void execute(Function&);

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
