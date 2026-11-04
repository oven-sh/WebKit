/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

enum class CheckKind : uint8_t { Plain, Next, Not, Label, Count };

struct PatternElement {
    enum class Kind : uint8_t { Text, Blanks, AnyText, Variable };

    Kind kind;
    String text;
};

struct CheckLine {
    CheckKind kind { CheckKind::Plain };
    unsigned count { 1 };
    unsigned line { 0 };
    String pattern;
    Vector<PatternElement> elements;
};

struct CheckLineError {
    unsigned line;
    String message;
};

enum class CheckFailureKind : uint8_t { NoMatch, NotOnNextLine, ExcludedTextFound, LabelNotFound };

struct CheckFailure {
    CheckFailureKind kind;
    unsigned line;
    String message;
};

std::optional<String> parseCheckPattern(StringView pattern, Vector<PatternElement>& elements);
std::optional<CheckLineError> validateCheckLines(std::span<const CheckLine>);
std::optional<CheckFailure> matchCheckLines(std::span<const CheckLine>, StringView output);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
