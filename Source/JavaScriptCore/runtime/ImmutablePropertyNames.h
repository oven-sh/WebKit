/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/Identifier.h>
#include <wtf/OptionSet.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class JSGlobalObject;

class ImmutablePropertyNames final : public RefCounted<ImmutablePropertyNames> {
    WTF_MAKE_TZONE_ALLOCATED_EXPORT(ImmutablePropertyNames, JS_EXPORT_PRIVATE);
public:
    enum class Part : uint8_t {
        // Every array index and every other canonical numeric string.
        NumericKeys = 1 << 0,
        Prototype = 1 << 1,
        // Every string that is not among the open strings. A symbol is immutable only when it is listed.
        OtherStrings = 1 << 2,
    };

    // Names kept elsewhere, in a form that costs nothing to set up. Checked last, so only a write that nothing else rejects gets here.
    class OutsideNames : public RefCounted<OutsideNames> {
    public:
        virtual ~OutsideNames() = default;
        virtual bool contains(UniquedStringImpl*) const = 0;
        // Error messages that name the object and the property. Null: the caller uses its generic message.
        virtual String storeErrorMessage(UniquedStringImpl*) const { return { }; }
        virtual String deleteErrorMessage(UniquedStringImpl*) const { return { }; }
        virtual String setPrototypeErrorMessage() const { return { }; }
        // A rejected store or delete throws in sloppy mode too, unless the spec rejects it anyway (a read-only or non-configurable property). Reflect still returns false.
        virtual bool throwsInSloppyMode() const { return false; }
        // Reflect.set(), Reflect.defineProperty() and Reflect.deleteProperty() throw as well, instead of returning false.
        virtual bool throwsFromReflect() const { return false; }
        // Called before a write to a name that is not among these and that nothing else rejects.
        virtual void willWrite(UniquedStringImpl*) const { }
    };

    static Ref<ImmutablePropertyNames> create(IdentifierSet&& names, OptionSet<Part> parts = { }, IdentifierSet&& openStrings = { }, RefPtr<const OutsideNames>&& outsideNames = nullptr) { return adoptRef(*new ImmutablePropertyNames(WTF::move(names), parts, WTF::move(openStrings), WTF::move(outsideNames))); }
    JS_EXPORT_PRIVATE static Ref<ImmutablePropertyNames> createUnion(const ImmutablePropertyNames&, const ImmutablePropertyNames&);

    bool contains(UniquedStringImpl* uid) const
    {
        if (m_names.contains(uid) || (m_parts.contains(Part::NumericKeys) && isNumericKey(uid)))
            return true;
        if (m_parts.contains(Part::OtherStrings) && !uid->isSymbol() && !m_openStrings.contains(uid))
            return true;
        return m_outsideNames && m_outsideNames->contains(uid);
    }
    String storeErrorMessage(UniquedStringImpl* uid) const { return m_outsideNames ? m_outsideNames->storeErrorMessage(uid) : String(); }
    String deleteErrorMessage(UniquedStringImpl* uid) const { return m_outsideNames ? m_outsideNames->deleteErrorMessage(uid) : String(); }
    String setPrototypeErrorMessage() const { return m_outsideNames ? m_outsideNames->setPrototypeErrorMessage() : String(); }
    bool throwsInSloppyMode() const { return m_outsideNames && m_outsideNames->throwsInSloppyMode(); }
    bool throwsFromReflect() const { return m_outsideNames && m_outsideNames->throwsFromReflect(); }
    void willWrite(UniquedStringImpl* uid) const
    {
        if (m_outsideNames)
            m_outsideNames->willWrite(uid);
    }
    JS_EXPORT_PRIVATE bool containsAll(const ImmutablePropertyNames&) const;
    bool isEmpty() const { return m_names.isEmpty() && m_parts.isEmpty() && !m_outsideNames; }
    OptionSet<Part> parts() const { return m_parts; }

private:
    ImmutablePropertyNames(IdentifierSet&& names, OptionSet<Part> parts, IdentifierSet&& openStrings, RefPtr<const OutsideNames>&& outsideNames)
        : m_names(WTF::move(names))
        , m_parts(parts)
        , m_openStrings(WTF::move(openStrings))
        , m_outsideNames(WTF::move(outsideNames))
    {
        ASSERT(m_openStrings.isEmpty() || m_parts.contains(Part::OtherStrings));
    }

    JS_EXPORT_PRIVATE static bool isNumericKey(UniquedStringImpl*);

    const IdentifierSet m_names;
    const OptionSet<Part> m_parts;
    const IdentifierSet m_openStrings;
    const RefPtr<const OutsideNames> m_outsideNames;
};

} // namespace JSC
