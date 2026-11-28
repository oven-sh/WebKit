/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSString.h"
#include "MarkedBlock.h"
#include <bit>
#include <span>
#include <wtf/UnalignedAccess.h>

namespace JSC { namespace AOT {

struct Instance;

static constexpr uint32_t minAdditionsToGrowingText = MarkedBlock::blockSize / sizeof(JSRopeString);
static constexpr uint32_t minLengthOfGrowingText = minAdditionsToGrowingText;
static constexpr uint32_t maxLengthOfPieceWrittenByStub = 256;
static constexpr uint32_t minLengthSharedWithBuffer = 64 * 1024;
static constexpr int32_t lengthWrittenAtOnce = 16;
static constexpr ptrdiff_t offsetOfEndOfTextInCharactersOfBuffer = 0;

static_assert(sizeof(JSRopeString) == 4 * sizeof(uint64_t) && JSRopeString::offsetOfFiber0() == sizeof(uint64_t) && JSRopeString::offsetOfLength() == 2 * sizeof(uint64_t) && JSRopeString::offsetOfFiber2() == 3 * sizeof(uint64_t));
static_assert(JSRopeString::CompactFibers::shiftOfFiber1LowerInLengthWord == 32 && JSRopeString::CompactFibers::widthOfFiber1LowerInLengthWord == 32 && JSRopeString::CompactFibers::shiftOfFiber2InUpperWord == 16);

ALWAYS_INLINE uint64_t wordOfRope(const JSString* rope, ptrdiff_t offset)
{
    return WTF::unalignedLoad<uint64_t>(std::bit_cast<const uint8_t*>(rope) + offset);
}

ALWAYS_INLINE JSString* firstPartOf(const JSString* rope) { return std::bit_cast<JSString*>(wordOfRope(rope, JSRopeString::offsetOfFiber0()) & JSRopeString::stringMask); }
ALWAYS_INLINE JSString* secondPartOf(const JSString* rope)
{
    constexpr uint64_t upperBitsOfSecondPart = (1ull << JSRopeString::CompactFibers::shiftOfFiber2InUpperWord) - 1;
    return std::bit_cast<JSString*>(wordOfRope(rope, JSRopeString::offsetOfLength()) >> JSRopeString::CompactFibers::shiftOfFiber1LowerInLengthWord | (wordOfRope(rope, JSRopeString::offsetOfFiber2()) & upperBitsOfSecondPart) << JSRopeString::CompactFibers::widthOfFiber1LowerInLengthWord);
}
ALWAYS_INLINE JSString* thirdPartOf(const JSString* rope) { return std::bit_cast<JSString*>(wordOfRope(rope, JSRopeString::offsetOfFiber2()) >> JSRopeString::CompactFibers::shiftOfFiber2InUpperWord); }

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
