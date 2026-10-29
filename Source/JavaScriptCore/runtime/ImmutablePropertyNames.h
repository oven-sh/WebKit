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

    // Names that somebody else keeps, in a form that costs nothing to set up. They are asked for last: only a write that nothing else refuses gets here.
    class OutsideNames : public RefCounted<OutsideNames> {
    public:
        virtual ~OutsideNames() = default;
        virtual bool contains(UniquedStringImpl*) const = 0;
        // What whoever is refused is told: which property of which object it is, and why it stays as it is. Null: the caller has a text of its own.
        virtual String messageOfRefusal(UniquedStringImpl*) const { return { }; }
        virtual String messageOfRefusedDeletion(UniquedStringImpl*) const { return { }; }
        virtual String messageOfRefusedPrototype() const { return { }; }
        // The refusal of a store or a deletion is an error wherever it stands, also in sloppy code, which otherwise goes on as if nothing had happened.
        // Not where the language refuses the change by itself: a property that reads as read-only, or as not configurable. A function of Reflect answers false as ever.
        virtual bool refusalAlwaysThrows() const { return false; }
        // Somebody is about to write under a name that is not among these, and nothing else refuses the write either.
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
    String messageOfRefusal(UniquedStringImpl* uid) const { return m_outsideNames ? m_outsideNames->messageOfRefusal(uid) : String(); }
    String messageOfRefusedDeletion(UniquedStringImpl* uid) const { return m_outsideNames ? m_outsideNames->messageOfRefusedDeletion(uid) : String(); }
    String messageOfRefusedPrototype() const { return m_outsideNames ? m_outsideNames->messageOfRefusedPrototype() : String(); }
    bool refusalAlwaysThrows() const { return m_outsideNames && m_outsideNames->refusalAlwaysThrows(); }
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
