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

static constexpr uint32_t minAdditionsToGrowingText = MarkedBlock::blockSize / sizeof(JSRopeString);
static constexpr uint32_t minLengthOfGrowingText = minAdditionsToGrowingText;
static constexpr uint32_t maxLengthOfPieceWrittenByStub = 256;
static constexpr uint32_t minLengthSharedWithBuffer = 64 * 1024;
static constexpr int32_t lengthWrittenAtOnce = 16;
static constexpr ptrdiff_t offsetOfEndOfTextInCharactersOfBuffer = 0;

JS_EXPORT_PRIVATE JSString* tryAppendToGrowingText(Instance&, JSString* text, std::span<JSString* const> pieces);

ALWAYS_INLINE JSString* joinTexts(JSGlobalObject* globalObject, Instance& instance, JSString* left, JSString* right)
{
    if (left->isRope() && left->length() >= minLengthOfGrowingText) {
        if (JSString* grown = tryAppendToGrowingText(instance, left, std::span<JSString* const> { &right, 1 }))
            return grown;
    }
    return jsString(globalObject, left, right);
}

JS_EXPORT_PRIVATE bool canGrowInPlace(Instance&, JSString* text);

bool tryResolveInGrowingBuffer(Instance&, JSRopeString*);
void noteResolvedText(Instance&, const JSString*);
bool tryResolveInOwnBuffer(Instance&, JSRopeString*);

ALWAYS_INLINE bool tryResolveLongText(Instance& instance, JSString* text)
{
    return text->length() >= minLengthSharedWithBuffer && text->isNonSubstringRope() && tryResolveInOwnBuffer(instance, uncheckedDowncast<JSRopeString>(text));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
