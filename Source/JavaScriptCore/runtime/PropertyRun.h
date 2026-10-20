/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/JSExportMacros.h>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class VM;

class PropertyRun final : public RefCounted<PropertyRun> {
    WTF_MAKE_TZONE_ALLOCATED_EXPORT(PropertyRun, JS_EXPORT_PRIVATE);
    WTF_MAKE_NONCOPYABLE(PropertyRun);
public:
    static constexpr unsigned maxSize = 64;

    std::span<const RefPtr<UniquedStringImpl>> names() const { return m_names.span(); }
    unsigned size() const { return m_names.size(); }
    UniquedStringImpl* transitionKey() const { return m_transitionKey.ptr(); }
    unsigned hash() const { return m_hash; }

    static unsigned computeHash(std::span<UniquedStringImpl* const> names);

private:
    friend class PropertyRunTable;

    PropertyRun(std::span<UniquedStringImpl* const> names, unsigned hash);

    const FixedVector<RefPtr<UniquedStringImpl>> m_names;
    const Ref<SymbolImpl> m_transitionKey;
    const unsigned m_hash;
};

class PropertyRunTable {
    WTF_MAKE_TZONE_ALLOCATED(PropertyRunTable);
    WTF_MAKE_NONCOPYABLE(PropertyRunTable);
public:
    PropertyRunTable() = default;

    JS_EXPORT_PRIVATE const PropertyRun* intern(std::span<UniquedStringImpl* const> names);
    const PropertyRun* find(std::span<UniquedStringImpl* const> names) const;
    // Shortest first.
    std::span<const PropertyRun* const> runsEndingWith(UniquedStringImpl* lastName) const;
    bool isEmpty() const { return m_runs.isEmpty(); }

private:
    struct RunHash {
        static unsigned hash(const Ref<PropertyRun>& run) { return run->hash(); }
        static bool equal(const Ref<PropertyRun>& a, const Ref<PropertyRun>& b) { return a.ptr() == b.ptr(); }
        static constexpr bool safeToCompareToEmptyOrDeleted = false;
    };
    struct NamesTranslator;

    HashSet<Ref<PropertyRun>, RunHash> m_runs;
    HashMap<UniquedStringImpl*, Vector<const PropertyRun*, 1>> m_runsByLastName;
};

} // namespace JSC
