/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFormatPatterns.h"
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

class ScratchRealms;

struct FormatterOption {
    String name;
    FormatterOptionKind kind;
    String string;
    double number { 0 };
};

// `new Intl.DateTimeFormat("en-US", { year: "numeric" })`, `new Intl.NumberFormat("de")`, or the formatter that a call of toLocaleString makes for itself.
struct FormatterRequest {
    FormatPatternKind kind;
    String locale;
    bool hasOptions { false };
    WTF::Vector<FormatterOption, 8> options { };
    IntlDateTimeFormat::RequiredComponent required { IntlDateTimeFormat::RequiredComponent::Any };
    IntlDateTimeFormat::Defaults defaults { IntlDateTimeFormat::Defaults::Date };
};

// The patterns of a build. Each request is planned once, whichever thread asks first.
class FormatPatternPlans {
    WTF_MAKE_NONCOPYABLE(FormatPatternPlans);
public:
    FormatPatternPlans() = default;

    // A FormatPatternHeader and what follows it, in words. Null where ICU has to format: a calendar, digits, a field or a rounding that a pattern does not have,
    // or a pattern that writes another text than ICU for one of the samples.
    const WTF::Vector<uint32_t>* planFor(ScratchRealms&, const FormatterRequest&);

private:
    Lock m_lock;
    HashMap<String, std::unique_ptr<WTF::Vector<uint32_t>>> m_plans WTF_GUARDED_BY_LOCK(m_lock);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
