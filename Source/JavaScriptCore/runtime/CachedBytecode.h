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
#include <array>
#include <wtf/BitVector.h>
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
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
#if USE(BUN_JSC_ADDITIONS)
    // Where this code's cache entry starts within the payload. Non-zero when several modules were encoded into one
    // payload (BytecodeLinkEncoder): every offset a Decoder keeps stays relative to the start of the shared payload.
    size_t entryOffset() const { return m_entryOffset; }
    void setEntryOffset(size_t offset) { m_entryOffset = offset; }
#endif
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
#if USE(BUN_JSC_ADDITIONS)
    size_t m_entryOffset { 0 };
#endif
    LeafExecutableMap m_leafExecutables;
    Vector<CacheUpdate> m_updates;
};

#if USE(BUN_JSC_ADDITIONS)
// The regions of a payload written by BytecodeLinkEncoder, in the order they lie in it.
namespace BytecodeLinkRegions {
enum : unsigned { EarlyHeads, Hot, Unknown, LateHeads, Cold, ExpressionInfo, Count };
}

// What one VM read out of its persistent payloads, in first-use order: the input of a payload order file. Exists only
// while recording (PersistentBytecodePayloads::enableOrderRecording). Every recorder of the process stays registered,
// whether or not its VM is still alive, and any thread may take a snapshot of it (the thread that writes the order file
// when the process exits is not the thread of a Worker's VM), so what it remembers must not die with the VM: functions
// and modules are hashes, and strings are ordinals into a table whose bytes the embedder keeps for good.
class BytecodeOrderRecorder final : public ThreadSafeRefCounted<BytecodeOrderRecorder> {
    WTF_MAKE_NONCOPYABLE(BytecodeOrderRecorder);
    WTF_MAKE_TZONE_ALLOCATED(BytecodeOrderRecorder);
public:
    static Ref<BytecodeOrderRecorder> create();
    ~BytecodeOrderRecorder();
    // In creation order.
    static Vector<Ref<BytecodeOrderRecorder>> allInProcess();
    // Null unless the VM is recording, and while a PauseScope is alive on it.
    static BytecodeOrderRecorder* ifRecording(VM&);

    // While one is alive on the recorder's VM (whose thread it belongs to) nothing is recorded: for code that decodes
    // everything a payload holds rather than what a program uses.
    class PauseScope {
        WTF_MAKE_NONCOPYABLE(PauseScope);
    public:
        explicit PauseScope(VM& vm)
            : m_recorder(ofVM(vm))
        {
            if (m_recorder)
                m_recorder->m_pauseDepth++;
        }
        ~PauseScope()
        {
            if (m_recorder)
                m_recorder->m_pauseDepth--;
        }

    private:
        RefPtr<BytecodeOrderRecorder> m_recorder;
    };

    // bytecodeOrderHash of the function or module.
    void didDecodeFunction(uint64_t hash);
    void didDecodeModule(uint64_t hash);
    void didReadString(std::span<const uint8_t> stringTable, uint32_t ordinal);

    struct Snapshot {
        Vector<uint64_t> functions;
        Vector<uint64_t> modules;
        std::span<const uint8_t> stringTable; // DecoderStringTable's bytes
        Vector<uint32_t> stringOrdinals;
    };
    Snapshot snapshot() const;

private:
    BytecodeOrderRecorder();
    static BytecodeOrderRecorder* ofVM(VM&);

    mutable Lock m_lock;
    Snapshot m_recorded WTF_GUARDED_BY_LOCK(m_lock);
    // A function is decoded again after its code was returned to the cache, and functions with the same text are one.
    using HashSetOfHashes = UncheckedKeyHashSet<uint64_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    HashSetOfHashes m_seenFunctions WTF_GUARDED_BY_LOCK(m_lock);
    HashSetOfHashes m_seenModules WTF_GUARDED_BY_LOCK(m_lock);
    BitVector m_seenStrings WTF_GUARDED_BY_LOCK(m_lock);
    unsigned m_pauseDepth { 0 }; // the VM's thread only
};
#endif

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

#if USE(BUN_JSC_ADDITIONS)
    JS_EXPORT_PRIVATE BytecodeOrderRecorder& enableOrderRecording();
    BytecodeOrderRecorder* orderRecorder() { return m_orderRecorder.get(); }

    // How well the order file a payload was laid out by (BytecodeLinkEncoder) matches what this VM runs: the function
    // bodies decoded out of the payload, by the region they lie in. A body decoded again after its code was returned to
    // the cache counts again. `bytes`: a body's own arrays and record, not what it shares with an identical earlier one.
    struct LinkedPayloadStatistics {
        struct Bodies {
            uint64_t count { 0 };
            uint64_t bytes { 0 };
        };
        std::array<uint32_t, BytecodeLinkRegions::Count> regionEnds { };
        Bodies hot;
        Bodies unknown;
        Bodies cold;
    };
    // The embedder says which of its payloads is a linked one. Nothing is counted for any other payload.
    JS_EXPORT_PRIVATE void setLinkedPayload(std::span<const uint8_t>, const std::array<uint32_t, BytecodeLinkRegions::Count>& regionEnds);
    const LinkedPayloadStatistics* linkedPayloadStatistics() const { return m_linkedPayloadBase ? &m_linkedPayloadStatistics : nullptr; }
    void didDecodeFunctionBody(const void* payloadBase, uint32_t recordOffset, uint32_t bytes)
    {
        if (payloadBase != m_linkedPayloadBase)
            return;
        auto& regionEnds = m_linkedPayloadStatistics.regionEnds;
        auto& bodies = recordOffset < regionEnds[BytecodeLinkRegions::Hot] ? m_linkedPayloadStatistics.hot
            : recordOffset < regionEnds[BytecodeLinkRegions::Unknown] ? m_linkedPayloadStatistics.unknown
            : m_linkedPayloadStatistics.cold;
        bodies.count++;
        bodies.bytes += bytes;
    }
#endif

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
#if USE(BUN_JSC_ADDITIONS)
    RefPtr<BytecodeOrderRecorder> m_orderRecorder;
    const void* m_linkedPayloadBase { nullptr }; // never a Decoder's base while null
    LinkedPayloadStatistics m_linkedPayloadStatistics;
#endif
};

} // namespace JSC
