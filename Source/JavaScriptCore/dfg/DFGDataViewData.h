/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
 * Copyright (C) 2018-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <cstdint>
#include <wtf/TriState.h>

namespace JSC { namespace DFG {

// The descriptor for a typed memory load/store: which width, signedness and (for stores/float loads)
// float-ness, plus endianness/resizability for the DataView case. Packed into one 64-bit OpInfo.
// Shared by the DataView nodes (DataViewGetInt/GetFloat/Set) and, under USE(BUN_JSC_ADDITIONS),
// bun:ffi's raw-memory reader node (FFIRawRead) -- hence its own dependency-free header.
struct DataViewData {
    union {
        struct {
            uint8_t byteSize;
            bool isSigned;
            bool isResizable;
            bool isFloatingPoint; // Used for the DataViewSet node.
            TriState isLittleEndian;
        };
        uint64_t asQuadWord;
    };
};
static_assert(sizeof(DataViewData) == sizeof(uint64_t));

} } // namespace JSC::DFG
