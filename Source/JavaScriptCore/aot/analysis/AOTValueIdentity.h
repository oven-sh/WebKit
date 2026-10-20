/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTEffectsDeclared.h"
#include <cstddef>
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
struct Subject;

namespace Typed {

class UseLists;

constexpr bool isCast(Opcode opcode) { return interpretedOf(opcode) == Interpretation::Cast; }
constexpr bool isCastWithoutTest(Opcode opcode) { return isCast(opcode) && !mayThrow(opcode); }
constexpr bool isCastOrConversion(Opcode opcode) { return isCast(opcode) || familyOf(opcode) == OperationFamily::Representation; }

ValueID sameObjectThroughCasts(const Function&, ValueID);
ValueID definitionThroughUpcasts(const Function&, ValueID);

class CastChain {
public:
    class Iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = ValueID;

        Iterator() = default;
        Iterator(const Function& function, ValueID value, ValueID last)
            : m_function(&function)
            , m_value(value)
            , m_last(last)
        {
        }

        ValueID operator*() const { return m_value; }

        Iterator& operator++()
        {
            m_value = m_value == m_last ? ValueID() : m_function->at(m_value).operand(0);
            return *this;
        }

        Iterator operator++(int)
        {
            Iterator previous = *this;
            ++*this;
            return previous;
        }

        friend bool operator==(const Iterator& a, const Iterator& b) { return a.m_value == b.m_value; }

    private:
        const Function* m_function { nullptr };
        ValueID m_value;
        ValueID m_last;
    };

    CastChain(const Function& function, ValueID value)
        : m_function(function)
        , m_first(value)
        , m_last(sameObjectThroughCasts(function, value))
    {
    }

    Iterator begin() const { return Iterator(m_function, m_first, m_last); }
    Iterator end() const { return { }; }
    ValueID last() const { return m_last; }

private:
    const Function& m_function;
    ValueID m_first;
    ValueID m_last;
};

class ValueIdentities {
    WTF_MAKE_NONCOPYABLE(ValueIdentities);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueIdentities);
public:
    ValueIdentities(const Function&, const CFG&);

    ValueID identityOf(ValueID id) const
    {
        RELEASE_ASSERT(id.index() < m_identities.size());
        return m_identities[id.index()];
    }

    bool areSameValue(ValueID a, ValueID b) const { return identityOf(a) == identityOf(b); }
    unsigned numberOfValues() const { return m_identities.size(); }

private:
    using MergingParameters = UncheckedKeyHashMap<ValueID, Vector<uint32_t, 1>>;

    ValueID identityOfParameter(const Function&, const CFG&, BlockID, uint32_t parameterIndex, MergingParameters&);
    bool receiveSameValues(const Function&, const CFG&, BlockID, uint32_t earlierIndex, uint32_t parameterIndex) const;

    Vector<ValueID> m_identities;
    bool m_didReadUnknownIdentity { false };
};

Vector<ValueID, 2> findSingleUseCastChain(const Function&, const UseLists&, ValueID user);
std::optional<ValueID> constantThroughCasts(const Program&, const Function&, ValueID);
std::optional<int64_t> integerConstantThroughCasts(const Program&, const Function&, ValueID);
bool isRematerializable(const Value&);
bool isDeferredClosure(const Function&, ValueID);
bool hasCodeAtDefinition(const Function&, ValueID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
