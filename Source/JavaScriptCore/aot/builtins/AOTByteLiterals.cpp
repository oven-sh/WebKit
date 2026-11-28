/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTByteLiterals.h"

#if ENABLE(AOT)

#include "JSGenericTypedArrayViewConstructor.h"
#include <wtf/text/Base64.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

static String literalAsRuntimeString(const TypeArena& types, StringID id)
{
    Vector<char16_t> buffer;
    String string = types.viewOf(id, buffer).toString();
    return string.is8Bit() || !string.containsOnlyLatin1() ? string : String::make8Bit(string.span16());
}

std::optional<Vector<uint8_t>> encodeByteLiteral(const TypeArena& types, const HostStringEncoders& host, ByteLiteralKind kind, StringID stringID, StringID encodingOrNull)
{
    String string = literalAsRuntimeString(types, stringID);
    Vector<uint8_t> bytes;
    switch (kind) {
    case ByteLiteralKind::BufferFrom: {
        if (!host.encodeForBuffer)
            return std::nullopt;
        String encoding = encodingOrNull ? literalAsRuntimeString(types, encodingOrNull) : String();
        auto append = [](void* sink, std::span<const uint8_t> encoded) {
            static_cast<Vector<uint8_t>*>(sink)->append(encoded);
        };
        if (!host.encodeForBuffer(string, encoding, &bytes, append))
            return std::nullopt;
        return bytes;
    }
    case ByteLiteralKind::Uint8ArrayFromBase64: {
        bytes.grow(maxLengthFromBase64(string));
        auto [shouldThrowError, readLength, writeLength] = fromBase64(string, bytes.mutableSpan(), WTF::Alphabet::Base64, WTF::LastChunkHandling::Loose, WTF::OutputSizeIsMaxLength::No);
        if (shouldThrowError == WTF::FromBase64ShouldThrowError::Yes)
            return std::nullopt;
        bytes.shrink(writeLength);
        return bytes;
    }
    case ByteLiteralKind::Uint8ArrayFromHex: {
        if (string.length() % 2)
            return std::nullopt;
        bytes.grow(string.length() / 2);
        size_t firstInvalid = string.is8Bit() ? decodeHex(string.span8(), bytes.mutableSpan()) : decodeHex(string.span16(), bytes.mutableSpan());
        if (firstInvalid != WTF::notFound)
            return std::nullopt;
        return bytes;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
