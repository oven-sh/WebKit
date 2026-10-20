/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PropertyRun.h"

#include "Identifier.h"
#include <wtf/HashFunctions.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyRun);
WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyRunTable);

unsigned PropertyRun::computeHash(std::span<UniquedStringImpl* const> names)
{
    unsigned hash = names.size();
    for (UniquedStringImpl* name : names)
        hash = WTF::pairIntHash(hash, PtrHash<UniquedStringImpl*>::hash(name));
    return hash;
}

PropertyRun::PropertyRun(std::span<UniquedStringImpl* const> names, unsigned hash)
    : m_names(FixedVector<RefPtr<UniquedStringImpl>>::createWithSizeFromGenerator(names.size(), [&](size_t index) {
        return RefPtr<UniquedStringImpl> { names[index] };
    }))
    , m_transitionKey(SymbolImpl::createNullSymbol())
    , m_hash(hash)
{
}

struct PropertyRunTable::NamesTranslator {
    static unsigned hash(const std::span<UniquedStringImpl* const>& names) { return PropertyRun::computeHash(names); }

    static bool equal(const Ref<PropertyRun>& run, const std::span<UniquedStringImpl* const>& names)
    {
        if (run->size() != names.size())
            return false;
        for (size_t i = 0; i < names.size(); ++i) {
            if (run->names()[i].get() != names[i])
                return false;
        }
        return true;
    }
};

const PropertyRun* PropertyRunTable::find(std::span<UniquedStringImpl* const> names) const
{
    auto iterator = m_runs.find<NamesTranslator>(names);
    return iterator == m_runs.end() ? nullptr : iterator->ptr();
}

std::span<const PropertyRun* const> PropertyRunTable::runsEndingWith(UniquedStringImpl* lastName) const
{
    auto iterator = m_runsByLastName.find(lastName);
    if (iterator == m_runsByLastName.end())
        return { };
    return iterator->value.span();
}

const PropertyRun* PropertyRunTable::intern(std::span<UniquedStringImpl* const> names)
{
    if (names.size() < 2 || names.size() > PropertyRun::maxSize)
        return nullptr;
    for (size_t i = 0; i < names.size(); ++i) {
        if (!names[i] || (!names[i]->isSymbol() && parseIndex(*names[i])))
            return nullptr;
        for (size_t j = 0; j < i; ++j) {
            if (names[j] == names[i])
                return nullptr;
        }
    }

    if (const PropertyRun* existing = find(names))
        return existing;
    Ref<PropertyRun> run = adoptRef(*new PropertyRun(names, PropertyRun::computeHash(names)));
    const PropertyRun* result = run.ptr();
    m_runs.add(WTF::move(run));
    auto& runsWithLastName = m_runsByLastName.add(names.back(), Vector<const PropertyRun*, 1> { }).iterator->value;
    size_t position = runsWithLastName.findIf([&](const PropertyRun* other) {
        return other->size() > result->size();
    });
    runsWithLastName.insert(position == notFound ? runsWithLastName.size() : position, result);
    return result;
}

} // namespace JSC
