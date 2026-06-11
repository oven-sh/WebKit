/*
 * Copyright (C) 2006-2019 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <wtf/TypeCasts.h>
#include <wtf/text/UniquedStringImpl.h>

namespace WTF {

class AtomStringTable;

class AtomStringImpl final : public UniquedStringImpl {
public:
    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> lookUp(std::span<const Latin1Character>);
    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> lookUp(std::span<const char16_t>);
    static RefPtr<AtomStringImpl> lookUp(StringImpl*);

    static void remove(AtomStringImpl*);

    // SPEC-vmstate §4.4.5 — shared-atom-table mode only. Called from
    // StringImpl::derefSharedZero() when an atom's refcount reaches 0
    // (0 is final, so the caller uniquely owns the string). Locks
    // shardForHash(string->existingHash()), finds the entry by existingHash +
    // POINTER equality (never characters: a racing add may have replaced the
    // dying entry with a fresh atom for the same characters), removes it only
    // on a pointer match, unlocks, then destroys the string (bypassing the
    // destructor's legacy isAtom() removal arm: isAtom is cleared under the
    // shard lock pre-destroy; the destructor never touches the table in shared
    // mode). Removal is conditional — no unconditional
    // RELEASE_ASSERT(wasRemoved) (invariant I6).
    WTF_EXPORT_PRIVATE static void removeDeadAtom(AtomStringImpl*);

    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> add(std::span<const Latin1Character>);
    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> add(std::span<const char16_t>);
    ALWAYS_INLINE static RefPtr<AtomStringImpl> add(std::span<const char> characters);

    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> add(HashTranslatorCharBuffer<Latin1Character>&);
    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> add(HashTranslatorCharBuffer<char16_t>&);

    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> add(StringImpl*, unsigned offset, unsigned length);
    ALWAYS_INLINE static RefPtr<AtomStringImpl> add(StringImpl*);
    ALWAYS_INLINE static RefPtr<AtomStringImpl> add(RefPtr<StringImpl>&&);
    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> add(const StaticStringImpl&);
    ALWAYS_INLINE static Ref<AtomStringImpl> add(ASCIILiteral);

    // Returns null if the input data contains an invalid UTF-8 sequence.
    static RefPtr<AtomStringImpl> add(std::span<const char8_t>);

#if USE(CF)
    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> add(CFStringRef);
#endif

    // SPEC-vmstate §4.3, rule A1: when the process-global shared atom table is
    // enabled (WTF::sharedAtomStringTableEnabled()), the provider's
    // AtomStringTable is IGNORED and the add routes to the shared table's
    // shards (via addSlowCase(AtomStringTable&, StringImpl&)). Honoring it
    // would split atomization into divergent per-VM and global atom universes,
    // breaking atom pointer-equality (invariant I1).
    template<typename StringTableProvider>
    ALWAYS_INLINE static RefPtr<AtomStringImpl> addWithStringTableProvider(StringTableProvider&, StringImpl*);

#if ASSERT_ENABLED
    WTF_EXPORT_PRIVATE static bool isInAtomStringTable(StringImpl*);
#endif

private:
    AtomStringImpl() = delete;

    ALWAYS_INLINE static Ref<AtomStringImpl> add(StringImpl&);
    ALWAYS_INLINE static Ref<AtomStringImpl> add(Ref<StringImpl>&&);
    WTF_EXPORT_PRIVATE static Ref<AtomStringImpl> addLiteral(std::span<const Latin1Character>);

    ALWAYS_INLINE static Ref<AtomStringImpl> add(AtomStringTable&, StringImpl&);

    WTF_EXPORT_PRIVATE static Ref<AtomStringImpl> addSlowCase(StringImpl&);
    WTF_EXPORT_PRIVATE static Ref<AtomStringImpl> addSlowCase(Ref<StringImpl>&&);
    // In shared-atom-table mode the AtomStringTable argument is ignored and
    // the add routes to the shared table's shards (rule A1; see
    // addWithStringTableProvider above). Legacy mode honors the argument.
    WTF_EXPORT_PRIVATE static Ref<AtomStringImpl> addSlowCase(AtomStringTable&, StringImpl&);

    WTF_EXPORT_PRIVATE static RefPtr<AtomStringImpl> lookUpSlowCase(StringImpl&);
};

inline RefPtr<AtomStringImpl> AtomStringImpl::lookUp(StringImpl* string)
{
    if (!string)
        return nullptr;
    if (auto* atom = dynamicDowncast<AtomStringImpl>(*string))
        return atom;
    return lookUpSlowCase(*string);
}

ALWAYS_INLINE RefPtr<AtomStringImpl> AtomStringImpl::add(std::span<const char> characters)
{
    return add(byteCast<Latin1Character>(characters));
}

ALWAYS_INLINE RefPtr<AtomStringImpl> AtomStringImpl::add(StringImpl* string)
{
    if (!string)
        return nullptr;
    return add(*string);
}

ALWAYS_INLINE RefPtr<AtomStringImpl> AtomStringImpl::add(RefPtr<StringImpl>&& string)
{
    if (!string)
        return nullptr;
    return add(string.releaseNonNull());
}

ALWAYS_INLINE Ref<AtomStringImpl> AtomStringImpl::add(ASCIILiteral literal)
{
    return addLiteral(literal.span8());
}

template<typename StringTableProvider>
ALWAYS_INLINE RefPtr<AtomStringImpl> AtomStringImpl::addWithStringTableProvider(StringTableProvider& stringTableProvider, StringImpl* string)
{
    if (!string)
        return nullptr;
    return add(*stringTableProvider.atomStringTable(), *string);
}

ALWAYS_INLINE Ref<AtomStringImpl> AtomStringImpl::add(StringImpl& string)
{
    if (auto* atom = dynamicDowncast<AtomStringImpl>(string)) {
        ASSERT_WITH_MESSAGE(!string.length() || isInAtomStringTable(&string), "The atom string comes from an other thread!");
        return *atom;
    }
#if USE(BUN_JSC_ADDITIONS)
    // TODO: remove once atomic strings are process-wide.
    // ADVISORY check (round-4 TOCTOU closure): canBecomeAtom() is an unlocked
    // read, so a concurrent setNeverAtomize() can land after it. That race is
    // NOT closed here — it is closed at publication: the shared-mode in-place
    // atomization claims atom-hood via StringImpl::trySetIsAtomIfAtomizable()
    // (same atomic word as setNeverAtomize()'s CAS, total-ordered) and
    // addSlowCase falls back to this same copying path if it loses. This
    // early check only steers the common case before any locking.
    if (!string.canBecomeAtom()) [[unlikely]] {
        if (string.is8Bit())
            return *add(string.span8());
        return *add(string.span16());
    }
#endif
    return addSlowCase(string);
}

ALWAYS_INLINE Ref<AtomStringImpl> AtomStringImpl::add(Ref<StringImpl>&& string)
{
    if (is<AtomStringImpl>(string)) {
        ASSERT_WITH_MESSAGE(!string->length() || isInAtomStringTable(string.ptr()), "The atom string comes from an other thread!");
        return uncheckedDowncast<AtomStringImpl>(WTF::move(string));
    }
#if USE(BUN_JSC_ADDITIONS)
    // TODO: remove once atomic strings are process-wide.
    // ADVISORY unlocked check — see the add(StringImpl&) overload above; the
    // setNeverAtomize() race is closed at publication, not here.
    if (!string->canBecomeAtom()) [[unlikely]] {
        if (string->is8Bit())
            return *add(string->span8());
        return *add(string->span16());
    }
#endif
    return addSlowCase(WTF::move(string));
}

// Shared-atom-table mode ignores the AtomStringTable argument (rule A1):
// addSlowCase(AtomStringTable&, StringImpl&) routes to the shared shards, and
// the canBecomeAtom() bail below routes through add(span) which does too.
ALWAYS_INLINE Ref<AtomStringImpl> AtomStringImpl::add(AtomStringTable& stringTable, StringImpl& string)
{
    if (auto* atom = dynamicDowncast<AtomStringImpl>(string)) {
        ASSERT_WITH_MESSAGE(!string.length() || isInAtomStringTable(&string), "The atom string comes from an other thread!");
        return *atom;
    }
#if USE(BUN_JSC_ADDITIONS)
    // TODO: remove once atomic strings are process-wide.
    // ADVISORY unlocked check — see the add(StringImpl&) overload above; the
    // setNeverAtomize() race is closed at publication, not here.
    if (!string.canBecomeAtom()) [[unlikely]] {
        if (string.is8Bit())
            return *add(string.span8());
        return *add(string.span16());
    }
#endif
    return addSlowCase(stringTable, string);
}

#if ASSERT_ENABLED

// AtomStringImpls created from StaticStringImpl will ASSERT in the generic ValueCheck<T>::checkConsistency,
// as they are not allocated by fastMalloc. We don't currently have any way to detect that case, so we don't
// do any consistency check for AtomStringImpl*.

template<> struct ValueCheck<AtomStringImpl*> {
    static void checkConsistency(const AtomStringImpl*) { }
};

template<> struct ValueCheck<const AtomStringImpl*> {
    static void checkConsistency(const AtomStringImpl*) { }
};

#endif // ASSERT_ENABLED

inline void printInternal(PrintStream& out, const AtomStringImpl* value) { printInternal(out, std::bit_cast<const StringImpl*>(value)); }
inline void printInternal(PrintStream& out, AtomStringImpl* value) { printInternal(out, static_cast<const AtomStringImpl*>(value)); }

} // namespace WTF

SPECIALIZE_TYPE_TRAITS_BEGIN(WTF::AtomStringImpl) \
    static bool isType(const WTF::StringImpl& string) { return string.isAtom(); } \
SPECIALIZE_TYPE_TRAITS_END()

using WTF::AtomStringImpl;
