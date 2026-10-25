/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(YARR_JIT)

#include "YarrCanonicalize.h"
#include "YarrPattern.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace Yarr {

enum class SharedTableID : uint8_t {
    BuiltInClassBitPlanes,
    Latin1Canonicalization,
    BMPCanonicalFormsUCS2,
    BMPCanonicalFormsUnicode,
};

inline constexpr std::array allSharedTableIDs {
    SharedTableID::BuiltInClassBitPlanes,
    SharedTableID::Latin1Canonicalization,
    SharedTableID::BMPCanonicalFormsUCS2,
    SharedTableID::BMPCanonicalFormsUnicode,
};

inline constexpr size_t sharedTableAlignment = 8;

JS_EXPORT_PRIVATE std::span<const uint8_t> sharedTable(SharedTableID);
std::optional<uint8_t> builtInClassBitPlane(CharacterClass::Table);
SharedTableID bmpCanonicalFormsTable(CanonicalMode);
const std::optional<Vector<CanonicalizationRange>>& rangeHiEntriesAboveBMP();

} } // namespace JSC::Yarr

#endif // ENABLE(YARR_JIT)
