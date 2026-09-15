/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <WebCore/StylePrimitiveNumericOrKeyword.h>

namespace WebCore {
namespace Style {

// Base for the sizing properties, which accept `calc-size()` in addition to a <length-percentage>
// and their keywords. Being a distinct base keeps the calc-size() handling off the other
// <length-percentage>-or-keyword types.
template<CSS::SpecificKeyword... Ks>
struct SizeOrKeyword : PrimitiveNumericOrKeyword<LengthPercentage<CSS::NonnegativeLayoutUnitClamped>, Ks...> {
    using NumericOrKeyword = PrimitiveNumericOrKeyword<LengthPercentage<CSS::NonnegativeLayoutUnitClamped>, Ks...>;
    using Base = SizeOrKeyword<Ks...>;

    using NumericOrKeyword::NumericOrKeyword;
};

template<typename T> concept SizeOrKeywordDerived = WTF::IsBaseOfTemplate<SizeOrKeyword, T>::value && VariantLike<T>;

} // namespace Style
} // namespace WebCore
