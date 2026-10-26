/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderCursor.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <limits>
#include <optional>
#include <wtf/MathExtras.h>
#include <wtf/text/StringBuilderJSON.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxJSONKeyPunctuationLength = 4;
static constexpr uint32_t exceedsEveryReservation = std::numeric_limits<uint32_t>::max();

static_assert(exceedsEveryReservation > maxStringBuilderReservation);

static size_t bytesReservedForJSONKey(size_t escapedLength)
{
    return WTF::roundUpToMultipleOf<8>(escapedLength + maxJSONKeyPunctuationLength);
}

// Nothing for a name that is not written at the cursor.
static std::optional<CursorBytes> escapedJSONKey(const Program& program, StringID name)
{
    if (!name)
        return std::nullopt;
    Vector<char16_t> decodedName;
    StringView text = program.strings.viewOf(name, decodedName);
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

bool canWriteJSONKeyAtCursor(const Program& program, StringID key)
{
    return escapedJSONKey(program, key).has_value();
}

CursorBytes bytesOfJSONKey(const Program& program, const Value& appendKey)
{
    RELEASE_ASSERT(appendKey.opcode == Opcode::JSONAppendKey && !(appendKey.opensObject() && appendKey.followsMember()));
    std::optional<CursorBytes> escapedName = escapedJSONKey(program, appendKey.string());
    RELEASE_ASSERT(escapedName);

    CursorBytes bytes;
    if (appendKey.opensObject())
        bytes.append('{');
    else if (appendKey.followsMember())
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
    case JSONToken::Null:
        return "null"_s.span8();
    case JSONToken::EmptyObject:
        return "{}"_s.span8();
    case JSONToken::EmptyArray:
        return "[]"_s.span8();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t bytesTakenFromReservation(const Program& program, const Value& cursorRow)
{
    switch (cursorRow.opcode) {
    case Opcode::StringBuilderReserve:
    case Opcode::StringBuilderCommit:
    case Opcode::JSONAppendString:
        return 0;
    case Opcode::JSONAppendKey: {
        std::optional<CursorBytes> escapedName = escapedJSONKey(program, cursorRow.string());
        return escapedName ? static_cast<uint32_t>(bytesReservedForJSONKey(escapedName->size())) : exceedsEveryReservation;
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
    case Opcode::JSONAppendDate:
        return jsonDateBytes;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
