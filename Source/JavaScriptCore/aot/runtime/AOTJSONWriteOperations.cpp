/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTJSONHooks.h"
#include "AOTJSONTextWriter.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTStringBuilderStorage.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "JSONObject.h"
#include "NumberPrototype.h"
#include "NumericStrings.h"
#include <array>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/EscapedFormsForJSON.h>
#include <wtf/text/StringBuilderJSON.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr size_t numberOfQuotes = 2;
static constexpr size_t maxEscapedCharacterLength = 6;

static_assert(maxEscapedJSONStringLength(1) == maxEscapedCharacterLength + numberOfQuotes && maxEscapedJSONStringLength(0) == numberOfQuotes);

Latin1Character* appendLongJSONInteger(Latin1Character* cursor, int64_t integer)
{
    if (integer != static_cast<int32_t>(integer)) {
        char* to = std::bit_cast<char*>(cursor);
        return std::bit_cast<Latin1Character*>(std::to_chars(to, to + maxJSONIntegerLength, integer).ptr);
    }
    uint32_t magnitude = static_cast<uint32_t>(integer);
    if (integer < 0) {
        *cursor++ = '-';
        magnitude = 0 - magnitude;
    }
    if (magnitude < 10000)
        return appendUpToFourJSONDigits(cursor, magnitude);
    if (magnitude < 100000000)
        return appendFourJSONDigits(appendUpToFourJSONDigits(cursor, magnitude / 10000), magnitude % 10000);
    uint32_t low = magnitude % 100000000;
    cursor = appendUpToFourJSONDigits(cursor, magnitude / 100000000);
    return appendFourJSONDigits(appendFourJSONDigits(cursor, low / 10000), low % 10000);
}

NEVER_INLINE Latin1Character* appendQuotedAndEscapedJSONString(Latin1Character* cursor, std::span<const Latin1Character> characters)
{
    *cursor++ = '"';
    std::span<Latin1Character> output { cursor, characters.size() * maxEscapedCharacterLength };
    WTF::appendEscapedJSONStringContent(output, characters);
    cursor = output.data();
    *cursor++ = '"';
    return cursor;
}

void crashOnJSONOutputMismatch(const String& written, const String& genericOutput)
{
    RuntimeCheckFailure failure("JSON.stringify check failed"_s);
    failure.detail("the fast path and the generic path produced ", written.length() == genericOutput.length() ? "different strings of the same length"_s : "strings of different lengths"_s);
    failure.report();
    CRASH();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
