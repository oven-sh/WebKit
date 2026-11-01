/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3IntRange.h"
#include "B3Value.h"
#include "B3Variable.h"
#include <wtf/HashMap.h>

namespace JSC { namespace B3 {

constexpr bool mayHaveKnownRange(Opcode opcode)
{
    switch (opcode) {
    case Phi:
    case Patchpoint:
    case CCall:
    case Extract:
    case ArgumentReg:
    case Trunc:
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
        return true;
    default:
        return false;
    }
}

inline bool mayHaveKnownDereferenceableBytes(const Value* value)
{
    auto isPointerFromOutside = [](const Value* value) {
        switch (value->opcode()) {
        case Phi:
        case Patchpoint:
        case CCall:
        case Extract:
        case ArgumentReg:
        case Load:
            return true;
        case Add:
            return value->child(0)->opcode() == ArgumentReg && value->child(1)->isConstant();
        default:
            return false;
        }
    };
    if (value->opcode() == Select)
        return !value->child(0)->isConstant() && isPointerFromOutside(value->child(1)) && isPointerFromOutside(value->child(2));
    return isPointerFromOutside(value);
}

class KnownProperties {
public:
    bool isEmpty() const { return m_entries.isEmpty(); }

    void setRange(const Value*, IntRange);
    std::optional<IntRange> range(const Value*) const;

    void setDereferenceableBytes(const Value*, uint32_t);
    uint32_t dereferenceableBytes(const Value*) const;

    void copy(const Value* from, const Value* to);
    void remove(const Value*);
    void clear();

    void didDemote(const Value*, const Variable*);
    void didCreatePhi(const Variable*, const Value* phi);
    void clearDemoted() { m_demotedEntries.clear(); }

private:
    struct Entry {
        std::optional<IntRange> range;
        uint32_t dereferenceableBytes { 0 };
        Opcode opcode { Oops };
    };

    using Map = UncheckedKeyHashMap<unsigned, Entry, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>>;

    const Entry* find(const Value*) const;
    Entry& ensure(const Value*);

    Map m_entries;
    Map m_demotedEntries;
};

inline auto KnownProperties::find(const Value* value) const -> const Entry*
{
    if (m_entries.isEmpty())
        return nullptr;
    auto entry = m_entries.find(value->index());
    if (entry == m_entries.end() || entry->value.opcode != value->opcode())
        return nullptr;
    return &entry->value;
}

inline auto KnownProperties::ensure(const Value* value) -> Entry&
{
    Entry& entry = m_entries.add(value->index(), Entry { }).iterator->value;
    if (entry.opcode != value->opcode())
        entry = Entry { std::nullopt, 0, value->opcode() };
    return entry;
}

inline void KnownProperties::setRange(const Value* value, IntRange range)
{
    ensure(value).range = range;
}

inline std::optional<IntRange> KnownProperties::range(const Value* value) const
{
    const Entry* entry = find(value);
    return entry ? entry->range : std::nullopt;
}

inline void KnownProperties::setDereferenceableBytes(const Value* value, uint32_t bytes)
{
    ensure(value).dereferenceableBytes = bytes;
}

inline uint32_t KnownProperties::dereferenceableBytes(const Value* value) const
{
    const Entry* entry = find(value);
    return entry ? entry->dereferenceableBytes : 0;
}

inline void KnownProperties::copy(const Value* from, const Value* to)
{
    if (const Entry* entry = find(from)) {
        Entry copied = *entry;
        m_entries.set(to->index(), copied);
    }
}

inline void KnownProperties::remove(const Value* value)
{
    if (!m_entries.isEmpty())
        m_entries.remove(value->index());
}

inline void KnownProperties::clear()
{
    m_entries.clear();
    m_demotedEntries.clear();
}

inline void KnownProperties::didDemote(const Value* value, const Variable* variable)
{
    if (const Entry* entry = find(value)) {
        Entry demoted = *entry;
        m_demotedEntries.set(variable->index(), demoted);
    }
}

inline void KnownProperties::didCreatePhi(const Variable* variable, const Value* phi)
{
    if (m_demotedEntries.isEmpty())
        return;
    auto demoted = m_demotedEntries.find(variable->index());
    if (demoted == m_demotedEntries.end())
        return;
    Entry entry = demoted->value;
    entry.opcode = phi->opcode();
    m_entries.set(phi->index(), entry);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
