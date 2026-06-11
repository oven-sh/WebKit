/*
 * Copyright (C) 2004-2022 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Patrick Gansterer <paroga@paroga.com>
 * Copyright (C) 2012 Google Inc. All rights reserved.
 * Copyright (C) 2015 Yusuke Suzuki<utatane.tea@gmail.com>. All rights reserved.
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

#include "config.h"
#include <wtf/text/AtomStringImpl.h>

#include <wtf/Lock.h>
#include <wtf/Threading.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/AtomStringTable.h>
#include <wtf/text/SharedAtomStringTable.h>
#include <wtf/text/WTFString.h>

namespace WTF {

using namespace Unicode;

#if USE(WEB_THREAD)

class AtomStringTableLocker : public Locker<Lock> {
    WTF_MAKE_NONCOPYABLE(AtomStringTableLocker);

    static Lock s_stringTableLock;
public:
    AtomStringTableLocker()
        : Locker<Lock>(s_stringTableLock)
    {
    }
};

Lock AtomStringTableLocker::s_stringTableLock;

#else

class AtomStringTableLocker {
    WTF_MAKE_NONCOPYABLE(AtomStringTableLocker);
public:
    AtomStringTableLocker() { }
};

#endif // USE(WEB_THREAD)

using StringTableImpl = AtomStringTable::StringTableImpl;

static ALWAYS_INLINE StringTableImpl& stringTable()
{
    return Thread::currentSingleton().atomStringTable()->table();
}

template<typename T, typename HashTranslator>
static inline Ref<AtomStringImpl> addToStringTable(AtomStringTableLocker&, StringTableImpl& atomStringTable, const T& value)
{
    // Drift guard (SPEC-vmstate §4.3, ex-M10): in shared-atom-table mode no
    // path may read or write any per-thread AtomStringTable (rule A1); the
    // routed entry points above never reach this legacy arm.
    ASSERT(!sharedAtomStringTableEnabled());

    auto addResult = atomStringTable.add<HashTranslator>(value);

    // If the string is newly-translated, then we need to adopt it.
    // The boolean in the pair tells us if that is so.
    if (addResult.isNewEntry)
        return adoptRef(uncheckedDowncast<AtomStringImpl>(*addResult.iterator->get()));
    return *uncheckedDowncast<AtomStringImpl>(addResult.iterator->get());
}

// Shared-atom-table add (SPEC-vmstate §4.4.4). MUST run under the lock of the
// shard chosen by shardForHash(HashTranslator::hash(value)) (invariant I5).
//
// Unlike the legacy arm, a table hit may land on a *dead* entry: deref()
// commits to destruction at refcount 0 before taking any lock (§4.4), so a
// resident entry whose refcount is 0 is owned by a thread racing toward
// AtomStringImpl::removeDeadAtom() on this same shard lock. Refcount 0 is
// final (no resurrection): live hits take their reference via tryRefAtom()
// in-lock; dead hits are removed and replaced with a fresh atom. The racing
// removeDeadAtom() removes only on pointer equality, so it skips the
// replacement (invariant I6).
template<typename T, typename HashTranslator>
static Ref<AtomStringImpl> addToSharedStringTable(SharedAtomStringTable::Shard& shard, const T& value) WTF_REQUIRES_LOCK(shard.lock)
{
    auto addResult = shard.table.add<HashTranslator>(value);

    // Newly translated: adopt the single reference translate() leakRef'd.
    // F1: the new atom is fully initialized (characters, hash, isAtom bit)
    // before the shard lock is released; consumers acquire the same lock.
    if (addResult.isNewEntry)
        return adoptRef(uncheckedDowncast<AtomStringImpl>(*addResult.iterator->get()));

    StringImpl* existing = addResult.iterator->get();
    if (existing->tryRefAtom()) {
        // Live hit: the Ref adopts the reference taken under the lock (§4.4.4).
        return adoptRef(uncheckedDowncast<AtomStringImpl>(*existing));
    }

    // Dead entry. Statics never fail tryRefAtom() (they rest at masked
    // refcount 0 and never die), so static atoms are never evicted here
    // (invariant I19).
    ASSERT(!existing->isStatic());
    shard.table.remove(*addResult.iterator);
    auto reAddResult = shard.table.add<HashTranslator>(value);
    ASSERT(reAddResult.isNewEntry);
    return adoptRef(uncheckedDowncast<AtomStringImpl>(*reAddResult.iterator->get()));
}

template<typename T, typename HashTranslator>
static inline Ref<AtomStringImpl> addToStringTable(const T& value)
{
    if (sharedAtomStringTableEnabled()) [[unlikely]] {
        auto& shard = SharedAtomStringTable::singleton().shardForHash(HashTranslator::hash(value));
        Locker locker { shard.lock };
        return addToSharedStringTable<T, HashTranslator>(shard, value);
    }
    AtomStringTableLocker locker;
    return addToStringTable<T, HashTranslator>(locker, stringTable(), value);
}

using UTF16Buffer = HashTranslatorCharBuffer<char16_t>;
struct UTF16BufferTranslator {
    static unsigned NODELETE hash(const UTF16Buffer& buf)
    {
        return buf.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& str, const UTF16Buffer& buf)
    {
        return WTF::equal(str.get(), buf.characters);
    }

    static void translate(AtomStringTable::StringEntry& location, const UTF16Buffer& buf, unsigned hash)
    {
        Ref stringImpl = StringImpl::create8BitIfPossible(buf.characters);
        stringImpl->setHash(hash);
        stringImpl->setIsAtom(true);
        location = &stringImpl.leakRef();
    }
};

RefPtr<AtomStringImpl> AtomStringImpl::add(std::span<const char16_t> characters)
{
    if (!characters.data())
        return nullptr;

    if (characters.empty())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    UTF16Buffer buffer { characters };
    return addToStringTable<UTF16Buffer, UTF16BufferTranslator>(buffer);
}

RefPtr<AtomStringImpl> AtomStringImpl::add(HashTranslatorCharBuffer<char16_t>& buffer)
{
    if (!buffer.characters.data())
        return nullptr;

    if (buffer.characters.empty())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    return addToStringTable<UTF16Buffer, UTF16BufferTranslator>(buffer);
}

struct SubstringLocation {
    SUPPRESS_UNCOUNTED_MEMBER StringImpl* baseString;
    unsigned start;
    unsigned length;
};

struct SubstringTranslator {
    static void translate(AtomStringTable::StringEntry& location, const SubstringLocation& buffer, unsigned hash)
    {
        SUPPRESS_UNCOUNTED_ARG Ref stringImpl = StringImpl::createSubstringSharingImpl(*buffer.baseString, buffer.start, buffer.length);
        stringImpl->setHash(hash);
        stringImpl->setIsAtom(true);
        location = &stringImpl.leakRef();
    }
};

struct SubstringTranslator8 : SubstringTranslator {
    static unsigned hash(const SubstringLocation& buffer)
    {
        return StringHasher::computeHashAndMaskTop8Bits(buffer.baseString->span8().subspan(buffer.start, buffer.length));
    }

    static bool equal(AtomStringTable::StringEntry const& string, const SubstringLocation& buffer)
    {
        return WTF::equal(string.get(), buffer.baseString->span8().subspan(buffer.start, buffer.length));
    }
};

struct SubstringTranslator16 : SubstringTranslator {
    static unsigned hash(const SubstringLocation& buffer)
    {
        return StringHasher::computeHashAndMaskTop8Bits(buffer.baseString->span16().subspan(buffer.start, buffer.length));
    }

    static bool equal(AtomStringTable::StringEntry const& string, const SubstringLocation& buffer)
    {
        return WTF::equal(string.get(), buffer.baseString->span16().subspan(buffer.start, buffer.length));
    }
};

RefPtr<AtomStringImpl> AtomStringImpl::add(StringImpl* baseString, unsigned start, unsigned length)
{
    if (!baseString)
        return nullptr;

    if (!length || start >= baseString->length())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    unsigned maxLength = baseString->length() - start;
    if (length >= maxLength) {
        if (!start)
            return add(baseString);
        length = maxLength;
    }

    SubstringLocation buffer = { baseString, start, length };
    if (baseString->is8Bit())
        return addToStringTable<SubstringLocation, SubstringTranslator8>(buffer);
    return addToStringTable<SubstringLocation, SubstringTranslator16>(buffer);
}
    
using Latin1Buffer = HashTranslatorCharBuffer<Latin1Character>;
struct Latin1BufferTranslator {
    static unsigned NODELETE hash(const Latin1Buffer& buf)
    {
        return buf.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& str, const Latin1Buffer& buf)
    {
        return WTF::equal(str.get(), buf.characters);
    }

    static void translate(AtomStringTable::StringEntry& location, const Latin1Buffer& buf, unsigned hash)
    {
        Ref stringImpl = StringImpl::create(buf.characters);
        stringImpl->setHash(hash);
        stringImpl->setIsAtom(true);
        location = &stringImpl.leakRef();
    }
};

template<typename CharType>
struct BufferFromStaticDataTranslator {
    using Buffer = HashTranslatorCharBuffer<CharType>;
    static unsigned NODELETE hash(const Buffer& buf)
    {
        return buf.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& str, const Buffer& buf)
    {
        return WTF::equal(str.get(), buf.characters);
    }

    static void translate(AtomStringTable::StringEntry& location, const Buffer& buf, unsigned hash)
    {
        Ref stringImpl = StringImpl::createWithoutCopying(buf.characters);
        stringImpl->setHash(hash);
        stringImpl->setIsAtom(true);
        location = &stringImpl.leakRef();
    }
};

template<typename CharType>
struct StaticStringAtomBuffer {
    SUPPRESS_UNCOUNTED_MEMBER const StringImpl& staticImpl;
    std::span<const CharType> characters;
    unsigned hash;
};

// Translator that stores a StaticStringImpl directly in the atom table without
// heap-allocating a copy. The StaticStringImpl must have been constructed with
// StringImpl::StringAtom so that isAtom() returns true. This enables global
// atom strings that share the same StringImpl* across all threads.
template<typename CharType>
struct StaticStringAtomTranslator {
    using Buffer = StaticStringAtomBuffer<CharType>;

    static unsigned NODELETE hash(const Buffer& buf)
    {
        return buf.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& str, const Buffer& buf)
    {
        return WTF::equal(str.get(), buf.characters);
    }

    static void translate(AtomStringTable::StringEntry& location, const Buffer& buf, unsigned)
    {
        location = const_cast<StringImpl*>(&buf.staticImpl);
    }
};

RefPtr<AtomStringImpl> AtomStringImpl::add(HashTranslatorCharBuffer<Latin1Character>& buffer)
{
    if (!buffer.characters.data())
        return nullptr;

    if (buffer.characters.empty())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    return addToStringTable<Latin1Buffer, Latin1BufferTranslator>(buffer);
}

RefPtr<AtomStringImpl> AtomStringImpl::add(std::span<const Latin1Character> characters)
{
    if (!characters.data())
        return nullptr;

    if (characters.empty())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    Latin1Buffer buffer { characters };
    return addToStringTable<Latin1Buffer, Latin1BufferTranslator>(buffer);
}

Ref<AtomStringImpl> AtomStringImpl::addLiteral(std::span<const Latin1Character> characters)
{
    ASSERT(characters.data());
    ASSERT(!characters.empty());

    Latin1Buffer buffer { characters };
    return addToStringTable<Latin1Buffer, BufferFromStaticDataTranslator<Latin1Character>>(buffer);
}

static Ref<AtomStringImpl> addSymbol(AtomStringTableLocker& locker, StringTableImpl& atomStringTable, StringImpl& base)
{
    // Drift guard (SPEC-vmstate §4.3, ex-M10): legacy arm, unreachable in shared mode.
    ASSERT(!sharedAtomStringTableEnabled());
    ASSERT(base.length());
    ASSERT(base.isSymbol());

    SubstringLocation buffer = { &base, 0, base.length() };
    if (base.is8Bit())
        return addToStringTable<SubstringLocation, SubstringTranslator8>(locker, atomStringTable, buffer);
    return addToStringTable<SubstringLocation, SubstringTranslator16>(locker, atomStringTable, buffer);
}

static Ref<AtomStringImpl> addSymbolShared(StringImpl& base)
{
    ASSERT(sharedAtomStringTableEnabled());
    ASSERT(base.length());
    ASSERT(base.isSymbol());

    SubstringLocation buffer = { &base, 0, base.length() };
    if (base.is8Bit()) {
        auto& shard = SharedAtomStringTable::singleton().shardForHash(SubstringTranslator8::hash(buffer));
        Locker locker { shard.lock };
        return addToSharedStringTable<SubstringLocation, SubstringTranslator8>(shard, buffer);
    }
    auto& shard = SharedAtomStringTable::singleton().shardForHash(SubstringTranslator16::hash(buffer));
    Locker locker { shard.lock };
    return addToSharedStringTable<SubstringLocation, SubstringTranslator16>(shard, buffer);
}

static inline Ref<AtomStringImpl> addSymbol(StringImpl& base)
{
    if (sharedAtomStringTableEnabled()) [[unlikely]]
        return addSymbolShared(base);
    AtomStringTableLocker locker;
    return addSymbol(locker, stringTable(), base);
}

static Ref<AtomStringImpl> addStatic(AtomStringTableLocker& locker, StringTableImpl& atomStringTable, const StringImpl& base)
{
    // Drift guard (SPEC-vmstate §4.3, ex-M10): legacy arm, unreachable in shared mode.
    ASSERT(!sharedAtomStringTableEnabled());
    ASSERT(base.length());
    ASSERT(base.isStatic());

    // StaticStringImpl with StringAtom: store the static pointer directly in the
    // atom table with no heap allocation. The isAtom() flag is already set at
    // construction time, enabling uncheckedDowncast<AtomStringImpl> and the
    // dynamicDowncast fast path in add(StringImpl&). All threads that register
    // the same StaticStringImpl share the same StringImpl pointer.
    if (base.isAtom()) {
        if (base.is8Bit()) {
            StaticStringAtomBuffer<Latin1Character> buffer { base, base.span8(), base.hash() };
            return addToStringTable<StaticStringAtomBuffer<Latin1Character>, StaticStringAtomTranslator<Latin1Character>>(locker, atomStringTable, buffer);
        }
        StaticStringAtomBuffer<char16_t> buffer { base, base.span16(), base.hash() };
        return addToStringTable<StaticStringAtomBuffer<char16_t>, StaticStringAtomTranslator<char16_t>>(locker, atomStringTable, buffer);
    }

    if (base.is8Bit()) {
        Latin1Buffer buffer { base.span8(), base.hash() };
        return addToStringTable<Latin1Buffer, BufferFromStaticDataTranslator<Latin1Character>>(locker, atomStringTable, buffer);
    }
    UTF16Buffer buffer { base.span16(), base.hash() };
    return addToStringTable<UTF16Buffer, BufferFromStaticDataTranslator<char16_t>>(locker, atomStringTable, buffer);
}

static Ref<AtomStringImpl> addStaticShared(const StringImpl& base)
{
    ASSERT(sharedAtomStringTableEnabled());
    ASSERT(base.length());
    ASSERT(base.isStatic());

#if USE(BUN_JSC_ADDITIONS)
    // Round-4 hardening, static sibling of the owned-string TOCTOU: a
    // refcount-static ExternalStringImpl (the no-ctx constructors set
    // s_refCountFlagIsStaticString) reaches this arm, and the non-isAtom
    // branch below parks a NEW StringImpl that ALIASES base's buffer
    // (BufferFromStaticDataTranslator -> createWithoutCopying). If the
    // embedder marked base NeverAtomize for early buffer release, that alias
    // must never be parked. Unlike the owned-string arm, this cannot be
    // closed by a word-level RMW (the parked object is a different
    // StringImpl with its own m_hashAndFlags), so: re-check here, after the
    // caller's advisory check, and rely on the embedder's set-BEFORE-sharing
    // rule for the residual instruction-window (cross-WS item 17,
    // INTEGRATE-vmstate.md — Bun sets NeverAtomize at creation, before the
    // string is visible to any other thread, making a mid-atomization flip
    // impossible for external strings). The copying fallback owns its
    // characters (Latin1/UTF16BufferTranslator -> StringImpl::create), so it
    // is immune to a later releaseBufferEarly.
    if (!base.canBecomeAtom()) [[unlikely]] {
        ASSERT(!base.isAtom()); // setNeverAtomize() refuses already-atom strings.
        if (base.is8Bit())
            return *AtomStringImpl::add(base.span8());
        return *AtomStringImpl::add(base.span16());
    }
#endif

    // I5: each buffer below carries base.hash() as the HashTranslator hash,
    // so the shard choice matches every translator's hash for this string.
    auto& shard = SharedAtomStringTable::singleton().shardForHash(base.hash());
    Locker locker { shard.lock };

    // Table-resident StaticStringImpls are stored by pointer (no copy) and
    // never die, so the same StringImpl* is returned to every thread and is
    // never evicted by the dead-entry replace arm (invariant I19).
    if (base.isAtom()) {
        if (base.is8Bit()) {
            StaticStringAtomBuffer<Latin1Character> buffer { base, base.span8(), base.hash() };
            return addToSharedStringTable<StaticStringAtomBuffer<Latin1Character>, StaticStringAtomTranslator<Latin1Character>>(shard, buffer);
        }
        StaticStringAtomBuffer<char16_t> buffer { base, base.span16(), base.hash() };
        return addToSharedStringTable<StaticStringAtomBuffer<char16_t>, StaticStringAtomTranslator<char16_t>>(shard, buffer);
    }

    if (base.is8Bit()) {
        Latin1Buffer buffer { base.span8(), base.hash() };
        return addToSharedStringTable<Latin1Buffer, BufferFromStaticDataTranslator<Latin1Character>>(shard, buffer);
    }
    UTF16Buffer buffer { base.span16(), base.hash() };
    return addToSharedStringTable<UTF16Buffer, BufferFromStaticDataTranslator<char16_t>>(shard, buffer);
}

static inline Ref<AtomStringImpl> addStatic(const StringImpl& base)
{
    if (sharedAtomStringTableEnabled()) [[unlikely]]
        return addStaticShared(base);
    AtomStringTableLocker locker;
    return addStatic(locker, stringTable(), base);
}

RefPtr<AtomStringImpl> AtomStringImpl::add(const StaticStringImpl& string)
{
    ASSERT(static_cast<const StringImpl&>(string).isStatic());
    SUPPRESS_UNCOUNTED_ARG return addStatic(static_cast<const StringImpl&>(string));
}

// Shared-mode counterpart of the no-translator addSlowCase arms: atomize a
// caller-owned StringImpl in place (SPEC-vmstate §4.4.4). The caller holds a
// reference to `string` (refcount > 0), so only the resident entry needs the
// tryRefAtom() protocol. MUST run under the lock of
// shardForHash(string.hash()) — the default StringHash the table uses (I5).
//
// Returns null IFF `string` lost the race with a concurrent
// StringImpl::setNeverAtomize() (round-4 TOCTOU closure): the insert is backed
// out, `string` is left a non-atom, and the caller must RELEASE THE SHARD LOCK
// and fall back to the copying add(span) path — which recomputes the same hash
// and therefore relocks this same shard (shard locks don't nest, §7).
static RefPtr<AtomStringImpl> addOwnedStringToSharedStringTable(SharedAtomStringTable::Shard& shard, StringImpl& string) WTF_REQUIRES_LOCK(shard.lock)
{
    auto addResult = shard.table.add(&string);

    if (!addResult.isNewEntry) {
        StringImpl* existing = addResult.iterator->get();
        // Note: existing == &string is possible post-GIL — the caller's
        // unlocked isAtom() check can race another thread atomizing the SAME
        // StringImpl*; the winner set isAtom under this shard lock. The hit
        // arm handles it: tryRefAtom() succeeds (the caller owns a reference,
        // so the refcount is nonzero) and the Ref adopts that reference.
        // (NeverAtomize is irrelevant on this arm: returning the resident
        // atom never parks `string` itself in the table, and if existing ==
        // &string, the winning atomization's guarded claim below already
        // beat any setNeverAtomize() in the m_hashAndFlags RMW order.)
        if (existing->tryRefAtom()) {
            // Live hit: adopt the in-lock reference (§4.4.4).
            return adoptRef(uncheckedDowncast<AtomStringImpl>(*existing));
        }
        // Dead entry: remove it and insert `string` fresh; the racing
        // removeDeadAtom() removes only on pointer match (I6). Statics never
        // fail tryRefAtom() (I19).
        ASSERT(!existing->isStatic());
        shard.table.remove(*addResult.iterator);
        addResult = shard.table.add(&string);
        ASSERT(addResult.isNewEntry);
    }
    ASSERT(addResult.iterator->get() == &string);

    // F1 + round-4 TOCTOU closure: claim atom-hood with a guarded RMW on the
    // SAME atomic word setNeverAtomize() CASes (trySetIsAtomIfAtomizable,
    // StringImpl.h). The caller's unlocked canBecomeAtom() check is advisory:
    // setNeverAtomize() takes no lock, so it can land between that check and
    // here. The two RMWs are total-ordered; if setNeverAtomize() won, back the
    // insert out — `string` was never published as an atom (isAtom never set,
    // and the shard lock is still held, so no table probe observed the entry;
    // the unlocked dynamicDowncast fast path never saw isAtom either) — and
    // report the loss so the caller takes the copying path. This is what makes
    // it impossible to park a NeverAtomize (early-buffer-releasable
    // ExternalStringImpl) string in the shared table.
    if (!string.trySetIsAtomIfAtomizable()) [[unlikely]] {
        shard.table.remove(*addResult.iterator);
        return nullptr;
    }
    // Publication (the isAtom claim) happened before the shard lock release.
    return uncheckedDowncast<AtomStringImpl>(&string);
}

Ref<AtomStringImpl> AtomStringImpl::addSlowCase(StringImpl& string)
{
    // This check is necessary for null symbols.
    // Their length is zero, but they are not AtomStringImpl.
    if (!string.length())
        return *uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    if (string.isStatic())
        return addStatic(string);

    if (string.isSymbol())
        return addSymbol(string);

    // Legacy-mode-only assert: in shared mode the caller's unlocked isAtom()
    // check can legally race another thread atomizing the SAME StringImpl* —
    // the winner sets isAtom under the shard lock and the in-lock hit arm of
    // addOwnedStringToSharedStringTable handles existing == &string — so
    // observing isAtom here is NOT a protocol violation (SPEC-vmstate §4.4.4).
    ASSERT_WITH_MESSAGE(sharedAtomStringTableEnabled() || !string.isAtom(), "AtomStringImpl should not hit the slow case if the string is already an atom.");

    if (sharedAtomStringTableEnabled()) [[unlikely]] {
        // In-place atomization: `string` may be co-owned by other threads —
        // including threads that predate the latch and last held it BEFORE the
        // latch flipped. This is why the §4.8 ordering contract
        // (SharedAtomStringTable.h) has NO pre-latch-owned exemption for
        // ref/deref: any co-owner's later final deref must observe the latch
        // (via its required happens-before edge to the initializer) so the
        // zero transition routes through removeDeadAtom, not the legacy
        // destroy path — otherwise this shard entry would dangle.
        {
            auto& shard = SharedAtomStringTable::singleton().shardForHash(string.hash());
            Locker locker { shard.lock };
            if (RefPtr<AtomStringImpl> atom = addOwnedStringToSharedStringTable(shard, string)) [[likely]]
                return atom.releaseNonNull();
        }
        // `string` lost the guarded-claim race with a concurrent
        // setNeverAtomize() (round-4 TOCTOU closure): it must not be parked
        // in the shared table. Fall back to the copying add(span) path —
        // OUTSIDE the shard lock, since the copy keys on the same characters
        // (same hash, same shard) and would self-deadlock under it.
        if (string.is8Bit())
            return *add(string.span8());
        return *add(string.span16());
    }

    // Drift guard (SPEC-vmstate §4.3, ex-M10): legacy arm, unreachable in shared mode.
    ASSERT(!sharedAtomStringTableEnabled());
    AtomStringTableLocker locker;
    auto addResult = stringTable().add(&string);

    if (addResult.isNewEntry) {
        ASSERT(addResult.iterator->get() == &string);
        string.setIsAtom(true);
    }

    return *uncheckedDowncast<AtomStringImpl>(addResult.iterator->get());
}

Ref<AtomStringImpl> AtomStringImpl::addSlowCase(Ref<StringImpl>&& string)
{
    // This check is necessary for null symbols.
    // Their length is zero, but they are not AtomStringImpl.
    if (!string->length())
        return *uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    if (string->isStatic())
        return addStatic(WTF::move(string));

    if (string->isSymbol())
        return addSymbol(WTF::move(string));

    // Legacy-mode-only assert — see the StringImpl& overload above: in shared
    // mode a racing atomization of the same StringImpl* can set isAtom first;
    // the in-lock hit arm handles it (SPEC-vmstate §4.4.4).
    ASSERT_WITH_MESSAGE(sharedAtomStringTableEnabled() || !string->isAtom(), "AtomStringImpl should not hit the slow case if the string is already an atom.");

    if (sharedAtomStringTableEnabled()) [[unlikely]] {
        {
            auto& shard = SharedAtomStringTable::singleton().shardForHash(string->hash());
            Locker locker { shard.lock };
            // The returned RefPtr carries its own reference; the parameter
            // Ref's deref on return keeps counts balanced in every arm.
            if (RefPtr<AtomStringImpl> atom = addOwnedStringToSharedStringTable(shard, string.get())) [[likely]]
                return atom.releaseNonNull();
        }
        // Lost the guarded-claim race with setNeverAtomize() — copying
        // fallback outside the shard lock; see the StringImpl& overload.
        if (string->is8Bit())
            return *add(string->span8());
        return *add(string->span16());
    }

    // Drift guard (SPEC-vmstate §4.3, ex-M10): legacy arm, unreachable in shared mode.
    ASSERT(!sharedAtomStringTableEnabled());
    AtomStringTableLocker locker;
    auto addResult = stringTable().add(string.ptr());

    if (addResult.isNewEntry) {
        ASSERT(addResult.iterator->get() == string.ptr());
        string->setIsAtom(true);
        return uncheckedDowncast<AtomStringImpl>(WTF::move(string));
    }

    return *uncheckedDowncast<AtomStringImpl>(addResult.iterator->get());
}

Ref<AtomStringImpl> AtomStringImpl::addSlowCase(AtomStringTable& stringTable, StringImpl& string)
{
    // Rule A1 (SPEC-vmstate §4.3): in shared mode this overload MUST ignore
    // the passed table and route to the process-global shards — otherwise
    // JSC Identifiers (which pass VM::m_atomStringTable through
    // addWithStringTableProvider) and bare WTF atomization would populate two
    // divergent atom universes, breaking I1 and atom pointer-equality. The
    // table-less overload performs exactly the shard routing for all arms.
    if (sharedAtomStringTableEnabled()) [[unlikely]]
        return addSlowCase(string);

    // Drift guard (SPEC-vmstate §4.3, ex-M10): legacy arms below honor the
    // passed table; unreachable in shared mode.
    ASSERT(!sharedAtomStringTableEnabled());

    // This check is necessary for null symbols.
    // Their length is zero, but they are not AtomStringImpl.
    if (!string.length())
        return *uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    if (string.isStatic()) {
        AtomStringTableLocker locker;
        return addStatic(locker, stringTable.table(), string);
    }

    if (string.isSymbol()) {
        AtomStringTableLocker locker;
        return addSymbol(locker, stringTable.table(), string);
    }

    ASSERT_WITH_MESSAGE(!string.isAtom(), "AtomStringImpl should not hit the slow case if the string is already an atom.");

    AtomStringTableLocker locker;
    auto addResult = stringTable.table().add(&string);

    if (addResult.isNewEntry) {
        ASSERT(addResult.iterator->get() == &string);
        string.setIsAtom(true);
    }

    return *uncheckedDowncast<AtomStringImpl>(addResult.iterator->get());
}

// When removing a string from the table, we know it's already the one in the table, so no need for a string equality check.
struct AtomStringTableRemovalHashTranslator {
    static unsigned hash(const AtomStringImpl* string) { return string->hash(); }
    static bool equal(const AtomStringTable::StringEntry& a, const AtomStringImpl* b) { return a == b; }
};

void AtomStringImpl::remove(AtomStringImpl* string)
{
    ASSERT(string->isAtom());

    if (sharedAtomStringTableEnabled()) [[unlikely]] {
        // Shared mode: dying atoms normally flow through removeDeadAtom()
        // (§4.4.5), not this path. If reached, mirror its protocol: find by
        // hash + POINTER equality (the removal translator compares pointers)
        // and remove only on a match — a racing add may already have replaced
        // this entry with a fresh atom for the same characters, so removal is
        // conditional; no unconditional RELEASE_ASSERT (invariant I6).
        auto& shard = SharedAtomStringTable::singleton().shardForHash(string->existingHash());
        Locker locker { shard.lock };
        auto iterator = shard.table.find<AtomStringTableRemovalHashTranslator>(string);
        if (iterator != shard.table.end())
            shard.table.remove(iterator);
        return;
    }

    // Drift guard (SPEC-vmstate §4.3, ex-M10): legacy arm, unreachable in shared mode.
    ASSERT(!sharedAtomStringTableEnabled());
    AtomStringTableLocker locker;
    auto& atomStringTable = stringTable();
    auto iterator = atomStringTable.find<AtomStringTableRemovalHashTranslator>(string);
    bool wasRemoved = atomStringTable.remove(iterator);
    RELEASE_ASSERT(wasRemoved, "The string being removed is an atom in the string table of an other thread!");
}

void AtomStringImpl::removeDeadAtom(AtomStringImpl* string)
{
    // SPEC-vmstate §4.4.5 — shared-atom-table mode only. Reached exclusively
    // from StringImpl::derefSharedZero(): the refcount just hit 0, and 0 is
    // final (tryRefAtom() refuses to revive at 0), so this thread uniquely
    // owns `string`. Exactly one removeDeadAtom runs per zero transition
    // (invariant I6) — deref()'s fetch_sub hands the zero transition to a
    // single thread.
    ASSERT(sharedAtomStringTableEnabled());
    ASSERT(string->isAtom());
    ASSERT(!string->isStatic()); // Statics rest at masked refcount 0 and never die (I19).
    ASSERT(!string->isSymbol());
    ASSERT(string->length());
    ASSERT(!string->hasAtLeastOneRef());

    {
        // The entry was inserted under shardForHash(<translator hash>), and
        // every translator's hash for a resident atom equals its stored hash
        // (invariant I5), so the dying atom's existingHash() finds the same
        // shard.
        auto& shard = SharedAtomStringTable::singleton().shardForHash(string->existingHash());
        Locker locker { shard.lock };

        // Find by existingHash + POINTER equality, NOT characters: between the
        // refcount hitting 0 and this lock acquisition, a racing add on this
        // shard may have observed the dead entry (tryRefAtom() failed),
        // removed it, and inserted a FRESH atom for the same characters
        // (§4.4.4). A character-based removal would evict that live
        // replacement. Removal is therefore conditional — no unconditional
        // RELEASE_ASSERT(wasRemoved); identity is debug-asserted (I6).
        auto iterator = shard.table.find<AtomStringTableRemovalHashTranslator>(string);
        if (iterator != shard.table.end()) {
            ASSERT(iterator->get() == string); // Translator equality is pointer equality.
            shard.table.remove(iterator);
        }

        // Destructor bypass (§4.4.5): clear isAtom under the shard lock,
        // before destruction, so ~StringImpl skips its legacy isAtom() removal
        // arm — the destructor must NEVER touch the table in shared mode (the
        // entry is already gone, and re-locking the shard from the destructor
        // would self-deadlock if destruction ever moved in-lock). fetch_and
        // (§4.5) cannot drop racing flag bits; no thread can be publishing
        // flags on a refcount-0 string anyway.
        string->setIsAtom(false);
    }

    // Destroy outside the shard lock (I7: the shard lock is a leaf and
    // destruction may run arbitrary deallocation, e.g. BufferExternal free
    // functions or substring-base deref chains that can themselves reach
    // removeDeadAtom for the base — taking another shard lock here would nest
    // shard locks, which is forbidden).
    StringImpl::destroy(string);
}

RefPtr<AtomStringImpl> AtomStringImpl::lookUpSlowCase(StringImpl& string)
{
    // Legacy-mode-only assert — same exemption as addSlowCase: in shared mode
    // the caller's unlocked isAtom()/dynamicDowncast check (lookUp(StringImpl*),
    // AtomStringImpl.h) can legally race another thread atomizing this SAME
    // co-owned StringImpl* in place (addSlowCase sets isAtom under the shard
    // lock), so observing isAtom here is NOT a protocol violation
    // (SPEC-vmstate §4.4.4). The shared arm below is race-correct regardless:
    // the character-keyed find under the shard lock locates the just-created
    // atom — possibly &string itself — and tryRefAtom() succeeds because the
    // caller owns a reference.
    ASSERT_WITH_MESSAGE(sharedAtomStringTableEnabled() || !string.isAtom(), "AtomStringImpl objects should return from the fast case.");

    if (!string.length())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    if (sharedAtomStringTableEnabled()) [[unlikely]] {
        auto& shard = SharedAtomStringTable::singleton().shardForHash(string.hash());
        Locker locker { shard.lock };
        auto iterator = shard.table.find(&string);
        if (iterator == shard.table.end())
            return nullptr;
        StringImpl* existing = iterator->get();
        if (!existing->tryRefAtom())
            return nullptr; // Dead entry == miss (§4.4.4); refcount 0 is final.
        return adoptRef(uncheckedDowncast<AtomStringImpl>(existing));
    }

    // Drift guard (SPEC-vmstate §4.3, ex-M10): legacy arm, unreachable in shared mode.
    ASSERT(!sharedAtomStringTableEnabled());
    AtomStringTableLocker locker;
    auto& atomStringTable = stringTable();
    auto iterator = atomStringTable.find(&string);
    if (iterator != atomStringTable.end())
        return uncheckedDowncast<AtomStringImpl>(iterator->get());
    return nullptr;
}

RefPtr<AtomStringImpl> AtomStringImpl::add(std::span<const char8_t> characters)
{
    if (charactersAreAllASCII(characters))
        return add(byteCast<Latin1Character>(characters));
    auto string = String::fromUTF8(characters);
    if (string.isNull())
        return nullptr;
    return add(string.releaseImpl());
}

// Shared-atom-table lookup (SPEC-vmstate §4.4.4): a hit must take its
// reference via tryRefAtom() under the shard lock; a dead entry is a miss.
template<typename Buffer, typename HashTranslator>
static RefPtr<AtomStringImpl> lookUpInSharedStringTable(const Buffer& buffer)
{
    ASSERT(sharedAtomStringTableEnabled());
    auto& shard = SharedAtomStringTable::singleton().shardForHash(HashTranslator::hash(buffer));
    Locker locker { shard.lock };
    auto iterator = shard.table.template find<HashTranslator>(buffer);
    if (iterator == shard.table.end())
        return nullptr;
    StringImpl* existing = iterator->get();
    if (!existing->tryRefAtom())
        return nullptr; // Dead entry == miss; refcount 0 is final (§4.4).
    return adoptRef(uncheckedDowncast<AtomStringImpl>(existing));
}

RefPtr<AtomStringImpl> AtomStringImpl::lookUp(std::span<const Latin1Character> characters)
{
    if (sharedAtomStringTableEnabled()) [[unlikely]] {
        Latin1Buffer buffer { characters };
        return lookUpInSharedStringTable<Latin1Buffer, Latin1BufferTranslator>(buffer);
    }

    // Drift guard (SPEC-vmstate §4.3, ex-M10): legacy arm, unreachable in shared mode.
    ASSERT(!sharedAtomStringTableEnabled());
    AtomStringTableLocker locker;
    auto& table = stringTable();

    Latin1Buffer buffer { characters };
    auto iterator = table.find<Latin1BufferTranslator>(buffer);
    if (iterator != table.end())
        return uncheckedDowncast<AtomStringImpl>(iterator->get());
    return nullptr;
}

RefPtr<AtomStringImpl> AtomStringImpl::lookUp(std::span<const char16_t> characters)
{
    if (sharedAtomStringTableEnabled()) [[unlikely]] {
        UTF16Buffer buffer { characters };
        return lookUpInSharedStringTable<UTF16Buffer, UTF16BufferTranslator>(buffer);
    }

    // Drift guard (SPEC-vmstate §4.3, ex-M10): legacy arm, unreachable in shared mode.
    ASSERT(!sharedAtomStringTableEnabled());
    AtomStringTableLocker locker;
    auto& table = stringTable();

    UTF16Buffer buffer { characters };
    auto iterator = table.find<UTF16BufferTranslator>(buffer);
    if (iterator != table.end())
        return uncheckedDowncast<AtomStringImpl>(iterator->get());
    return nullptr;
}

#if ASSERT_ENABLED
bool AtomStringImpl::isInAtomStringTable(StringImpl* string)
{
    if (sharedAtomStringTableEnabled()) [[unlikely]] {
        // Rule A1: consult the shard, never any per-thread table. Membership
        // is process-global, so atoms created on other threads are "in the
        // table" here — the cross-thread atom asserts in AtomStringImpl::add
        // remain valid in shared mode.
        auto& shard = SharedAtomStringTable::singleton().shardForHash(string->hash());
        Locker locker { shard.lock };
        return shard.table.contains(string);
    }
    AtomStringTableLocker locker;
    return stringTable().contains(string);
}
#endif

} // namespace WTF
