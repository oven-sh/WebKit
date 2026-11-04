/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <array>
#include <wtf/SHA1.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

// The same for the same characters, whether they come in 8 or in 16 bits. The text is fixed when the program is built, so another text with its digest is a second preimage of SHA-1: about 2^160 tries.
inline std::array<uint8_t, SHA1::hashSize> digestOfLiteralString(StringView text)
{
    SHA1 sha1;
    bool fitsLatin1 = text.is8Bit() || std::ranges::all_of(text.span16(), [](char16_t character) { return character <= 0xff; });
    sha1.addBytes(std::array<uint8_t, 1> { fitsLatin1 });
    if (text.is8Bit())
        sha1.addBytes(asBytes(text.span8()));
    else if (fitsLatin1) {
        Vector<uint8_t, 64> characters(text.length(), [&](size_t index) {
            return static_cast<uint8_t>(text.span16()[index]);
        });
        sha1.addBytes(characters.span());
    } else
        sha1.addBytes(asBytes(text.span16()));
    SHA1::Digest digest;
    sha1.computeHash(digest);
    return digest;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
