/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "CacheUpdate.h"
#include "LeafExecutable.h"
#include "ParserModes.h"
#include "Weak.h"
#include "WeakGCHashTable.h"
#include <wtf/HashMap.h>
#include <wtf/FixedVector.h>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class Decoder;
class SourceProvider;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

class CachedBytecode : public RefCounted<CachedBytecode> {
    WTF_MAKE_NONCOPYABLE(CachedBytecode);

public:
    static Ref<CachedBytecode> create()
    {
        return adoptRef(*new CachedBytecode(CachePayload::makeEmptyPayload()));
    }

    static Ref<CachedBytecode> create(FileSystem::MappedFileData&& data, LeafExecutableMap&& leafExecutables = { })
    {
        return adoptRef(*new CachedBytecode(CachePayload::makeMappedPayload(WTF::move(data)), WTF::move(leafExecutables)));
    }

    static Ref<CachedBytecode> create(MallocSpan<uint8_t, VMMalloc>&& data, LeafExecutableMap&& leafExecutables)
    {
        return adoptRef(*new CachedBytecode(CachePayload::makeMallocPayload(WTF::move(data)), WTF::move(leafExecutables)));
    }

    static Ref<CachedBytecode> create(std::span<uint8_t> data, CachePayload::Destructor&& destructor, LeafExecutableMap&& leafExecutables)
    {
        return adoptRef(*new CachedBytecode(CachePayload::makePayloadWithDestructor(data, WTF::move(destructor)), WTF::move(leafExecutables)));
    }

    LeafExecutableMap& leafExecutables() LIFETIME_BOUND { return m_leafExecutables; }

    JS_EXPORT_PRIVATE void addGlobalUpdate(Ref<CachedBytecode>);
    JS_EXPORT_PRIVATE void addFunctionUpdate(const UnlinkedFunctionExecutable*, CodeSpecializationKind, Ref<CachedBytecode>);

    using ForEachUpdateCallback = Function<void(off_t, std::span<const uint8_t>)>;
    JS_EXPORT_PRIVATE void commitUpdates(const ForEachUpdateCallback&) const;

    std::span<const uint8_t> span() const LIFETIME_BOUND { return m_payload.span(); }
    size_t size() const { return m_payload.size(); }
    bool payloadIsPersistent() const { return m_payload.isPersistent(); }
    // Where the root record starts within the payload (a function code block is written after its own arrays).
    size_t rootOffset() const { return m_rootOffset; }
    void setRootOffset(size_t offset) { m_rootOffset = offset; }
    void setPayloadIsPersistent() { m_payload.setIsPersistent(); }
    bool payloadIsOwnedOrPersistent() const { return m_payload.isOwnedOrPersistent(); }
    bool hasUpdates() const { return !m_updates.isEmpty(); }
    size_t sizeForUpdate() const { return m_size; }

private:
    CachedBytecode(CachePayload&& payload, LeafExecutableMap&& leafExecutables = { })
        : m_size(payload.size())
        , m_payload(WTF::move(payload))
        , m_leafExecutables(WTF::move(leafExecutables))
    {
    }

    void copyLeafExecutables(const CachedBytecode&);

    size_t m_size { 0 };
    CachePayload m_payload;
    size_t m_rootOffset { 0 };
    LeafExecutableMap m_leafExecutables;
    Vector<CacheUpdate> m_updates;
};

// The persistent payloads (CachePayload::isPersistent) a VM has code from. An UnlinkedCodeBlock decoded from one
// remembers its slot here and its record's offset, which is enough to decode it again after it was dropped
// (UnlinkedFunctionExecutable::returnCodeToCache) without every block holding on to a Decoder.
// A slot stands for one tree of unlinked code: the payload's bytes as decoded for one SourceProvider. An embedder may wrap
// the same bytes in a new CachedBytecode and SourceProvider per load, and what is decoded for one provider names that
// provider (class sources, source URLs) and belongs to the records that use it; nothing decoded under one slot is ever
// handed to code decoded under another. Code that is private to one executable (a module whose loader has a module scope
// of its own, CodeCache::getUnlinkedGlobalCodeBlock) is not registered at all.
// Mutator thread only.
class PersistentBytecodePayloads final : public WeakGCHashTable {
    WTF_MAKE_NONCOPYABLE(PersistentBytecodePayloads);
    WTF_MAKE_TZONE_ALLOCATED(PersistentBytecodePayloads);
public:
    explicit PersistentBytecodePayloads(VM&);
    ~PersistentBytecodePayloads();

    // 0 if the payload cannot be registered. The slot lives for as long as something holds it: every Decoder made for it
    // and every UnlinkedCodeBlock that carries its index (retain / release). With the last of them the references to the
    // payload and the provider go, and what was remembered under the slot; the index is used again.
    uint16_t indexFor(CachedBytecode&, SourceProvider&);
    void didCreateDecoder(uint16_t index, Decoder&);
    void willDestroyDecoder(uint16_t index, Decoder&);
    void retain(uint16_t index);
    void release(uint16_t index);
    // The payload's live Decoder if it has one (so what it already decoded stays shared), else a new one.
    RefPtr<Decoder> decoderFor(VM&, uint16_t index);

    // What a dropped code block hands on to the one decoded from the same record later:
    // its child executables (declarations first, then expressions), so that the ones still alive by then
    // (closures made from them exist) are used again and closures made afterwards share their code, instead of starting a
    // second generation. Keyed by the parent's record, which the parent knows without looking at the payload: its pages
    // may be paged out when the parent is dropped.
    void rememberChildExecutables(UnlinkedCodeBlock&);
    FixedVector<Weak<UnlinkedFunctionExecutable>> takeChildExecutables(uint16_t index, uint32_t recordOffset)
    {
        return m_childExecutables.isEmpty() ? FixedVector<Weak<UnlinkedFunctionExecutable>>() : m_childExecutables.take(key(index, recordOffset));
    }
    // After each full collection: forget the parents none of whose children are alive any more.
    void pruneStaleEntries() final;
    // Before the heap's last finalization takes the weak references' storage.
    void clearChildExecutables() { m_childExecutables.clear(); }

    // For diagnostics: what this and the Decoders it can reach hold on to.
    struct Statistics {
        size_t payloads { 0 };
        size_t liveDecoders { 0 };
        size_t decoderMappedPointers { 0 };
        size_t decoderAtomsByOrdinal { 0 };
        size_t decoderFinalizers { 0 };
        size_t parentsWithRememberedChildren { 0 };
    };
    JS_EXPORT_PRIVATE Statistics statistics() const;

private:
    static uint64_t key(uint16_t index, uint32_t recordOffset) { return static_cast<uint64_t>(index) << 32 | recordOffset; }

    struct Entry {
        RefPtr<CachedBytecode> payload; // null: the slot is free
        RefPtr<SourceProvider> provider;
        Decoder* decoder { nullptr }; // the first one made for the slot that is still alive
        unsigned users { 0 };
    };
    using Identity = std::pair<const uint8_t*, SourceProvider*>; // the payload's bytes (an embedder may wrap them in a new CachedBytecode per fetch) and who they were decoded for
    void removeEntry(uint16_t index);

    Vector<Entry> m_entries;
    Vector<uint16_t> m_freeIndices;
    UncheckedKeyHashMap<Identity, uint16_t> m_indices;
    VM& m_vm;
    UncheckedKeyHashMap<uint64_t, FixedVector<Weak<UnlinkedFunctionExecutable>>> m_childExecutables;
};

} // namespace JSC
