/*
 * Copyright (C) 2018-2025 Apple Inc. All rights reserved.
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

#include "CollectionScope.h"
#include <wtf/Lock.h>

#include "JSCast.h"
#include "ParserModes.h"
#include "VariableEnvironment.h"
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/UniqueArray.h>
#include <wtf/text/AtomStringImpl.h>
#include <optional>

namespace JSC {

class BytecodeCacheError;
class CachedBytecode;
class SourceCodeKey;
class SourceProvider;
class CachedSymbolTable;
class SymbolTable;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;

enum class SourceCodeType;

// This struct has to be updated when incrementally writing to the bytecode
// cache, since this will only be filled in when we parse the function
struct CachedFunctionExecutableMetadata {
    CodeFeatures m_features;
    LexicallyScopedFeatures m_lexicallyScopedFeatures;
    bool m_hasCapturedVariables;
};

// Per-record CRC-32C checksums. A persistent payload (a section of the executable) does without: corruption there means
// the program is already broken and code signing covers it. Separate on-disk cache files keep them.
enum class BytecodeCacheChecksums : bool { No, Yes };
// Whether executable records keep the fixed fields CachedBytecode::addFunctionUpdate patches when a lazily compiled
// function joins the cache later. A payload generated all at once (bun --compile) needs none of that.
enum class BytecodeCacheUpdatable : bool { No, Yes };

// Offsets within an updatable executable record (the jsc shell's disk cache patches these fields in place).
struct CachedFunctionExecutableOffsets {
    static ptrdiff_t NODELETE codeBlockForCallOffset();
    static ptrdiff_t NODELETE codeBlockForConstructOffset();
    static ptrdiff_t NODELETE metadataOffset();
    // For re-sealing a record after it is patched in place: the checksum covers [0, extent).
    static ptrdiff_t NODELETE checksumOffset();
    static ptrdiff_t NODELETE extentOffset(); // uint32_t: bytes covered by the record's checksum
    static size_t NODELETE fixedSize();
    static bool NODELETE isUpdatable(std::span<const uint8_t> record);
};

// CRC-32C of `record` with the 4 bytes at `checksumOffset` read as zero (how every checksummed record is sealed).
JS_EXPORT_PRIVATE uint32_t bytecodeCacheRecordChecksum(std::span<const uint8_t> record, size_t checksumOffset);

struct CachedWriteBarrierOffsets {
    static ptrdiff_t NODELETE ptrOffset();
};

struct CachedPtrOffsets {
    static ptrdiff_t offsetOffset();
};

// One shared string table across every encodeCodeBlock in a build session (bun --compile --bytecode): each ≥4-char non-symbol string becomes a 4-byte externalStringTag ordinal in every chunk's payload, and the characters are written once by serialize(). Decode reads them from the DecoderStringTable the embedder hands back via VM::ClientData.
class EncoderStringTable {
    WTF_MAKE_NONCOPYABLE(EncoderStringTable);
    WTF_MAKE_TZONE_ALLOCATED_EXPORT(EncoderStringTable, JS_EXPORT_PRIVATE);
public:
    EncoderStringTable() = default;
    JS_EXPORT_PRIVATE ~EncoderStringTable();
    uint32_t ordinalFor(const StringImpl&);
    // The 4-byte slot a cached non-symbol string occupies (CachedPtr's encoding): a 1-3 character Latin-1 string inline,
    // else an ordinal into this table, or the empty sentinel. DecoderStringTable::atomForSlot reads it back.
    JS_EXPORT_PRIVATE uint32_t slotFor(const StringImpl&);
    JS_EXPORT_PRIVATE Vector<uint8_t> serialize() const;
    static constexpr uint32_t maxOrdinal = (1u << 30) - 1;
private:
    UncheckedKeyHashMap<String, uint32_t> m_ordinals;
    Vector<Ref<StringImpl>> m_strings;
};

// Decode side of EncoderStringTable: the mmapped serialize() blob and a demand-zero AtomStringImpl* slot per ordinal so each string goes through the atom table once. One per VM (per thread's atom table); the embedder owns it and returns it from VM::ClientData::decoderStringTable().
class DecoderStringTable {
    WTF_MAKE_NONCOPYABLE(DecoderStringTable);
    WTF_MAKE_TZONE_ALLOCATED_EXPORT(DecoderStringTable, JS_EXPORT_PRIVATE);
public:
    JS_EXPORT_PRIVATE explicit DecoderStringTable(std::span<const uint8_t>);
    JS_EXPORT_PRIVATE ~DecoderStringTable();
    Ref<AtomStringImpl> atomFor(VM&, uint32_t ordinal);
    // The atom for a slot EncoderStringTable::slotFor wrote, resolved as the Decoder resolves the same slot in a code
    // block; null for a malformed slot.
    JS_EXPORT_PRIVATE RefPtr<AtomStringImpl> atomForSlot(VM&, uint32_t slot);
    // StringImpl::hash() of the string atomForSlot(slot) would return, without creating it; nullopt where atomForSlot
    // returns null (a record with bad bounds crashes in both).
    std::optional<uint32_t> hashForSlot(uint32_t slot) const;
    // atomForSlot(vm, slot) would return `string`'s atom, decided without creating it (by the slot's cached pointer when
    // it has one, else by contents); false for a symbol and where atomForSlot returns null (a record with bad bounds
    // crashes in both). Mutator only.
    bool slotEquals(uint32_t slot, const StringImpl&) const;
    // The one JSString this VM uses for the string constant with this ordinal (single characters come from SmallStrings
    // instead). Once a slot holds a cell it keeps it — the cell adopts the StringImpl the slot held, if any — and the
    // table visits it for as long as the VM lives.
    JSString* jsStringFor(VM&, uint32_t ordinal);
    template<typename Visitor> void visitStrongReferences(Visitor&, CollectionScope);
    void didFinishCollection();
    // Options::useFastCachedAtoms(): unaided, atomFor is two to four dependent cache misses (slot -> [cell ->] string
    // header, or slot -> offsets[] -> record -> atom-table bucket) and jsStringFor's miss is three. A caller about to
    // resolve a run of ordinals makes one pass per hop over the run first; each pass is a burst of independent loads, so
    // its misses overlap instead of queueing behind each other inside the decode:
    //   prefetchSlot   - the slot and its offsets[] entry; address arithmetic only, issue as early as the run is known;
    //   prefetchTarget - reads the slot: the StringImpl (For::Atom: or JSString) it holds, else (reads offsets[]) the record;
    //   prefetchLookup - For::Atom only; reads that: a cell's StringImpl header, or an empty slot's atom-table bucket
    //                    by stored hash.
    // noOrdinal (CachedRefPtr::externalStringOrdinal() for a non-table string), or any ordinal out of range, is ignored
    // by all three. Defined in CachedTypes.cpp, their only user.
    enum class PrefetchFor : uint8_t { Atom, JSString }; // atomFor / jsStringFor: the latter touches neither a held cell nor the atom table
    static constexpr unsigned prefetchWindow = 32; // ordinals per burst: about what stays in flight at once, and still cached when the decode reaches the last
    static constexpr uint32_t noOrdinal = std::numeric_limits<uint32_t>::max();
    void prefetchSlot(uint32_t ordinal) const;
    template<PrefetchFor> void prefetchTarget(uint32_t ordinal) const;
    void prefetchLookup(AtomStringTable&, uint32_t ordinal) const;
private:
    static constexpr size_t recordHashOffset = sizeof(uint32_t); // EncoderStringTable::serialize's record layout
    struct Record {
        const uint8_t* characters;
        uint32_t length;
        uint32_t hash;
        bool is8Bit;
    };
    const uint32_t* offsets() const { return std::bit_cast<const uint32_t*>(m_bytes.data() + sizeof(uint32_t)); }
    Record record(uint32_t ordinal) const;
    static Ref<StringImpl> createImpl(const Record&);
    // A slot is empty, a StringImpl* (+1 ref held by the table), or a JSString* tagged with cellTag whose value is that
    // StringImpl. empty -> impl -> cell, never backwards.
    static constexpr uintptr_t cellTag = 1;
    static bool isCell(uintptr_t slot) { return slot & cellTag; }
    static JSString* cell(uintptr_t slot) { return std::bit_cast<JSString*>(slot & ~cellTag); }
    static StringImpl* impl(uintptr_t slot);

    std::span<const uint8_t> m_bytes;
    uintptr_t* m_slots { nullptr }; // demand-zero, one per ordinal
    size_t m_slotsReservation { 0 };
    uint32_t m_count { 0 };
    Lock m_cellsLock;
    Vector<uint32_t> m_cellOrdinals WTF_GUARDED_BY_LOCK(m_cellsLock); // the slots that hold a cell, for visitStrongReferences
    size_t m_visitedCount WTF_GUARDED_BY_LOCK(m_cellsLock) { 0 };
    bool m_visitedThisCycle WTF_GUARDED_BY_LOCK(m_cellsLock) { false };
};

class VariableLengthObjectBase {
    friend class CachedBytecode;

public:
    // Relative offset from this field to the object's payload. A payload is one code block tree, far below 2 GB.
    using Offset = int32_t;

    // A 1-3 character Latin-1 string that decodes to an atom fits in the 4-byte slot that would otherwise hold the offset
    // of its record: low two bits 01 (record offsets are multiples of 4 and the empty sentinel ends in 11), then the
    // length, then the characters. Minified code is mostly such names. Tag 10 is a ≥4-char string held by ordinal in the
    // embedder's EncoderStringTable/DecoderStringTable.
    static constexpr uint32_t inlineStringTag = 1;
    static constexpr uint32_t inlineStringTagMask = 3;
    static constexpr unsigned inlineStringMaxLength = 3;
    static constexpr uint32_t externalStringTag = 2;
    static constexpr uint32_t emptySentinel = std::numeric_limits<int32_t>::max(); // s_invalidOffset
    static std::optional<uint32_t> packInlineString(const StringImpl& string)
    {
        if (string.isSymbol() || !string.length() || string.length() > inlineStringMaxLength)
            return std::nullopt;
        uint32_t packed = inlineStringTag | string.length() << 2;
        for (unsigned i = 0; i < string.length(); ++i) {
            char16_t character = string[i]; // whether the atom happens to be stored 16-bit is not a property of the source
            if (!isLatin1(character))
                return std::nullopt;
            packed |= static_cast<uint32_t>(character) << (8 * (i + 1));
        }
        return packed;
    }

protected:
    VariableLengthObjectBase(Offset offset)
        : m_offset(offset)
    {
    }

    Offset m_offset;
};

class Decoder : public RefCounted<Decoder> {
    WTF_MAKE_NONCOPYABLE(Decoder);

public:
    static Ref<Decoder> create(VM&, Ref<CachedBytecode>, RefPtr<SourceProvider> = nullptr);
    bool canBorrowPayload() const; // the embedder promised the payload outlives every use, so decoded objects may alias it
    bool canDeferIntoPayload() const { return m_canDeferIntoPayload; } // decoded cells may keep a reference to this Decoder plus pointers into the payload and finish decoding on first use
    // While a code block record is being decoded, its parsed varint tail, so the several accessors that need it share one parse.
    void setActiveCodeBlockTail(const void* record, const void* tail) { m_activeRecord = record; m_activeTail = tail; }
    const void* activeCodeBlockTail(const void* record) const { return m_activeRecord == record ? m_activeTail : nullptr; }
    bool regionChecksumMatches(const void* start, uint32_t size, const uint32_t* storedChecksum, std::span<const std::span<const uint8_t>> externalArrays = { }) const;
    bool payloadContains(const void* start, size_t size) const;
    std::span<const uint8_t> payloadSpan() const;
    bool verifiesChecksums() const;
    // The embedder pre-verified the payload (Options::useTrustedEmbeddedBytecodeIntegrity): code blocks skip their checksum
    // and the up-front walk over their child records; the O(1) bounds checks stay.
    bool trustsPayloadIntegrity() const { return m_trustsPayloadIntegrity; }
    // The atom each numbered string record decoded to so far (a +1 reference held until the decoder dies).
    AtomStringImpl* atomForOrdinal(uint32_t) const;
    void setAtomForOrdinal(uint32_t, AtomStringImpl&);
    // 1-3 character strings stored in their slot: length 1 hits SmallStrings, length 2 the VM's shared 65536-entry table.
    static Ref<AtomStringImpl> atomForInlineString(VM&, std::span<const uint8_t, 4> slot);
    Ref<AtomStringImpl> atomForInlineString(std::span<const uint8_t, 4> slot) { return atomForInlineString(m_vm, slot); }
    // Strings stored by ordinal in the embedder's shared DecoderStringTable (externalStringTag slots): every non-empty,
    // non-symbol string when encoding against a table; EncoderStringTable::slotFor (module_info) still inlines 1-3 chars.
    Ref<AtomStringImpl> atomForExternalString(uint32_t ordinal);
    JSString* jsStringForExternalString(uint32_t ordinal);
    // See DecoderStringTable::prefetchSlot. Null with useFastCachedAtoms off or no embedder table (a payload that then
    // names a table string still fails in atomForExternalString, not here).
    const DecoderStringTable* stringsToPrefetch();

    ~Decoder();

    VM& NODELETE vm() { return m_vm; }
    size_t size() const;

    ptrdiff_t offsetOf(const void*);
    void cacheOffset(ptrdiff_t, void*);
    std::optional<void*> cachedPtrForOffset(ptrdiff_t);
    const void* ptrForOffsetFromBase(ptrdiff_t);
    CompactTDZEnvironmentMap::Handle handleForTDZEnvironment(CompactTDZEnvironment*) const;
    void setHandleForTDZEnvironment(CompactTDZEnvironment*, const CompactTDZEnvironmentMap::Handle&);
    void addLeafExecutable(const UnlinkedFunctionExecutable*, ptrdiff_t);
    RefPtr<SourceProvider> NODELETE provider() const;

    template<typename Functor>
    void addFinalizer(const Functor&);

private:
    Decoder(VM&, Ref<CachedBytecode>, RefPtr<SourceProvider>);
    DecoderStringTable& externalStrings();

    VM& m_vm;
    const Ref<CachedBytecode> m_cachedBytecode;
    Vector<AtomStringImpl*> m_atomsByOrdinal;
    DecoderStringTable* m_externalStrings { nullptr };
    bool m_lookedUpExternalStrings { false }; // stringsToPrefetch asked the embedder (m_externalStrings may still be null)
    const void* m_activeRecord { nullptr };
    const void* m_activeTail { nullptr };
    UncheckedKeyHashMap<ptrdiff_t, void*> m_offsetToPtrMap;
    Vector<std::function<void()>> m_finalizers;
    UncheckedKeyHashMap<CompactTDZEnvironment*, CompactTDZEnvironmentMap::Handle> m_environmentToHandleMap;
    RefPtr<SourceProvider> m_provider;
    bool m_trustsPayloadIntegrity { false };
    bool m_canDeferIntoPayload { false };
};

JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeCodeBlock(VM&, const SourceCodeKey&, const UnlinkedCodeBlock*, EncoderStringTable* = nullptr, BytecodeCacheChecksums = BytecodeCacheChecksums::Yes, BytecodeCacheUpdatable = BytecodeCacheUpdatable::Yes);
JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeCodeBlock(VM&, const SourceCodeKey&, const UnlinkedCodeBlock*, FileSystem::FileHandle&, BytecodeCacheError&, EncoderStringTable* = nullptr, BytecodeCacheChecksums = BytecodeCacheChecksums::Yes, BytecodeCacheUpdatable = BytecodeCacheUpdatable::Yes);

UnlinkedCodeBlock* decodeCodeBlockImpl(VM&, const SourceCodeKey&, Ref<CachedBytecode>);

// An embedder's JS builtin (a root UnlinkedFunctionExecutable from BuiltinExecutables::createExecutable), with its code
// blocks generated recursively beforehand (see recursivelyGenerateUnlinkedCodeBlocksForFunction). `embedderStamp`
// identifies the builtin source's contents; decode checks it and the source length instead of hashing the source.
JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeBuiltinFunction(VM&, const UnlinkedFunctionExecutable*, unsigned sourceLength, unsigned embedderStamp, EncoderStringTable* = nullptr, BytecodeCacheChecksums = BytecodeCacheChecksums::Yes, BytecodeCacheUpdatable = BytecodeCacheUpdatable::Yes);
JS_EXPORT_PRIVATE UnlinkedFunctionExecutable* decodeBuiltinFunction(VM&, Ref<CachedBytecode>, SourceProvider&, unsigned embedderStamp);

template<typename UnlinkedCodeBlockType>
UnlinkedCodeBlockType* decodeCodeBlock(VM& vm, const SourceCodeKey& key, Ref<CachedBytecode> cachedBytecode)
{
    return uncheckedDowncast<UnlinkedCodeBlockType>(decodeCodeBlockImpl(vm, key, WTF::move(cachedBytecode)));
}

std::optional<SourceCodeKey> decodeSourceCodeKey(VM& vm, Ref<CachedBytecode> cachedBytecode);

JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeFunctionCodeBlock(VM&, const UnlinkedFunctionCodeBlock*, BytecodeCacheError&);

JS_EXPORT_PRIVATE void decodeFunctionCodeBlock(Decoder&, int32_t cachedFunctionCodeBlockOffset, WriteBarrier<UnlinkedFunctionCodeBlock>&, const JSCell*);

// Options::useLazySymbolTableConstants(): fill in the entries of a SymbolTable whose CachedSymbolTable record was left
// undecoded (SymbolTable::materializeCachedEntries). Mutator only; allocates no GC cells.
void decodeSymbolTableEntries(Decoder&, const CachedSymbolTable&, SymbolTable&, bool scopePartOnly);

bool isCachedBytecodeStillValid(VM&, Ref<CachedBytecode>, const SourceCodeKey&, SourceCodeType);

} // namespace JSC
