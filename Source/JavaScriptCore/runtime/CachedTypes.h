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

#include "JSCast.h"
#include "ParserModes.h"
#include "VariableEnvironment.h"
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/UniqueArray.h>
#include <wtf/text/AtomStringImpl.h>

namespace JSC {

class BytecodeCacheError;
class CachedBytecode;
class SourceCodeKey;
class SourceProvider;
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

struct CachedFunctionExecutableOffsets {
    static ptrdiff_t NODELETE codeBlockForCallOffset();
    static ptrdiff_t NODELETE codeBlockForConstructOffset();
    static ptrdiff_t NODELETE metadataOffset();
    // For re-sealing a record after it is patched in place: the checksum covers [0, extent).
    static ptrdiff_t NODELETE checksumOffset();
    static ptrdiff_t NODELETE extentOffset(); // uint32_t: bytes covered by the record's checksum
    static size_t NODELETE fixedSize();
};

// CRC-32C of `record` with the 4 bytes at `checksumOffset` read as zero (how every checksummed record is sealed).
JS_EXPORT_PRIVATE uint32_t bytecodeCacheRecordChecksum(std::span<const uint8_t> record, size_t checksumOffset);

struct CachedWriteBarrierOffsets {
    static ptrdiff_t NODELETE ptrOffset();
};

struct CachedPtrOffsets {
    static ptrdiff_t offsetOffset();
};

class VariableLengthObjectBase {
    friend class CachedBytecode;

public:
    // Relative offset from this field to the object's payload. A payload is one code block tree, far below 2 GB.
    using Offset = int32_t;

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
    // While a code block record is being decoded, its parsed varint tail, so the several accessors that need it share one parse.
    void setActiveCodeBlockTail(const void* record, const void* tail) { m_activeRecord = record; m_activeTail = tail; }
    const void* activeCodeBlockTail(const void* record) const { return m_activeRecord == record ? m_activeTail : nullptr; }
    bool regionChecksumMatches(const void* start, uint32_t size, const uint32_t* storedChecksum, std::span<const std::span<const uint8_t>> externalArrays = { }) const;
    bool payloadContains(const void* start, size_t size) const;
    // The atom each numbered string record decoded to so far (a +1 reference held until the decoder dies).
    AtomStringImpl* atomForOrdinal(uint32_t) const;
    void setAtomForOrdinal(uint32_t, AtomStringImpl&);
    // Same for 1-3 character strings stored in their slot (keyed by the packed slot value).
    AtomStringImpl* atomForInlineString(uint32_t packed) const;
    void setAtomForInlineString(uint32_t packed, AtomStringImpl&);
    bool recordAndArrayChecksumMatches(const void* record, size_t recordSize, const uint32_t* storedChecksum, const void* array, size_t arraySize) const;

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

    VM& m_vm;
    const Ref<CachedBytecode> m_cachedBytecode;
    Vector<AtomStringImpl*> m_atomsByOrdinal;
    UniqueArray<AtomStringImpl*> m_atomsOfLength1;
    UniqueArray<AtomStringImpl*> m_atomsOfLength2;
    const void* m_activeRecord { nullptr };
    const void* m_activeTail { nullptr };
    UncheckedKeyHashMap<ptrdiff_t, void*> m_offsetToPtrMap;
    Vector<std::function<void()>> m_finalizers;
    UncheckedKeyHashMap<CompactTDZEnvironment*, CompactTDZEnvironmentMap::Handle> m_environmentToHandleMap;
    RefPtr<SourceProvider> m_provider;
};

JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeCodeBlock(VM&, const SourceCodeKey&, const UnlinkedCodeBlock*);
JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeCodeBlock(VM&, const SourceCodeKey&, const UnlinkedCodeBlock*, FileSystem::FileHandle&, BytecodeCacheError&);

UnlinkedCodeBlock* decodeCodeBlockImpl(VM&, const SourceCodeKey&, Ref<CachedBytecode>);

// An embedder's JS builtin (a root UnlinkedFunctionExecutable from BuiltinExecutables::createExecutable), with its code
// blocks generated recursively beforehand (see recursivelyGenerateUnlinkedCodeBlocksForFunction). `embedderStamp`
// identifies the builtin source's contents; decode checks it and the source length instead of hashing the source.
JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeBuiltinFunction(VM&, const UnlinkedFunctionExecutable*, unsigned sourceLength, unsigned embedderStamp);
JS_EXPORT_PRIVATE UnlinkedFunctionExecutable* decodeBuiltinFunction(VM&, Ref<CachedBytecode>, SourceProvider&, unsigned embedderStamp);

template<typename UnlinkedCodeBlockType>
UnlinkedCodeBlockType* decodeCodeBlock(VM& vm, const SourceCodeKey& key, Ref<CachedBytecode> cachedBytecode)
{
    return uncheckedDowncast<UnlinkedCodeBlockType>(decodeCodeBlockImpl(vm, key, WTF::move(cachedBytecode)));
}

std::optional<SourceCodeKey> decodeSourceCodeKey(VM& vm, Ref<CachedBytecode> cachedBytecode);

JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeFunctionCodeBlock(VM&, const UnlinkedFunctionCodeBlock*, BytecodeCacheError&);

JS_EXPORT_PRIVATE void decodeFunctionCodeBlock(Decoder&, int32_t cachedFunctionCodeBlockOffset, WriteBarrier<UnlinkedFunctionCodeBlock>&, const JSCell*);

bool isCachedBytecodeStillValid(VM&, Ref<CachedBytecode>, const SourceCodeKey&, SourceCodeType);

} // namespace JSC
