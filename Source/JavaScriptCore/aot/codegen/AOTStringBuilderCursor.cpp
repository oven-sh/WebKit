/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderCursor.h"

#if ENABLE(AOT)

#include <limits>
#include <optional>
#include <wtf/MathExtras.h>
#include <wtf/text/StringBuilderJSON.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxJSONKeyPunctuationLength = 4;
static constexpr uint32_t exceedsEveryBuilderReservation = std::numeric_limits<uint32_t>::max();

static_assert(exceedsEveryBuilderReservation > maxStringBuilderReservation);

static size_t bytesReservedForJSONKey(size_t escapedLength)
{
    return WTF::roundUpToMultipleOf<8>(escapedLength + maxJSONKeyPunctuationLength);
}

static std::optional<CursorBytes> escapedJSONKey(const TypeArena& types, StringID name)
{
    if (!name)
        return std::nullopt;
    Vector<char16_t> decodedName;
    StringView text = types.viewOf(name, decodedName);
    if (text.length() > maxStringBuilderReservation)
        return std::nullopt;

    Vector<Latin1Character, 256> buffer(6 * text.length() + 32);
    std::span<Latin1Character> output = buffer.mutableSpan();
    bool isLatin1 = text.is8Bit() ? WTF::appendEscapedJSONStringContent(output, text.span8()) : WTF::appendEscapedJSONStringContent(output, text.span16());
    size_t escapedLength = buffer.size() - output.size();
    if (!isLatin1 || bytesReservedForJSONKey(escapedLength) > maxStringBuilderReservation)
        return std::nullopt;
    return CursorBytes(buffer.span().first(escapedLength));
}

bool canWriteJSONKeyAtCursor(const TypeArena& types, StringID key)
{
    return escapedJSONKey(types, key).has_value();
}

CursorBytes bytesOfJSONKey(const TypeArena& types, StringID key, bool opensObject, bool followsMember)
{
    RELEASE_ASSERT(!(opensObject && followsMember));
    std::optional<CursorBytes> escapedName = escapedJSONKey(types, key);
    RELEASE_ASSERT(escapedName);

    CursorBytes bytes;
    if (opensObject)
        bytes.append('{');
    else if (followsMember)
        bytes.append(',');
    bytes.append('"');
    bytes.appendVector(*escapedName);
    bytes.append("\":"_s.span8());
    return bytes;
}

std::span<const Latin1Character> bytesOfJSONToken(JSONToken token)
{
    switch (token) {
    case JSONToken::OpenObject:
        return "{"_s.span8();
    case JSONToken::CloseObject:
    case JSONToken::CloseObjectOverComma:
        return "}"_s.span8();
    case JSONToken::OpenArray:
        return "["_s.span8();
    case JSONToken::CloseArray:
        return "]"_s.span8();
    case JSONToken::Comma:
        return ","_s.span8();
    case JSONToken::Colon:
        return ":"_s.span8();
    case JSONToken::Null:
        return "null"_s.span8();
    case JSONToken::EmptyObject:
        return "{}"_s.span8();
    case JSONToken::EmptyArray:
        return "[]"_s.span8();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t bytesTakenFromReservation(const TypeArena& types, Opcode cursorOperation, StringID key)
{
    switch (cursorOperation) {
    case Opcode::StringBuilderReserve:
    case Opcode::StringBuilderCommit:
    case Opcode::JSONAppendString:
        return 0;
    case Opcode::JSONAppendKey: {
        std::optional<CursorBytes> escapedName = escapedJSONKey(types, key);
        return escapedName ? static_cast<uint32_t>(bytesReservedForJSONKey(escapedName->size())) : exceedsEveryBuilderReservation;
    }
    case Opcode::JSONAppendToken:
        return jsonTokenBytes;
    case Opcode::JSONAppendBoolean:
        return jsonBooleanBytes;
    case Opcode::JSONAppendInteger:
        return jsonIntegerBytes;
    case Opcode::JSONAppendNumber:
    case Opcode::JSONAppendValue:
        return jsonNumberBytes;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
