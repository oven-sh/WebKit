/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "ImmutablePropertyNames.h"

#include "PropertyName.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImmutablePropertyNames);

namespace {

class EitherOutsideNames final : public ImmutablePropertyNames::OutsideNames {
public:
    EitherOutsideNames(Ref<const OutsideNames>&& first, Ref<const OutsideNames>&& second)
        : m_first(WTF::move(first))
        , m_second(WTF::move(second))
    {
    }

    bool contains(UniquedStringImpl* uid) const final { return m_first->contains(uid) || m_second->contains(uid); }
    String storeErrorMessage(UniquedStringImpl* uid) const final { return (m_first->contains(uid) ? m_first : m_second)->storeErrorMessage(uid); }
    String deleteErrorMessage(UniquedStringImpl* uid) const final { return (m_first->contains(uid) ? m_first : m_second)->deleteErrorMessage(uid); }
    String setPrototypeErrorMessage() const final
    {
        String message = m_first->setPrototypeErrorMessage();
        return message.isNull() ? m_second->setPrototypeErrorMessage() : message;
    }
    bool throwsInSloppyMode() const final { return m_first->throwsInSloppyMode() || m_second->throwsInSloppyMode(); }
    bool throwsFromReflect() const final { return m_first->throwsFromReflect() || m_second->throwsFromReflect(); }
    void willWrite(UniquedStringImpl* uid) const final
    {
        m_first->willWrite(uid);
        m_second->willWrite(uid);
    }

private:
    Ref<const OutsideNames> m_first;
    Ref<const OutsideNames> m_second;
};

} // anonymous namespace

Ref<ImmutablePropertyNames> ImmutablePropertyNames::createUnion(const ImmutablePropertyNames& first, const ImmutablePropertyNames& second)
{
    IdentifierSet names = first.m_names;
    for (auto& name : second.m_names)
        names.add(name);
    IdentifierSet openStrings;
    for (auto& string : first.m_openStrings) {
        if (!second.contains(string.get()))
            openStrings.add(string);
    }
    for (auto& string : second.m_openStrings) {
        if (!first.contains(string.get()))
            openStrings.add(string);
    }
    RefPtr<const OutsideNames> outsideNames = first.m_outsideNames ? first.m_outsideNames : second.m_outsideNames;
    if (second.m_outsideNames && second.m_outsideNames != outsideNames)
        outsideNames = adoptRef(*new EitherOutsideNames(outsideNames.releaseNonNull(), *second.m_outsideNames));
    return create(WTF::move(names), first.parts() | second.parts(), WTF::move(openStrings), WTF::move(outsideNames));
}

bool ImmutablePropertyNames::containsAll(const ImmutablePropertyNames& other) const
{
    if (!m_parts.containsAll(other.m_parts))
        return false;
    if (other.m_outsideNames && other.m_outsideNames != m_outsideNames)
        return false;
    for (auto& name : other.m_names) {
        if (!contains(name.get()))
            return false;
    }
    for (auto& string : m_openStrings) {
        if (!contains(string.get()) && other.contains(string.get()))
            return false;
    }
    return true;
}

bool ImmutablePropertyNames::isNumericKey(UniquedStringImpl* uid)
{
    return isCanonicalNumericIndexString(uid);
}

} // namespace JSC
