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

class ScratchRealm;

struct FormatterOption {
    String name;
    FormatterOptionKind kind;
    String string;
    double number { 0 };
};

struct FormatterRequest {
    FormatPatternKind kind;
    String locale;
    bool hasOptions { false };
    WTF::Vector<FormatterOption, 8> options { };
    IntlDateTimeFormat::RequiredComponent required { IntlDateTimeFormat::RequiredComponent::Any };
    IntlDateTimeFormat::Defaults defaults { IntlDateTimeFormat::Defaults::Date };
};

class FormatPatternPlans {
    WTF_MAKE_NONCOPYABLE(FormatPatternPlans);
public:
    FormatPatternPlans() = default;

    const WTF::Vector<uint32_t>* planFor(ScratchRealm& ofThisThread, const FormatterRequest&);

private:
    Lock m_lock;
    HashMap<String, std::unique_ptr<WTF::Vector<uint32_t>>> m_plans WTF_GUARDED_BY_LOCK(m_lock);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
