/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCachesWords.h"
#include "Structure.h"
#include "StructureID.h"
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class StructureRows {
    WTF_MAKE_NONCOPYABLE(StructureRows);
public:
    static StructureRows& singleton();

    uint32_t rowOf(StructureID id) const { return *wordFor(id); }
    void setRowOf(StructureID id, uint32_t row) { *wordFor(id) = row; }
    const uint32_t* rowsForStructureIDZero() const { return m_rowsForStructureIDZero; }

private:
    StructureRows();

    uint32_t* wordFor(StructureID id) const
    {
        ASSERT(id && !id.isNuked());
        return m_rowsForStructureIDZero + (id.bits() >> StructureRowWords::granuleShift);
    }

    uint32_t* m_rowsForStructureIDZero { nullptr };
};

static_assert(sizeof(Structure) >= 1u << StructureRowWords::granuleShift);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
