/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTRegExpRecord.h"
#include "AOTRegExpSection.h"
#include "AOTVector.h"

namespace JSC { namespace AOT {

struct RegExpRecordInputs {
    const RegExpRequests& requests;
    const CompiledRegExps& compiled;
    const RegExpSection& section;
};

// The match stub stores the start and the end of a match there for a RegExp without a Code record too: an atom, also one created at run time.
inline constexpr uint32_t minRegExpOutputSize = 2;

struct RegExpSections {
    Vector<uint8_t> records;
    Vector<uint8_t> hashes;
    Vector<uint8_t> details;
    uint32_t maxOutputSize { minRegExpOutputSize };
};

JS_EXPORT_PRIVATE RegExpSections encodeRegExpRecords(const RegExpRecordInputs&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
