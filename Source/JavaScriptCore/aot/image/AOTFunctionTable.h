/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class CodeIndex;
class LinkedFunctions;
struct CodeLayout;
struct EntryAlias;
struct ProgramTables;

struct FunctionTableInputs {
    const LinkedFunctions& functions;
    const Vector<EntryAlias>& aliases;
    const CodeLayout& layout;
    const CodeIndex& index;
    std::span<const uint32_t> callSiteOffsets;
    std::span<const uint32_t> sideTableOffsets;
    const ProgramTables& programTables;
    bool hasPositions;
};

struct EncodedFunctionTables {
    Vector<uint8_t> functions;
    Vector<uint8_t> functionStarts;
    Vector<uint8_t> codeGranules;
    Vector<uint8_t> functionsByID;
    Vector<uint8_t> innerEntries;
    Vector<uint8_t> frames;
    Vector<uint8_t> closureWords;
};

EncodedFunctionTables encodeFunctionTables(const FunctionTableInputs&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
