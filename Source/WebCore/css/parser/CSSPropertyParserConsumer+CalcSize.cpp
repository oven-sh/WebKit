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

#include "config.h"
#include "CSSPropertyParserConsumer+CalcSize.h"

#include "CSSCalcSizeValue.h"
#include "CSSCalcSymbolsAllowed.h"
#include "CSSCalcTree+Parser.h"
#include "CSSCalcTree+Simplification.h"
#include "CSSCalcValue.h"
#include "CSSParserContext.h"
#include "CSSParserTokenRange.h"
#include "CSSPrimitiveNumericCategory.h"
#include "CSSPrimitiveNumericRange.h"
#include "CSSPropertyParserConsumer+Primitives.h"
#include "CSSPropertyParserState.h"
#include "CSSUnits.h"
#include "CSSValueKeywords.h"
#include <wtf/UniqueRef.h>

namespace WebCore {
namespace CSSPropertyParserHelpers {

static bool isValidBasisKeyword(CSSValueID keyword, CSSPropertyID property)
{
    switch (keyword) {
    case CSSValueAuto:
        switch (property) {
        case CSSPropertyMaxWidth:
        case CSSPropertyMaxHeight:
        case CSSPropertyMaxBlockSize:
        case CSSPropertyMaxInlineSize:
            return false;
        default:
            return true;
        }
    case CSSValueContent:
        return property == CSSPropertyFlexBasis;
    default:
        return true;
    }
}

static std::optional<CSS::CalcSizeBasis> consumeBasisKeyword(CSSParserTokenRange& args, CSS::PropertyParserState& state)
{
    auto keyword = args.peek().id();
    if (!isValidBasisKeyword(keyword, state.currentProperty))
        return { };

    auto consume = [&]<typename Keyword>(Keyword) -> std::optional<CSS::CalcSizeBasis> {
        args.consumeIncludingWhitespace();
        return CSS::CalcSizeBasis { Keyword { } };
    };

    switch (keyword) {
    case CSSValueAny:
        return consume(CSS::Keyword::Any { });
    case CSSValueAuto:
        return consume(CSS::Keyword::Auto { });
    case CSSValueContent:
        return consume(CSS::Keyword::Content { });
    case CSSValueMinContent:
        return consume(CSS::Keyword::MinContent { });
    case CSSValueWebkitMinContent:
        return consume(CSS::Keyword::WebkitMinContent { });
    case CSSValueMaxContent:
        return consume(CSS::Keyword::MaxContent { });
    case CSSValueWebkitMaxContent:
        return consume(CSS::Keyword::WebkitMaxContent { });
    case CSSValueFitContent:
        return consume(CSS::Keyword::FitContent { });
    case CSSValueWebkitFitContent:
        return consume(CSS::Keyword::WebkitFitContent { });
    case CSSValueStretch:
        return consume(CSS::Keyword::Stretch { });
    case CSSValueWebkitFillAvailable:
        return consume(CSS::Keyword::WebkitFillAvailable { });
    case CSSValueIntrinsic:
        return consume(CSS::Keyword::Intrinsic { });
    case CSSValueMinIntrinsic:
        return consume(CSS::Keyword::MinIntrinsic { });
    default:
        return { };
    }
}

enum class SizeKeywordPolicy : bool { Forbid, Allow };

// The range is unrestricted even for properties that only accept non-negative sizes, since the
// calculation may go negative internally and is clamped when resolved.
static std::optional<CSS::CalcSizeCalculation> consumeCalcSum(CSSParserTokenRange& args, CSS::PropertyParserState& state, SizeKeywordPolicy sizeKeywordPolicy)
{
    auto parserOptions = CSSCalc::ParserOptions {
        .category = CSS::Category::LengthPercentage,
        .range = CSS::All,
        .allowedSymbols = sizeKeywordPolicy == SizeKeywordPolicy::Allow ? CSSCalcSymbolsAllowed { { CSSValueSize, CSSUnitType::Px } } : CSSCalcSymbolsAllowed { },
        .propertyOptions = { }
    };
    auto simplificationOptions = CSSCalc::SimplificationOptions {
        .category = CSS::Category::LengthPercentage,
        .range = CSS::All,
        .conversionData = std::nullopt,
        .symbolTable = { },
        .allowZeroValueLengthRemovalFromSum = false,
    };

    return CSSCalc::parseAndSimplifyCalcSum(args, state, parserOptions, simplificationOptions);
}

static std::optional<CSS::CalcSizeFunction> consumeCalcSizeFunction(CSSParserTokenRange&, CSS::PropertyParserState&);

static std::optional<CSS::CalcSizeBasis> consumeCalcSizeBasis(CSSParserTokenRange& args, CSS::PropertyParserState& state)
{
    // <calc-size-basis> = [ <size-keyword> | <calc-sum> | <calc-size()> | any ]

    if (args.peek().type() == IdentToken) {
        if (auto keyword = consumeBasisKeyword(args, state))
            return keyword;
    }

    if (args.peek().functionId() == CSSValueCalcSize) {
        auto nested = consumeCalcSizeFunction(args, state);
        if (!nested)
            return { };
        return CSS::CalcSizeBasis { makeUniqueRef<CSS::CalcSizeFunction>(WTF::move(*nested)) };
    }

    auto basis = consumeCalcSum(args, state, SizeKeywordPolicy::Forbid);
    if (!basis)
        return { };
    return CSS::CalcSizeBasis { WTF::move(*basis) };
}

static std::optional<CSS::CalcSizeFunction> consumeCalcSizeFunction(CSSParserTokenRange& range, CSS::PropertyParserState& state)
{
    // <calc-size()> = calc-size( <calc-size-basis>, <calc-sum> )

    ASSERT(range.peek().functionId() == CSSValueCalcSize);

    auto rangeCopy = range;
    auto args = consumeFunction(rangeCopy);

    auto basis = consumeCalcSizeBasis(args, state);
    if (!basis)
        return { };

    if (!consumeCommaIncludingWhitespace(args))
        return { };

    // `size` is a syntax error when the basis is `any`, but is allowed when the basis is a nested
    // calc-size() whose own basis is `any`.
    auto sizeKeywordPolicy = std::holds_alternative<CSS::Keyword::Any>(*basis) ? SizeKeywordPolicy::Forbid : SizeKeywordPolicy::Allow;

    auto calculation = consumeCalcSum(args, state, sizeKeywordPolicy);
    if (!calculation || !args.atEnd())
        return { };

    range = rangeCopy;
    return CSS::CalcSizeFunction {
        CSS::CalcSizeFunctionValue {
            .parameters = CSS::CalcSizeParameters { WTF::move(*basis), WTF::move(*calculation) }
        }
    };
}

RefPtr<CSSValue> consumeCalcSize(CSSParserTokenRange& range, CSS::PropertyParserState& state)
{
    if (!state.context.cssCalcSizeFunctionEnabled)
        return { };

    if (range.peek().functionId() != CSSValueCalcSize)
        return { };

    auto calcSize = consumeCalcSizeFunction(range, state);
    if (!calcSize)
        return { };

    return CSSCalcSizeValue::create(WTF::move(*calcSize));
}

} // namespace CSSPropertyParserHelpers
} // namespace WebCore
