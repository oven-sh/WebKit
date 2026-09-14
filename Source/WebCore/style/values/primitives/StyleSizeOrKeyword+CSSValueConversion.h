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

#include "CSSCalcSizeValue.h"
#include "CSSCalcTree+Copy.h"
#include "CSSCalcValue.h"
#include "StylePrimitiveNumericOrKeyword+CSSValueConversion.h"
#include "StyleSizeOrKeyword.h"

namespace WebCore {
namespace Style {

// MARK: - <calc-size()> conversion

// FIXME: Stub until calc-size() resolves at used value time, when this will construct a calc-size()
// alternative rather than degrade to what the function is based on.
template<SizeOrKeywordDerived StyleType, typename ConversionState, typename... Rest>
auto convertCalcSizeForCSSValueConversion(ConversionState& conversionState, const CSS::CalcSizeParameters& parameters, Rest&&... rest) -> std::optional<StyleType>
{
    using CSSRaw = typename StyleType::Specified::CSS::Raw;

    auto convertCalculation = [&](const CSS::CalcSizeCalculation& calculation) -> std::optional<StyleType> {
        return StyleType { toStyle(CSS::UnevaluatedCalc<CSSRaw> { CSSCalc::Value::create(CSS::Category::LengthPercentage, CSS::All, CSSCalc::copy(calculation)) }, conversionState, std::forward<Rest>(rest)...) };
    };

    return WTF::switchOn(parameters.basis,
        [&](const CSS::Keyword::Any&) -> std::optional<StyleType> {
            return convertCalculation(parameters.calculation);
        },
        [&]<CSSValueID Id>(const Constant<Id>&) -> std::optional<StyleType> {
            return convertKeywordIDForCSSValueConversion<StyleType>(Id);
        },
        [&](const CSS::CalcSizeCalculation& basis) -> std::optional<StyleType> {
            return convertCalculation(basis);
        },
        [&](const UniqueRef<CSS::CalcSizeFunction>& nested) -> std::optional<StyleType> {
            return convertCalcSizeForCSSValueConversion<StyleType>(conversionState, nested->value.parameters, std::forward<Rest>(rest)...);
        }
    );
}

// MARK: - Conversion

// Only the CSSValue overloads need calc-size() handling.
template<SizeOrKeywordDerived StyleType> struct CSSValueConversion<StyleType> : NumericOrKeywordCSSValueConversion<StyleType> {
    using Base = NumericOrKeywordCSSValueConversion<StyleType>;
    using Base::operator();
    using Base::invalidValue;

    template<typename... Rest> auto operator()(const CSSToLengthConversionData& conversionData, const CSSValue& value, Rest&&... rest) -> StyleType
    {
        if (RefPtr calcSizeValue = dynamicDowncast<CSSCalcSizeValue>(value))
            return convertCalcSizeForCSSValueConversion<StyleType>(conversionData, calcSizeValue->calcSize()->parameters, std::forward<Rest>(rest)...).value_or(invalidValue());

        return Base::operator()(conversionData, value, std::forward<Rest>(rest)...);
    }

    template<typename... Rest> auto operator()(BuilderState& state, const CSSValue& value, Rest&&... rest) -> StyleType
    {
        if (RefPtr calcSizeValue = dynamicDowncast<CSSCalcSizeValue>(value)) {
            if (auto result = convertCalcSizeForCSSValueConversion<StyleType>(state, calcSizeValue->calcSize()->parameters, std::forward<Rest>(rest)...))
                return *result;
            state.setCurrentPropertyInvalidAtComputedValueTime();
            return invalidValue();
        }

        return Base::operator()(state, value, std::forward<Rest>(rest)...);
    }
};

} // namespace Style
} // namespace WebCore
