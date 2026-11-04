/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLiteralStrings.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(literalStringWord(0) == emptyLiteralStringWord);
static_assert(maxLiteralStrings > 0);

// For planProgram(). No header declares it.
LiteralStringTable computeLiteralStrings(const Program&);

// Image names the texts of the strings that a type record lists and that have no word: nameWhatTypeRecordNames() of image/AOTTypeTables.cpp. So none is listed here without one.
LiteralStringTable computeLiteralStrings(const Program& program)
{
    StringID emptyString = program.strings.emptyString();
    RELEASE_ASSERT(emptyString);

    Vector<uint32_t> numbersPlusOne;
    numbersPlusOne.fill(0, emptyString.index() + 1);
    numbersPlusOne[emptyString.index()] = 1;
    return LiteralStringTable(Vector<StringID> { emptyString }, { }, WTF::move(numbersPlusOne));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
