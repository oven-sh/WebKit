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
    unsigned same = 0;
    while (same < written.length() && same < genericOutput.length() && written[same] == genericOutput[same])
        ++same;
    unsigned start = same > 20 ? same - 20 : 0;
    dataLogLn("AOT: the quick writer of JSON answered ", written.length(), " characters, and the general way writes ", genericOutput.length(), ": they part at ", same, ": ", written.substring(start, 60), " | ", genericOutput.substring(start, 60), ". This is a bug in Bun.");
    CRASH();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
