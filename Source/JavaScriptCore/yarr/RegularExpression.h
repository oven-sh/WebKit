/*
 * Copyright (C) 2003-2023 Apple Inc. All rights reserved.
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

#include "YarrFlags.h"
#include <wtf/OptionSet.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace Yarr {

enum class MatchMode : uint8_t {
    Partial, // Default: match anywhere in the string.
    EntireInput // Anchored: pattern must match the entire input (equivalent to ^(?:pattern)$).
};

enum class MatchStatus : uint8_t {
    NoMatch,
    Match,
    // The search ran out of steps (Options::regExpMatchLimit) or of backtracking memory
    // (Options::maxRegExpStackSize) before it had an answer. The pattern may match: this is not a miss.
    Abandoned,
};

class JS_EXPORT_PRIVATE RegularExpression {
    WTF_MAKE_TZONE_ALLOCATED(RegularExpression);
public:
    explicit RegularExpression(StringView, OptionSet<Flags> = { }, MatchMode = MatchMode::Partial);
    ~RegularExpression();

    RegularExpression(const RegularExpression&);
    RegularExpression& operator=(const RegularExpression&);
    RegularExpression(RegularExpression&&);
    RegularExpression& operator=(RegularExpression&&);

    // On Match, position and matchLength describe the match.
    [[nodiscard]] MatchStatus match(StringView, unsigned startFrom, int& position, int& matchLength) const;
#if !USE(BUN_JSC_ADDITIONS)
    // Returns -1 for an abandoned search too, so its caller reads it as a miss. Nothing Bun builds
    // may do that; the WebCore callers Bun does not build still use this.
    int match(StringView, unsigned startFrom = 0, int* matchLength = nullptr) const;
#endif
    int searchRev(StringView) const;

    int LEGACY_NODELETE matchedLength() const;
    bool LEGACY_NODELETE isValid() const;

private:
    class Private;
    RefPtr<Private> d;
};

void JS_EXPORT_PRIVATE replace(String&, const RegularExpression&, StringView);

} } // namespace JSC::Yarr
