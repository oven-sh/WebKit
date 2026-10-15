/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <algorithm>
#include <span>
#include <tuple>
#include <wtf/FastMalloc.h>
#include <wtf/OptionSet.h>
#include <wtf/Ref.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace JSC {

enum class AuditedValueClass : uint16_t {
    Undefined = 1 << 0,
    Null = 1 << 1,
    False = 1 << 2,
    True = 1 << 3,
    Number = 1 << 4,
    String = 1 << 5,
    Symbol = 1 << 6,
    BigInt = 1 << 7,
    Object = 1 << 8,
    Function = 1 << 9,
    // No class of its own: a plain object with a property that is not enumerable, as the audit's tag is, or whose prototype is a plain object.
    TaggedObject = 1 << 10,
};

struct AuditedExpression {
    uint32_t start;
    uint32_t end;
    uint32_t id;
    uint32_t passes;
    int32_t least { 1 };
    int32_t most { 0 };
};

struct AuditedParameter {
    uint32_t functionStart;
    uint32_t index;
    uint32_t id;
    uint32_t passes;
    int32_t least { 1 };
    int32_t most { 0 };
};

class AuditSites final : public ThreadSafeRefCounted<AuditSites> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(AuditSites);
public:
    static Ref<AuditSites> create(Vector<AuditedExpression>&& expressions, Vector<AuditedParameter>&& parameters)
    {
        return adoptRef(*new AuditSites(WTF::move(expressions), WTF::move(parameters)));
    }

    static constexpr OptionSet<AuditedValueClass> allValueClasses()
    {
        return OptionSet<AuditedValueClass>::fromRaw((1 << 10) - 1);
    }

    static constexpr OptionSet<AuditedValueClass> valueClasses(uint32_t passes)
    {
        return OptionSet<AuditedValueClass>::fromRaw(static_cast<uint16_t>(passes)) & (allValueClasses() | AuditedValueClass::TaggedObject);
    }

    std::span<const AuditedExpression> find(uint32_t start, uint32_t end) const
    {
        return std::span<const AuditedExpression>(std::ranges::equal_range(m_expressions.span(), AuditedExpression { start, end, 0, 0 }, isBefore));
    }

    std::span<const AuditedParameter> parametersOfFunction(uint32_t functionStart) const
    {
        return std::span<const AuditedParameter>(std::ranges::equal_range(m_parameters.span(), functionStart, { }, &AuditedParameter::functionStart));
    }

private:
    AuditSites(Vector<AuditedExpression>&& expressions, Vector<AuditedParameter>&& parameters)
        : m_expressions(WTF::move(expressions))
        , m_parameters(WTF::move(parameters))
    {
        std::ranges::stable_sort(m_expressions.mutableSpan(), isBefore);
        std::ranges::stable_sort(m_parameters.mutableSpan(), [](auto& a, auto& b) {
            return std::tie(a.functionStart, a.index) < std::tie(b.functionStart, b.index);
        });
    }

    static bool isBefore(const AuditedExpression& a, const AuditedExpression& b)
    {
        return std::tie(a.start, a.end) < std::tie(b.start, b.end);
    }

    Vector<AuditedExpression> m_expressions;
    Vector<AuditedParameter> m_parameters;
};

} // namespace JSC
