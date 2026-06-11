/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "GetVM.h"
#include "JSGlobalObject.h"
#include "RegExp.h"
#include "RegExpGlobalData.h"
#include "RegExpInlines.h"

namespace JSC {

// UNGIL AUD1.K2/SD19 consumer re-point (TSAN wave 1, regexp-shared family):
// THE accessor for the RegExp legacy-statics match-result stream. Flag-off /
// GIL-on this is exactly globalObject->regExpGlobalData() behind a read-only
// Config-page test (the gilOffWithProcessGate idiom RegExpInlines.h's
// per-match paths already use — no out-of-line call, no semantic change);
// gilOff it resolves the CURRENT thread's per-(global, lite) stream via the
// slow path in JSGlobalObject.cpp, making record()'s multi-word update and
// the lazy reify flip single-thread-private (SD19: RegExp.$1-$9 etc.
// observe only the current thread's matches).
ALWAYS_INLINE RegExpGlobalData& threadRegExpGlobalData(JSGlobalObject* globalObject)
{
    if (!getVM(globalObject).gilOffWithProcessGate()) [[likely]]
        return globalObject->regExpGlobalData();
    return threadRegExpGlobalDataSlow(globalObject);
}

ALWAYS_INLINE void RegExpCachedResult::record(VM& vm, JSObject* owner, RegExp* regExp, JSString* input, MatchResult result, bool oneCharacterMatch)
{
    m_lastRegExp.setWithoutWriteBarrier(regExp);
    m_lastInput.setWithoutWriteBarrier(input);
    m_result = result;
    m_reified = false;
    m_oneCharacterMatch = oneCharacterMatch;
    vm.writeBarrier(owner);
}

inline void RegExpGlobalData::setInput(JSGlobalObject* globalObject, JSString* string)
{
    m_cachedResult.setInput(globalObject, globalObject, string);
}

/*
   To facilitate result caching, exec(), test(), match(), search(), and replace() dipatch regular
   expression matching through the performMatch function. We use cached results to calculate,
   e.g., RegExp.lastMatch and RegExp.leftParen.
*/
ALWAYS_INLINE MatchResult RegExpGlobalData::performMatch(JSGlobalObject* owner, RegExp* regExp, JSString* string, StringView input, int startOffset, int** ovector)
{
    ASSERT(owner);
    VM& vm = owner->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int position = regExp->match(owner, input, startOffset, regExp->ovectorSpan(vm));
    RETURN_IF_EXCEPTION(scope, MatchResult::failed());

    if (ovector)
        *ovector = regExp->ovectorSpan(vm).data();

    if (position == -1)
        return MatchResult::failed();

    auto ovectorSpan = regExp->ovectorSpan(vm);
    ASSERT(!ovectorSpan.empty());
    ASSERT(ovectorSpan[0] == position);
    ASSERT(ovectorSpan[1] >= position);
    size_t end = ovectorSpan[1];

    m_cachedResult.record(vm, owner, regExp, string, MatchResult(position, end), /* oneCharacterMatch */ false);

    return MatchResult(position, end);
}

ALWAYS_INLINE MatchResult RegExpGlobalData::performMatch(JSGlobalObject* owner, RegExp* regExp, JSString* string, StringView input, int startOffset)
{
    ASSERT(owner);
    VM& vm = owner->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MatchResult result = regExp->match(owner, input, startOffset);
    RETURN_IF_EXCEPTION(scope, MatchResult::failed());
    if (result)
        m_cachedResult.record(vm, owner, regExp, string, result, /* oneCharacterMatch */ false);
    return result;
}

ALWAYS_INLINE void RegExpGlobalData::recordMatch(VM& vm, JSGlobalObject* owner, RegExp* regExp, JSString* string, const MatchResult& result, bool oneCharacterMatch)
{
    ASSERT(result);
    m_cachedResult.record(vm, owner, regExp, string, result, oneCharacterMatch);
}

inline MatchResult RegExpGlobalData::matchResult() const
{
    return m_cachedResult.result();
}

inline void RegExpGlobalData::resetResultFromCache(JSGlobalObject* owner, RegExp* regExp, JSString* string, MatchResult matchResult, std::span<const int> ovector)
{
    auto dest = regExp->ovectorSpan(getVM(owner));
    ASSERT(dest.size() >= ovector.size());
    std::ranges::copy(ovector, dest.begin());
    m_cachedResult.record(getVM(owner), owner, regExp, string, matchResult, /* oneCharacterMatch */ false);
}

}
