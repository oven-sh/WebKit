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

namespace JSC {

class BytecodeCacheError;
class CachedBytecode;
class SourceCodeKey;
class SourceProvider;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;

enum class SourceCodeType;

#if USE(BUN_JSC_ADDITIONS)
// Portable on-disk scalar. See the "Portable wire format" comment in
// CachedTypes.cpp for the full rationale. Primary template is intentionally
// undefined so only fixed-width primitives are usable; alignment is 1 so
// Cached* layout has no implementation-defined padding.
template<typename T> struct Wire;

#define BUN_DEFINE_WIRE(T)                                                    \
    template<> struct Wire<T> {                                               \
        uint8_t m_bytes[sizeof(T)];                                           \
        Wire() = default;                                                     \
        ALWAYS_INLINE Wire(T v) { memcpy(m_bytes, &v, sizeof(T)); }           \
        ALWAYS_INLINE operator T() const                                      \
        {                                                                     \
            T v;                                                              \
            memcpy(&v, m_bytes, sizeof(T));                                   \
            return v;                                                         \
        }                                                                     \
        ALWAYS_INLINE Wire& operator=(T v)                                    \
        {                                                                     \
            memcpy(m_bytes, &v, sizeof(T));                                   \
            return *this;                                                     \
        }                                                                     \
    };                                                                        \
    static_assert(sizeof(Wire<T>) == sizeof(T) && alignof(Wire<T>) == 1);     \
    static_assert(std::has_unique_object_representations_v<Wire<T>>           \
        || std::is_same_v<T, double>);

BUN_DEFINE_WIRE(uint8_t)
BUN_DEFINE_WIRE(uint16_t)
BUN_DEFINE_WIRE(uint32_t)
BUN_DEFINE_WIRE(uint64_t)
BUN_DEFINE_WIRE(int8_t)
BUN_DEFINE_WIRE(int16_t)
BUN_DEFINE_WIRE(int32_t)
BUN_DEFINE_WIRE(int64_t)
BUN_DEFINE_WIRE(double)
#undef BUN_DEFINE_WIRE
#endif // USE(BUN_JSC_ADDITIONS)

// This struct has to be updated when incrementally writing to the bytecode
// cache, since this will only be filled in when we parse the function
struct CachedFunctionExecutableMetadata {
#if USE(BUN_JSC_ADDITIONS)
    static_assert(sizeof(CodeFeatures) <= sizeof(uint16_t) && bitWidthOfCodeFeatures <= 16);
    static_assert(sizeof(LexicallyScopedFeatures) <= sizeof(uint8_t) && bitWidthOfLexicallyScopedFeatures <= 8);
    Wire<uint16_t> m_features; // CodeFeatures
    Wire<uint8_t> m_lexicallyScopedFeatures; // LexicallyScopedFeatures
    Wire<uint8_t> m_hasCapturedVariables;
#else
    CodeFeatures m_features;
    LexicallyScopedFeatures m_lexicallyScopedFeatures;
    bool m_hasCapturedVariables;
#endif
};

struct CachedFunctionExecutableOffsets {
    static ptrdiff_t NODELETE codeBlockForCallOffset();
    static ptrdiff_t NODELETE codeBlockForConstructOffset();
    static ptrdiff_t NODELETE metadataOffset();
};

struct CachedWriteBarrierOffsets {
    static ptrdiff_t NODELETE ptrOffset();
};

struct CachedPtrOffsets {
    static ptrdiff_t offsetOffset();
};

class VariableLengthObjectBase {
    friend class CachedBytecode;

protected:
    VariableLengthObjectBase(ptrdiff_t offset)
#if USE(BUN_JSC_ADDITIONS)
    {
        m_offset = offset;
    }
    static_assert(sizeof(ptrdiff_t) == sizeof(int64_t));
    Wire<int64_t> m_offset;
#else
        : m_offset(offset)
    {
    }

    ptrdiff_t m_offset;
#endif
};

class Decoder : public RefCounted<Decoder> {
    WTF_MAKE_NONCOPYABLE(Decoder);

public:
    static Ref<Decoder> create(VM&, Ref<CachedBytecode>, RefPtr<SourceProvider> = nullptr);

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
    UncheckedKeyHashMap<ptrdiff_t, void*> m_offsetToPtrMap;
    Vector<std::function<void()>> m_finalizers;
    UncheckedKeyHashMap<CompactTDZEnvironment*, CompactTDZEnvironmentMap::Handle> m_environmentToHandleMap;
    RefPtr<SourceProvider> m_provider;
};

JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeCodeBlock(VM&, const SourceCodeKey&, const UnlinkedCodeBlock*);
JS_EXPORT_PRIVATE RefPtr<CachedBytecode> encodeCodeBlock(VM&, const SourceCodeKey&, const UnlinkedCodeBlock*, FileSystem::FileHandle&, BytecodeCacheError&);

UnlinkedCodeBlock* decodeCodeBlockImpl(VM&, const SourceCodeKey&, Ref<CachedBytecode>);

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
