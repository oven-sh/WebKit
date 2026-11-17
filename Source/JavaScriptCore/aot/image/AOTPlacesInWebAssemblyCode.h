/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "AOTVector.h"

namespace JSC { namespace AOT {

// What AOTReturnAddressCheck.cpp has to know and no record of the directory says.
struct PlacesInWebAssemblyCode {
    struct Data {
        uint32_t offset;
        uint32_t size;
    };

    // An entry is the distance from the start of the table to its target.
    struct JumpThroughTable {
        uint32_t offsetBehindJump;
        uint32_t offsetOfTable;
        uint32_t numberOfEntries;
    };

    void append(const PlacesInWebAssemblyCode& other, uint32_t offsetOfOther)
    {
        for (JumpThroughTable jump : other.jumpsThroughTables)
            jumpsThroughTables.append({ offsetOfOther + jump.offsetBehindJump, offsetOfOther + jump.offsetOfTable, jump.numberOfEntries });
        for (Data piece : other.data)
            data.append({ offsetOfOther + piece.offset, piece.size });
    }

    Vector<JumpThroughTable> jumpsThroughTables;
    Vector<Data> data;
};

} } // namespace JSC::AOT
