/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSString.h"
#include "MarkedBlock.h"
#include <span>

namespace JSC { namespace AOT {

struct Instance;

// `text += piece`, again and again. As ropes, 32 bytes and the piece stay alive for each `+=` until somebody reads the text: 20 million of them are 600 MB.
// So a text that was added to this many times, each time as a rope that begins with what was there, is copied once into a buffer with as much room again behind the characters, and the text is a substring of the buffer.
// Whoever adds to a text that ends where the characters of its buffer end writes the piece behind them and gets a longer substring of the same buffer.
// No string changes by that: an older text names its own characters alone, and adding to it makes a rope as ever. What is left behind by each `+=` is 32 bytes that nothing names.
// The stubs write short flat pieces by themselves. Whatever else they are handed they join as a rope, as ever, and the operation that they call when they are out of cells,
// which is once in some hundred ropes, writes the whole row of ropes behind the text: the way out of a stub costs more than ten such pieces.
// It goes by how often, never by how long. The ropes of a text that was added to fewer times than a block has cells weigh less than a block, while a buffer holds all the characters once more:
// `host + ":" + certificates.join() + ":" + ..` is 400,000 characters in thirty ropes.
static constexpr uint32_t minAdditionsToGrowingText = MarkedBlock::blockSize / sizeof(JSRopeString);
// Each addition brought a character.
static constexpr uint32_t minLengthOfGrowingText = minAdditionsToGrowingText;
// The stubs that join strings write a flat 8-bit piece of up to this length into an 8-bit buffer by themselves, a byte at a time. From here on the call of the operation, which copies words, costs less.
static constexpr uint32_t maxLengthOfPieceWrittenByStub = 16;
// The index at which the text in a buffer ends lies in its first characters.
static constexpr ptrdiff_t offsetOfEndOfTextInCharactersOfBuffer = 0;

// `text` is a rope of at least minLengthOfGrowingText. Null, and no exception: they are to be joined as ropes.
JS_EXPORT_PRIVATE JSString* tryAppendToGrowingText(Instance&, JSString* text, std::span<JSString* const> pieces);

// What `left + right` gives for two strings. Null: an exception was thrown.
ALWAYS_INLINE JSString* joinTexts(JSGlobalObject* globalObject, Instance& instance, JSString* left, JSString* right)
{
    if (left->isRope() && left->length() >= minLengthOfGrowingText) {
        if (JSString* grown = tryAppendToGrowingText(instance, left, std::span<JSString* const> { &right, 1 }))
            return grown;
    }
    return jsString(globalObject, left, right);
}

// The text is a substring of a buffer and ends where the characters of the buffer end. For tests.
JS_EXPORT_PRIVATE bool canGrowInPlace(Instance&, JSString* text);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
