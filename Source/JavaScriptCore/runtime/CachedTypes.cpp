/*
 * Copyright (C) 2019-2024 Apple Inc. All rights reserved.
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

#include "config.h"
#include "CachedTypes.h"
#include <wtf/Deque.h>
#include <wtf/Function.h>

#include "BaselineJITCode.h"
#include "BuiltinNames.h"
#include "BytecodeCacheError.h"
#include "BytecodeLivenessAnalysis.h"
#include "JSCBytecodeCacheVersion.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSTemplateObjectDescriptor.h"
#include "ScopedArgumentsTable.h"
#include "SourceCodeKey.h"
#include "SourceProvider.h"
#include "SymbolTableInlines.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedMetadataTableInlines.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VariableEnvironmentInlines.h"
#include <wtf/FileHandle.h>
#include <wtf/InlineMap.h>
#include <wtf/MallocSpan.h>
#include <wtf/Packed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UUID.h>
#include <wtf/text/AtomStringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

bool Decoder::canBorrowPayload() const
{
#if USE(BUN_JSC_ADDITIONS)
    return Options::useBorrowedBytecodeFromCache() && m_cachedBytecode->payloadIsPersistent();
#else
    return false;
#endif
}

// Scalars of the per-function records are written as a LEB128 tail right after the fixed part of the record: most of them
// are small or zero in almost every function, and they are read exactly once, into the object being constructed.
class VarintWriter {
public:
    void u32(uint32_t v)
    {
        while (v >= 0x80) {
            m_bytes.append(static_cast<uint8_t>(v) | 0x80);
            v >>= 7;
        }
        m_bytes.append(static_cast<uint8_t>(v));
    }
    void i32(int32_t v) { u32((static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31)); }
    void u8(uint8_t v) { m_bytes.append(v); }
    size_t size() const { return m_bytes.size(); }
    void copyTo(uint8_t* out) const { memcpy(out, m_bytes.span().data(), m_bytes.size()); }

private:
    Vector<uint8_t, 128> m_bytes;
};

class VarintReader {
public:
    explicit VarintReader(const uint8_t* p)
        : m_p(p)
    {
    }
    uint32_t u32()
    {
        uint32_t v = 0;
        for (unsigned shift = 0;; shift += 7) {
            uint8_t b = *m_p++;
            v |= static_cast<uint32_t>(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
            RELEASE_ASSERT(shift < 28);
        }
    }
    int32_t i32()
    {
        uint32_t v = u32();
        return static_cast<int32_t>((v >> 1) ^ -(v & 1));
    }
    uint8_t u8() { return *m_p++; }

private:
    const uint8_t* m_p;
};

namespace Yarr {
enum class Flags : uint16_t;
}

template <typename T, typename = void>
struct SourceTypeImpl {
    using type = T;
};

template<typename T>
struct SourceTypeImpl<T, std::enable_if_t<!std::is_fundamental<T>::value && !std::is_same<typename T::SourceType_, void>::value>> {
    using type = typename T::SourceType_;

};

template<typename T>
using SourceType = typename SourceTypeImpl<T>::type;

class Encoder {
    WTF_MAKE_NONCOPYABLE(Encoder);
    WTF_FORBID_HEAP_ALLOCATION;

public:
    class Allocation {
        friend class Encoder;

    public:
        uint8_t* NODELETE buffer() const { return m_buffer; }
        ptrdiff_t NODELETE offset() const { return m_offset; }

    private:
        Allocation(uint8_t* buffer, ptrdiff_t offset)
            : m_buffer(buffer)
            , m_offset(offset)
        {
        }

        uint8_t* m_buffer;
        ptrdiff_t m_offset;
    };

    Encoder(VM& vm, FileSystem::FileHandle& fileHandle)
        : m_vm(vm)
        , m_fileHandle(fileHandle)
        , m_baseOffset(0)
        , m_currentPage(nullptr)
    {
        allocateNewPage();
    }

    VM& vm() { return m_vm; }

    Allocation malloc(unsigned size, size_t alignment)
    {
        RELEASE_ASSERT(size);
        ptrdiff_t offset;
        if (m_currentPage->malloc(size, alignment, offset))
            return Allocation { m_currentPage->buffer() + offset, m_baseOffset + offset };
        allocateNewPage(size);
        return malloc(size, alignment);
    }

    template<typename T, typename... Args>
    T* malloc(Args&&... args)
    {
        return new (malloc(sizeof(T), alignof(T)).buffer()) T(std::forward<Args>(args)...);
    }

    template<typename T, typename SourceArg>
    T* mallocFor(const SourceArg& source)
    {
        size_t tail = 0;
        if constexpr (requires { T::tailSize(source); })
            tail = T::tailSize(source);
        return new (malloc(sizeof(T) + tail, alignof(T)).buffer()) T();
    }

    std::span<const uint8_t> bytesAt(ptrdiff_t offset, size_t size)
    {
        ptrdiff_t baseOffset = 0;
        for (const auto& page : m_pages) {
            if (offset - baseOffset < static_cast<ptrdiff_t>(page.size()))
                return page.span().subspan(offset - baseOffset, size);
            baseOffset += page.size();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ptrdiff_t offsetOf(const void* address)
    {
        ptrdiff_t offset;
        ptrdiff_t baseOffset = 0;
        for (const auto& page : m_pages) {
            if (page.getOffset(address, offset))
                return baseOffset + offset;
            baseOffset += page.size();
        }
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
    }

    void cachePtr(const void* ptr, ptrdiff_t offset)
    {
        m_ptrToOffsetMap.add(ptr, offset);
    }

    // Byte-identical immutable arrays (instruction streams, expression info, jump tables of small functions repeat a lot)
    // are stored once; later occurrences point at the first. Decoded objects are per code block either way.
    std::optional<ptrdiff_t> existingIdenticalArray(std::span<const uint8_t> bytes, unsigned hash)
    {
        auto it = m_arraysByHash.find(hash);
        if (it == m_arraysByHash.end())
            return std::nullopt;
        for (auto [candidate, size] : it->value) {
            if (size == bytes.size() && equalSpans(bytesAt(candidate, size), bytes))
                return candidate;
        }
        return std::nullopt;
    }
    void registerArray(unsigned hash, ptrdiff_t offset, size_t size)
    {
        m_arraysByHash.add(hash, Vector<std::pair<ptrdiff_t, size_t>, 1> { }).iterator->value.append({ offset, size });
    }

    std::optional<ptrdiff_t> cachedOffsetForPtr(const void* ptr)
    {
        auto it = m_ptrToOffsetMap.find(ptr);
        if (it == m_ptrToOffsetMap.end())
            return std::nullopt;
        return { it->value };
    }

    void addLeafExecutable(const UnlinkedFunctionExecutable* executable, ptrdiff_t offset)
    {
        m_leafExecutables.add(executable, offset);
    }

    // Layout: a code block's own arrays and its children's executable records are written contiguously; the children's
    // bodies follow breadth-first, and data that is only read on rare paths (expression info) goes after every body.
    // Decoding one block then reads one contiguous run of the payload rather than records scattered through every
    // descendant's subtree, so a mapped payload pages in only what is decoded.
    void deferBody(Function<void()>&& encodeBody) { m_bodies.append(WTF::move(encodeBody)); }
    void deferCold(Function<void()>&& encodeCold) { m_cold.append(WTF::move(encodeCold)); }
    void encodeDeferred()
    {
        while (!m_bodies.isEmpty())
            m_bodies.takeFirst()();
        while (!m_cold.isEmpty()) {
            m_cold.takeFirst()();
            RELEASE_ASSERT(m_bodies.isEmpty());
        }
    }

    RefPtr<CachedBytecode> release(BytecodeCacheError& error)
    {
        if (!m_currentPage)
            return nullptr;
        m_currentPage->alignEnd();

        if (m_fileHandle) {
            return releaseMapped(error);
        }

        size_t size = m_baseOffset + m_currentPage->size();
        auto buffer = MallocSpan<uint8_t, VMMalloc>::malloc(size);
        auto bufferSpan = buffer.mutableSpan();
        for (const auto& page : m_pages)
            memcpySpan(consumeSpan(bufferSpan, page.size()), page.span());
        RELEASE_ASSERT(bufferSpan.empty());
        return CachedBytecode::create(WTF::move(buffer), WTF::move(m_leafExecutables));
    }

private:
    RefPtr<CachedBytecode> releaseMapped(BytecodeCacheError& error)
    {
        size_t size = m_baseOffset + m_currentPage->size();
        if (!m_fileHandle.truncate(size)) {
            error = BytecodeCacheError::StandardError(errno);
            return nullptr;
        }

        for (const auto& page : m_pages) {
            auto bytesWritten = m_fileHandle.write(page.span());
            if (!bytesWritten) {
                error = BytecodeCacheError::StandardError(errno);
                return nullptr;
            }

            if (*bytesWritten != page.size()) {
                error = BytecodeCacheError::WriteError(*bytesWritten, page.size());
                return nullptr;
            }
        }

        auto mappedFileData = m_fileHandle.map(FileSystem::MappedFileMode::Private);
        if (!mappedFileData) {
            error = BytecodeCacheError::StandardError(errno);
            return nullptr;
        }

        return CachedBytecode::create(WTF::move(*mappedFileData), WTF::move(m_leafExecutables));
    }

    class Page {
    public:
        Page(size_t size)
            : m_buffer(MallocSpan<uint8_t, VMMalloc>::malloc(size))
        {
        }

        bool malloc(size_t size, size_t alignment, ptrdiff_t& result)
        {
            ASSERT(alignment && alignment <= alignof(std::max_align_t) && isPowerOfTwo(alignment));
            ptrdiff_t offset = roundUpToMultipleOf(alignment, m_offset);
            if (static_cast<size_t>(offset + size) > capacity())
                return false;

            result = offset;
            m_offset = offset + size;
            return true;
        }

        // FIXME: Port call sites for span() / mutableSpan() and remove.
        const uint8_t* NODELETE buffer() const { return m_buffer.span().data(); }
        uint8_t* NODELETE buffer() { return m_buffer.mutableSpan().data(); }
        size_t size() const { return static_cast<size_t>(m_offset); }

        std::span<uint8_t> mutableSpan() LIFETIME_BOUND { return m_buffer.mutableSpan().first(size()); }
        std::span<const uint8_t> span() const LIFETIME_BOUND { return m_buffer.span().first(size()); }

        bool NODELETE getOffset(const void* address, ptrdiff_t& result) const
        {
            auto* addr = static_cast<const uint8_t*>(address);
            auto* bufferStart = buffer();
            if (addr >= bufferStart && addr < bufferStart + m_offset) {
                result = addr - bufferStart;
                return true;
            }
            return false;
        }

        void NODELETE alignEnd()
        {
            ptrdiff_t size = roundUpToMultipleOf(alignof(std::max_align_t), m_offset);
            if (size == m_offset)
                return;
            RELEASE_ASSERT(static_cast<size_t>(size) <= capacity());
            m_offset = size;
        }

    private:
        size_t capacity() const { return m_buffer.sizeInBytes(); }

        MallocSpan<uint8_t, VMMalloc> m_buffer;
        ptrdiff_t m_offset { 0 };
    };

    void allocateNewPage(size_t size = 0)
    {
        static size_t minPageSize = pageSize();
        if (m_currentPage) {
            m_currentPage->alignEnd();
            m_baseOffset += m_currentPage->size();
        }
        if (size < minPageSize)
            size = minPageSize;
        else
            size = roundUpToMultipleOf(minPageSize, size);
        m_pages.append(Page { size });
        m_currentPage = &m_pages.last();
    }

    VM& m_vm;
    FileSystem::FileHandle& m_fileHandle;
    ptrdiff_t m_baseOffset;
    Page* m_currentPage;
    Vector<Page> m_pages;
    UncheckedKeyHashMap<const void*, ptrdiff_t> m_ptrToOffsetMap;
    LeafExecutableMap m_leafExecutables;
    Deque<Function<void()>> m_bodies;
    Deque<Function<void()>> m_cold;
    UncheckedKeyHashMap<unsigned, Vector<std::pair<ptrdiff_t, size_t>, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_arraysByHash;
};

Decoder::Decoder(VM& vm, Ref<CachedBytecode> cachedBytecode, RefPtr<SourceProvider> provider)
    : m_vm(vm)
    , m_cachedBytecode(WTF::move(cachedBytecode))
    , m_provider(provider)
{
}

Decoder::~Decoder()
{
    for (auto& finalizer : m_finalizers)
        finalizer();
}

Ref<Decoder> Decoder::create(VM& vm, Ref<CachedBytecode> cachedBytecode, RefPtr<SourceProvider> provider)
{
    return adoptRef(*new Decoder(vm, WTF::move(cachedBytecode), WTF::move(provider)));
}

size_t Decoder::size() const
{
    return m_cachedBytecode->size();
}

ptrdiff_t Decoder::offsetOf(const void* ptr)
{
    auto* addr = static_cast<const uint8_t*>(ptr);
    auto cachedBytecodeSpan = m_cachedBytecode->span();
    ASSERT(addr >= cachedBytecodeSpan.data() && addr < std::to_address(cachedBytecodeSpan.end()));
    return addr - cachedBytecodeSpan.data();
}

void Decoder::cacheOffset(ptrdiff_t offset, void* ptr)
{
    m_offsetToPtrMap.add(offset, ptr);
}

std::optional<void*> Decoder::cachedPtrForOffset(ptrdiff_t offset)
{
    auto it = m_offsetToPtrMap.find(offset);
    if (it == m_offsetToPtrMap.end())
        return std::nullopt;
    return { it->value };
}

const void* Decoder::ptrForOffsetFromBase(ptrdiff_t offset)
{
    ASSERT(offset > 0 && static_cast<size_t>(offset) < m_cachedBytecode->size());
    return m_cachedBytecode->span().subspan(offset).data();
}

CompactTDZEnvironmentMap::Handle Decoder::handleForTDZEnvironment(CompactTDZEnvironment* environment) const
{
    auto it = m_environmentToHandleMap.find(environment);
    RELEASE_ASSERT(it != m_environmentToHandleMap.end());
    return it->value;
}

void Decoder::setHandleForTDZEnvironment(CompactTDZEnvironment* environment, const CompactTDZEnvironmentMap::Handle& handle)
{
    auto addResult = m_environmentToHandleMap.add(environment, handle);
    RELEASE_ASSERT(addResult.isNewEntry);
}

void Decoder::addLeafExecutable(const UnlinkedFunctionExecutable* executable, ptrdiff_t offset)
{
#if USE(BUN_JSC_ADDITIONS)
    // Only CachedBytecode::addFunctionUpdate reads this map, and Bun never calls it.
    if (Options::useLeanBytecodeCacheDecoder())
        return;
#endif
    m_cachedBytecode->leafExecutables().add(executable, offset);
}

template<typename Functor>
void Decoder::addFinalizer(const Functor& fn)
{
    m_finalizers.append(fn);
}

RefPtr<SourceProvider> Decoder::provider() const
{
    return m_provider;
}

template<typename T>
static void encode(Encoder& encoder, T& dst, const SourceType<T>& src)
{
    if constexpr (std::is_same_v<T, SourceType<T>>)
        dst = src;
    else
        dst.encode(encoder, src);
}

template<typename T, typename... Args>
static void decode(Decoder& decoder, const T& src, SourceType<T>& dst, Args... args)
{
    if constexpr (std::is_same_v<T, SourceType<T>>)
        dst = src;
    else
        src.decode(decoder, dst, args...);
}

template<typename Source>
class CachedObject {
    WTF_MAKE_NONCOPYABLE(CachedObject);

public:
    using SourceType_ = Source;

    CachedObject() = default;

    inline void* operator new(size_t, void* where) { return where; }
    void* operator new[](size_t, void* where) { return where; }

    // Copied from WTF_FORBID_HEAP_ALLOCATION, since we only want to allow placement new
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;
    void* operator new[](size_t size) = delete;
    void operator delete[](void*) = delete;
    void* operator new(size_t, NotNullTag, void* location) = delete;
};

template<typename Source>
class VariableLengthObject : public CachedObject<Source>, VariableLengthObjectBase {
    template<typename, typename>
    friend class CachedPtr;
    friend struct CachedPtrOffsets;

public:
    using typename VariableLengthObjectBase::Offset;

    VariableLengthObject()
        : VariableLengthObjectBase(s_invalidOffset)
    {
    }

    bool NODELETE isEmpty() const
    {
        return m_offset == s_invalidOffset;
    }

protected:
    const uint8_t* NODELETE buffer() const
    {
        ASSERT(!isEmpty());
        return std::bit_cast<const uint8_t*>(this) + m_offset;
    }

    template<typename T>
    const T* NODELETE buffer() const
    {
        ASSERT(!(std::bit_cast<uintptr_t>(buffer()) % alignof(T)));
        return std::bit_cast<const T*>(buffer());
    }

    uint8_t* allocate(Encoder& encoder, size_t size, size_t alignment)
    {
        ptrdiff_t offsetOffset = encoder.offsetOf(&m_offset);
        auto result = encoder.malloc(size, alignment);
        m_offset = safeCast<Offset>(result.offset() - offsetOffset);
        return result.buffer();
    }

    template<typename T>
#if CPU(ARM64) && CPU(ADDRESS32)
    // FIXME: Remove this once it's no longer needed and LLVM doesn't miscompile us:
    // <rdar://problem/49792205>
    __attribute__((optnone))
#endif
    T* allocate(Encoder& encoder, unsigned size = 1)
    {
        uint8_t* result = allocate(encoder, sizeof(T) * size, alignof(T));
        ASSERT(!(std::bit_cast<uintptr_t>(result) % alignof(T)));
        return new (result) T[size];
    }

    // For arrays whose encoding is a plain copy of the source bytes: share an earlier identical array if there is one.
    void allocateOrShareBytes(Encoder& encoder, std::span<const uint8_t> bytes, size_t alignment)
    {
        unsigned hash = StringHasher::computeHashAndMaskTop8Bits(bytes) ^ static_cast<unsigned>(bytes.size());
        if (auto existing = encoder.existingIdenticalArray(bytes, hash)) {
            m_offset = safeCast<Offset>(*existing - encoder.offsetOf(&m_offset));
            return;
        }
        ptrdiff_t offsetOffset = encoder.offsetOf(&m_offset);
        auto result = encoder.malloc(bytes.size(), alignment);
        m_offset = safeCast<Offset>(result.offset() - offsetOffset);
        memcpySpan(std::span { result.buffer(), bytes.size() }, bytes);
        encoder.registerArray(hash, result.offset(), bytes.size());
    }

    // One T followed, in the same allocation, by the variable-length tail T asks for (see VarintWriter).
    template<typename T, typename SourceArg>
    T* allocateFor(Encoder& encoder, const SourceArg& source)
    {
        size_t tail = 0;
        if constexpr (requires { T::tailSize(source); })
            tail = T::tailSize(source);
        uint8_t* result = allocate(encoder, sizeof(T) + tail, alignof(T));
        return new (result) T();
    }

private:
    constexpr static Offset s_invalidOffset = std::numeric_limits<Offset>::max();
};

template<typename T, typename Source = SourceType<T>>
class CachedArray : public VariableLengthObject<Source*> {
public:
    void encode(Encoder& encoder, const Source* array, unsigned size)
    {
        if (!size)
            return;
        if constexpr (std::is_same_v<T, Source> && std::is_trivially_copyable_v<T>) {
            this->allocateOrShareBytes(encoder, std::span { std::bit_cast<const uint8_t*>(array), sizeof(T) * size }, alignof(T));
            return;
        }
        T* dst = this->template allocate<T>(encoder, size);
        for (unsigned i = 0; i < size; ++i)
            ::JSC::encode(encoder, dst[i], array[i]);
    }

    template<typename... Args>
    void decode(Decoder& decoder, Source* array, unsigned size, Args... args) const
    {
        if (!size)
            return;
        const T* buffer = this->template buffer<T>();
        for (unsigned i = 0; i < size; ++i)
            ::JSC::decode(decoder, buffer[i], array[i], args...);
    }

    // Raw view of the encoded elements, for element types whose encoding is the identity.
    const T* borrow() const
    {
        static_assert(std::is_same_v<T, Source> && std::is_trivially_copyable_v<T>);
        return this->isEmpty() ? nullptr : this->template buffer<T>();
    }
};

#if USE(BUN_JSC_ADDITIONS)
// A cached type declares `static constexpr bool isSingleOwner = true` when the Encoder
// only ever reaches it through one CachedPtr, so there is nothing for the
// ptr <-> offset maps to deduplicate on either side.
template<typename T> inline constexpr bool isSingleOwnerCachedType = requires { T::isSingleOwner; };

// A cached type declares `static constexpr bool decodesToCanonicalObject = true` when its
// decode() returns a +1 reference to an object that is already unique for its content
// (atoms, registry symbols), so shared references can be re-decoded instead of mapped.
template<typename T> inline constexpr bool isCanonicalCachedType = requires { T::decodesToCanonicalObject; };
#endif

template<typename T, typename Source = SourceType<T>>
class CachedPtr : public VariableLengthObject<Source*> {
    template<typename, typename, typename>
    friend class CachedRefPtr;

    friend struct CachedPtrOffsets;

public:
    void encode(Encoder& encoder, const Source* src)
    {
        if (!src)
            return;

#if USE(BUN_JSC_ADDITIONS)
        if constexpr (isSingleOwnerCachedType<T>) {
            ASSERT(!encoder.cachedOffsetForPtr(src));
            this->template allocateFor<T>(encoder, *src)->encode(encoder, *src);
            return;
        }
#endif

        if (std::optional<ptrdiff_t> offset = encoder.cachedOffsetForPtr(src)) {
            this->m_offset = safeCast<VariableLengthObjectBase::Offset>(*offset - encoder.offsetOf(&this->m_offset));
            return;
        }

        T* cachedObject = this->template allocateFor<T>(encoder, *src);
        cachedObject->encode(encoder, *src);
        encoder.cachePtr(src, encoder.offsetOf(cachedObject));
    }

    template<typename... Args>
    Source* decode(Decoder& decoder, bool& isNewAllocation, Args&&... args) const
    {
        if (this->isEmpty()) {
            isNewAllocation = false;
            return nullptr;
        }

#if USE(BUN_JSC_ADDITIONS)
        if constexpr (isSingleOwnerCachedType<T>) {
            if (Options::useLeanBytecodeCacheDecoder()) {
                isNewAllocation = true;
                return get()->decode(decoder, std::forward<Args>(args)...);
            }
        }
#endif

        ptrdiff_t bufferOffset = decoder.offsetOf(this->buffer());
        if (std::optional<void*> ptr = decoder.cachedPtrForOffset(bufferOffset)) {
            isNewAllocation = false;
            return static_cast<Source*>(*ptr);
        }

        isNewAllocation = true;
        Source* ptr = get()->decode(decoder, std::forward<Args>(args)...);
        decoder.cacheOffset(bufferOffset, ptr);
        return ptr;
    }

    template<typename... Args>
    Source* decode(Decoder& decoder, Args&&... args) const
    {
        bool unusedIsNewAllocation;
        return decode(decoder, unusedIsNewAllocation, std::forward<Args>(args)...);
    }

    const T* NODELETE operator->() const { return get(); }

private:
    const T* NODELETE get() const
    {
        RELEASE_ASSERT(!this->isEmpty());
        return this->template buffer<T>();
    }
};

ptrdiff_t CachedPtrOffsets::offsetOffset()
{
    return OBJECT_OFFSETOF(CachedPtr<void>, m_offset);
}

template<typename T, typename Source = SourceType<T>, typename PtrTraits = RawPtrTraits<Source>>
class CachedRefPtr : public CachedObject<RefPtr<Source, PtrTraits>> {
public:
    void encode(Encoder& encoder, const Source* src)
    {
        m_ptr.encode(encoder, src);
    }

    void encode(Encoder& encoder, const RefPtr<Source, PtrTraits> src)
    {
        encode(encoder, src.get());
    }

    RefPtr<Source, PtrTraits> decode(Decoder& decoder) const
    {
#if USE(BUN_JSC_ADDITIONS)
        if constexpr (isCanonicalCachedType<T>) {
            if (Options::useLeanBytecodeCacheDecoder()) {
                if (m_ptr.isEmpty())
                    return nullptr;
                return adoptRef<Source, PtrTraits>(m_ptr.get()->decode(decoder));
            }
        }
#endif
        bool isNewAllocation;
        Source* decodedPtr = m_ptr.decode(decoder, isNewAllocation);
        if (!decodedPtr)
            return nullptr;
        if (isNewAllocation) {
            decoder.addFinalizer([=] {
                WTF::DefaultRefDerefTraits<Source>::derefIfNotNull(decodedPtr);
            });
        }
        auto result = adoptRef<Source, PtrTraits>(decodedPtr);
        result->ref();
        return result;
    }

    void decode(Decoder& decoder, RefPtr<Source, PtrTraits>& src) const
    {
        src = decode(decoder);
    }

private:
    CachedPtr<T, Source> m_ptr;
};

template<typename T, typename Source = SourceType<T>>
class CachedWriteBarrier : public CachedObject<WriteBarrier<Source>> {
    friend struct CachedWriteBarrierOffsets;

public:
    bool NODELETE isEmpty() const { return m_ptr.isEmpty(); }

    void encode(Encoder& encoder, const WriteBarrier<Source> src)
    {
        m_ptr.encode(encoder, src.get());
    }

    void decode(Decoder& decoder, WriteBarrier<Source>& src, const JSCell* owner) const
    {
        Source* decodedPtr = m_ptr.decode(decoder);
        if (decodedPtr)
            src.set(decoder.vm(), owner, decodedPtr);
    }

private:
    CachedPtr<T, Source> m_ptr;
};

ptrdiff_t CachedWriteBarrierOffsets::ptrOffset()
{
    return OBJECT_OFFSETOF(CachedWriteBarrier<void>, m_ptr);
}

template<typename T, size_t InlineCapacity = 0, typename OverflowHandler = CrashOnOverflow, typename Malloc = WTF::VectorBufferMalloc>
class CachedVector : public VariableLengthObject<Vector<SourceType<T>, InlineCapacity, OverflowHandler, 16, Malloc>> {
public:
    template<typename VectorContainer>
    void encode(Encoder& encoder, const VectorContainer& vector)
    {
        m_size = vector.size();
        if (!m_size)
            return;
        if constexpr (std::is_same_v<T, SourceType<T>> && std::is_trivially_copyable_v<T>) {
            this->allocateOrShareBytes(encoder, std::span { std::bit_cast<const uint8_t*>(vector.span().data()), sizeof(T) * m_size }, alignof(T));
            return;
        }
        T* buffer = this->template allocate<T>(encoder, m_size);
        for (unsigned i = 0; i < m_size; ++i)
            ::JSC::encode(encoder, buffer[i], vector[i]);
    }

    template<typename... Args, typename VectorContainer>
    void decode(Decoder& decoder, VectorContainer& vector, Args... args) const
    {
        if (!m_size)
            return;
        vector = VectorContainer(m_size);
        const T* buffer = this->template buffer<T>();
        for (unsigned i = 0; i < m_size; ++i)
            ::JSC::decode(decoder, buffer[i], vector[i], args...);
    }

    // Raw view of the encoded elements, for element types whose encoding is the identity.
    std::span<const T> borrow() const
    {
        static_assert(std::is_same_v<T, SourceType<T>> && std::is_trivially_copyable_v<T>);
        if (!m_size)
            return { };
        return { this->template buffer<T>(), m_size };
    }

private:
    unsigned m_size;
};

template<typename First, typename Second>
class CachedPair : public CachedObject<std::pair<SourceType<First>, SourceType<Second>>> {
public:
    void encode(Encoder& encoder, const std::pair<SourceType<First>, SourceType<Second>>& pair)
    {
        ::JSC::encode(encoder, m_first, pair.first);
        ::JSC::encode(encoder, m_second, pair.second);
    }

    void decode(Decoder& decoder, std::pair<SourceType<First>, SourceType<Second>>& pair) const
    {
        ::JSC::decode(decoder, m_first, pair.first);
        ::JSC::decode(decoder, m_second, pair.second);
    }

private:
    First m_first;
    Second m_second;
};

template<typename Key, typename Value, typename HashArg = DefaultHash<SourceType<Key>>, typename KeyTraitsArg = HashTraits<SourceType<Key>>, typename MappedTraitsArg = HashTraits<SourceType<Value>>, typename TableTraits = WTF::HashTableTraits>
class CachedHashMap : public CachedObject<HashMap<SourceType<Key>, SourceType<Value>, HashArg, KeyTraitsArg, MappedTraitsArg, TableTraits>> {
    template<typename K, typename V, WTF::ShouldValidateKey shouldValidateKey>
    using Map = HashMap<K, V, HashArg, KeyTraitsArg, MappedTraitsArg, TableTraits, shouldValidateKey>;

public:
    template<WTF::ShouldValidateKey shouldValidateKey>
    void encode(Encoder& encoder, const Map<SourceType<Key>, SourceType<Value>, shouldValidateKey>& map)
    {
        SourceType<decltype(m_entries)> entriesVector(map.size());
        unsigned i = 0;
        for (const auto& it : map)
            entriesVector[i++] = { it.key, it.value };
        m_entries.encode(encoder, entriesVector);
    }

    template<WTF::ShouldValidateKey shouldValidateKey>
    void decode(Decoder& decoder, Map<SourceType<Key>, SourceType<Value>, shouldValidateKey>& map) const
    {
        SourceType<decltype(m_entries)> decodedEntries;
        m_entries.decode(decoder, decodedEntries);
        for (const auto& pair : decodedEntries)
            map.set(pair.first, pair.second);
    }

private:
    CachedVector<CachedPair<Key, Value>> m_entries;
};

template<typename Key, typename Value, typename HashArg = DefaultHash<SourceType<Key>>, typename KeyTraitsArg = HashTraits<SourceType<Key>>, typename MappedTraitsArg = HashTraits<SourceType<Value>>>
using CachedMemoryCompactLookupOnlyRobinHoodHashMap = CachedHashMap<Key, Value, HashArg, KeyTraitsArg, MappedTraitsArg, WTF::MemoryCompactLookupOnlyRobinHoodHashTableTraits>;

template<typename Key, typename Value, unsigned Capacity, typename HashArg = DefaultHash<SourceType<Key>>, typename KeyTraitsArg = HashTraits<SourceType<Key>>, typename MappedTraitsArg = HashTraits<SourceType<Value>>>
class CachedInlineMap : public CachedObject<InlineMap<SourceType<Key>, SourceType<Value>, Capacity, HashArg, KeyTraitsArg, MappedTraitsArg>> {

    using Map = InlineMap<SourceType<Key>, SourceType<Value>, Capacity, HashArg, KeyTraitsArg, MappedTraitsArg>;

public:

    void encode(Encoder& encoder, const Map& map)
    {
        SourceType<decltype(m_entries)> entriesVector(map.size());
        unsigned i = 0;
        for (const auto& it : map)
            entriesVector[i++] = { it.key, it.value };
        m_entries.encode(encoder, entriesVector);
    }

    void decode(Decoder& decoder, Map& map) const
    {
        SourceType<decltype(m_entries)> decodedEntries;
        m_entries.decode(decoder, decodedEntries);
        map.reserveInitialCapacity(decodedEntries.size());
        for (const auto& pair : decodedEntries)
            map.add(pair.first, pair.second);
    }

private:
    CachedVector<CachedPair<Key, Value>> m_entries;
};

template<typename T>
class CachedUniquedStringImplBase : public CachedObject<T> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool decodesToCanonicalObject = true;
#endif

    // The characters follow this 4-byte header directly (see tailSize), instead of a separately aligned allocation
    // reached through an offset.
    static size_t tailSize(const StringImpl& string) { return Shape(string).byteLength(); }

    void encode(Encoder&, const StringImpl& string)
    {
        Shape shape(string);
        m_isSymbol = shape.isSymbol;
        m_isRegistered = shape.isRegistered;
        m_isWellKnownSymbol = shape.isWellKnownSymbol;
        m_isPrivate = shape.isPrivate;
        m_is8Bit = shape.characters->is8Bit();
        m_length = shape.characters->length();
        RELEASE_ASSERT(m_length == shape.characters->length()); // fits the bitfield
        if (m_is8Bit)
            memcpy(tail(), shape.characters->span8().data(), shape.byteLength());
        else
            memcpy(tail(), shape.characters->span16().data(), shape.byteLength());
    }

    UniquedStringImpl* decode(Decoder& decoder) const
    {
        auto create = [&](auto buffer) -> UniquedStringImpl* {
            if (!m_isSymbol)
                return AtomStringImpl::add(buffer).leakRef();

            SymbolImpl* symbol;
            VM& vm = decoder.vm();
            if (m_isRegistered) {
                String str(buffer);
                if (m_isPrivate)
                    symbol = static_cast<SymbolImpl*>(&protect(vm.privateSymbolRegistry())->symbolForKey(str).leakRef());
                else
                    symbol = static_cast<SymbolImpl*>(&protect(vm.symbolRegistry())->symbolForKey(str).leakRef());
            } else if (m_isWellKnownSymbol)
                symbol = vm.propertyNames->builtinNames().lookUpWellKnownSymbol(buffer);
            else
                symbol = vm.propertyNames->builtinNames().lookUpPrivateName(buffer);
            RELEASE_ASSERT(symbol);
            String str = symbol;
            StringImpl* impl = str.releaseImpl().unsafeGet();
            ASSERT(impl->isSymbol());
            if (m_isWellKnownSymbol)
                ASSERT(!static_cast<SymbolImpl*>(impl)->isPrivate());
            else
                ASSERT(static_cast<SymbolImpl*>(impl)->isPrivate());
            return static_cast<UniquedStringImpl*>(impl);
        };

        if (!m_length) {
            if (m_isSymbol)
                return &SymbolImpl::createNullSymbol().leakRef();
            return RefPtr { emptyAtom().impl() }.leakRef();
        }

        return m_is8Bit ? create(span8()) : create(span16());
    }

    std::span<const Latin1Character> NODELETE span8() const LIFETIME_BOUND { return { std::bit_cast<const Latin1Character*>(tail()), m_length }; }
    std::span<const char16_t> NODELETE span16() const LIFETIME_BOUND { return { std::bit_cast<const char16_t*>(tail()), m_length }; }

private:
    // What is actually stored for a given string: well-known symbols are stored by their description minus "Symbol.".
    struct Shape {
        explicit Shape(const StringImpl& string)
            : characters(const_cast<StringImpl*>(&string))
            , isSymbol(string.isSymbol())
        {
            if (isSymbol) {
                SymbolImpl& symbol = static_cast<SymbolImpl&>(*characters);
                isRegistered = symbol.isRegistered();
                isPrivate = symbol.isPrivate();
                if (!symbol.isNullSymbol() && !isPrivate) {
                    isWellKnownSymbol = true;
                    characters = symbol.substring(strlen("Symbol."));
                }
            }
        }
        size_t byteLength() const { return characters->length() * (characters->is8Bit() ? 1 : 2); }
        RefPtr<StringImpl> characters;
        bool isSymbol { false };
        bool isRegistered { false };
        bool isWellKnownSymbol { false };
        bool isPrivate { false };
    };
    const uint8_t* tail() const { return std::bit_cast<const uint8_t*>(this + 1); }
    uint8_t* tail() { return std::bit_cast<uint8_t*>(this + 1); }

    uint32_t m_length : 27;
    uint32_t m_is8Bit : 1;
    uint32_t m_isSymbol : 1;
    uint32_t m_isWellKnownSymbol : 1;
    uint32_t m_isRegistered : 1;
    uint32_t m_isPrivate : 1;
};
class CachedUniquedStringImpl : public CachedUniquedStringImplBase<UniquedStringImpl> { };
class CachedStringImpl : public CachedUniquedStringImplBase<StringImpl> { };

class CachedString : public CachedObject<String> {
public:
    void encode(Encoder& encoder, const String& string)
    {
        m_impl.encode(encoder, static_cast<UniquedStringImpl*>(string.impl()));
    }

    String decode(Decoder& decoder) const
    {
        return String(static_cast<RefPtr<StringImpl>>(m_impl.decode(decoder)));
    }

    void decode(Decoder& decoder, String& dst) const
    {
        dst = decode(decoder);
    }

private:
    CachedRefPtr<CachedUniquedStringImpl> m_impl;
};

class CachedIdentifier : public CachedObject<Identifier> {
public:
    void encode(Encoder& encoder, const Identifier& identifier)
    {
        m_string.encode(encoder, identifier.string());
    }

    Identifier decode(Decoder& decoder) const
    {
        String str = m_string.decode(decoder);
        if (str.isNull())
            return Identifier();

        return Identifier::fromUid(decoder.vm(), (UniquedStringImpl*)str.impl());
    }

    void decode(Decoder& decoder, Identifier& ident) const
    {
        ident = decode(decoder);
    }

private:
    CachedString m_string;
};

template<typename T>
class CachedOptional : public VariableLengthObject<std::optional<SourceType<T>>> {
public:
    void encode(Encoder& encoder, const std::optional<SourceType<T>>& source)
    {
        if (!source)
            return;

        this->template allocateFor<T>(encoder, *source)->encode(encoder, *source);
    }

    std::optional<SourceType<T>> decode(Decoder& decoder) const
    {
        if (this->isEmpty())
            return std::nullopt;

        return { this->template buffer<T>()->decode(decoder) };
    }

    void decode(Decoder& decoder, std::optional<SourceType<T>>& dst) const
    {
        dst = decode(decoder);
    }

    void encode(Encoder& encoder, const std::unique_ptr<SourceType<T>>& source)
    {
        if (!source)
            encode(encoder, std::nullopt);
        else
            encode(encoder, { *source });
    }

    SourceType<T>* decodeAsPtr(Decoder& decoder) const
    {
        RELEASE_ASSERT(!this->isEmpty());
        return this->template buffer<T>()->decode(decoder);
    }
};

class CachedSimpleJumpTable : public CachedObject<UnlinkedSimpleJumpTable> {
public:
    void encode(Encoder& encoder, const UnlinkedSimpleJumpTable& jumpTable)
    {
        m_min = jumpTable.m_min;
        m_defaultOffset = jumpTable.m_defaultOffset;
        m_isList = jumpTable.m_isList;
        m_branchOffsets.encode(encoder, jumpTable.m_branchOffsets);
    }

    void decode(Decoder& decoder, UnlinkedSimpleJumpTable& jumpTable) const
    {
        jumpTable.m_min = m_min;
        jumpTable.m_defaultOffset = m_defaultOffset;
        jumpTable.m_isList = m_isList;
        m_branchOffsets.decode(decoder, jumpTable.m_branchOffsets);
    }

private:
    int32_t m_min;
    int32_t m_defaultOffset;
    int32_t m_isList;
    CachedVector<int32_t> m_branchOffsets;
};

class CachedStringJumpTable : public CachedObject<UnlinkedStringJumpTable> {
public:
    void encode(Encoder& encoder, const UnlinkedStringJumpTable& jumpTable)
    {
        m_offsetTable.encode(encoder, jumpTable.m_offsetTable);
        m_minLength = jumpTable.m_minLength;
        m_maxLength = jumpTable.m_maxLength;
        m_defaultOffset = jumpTable.m_defaultOffset;
    }

    void decode(Decoder& decoder, UnlinkedStringJumpTable& jumpTable) const
    {
        m_offsetTable.decode(decoder, jumpTable.m_offsetTable);
        jumpTable.m_minLength = m_minLength;
        jumpTable.m_maxLength = m_maxLength;
        jumpTable.m_defaultOffset = m_defaultOffset;
    }

private:
    CachedMemoryCompactLookupOnlyRobinHoodHashMap<CachedRefPtr<CachedStringImpl>, UnlinkedStringJumpTable::OffsetLocation> m_offsetTable;
    unsigned m_minLength { 0 };
    unsigned m_maxLength { 0 };
    int32_t m_defaultOffset { 0 };
};

class CachedBitVector : public VariableLengthObject<BitVector> {
public:
    void encode(Encoder& encoder, const BitVector& bitVector)
    {
        m_numBits = bitVector.size();
        if (!m_numBits)
            return;
        size_t sizeInBytes = BitVector::byteCount(m_numBits);
        uint8_t* buffer = this->allocate(encoder, sizeInBytes, alignof(uintptr_t));
        memcpy(buffer, bitVector.words().data(), sizeInBytes);
    }

    void decode(Decoder&, BitVector& bitVector) const
    {
        if (!m_numBits)
            return;
        bitVector.ensureSize(m_numBits);
        size_t sizeInBytes = BitVector::byteCount(m_numBits);
        memcpy(bitVector.words().data(), this->buffer(), sizeInBytes);
    }

private:
    size_t m_numBits;
};

template<typename T, typename HashArg = DefaultHash<T>>
class CachedHashSet : public CachedObject<UncheckedKeyHashSet<SourceType<T>, HashArg>> {
public:
    void encode(Encoder& encoder, const UncheckedKeyHashSet<SourceType<T>, HashArg>& set)
    {
        SourceType<decltype(m_entries)> entriesVector(set.size());
        unsigned i = 0;
        for (const auto& item : set)
            entriesVector[i++] = item;
        m_entries.encode(encoder, entriesVector);
    }

    void decode(Decoder& decoder, UncheckedKeyHashSet<SourceType<T>, HashArg>& set) const
    {
        SourceType<decltype(m_entries)> entriesVector;
        m_entries.decode(decoder, entriesVector);
        for (const auto& item : entriesVector)
            set.add(item);
    }

private:
    CachedVector<T> m_entries;
};

class CachedCodeBlockRareData : public CachedObject<UnlinkedCodeBlock::RareData> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const UnlinkedCodeBlock::RareData& rareData)
    {
        m_exceptionHandlers.encode(encoder, rareData.m_exceptionHandlers);
        m_unlinkedSwitchJumpTables.encode(encoder, rareData.m_unlinkedSwitchJumpTables);
        m_unlinkedStringSwitchJumpTables.encode(encoder, rareData.m_unlinkedStringSwitchJumpTables);
        m_typeProfilerInfoMap.encode(encoder, rareData.m_typeProfilerInfoMap);
        m_opProfileControlFlowBytecodeOffsets.encode(encoder, rareData.m_opProfileControlFlowBytecodeOffsets);
        m_bitVectors.encode(encoder, rareData.m_bitVectors);
        m_constantIdentifierSets.encode(encoder, rareData.m_constantIdentifierSets);
        m_needsClassFieldInitializer = rareData.m_needsClassFieldInitializer;
        m_privateBrandRequirement = rareData.m_privateBrandRequirement;
    }

    UnlinkedCodeBlock::RareData* decode(Decoder& decoder) const
    {
        UnlinkedCodeBlock::RareData* rareData = new UnlinkedCodeBlock::RareData { };
        m_exceptionHandlers.decode(decoder, rareData->m_exceptionHandlers);
        m_unlinkedSwitchJumpTables.decode(decoder, rareData->m_unlinkedSwitchJumpTables);
        m_unlinkedStringSwitchJumpTables.decode(decoder, rareData->m_unlinkedStringSwitchJumpTables);
        m_typeProfilerInfoMap.decode(decoder, rareData->m_typeProfilerInfoMap);
        m_opProfileControlFlowBytecodeOffsets.decode(decoder, rareData->m_opProfileControlFlowBytecodeOffsets);
        m_bitVectors.decode(decoder, rareData->m_bitVectors);
        m_constantIdentifierSets.decode(decoder, rareData->m_constantIdentifierSets);
        rareData->m_needsClassFieldInitializer = m_needsClassFieldInitializer;
        rareData->m_privateBrandRequirement = m_privateBrandRequirement;
        return rareData;
    }

private:
    CachedVector<UnlinkedHandlerInfo> m_exceptionHandlers;
    CachedVector<CachedSimpleJumpTable> m_unlinkedSwitchJumpTables;
    CachedVector<CachedStringJumpTable> m_unlinkedStringSwitchJumpTables;
    CachedHashMap<unsigned, UnlinkedCodeBlock::RareData::TypeProfilerExpressionRange> m_typeProfilerInfoMap;
    CachedVector<JSInstructionStream::Offset> m_opProfileControlFlowBytecodeOffsets;
    CachedVector<CachedBitVector> m_bitVectors;
    CachedVector<CachedHashSet<CachedRefPtr<CachedUniquedStringImpl>, IdentifierRepHash>> m_constantIdentifierSets;
    unsigned m_needsClassFieldInitializer : 1;
    unsigned m_privateBrandRequirement : 1;
};

class CachedExpressionInfo : public CachedObject<ExpressionInfo> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const ExpressionInfo& info)
    {
        m_numberOfChapters = info.m_numberOfChapters;
        m_numberOfEncodedInfo = info.m_numberOfEncodedInfo;
        m_numberOfEncodedInfoExtensions = info.m_numberOfEncodedInfoExtensions;
        m_storage.encode(encoder, info.payload(), info.payloadSize());
    }

    std::unique_ptr<ExpressionInfo> decode(Decoder& decoder) const
    {
        if (decoder.canBorrowPayload() && !m_storage.isEmpty())
            return ExpressionInfo::createBorrowed(m_numberOfChapters, m_numberOfEncodedInfo, m_numberOfEncodedInfoExtensions, m_storage.borrow());
        auto info = ExpressionInfo::createUninitialized(m_numberOfChapters, m_numberOfEncodedInfo, m_numberOfEncodedInfoExtensions);
        m_storage.decode(decoder, info->payload(), info->payloadSize());
        return info;
    }

private:
    unsigned m_numberOfChapters;
    unsigned m_numberOfEncodedInfo;
    unsigned m_numberOfEncodedInfoExtensions;
    CachedArray<unsigned> m_storage;
};

typedef CachedHashMap<CachedRefPtr<CachedUniquedStringImpl, UniquedStringImpl, WTF::PackedPtrTraits<UniquedStringImpl>>, PrivateNameEntry, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>, PrivateNameEntryHashTraits> CachedPrivateNameEnvironment;

class CachedVariableEnvironmentRareData : public CachedObject<VariableEnvironment::RareData> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const VariableEnvironment::RareData& rareData)
    {
        m_privateNames.encode(encoder, rareData.m_privateNames);
    }

    void decode(Decoder& decoder, VariableEnvironment::RareData& rareData) const
    {
        m_privateNames.decode(decoder, rareData.m_privateNames);
    }

private:
    CachedPrivateNameEnvironment m_privateNames;
};

class CachedVariableEnvironment : public CachedObject<VariableEnvironment> {
public:
    void encode(Encoder& encoder, const VariableEnvironment& env)
    {
        m_isEverythingCaptured = env.m_isEverythingCaptured;
        m_hasAwaitUsingDeclaration = env.m_hasAwaitUsingDeclaration;
        m_map.encode(encoder, env.m_map);
        m_rareData.encode(encoder, env.m_rareData.get());
    }

    void decode(Decoder& decoder, VariableEnvironment& env) const
    {
        env.m_isEverythingCaptured = m_isEverythingCaptured;
        env.m_hasAwaitUsingDeclaration = m_hasAwaitUsingDeclaration;
        m_map.decode(decoder, env.m_map);
        if (!m_rareData.isEmpty()) {
            env.m_rareData = WTF::makeUnique<VariableEnvironment::RareData>();
            m_rareData->decode(decoder, *env.m_rareData);
        }
    }

private:
    bool m_isEverythingCaptured;
    bool m_hasAwaitUsingDeclaration;
    CachedInlineMap<CachedRefPtr<CachedUniquedStringImpl, UniquedStringImpl, WTF::PackedPtrTraits<UniquedStringImpl>>, VariableEnvironmentEntry, VariableEnvironment::inlineMapCapacity, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>, VariableEnvironmentEntryHashTraits> m_map;
    CachedPtr<CachedVariableEnvironmentRareData> m_rareData;
};

class CachedCompactTDZEnvironment : public CachedObject<CompactTDZEnvironment> {
public:
    void encode(Encoder& encoder, const CompactTDZEnvironment& env)
    {
        if (std::holds_alternative<CompactTDZEnvironment::Compact>(env.m_variables))
            m_variables.encode(encoder, std::get<CompactTDZEnvironment::Compact>(env.m_variables));
        else {
            CompactTDZEnvironment::Compact compact;
            for (auto& key : std::get<CompactTDZEnvironment::Inflated>(env.m_variables))
                compact.append(key);
            m_variables.encode(encoder, compact);
        }
        m_hash = env.m_hash;
    }

    void decode(Decoder& decoder, CompactTDZEnvironment& env) const
    {
        {
            CompactTDZEnvironment::Compact compact;
            m_variables.decode(decoder, compact);
            CompactTDZEnvironment::sortCompact(compact);
            env.m_variables = CompactTDZEnvironment::Variables(WTF::move(compact));
        }
        env.m_hash = m_hash;
    }

    CompactTDZEnvironment* decode(Decoder& decoder) const
    {
        CompactTDZEnvironment* env = new CompactTDZEnvironment;
        decode(decoder, *env);
        return env;
    }

private:
    CachedVector<CachedRefPtr<CachedUniquedStringImpl, UniquedStringImpl, WTF::PackedPtrTraits<UniquedStringImpl>>> m_variables;
    unsigned m_hash;
};

class CachedCompactTDZEnvironmentMapHandle : public CachedObject<CompactTDZEnvironmentMap::Handle> {
public:
    void encode(Encoder& encoder, const CompactTDZEnvironmentMap::Handle& handle)
    {
        m_environment.encode(encoder, handle.m_environment);
    }

    CompactTDZEnvironmentMap::Handle decode(Decoder& decoder) const
    {
        bool isNewAllocation;
        CompactTDZEnvironment* environment = m_environment.decode(decoder, isNewAllocation);
        if (!environment) {
            ASSERT(!isNewAllocation);
            return CompactTDZEnvironmentMap::Handle();
        }

        if (!isNewAllocation)
            return decoder.handleForTDZEnvironment(environment);
        bool isNewEntry;
        CompactTDZEnvironmentMap::Handle handle = decoder.vm().m_compactVariableMap->get(environment, isNewEntry);
        if (!isNewEntry) {
            decoder.addFinalizer([=] {
                delete environment;
            });
        }
        decoder.setHandleForTDZEnvironment(environment, handle);
        return handle;
    }

    void decode(Decoder& decoder, CompactTDZEnvironmentMap::Handle& handle) const
    {
        handle = decode(decoder);
    }

private:
    CachedPtr<CachedCompactTDZEnvironment> m_environment;
};

class CachedScopedArgumentsTable : public CachedObject<ScopedArgumentsTable> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const ScopedArgumentsTable& scopedArgumentsTable)
    {
        m_length = scopedArgumentsTable.m_arguments.size();
        m_arguments.encode(encoder, scopedArgumentsTable.m_arguments.span().data(), m_length);
    }

    ScopedArgumentsTable* decode(Decoder& decoder) const
    {
        ScopedArgumentsTable* scopedArgumentsTable = ScopedArgumentsTable::tryCreate(decoder.vm(), m_length);
        RELEASE_ASSERT(scopedArgumentsTable); // We crash here. This is unlikely to continue execution if we hit this condition when decoding UnlinkedCodeBlock.
        m_arguments.decode(decoder, scopedArgumentsTable->m_arguments.mutableSpan().data(), m_length);
        return scopedArgumentsTable;
    }

private:
    uint32_t m_length;
    CachedArray<ScopeOffset> m_arguments;
};

class CachedSymbolTableEntry : public CachedObject<SymbolTableEntry> {
public:
    void encode(Encoder&, const SymbolTableEntry& symbolTableEntry)
    {
        m_bits = symbolTableEntry.m_bits | SymbolTableEntry::SlimFlag;
    }

    void decode(Decoder&, SymbolTableEntry& symbolTableEntry) const
    {
        symbolTableEntry.m_bits = m_bits;
    }

private:
    intptr_t m_bits;
};

class CachedSymbolTableRareData : public CachedObject<SymbolTable::SymbolTableRareData> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const SymbolTable::SymbolTableRareData& rareData)
    {
        m_privateNames.encode(encoder, rareData.m_privateNames);
    }

    void decode(Decoder& decoder, SymbolTable::SymbolTableRareData& rareData) const
    {
        m_privateNames.decode(decoder, rareData.m_privateNames);
    }

private:
    CachedPrivateNameEnvironment m_privateNames;
};

class CachedSymbolTable : public CachedObject<SymbolTable> {
public:
    void encode(Encoder& encoder, const SymbolTable& symbolTable)
    {
        m_map.encode(encoder, symbolTable.m_map);
        m_maxScopeOffset = symbolTable.m_maxScopeOffset;
        m_usesSloppyEval = symbolTable.m_usesSloppyEval;
        m_nestedLexicalScope = symbolTable.m_nestedLexicalScope;
        m_scopeType = symbolTable.m_scopeType;
        m_arguments.encode(encoder, symbolTable.m_arguments.get());
        m_rareData.encode(encoder, symbolTable.m_rareData.get());
    }

    SymbolTable* decode(Decoder& decoder) const
    {
        SymbolTable* symbolTable = SymbolTable::create(decoder.vm());
        m_map.decode(decoder, symbolTable->m_map);
        symbolTable->m_maxScopeOffset = m_maxScopeOffset;
        symbolTable->m_usesSloppyEval = m_usesSloppyEval;
        symbolTable->m_nestedLexicalScope = m_nestedLexicalScope;
        symbolTable->m_scopeType = m_scopeType;
        ScopedArgumentsTable* scopedArgumentsTable = m_arguments.decode(decoder);
        if (scopedArgumentsTable)
            symbolTable->m_arguments.set(decoder.vm(), symbolTable, scopedArgumentsTable);
        if (!m_rareData.isEmpty()) {
            symbolTable->m_rareData = WTF::makeUnique<SymbolTable::SymbolTableRareData>();
            m_rareData->decode(decoder, *symbolTable->m_rareData);
        }

        return symbolTable;
    }

private:
    CachedHashMap<CachedRefPtr<CachedUniquedStringImpl>, CachedSymbolTableEntry, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>, SymbolTableIndexHashTraits> m_map;
    ScopeOffset m_maxScopeOffset;
    unsigned m_usesSloppyEval : 1;
    unsigned m_nestedLexicalScope : 1;
    unsigned m_scopeType : 3;
    CachedPtr<CachedScopedArgumentsTable> m_arguments;
    CachedPtr<CachedSymbolTableRareData> m_rareData;
};

class CachedJSValue;
class CachedImmutableButterfly : public CachedObject<JSCellButterfly> {
public:
    CachedImmutableButterfly()
        : m_cachedDoubles()
    {
    }

    void encode(Encoder& encoder, JSCellButterfly& immutableButterfly)
    {
        m_length = immutableButterfly.length();
        m_indexingType = immutableButterfly.indexingTypeAndMisc();
        if (hasDouble(m_indexingType))
            m_cachedDoubles.encode(encoder, immutableButterfly.toButterfly()->contiguousDouble().data(), m_length);
        else
            m_cachedValues.encode(encoder, immutableButterfly.toButterfly()->contiguous().data(), m_length);
    }

    JSCellButterfly* decode(Decoder& decoder) const
    {
        JSCellButterfly* immutableButterfly = JSCellButterfly::create(decoder.vm(), m_indexingType, m_length);
        if (hasDouble(m_indexingType))
            m_cachedDoubles.decode(decoder, immutableButterfly->toButterfly()->contiguousDouble().data(), m_length, immutableButterfly);
        else
            m_cachedValues.decode(decoder, immutableButterfly->toButterfly()->contiguous().data(), m_length, immutableButterfly);
        return immutableButterfly;
    }

private:
    IndexingType m_indexingType;
    unsigned m_length;
    union {
        CachedArray<double> m_cachedDoubles;
        CachedArray<CachedJSValue, WriteBarrier<Unknown>> m_cachedValues;
    };
};

class CachedRegExp : public CachedObject<RegExp> {
public:
    void encode(Encoder& encoder, const RegExp& regExp)
    {
        m_patternString.encode(encoder, regExp.m_patternString);
        m_flags = regExp.m_flags;
    }

    RegExp* decode(Decoder& decoder) const
    {
        String pattern { m_patternString.decode(decoder) };
        return RegExp::create(decoder.vm(), pattern, m_flags);
    }

private:
    CachedString m_patternString;
    OptionSet<Yarr::Flags> m_flags;
};

class CachedTemplateObjectDescriptor : public CachedObject<TemplateObjectDescriptor> {
public:
    void encode(Encoder& encoder, const JSTemplateObjectDescriptor& descriptor)
    {
        m_rawStrings.encode(encoder, descriptor.descriptor().rawStrings());
        m_cookedStrings.encode(encoder, descriptor.descriptor().cookedStrings());
        m_endOffset = descriptor.endOffset();
    }

    JSTemplateObjectDescriptor* decode(Decoder& decoder) const
    {
        TemplateObjectDescriptor::StringVector decodedRawStrings;
        TemplateObjectDescriptor::OptionalStringVector decodedCookedStrings;
        m_rawStrings.decode(decoder, decodedRawStrings);
        m_cookedStrings.decode(decoder, decodedCookedStrings);
        return JSTemplateObjectDescriptor::create(decoder.vm(), TemplateObjectDescriptor::create(WTF::move(decodedRawStrings), WTF::move(decodedCookedStrings)), m_endOffset);
    }

private:
    CachedVector<CachedString, 4> m_rawStrings;
    CachedVector<CachedOptional<CachedString>, 4> m_cookedStrings;
    int m_endOffset;
};

class CachedBigInt : public VariableLengthObject<JSBigInt> {
public:
    void encode(Encoder& encoder, JSBigInt& bigInt)
    {
        m_length = bigInt.length();
        m_sign = bigInt.sign();

        if (!m_length)
            return;

        unsigned size = sizeof(JSBigInt::Digit) * m_length;
        uint8_t* buffer = this->allocate(encoder, size, alignof(JSBigInt::Digit));
        memcpy(buffer, bigInt.dataStorage(), size);
    }

    JSBigInt* decode(Decoder& decoder) const
    {
        if (!m_length)
            return decoder.vm().heapBigIntConstantZero.get();

        JSBigInt* bigInt = JSBigInt::tryCreateWithLength(decoder.vm(), m_length);
        RELEASE_ASSERT(bigInt);
        bigInt->setSign(m_sign);
        if (m_length)
            memcpy(bigInt->dataStorage(), this->buffer(), sizeof(JSBigInt::Digit) * m_length);
        return bigInt;
    }

private:
    unsigned m_length;
    bool m_sign;
};

class CachedJSValue : public VariableLengthObject<WriteBarrier<Unknown>> {
public:
    void encode(Encoder& encoder, const WriteBarrier<Unknown> value)
    {
        JSValue v = value.get();

        if (!v.isCell() || v.isEmpty()) {
            m_type = EncodedType::JSValue;
            *this->allocate<EncodedJSValue>(encoder) = JSValue::encode(v);
            return;
        }

        JSCell* cell = v.asCell();

        if (auto* symbolTable = dynamicDowncast<SymbolTable>(cell)) {
            m_type = EncodedType::SymbolTable;
            this->allocate<CachedSymbolTable>(encoder)->encode(encoder, *symbolTable);
            return;
        }

        if (auto* string = dynamicDowncast<JSString>(cell)) {
            m_type = EncodedType::String;
            // TODO: This seems wrong? What if this fails.
            auto str = string->tryGetValue();
            this->allocateFor<CachedUniquedStringImpl>(encoder, *str.data.impl())->encode(encoder, *str.data.impl());
            return;
        }

        if (auto* immutableButterfly = dynamicDowncast<JSCellButterfly>(cell)) {
            m_type = EncodedType::ImmutableButterfly;
            this->allocate<CachedImmutableButterfly>(encoder)->encode(encoder, *immutableButterfly);
            return;
        }

        if (auto* regexp = dynamicDowncast<RegExp>(cell)) {
            m_type = EncodedType::RegExp;
            this->allocate<CachedRegExp>(encoder)->encode(encoder, *regexp);
            return;
        }

        if (auto* templateObjectDescriptor = dynamicDowncast<JSTemplateObjectDescriptor>(cell)) {
            m_type = EncodedType::TemplateObjectDescriptor;
            this->allocate<CachedTemplateObjectDescriptor>(encoder)->encode(encoder, *templateObjectDescriptor);
            return;
        }

        if (auto* bigInt = dynamicDowncast<JSBigInt>(cell)) {
            m_type = EncodedType::BigInt;
            this->allocate<CachedBigInt>(encoder)->encode(encoder, *bigInt);
            return;
        }

        RELEASE_ASSERT_NOT_REACHED();
    }

    void decode(Decoder& decoder, WriteBarrier<Unknown>& value, const JSCell* owner) const
    {
        JSValue v;
        switch (m_type) {
        case EncodedType::JSValue:
            v = JSValue::decode(*this->buffer<EncodedJSValue>());
            break;
        case EncodedType::SymbolTable:
            v = this->buffer<CachedSymbolTable>()->decode(decoder);
            break;
        case EncodedType::String: {
            StringImpl* impl = this->buffer<CachedUniquedStringImpl>()->decode(decoder);
            v = jsString(decoder.vm(), adoptRef(*impl));
            break;
        }
        case EncodedType::ImmutableButterfly:
            v = this->buffer<CachedImmutableButterfly>()->decode(decoder);
            break;
        case EncodedType::RegExp:
            v = this->buffer<CachedRegExp>()->decode(decoder);
            break;
        case EncodedType::TemplateObjectDescriptor:
            v = this->buffer<CachedTemplateObjectDescriptor>()->decode(decoder);
            break;
        case EncodedType::BigInt:
            v = this->buffer<CachedBigInt>()->decode(decoder);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        value.set(decoder.vm(), owner, v);
    }

private:
    enum class EncodedType : uint8_t {
        JSValue,
        SymbolTable,
        String,
        ImmutableButterfly,
        RegExp,
        TemplateObjectDescriptor,
        BigInt,
    };

    EncodedType m_type;
};

class CachedInstructionStream : public CachedObject<JSInstructionStream> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const JSInstructionStream& stream)
    {
        RELEASE_ASSERT(!stream.isBorrowed()); // a borrowed stream's bytes live in the payload being read, not in m_instructions
        m_instructions.encode(encoder, stream.m_instructions);
    }

    JSInstructionStream* decode(Decoder& decoder) const
    {
        if (decoder.canBorrowPayload())
            return new JSInstructionStream(m_instructions.borrow(), JSInstructionStream::Borrow);
        Vector<uint8_t, 0, UnsafeVectorOverflow, 16, InstructionStreamBufferMalloc> instructionsVector;
        m_instructions.decode(decoder, instructionsVector);
        return new JSInstructionStream(WTF::move(instructionsVector));
    }

private:
    CachedVector<uint8_t, 0, UnsafeVectorOverflow, InstructionStreamBufferMalloc> m_instructions;
};

class CachedMetadataTable : public CachedObject<UnlinkedMetadataTable> {
    // The offset table is cumulative and most opcodes have no metadata in a given function, so only the entries where the
    // running offset changes are stored: (index << 24 | delta). A typical function has a handful instead of 51.
    static constexpr unsigned indexShift = 24;
    static constexpr uint32_t deltaMask = (1u << indexShift) - 1;
    static_assert(UnlinkedMetadataTable::s_offsetTableEntries < (1u << (32 - indexShift)));

public:
    void encode(Encoder& encoder, const UnlinkedMetadataTable& metadataTable)
    {
        ASSERT(metadataTable.m_isFinalized);
        m_hasMetadata = metadataTable.m_hasMetadata;
        if (!m_hasMetadata)
            return;
        m_is32Bit = metadataTable.m_is32Bit;
        m_numValueProfiles = metadataTable.m_numValueProfiles;
        Vector<uint32_t, 16> steps;
        uint32_t previous = 0;
        for (unsigned i = 0; i < UnlinkedMetadataTable::s_offsetTableEntries; ++i) {
            uint32_t value = m_is32Bit ? metadataTable.offsetTable32()[i] : metadataTable.offsetTable16()[i];
            if (value == previous)
                continue;
            RELEASE_ASSERT(value > previous && value - previous <= deltaMask);
            steps.append(i << indexShift | (value - previous));
            previous = value;
        }
        m_steps.encode(encoder, steps);
    }

    Ref<UnlinkedMetadataTable> decode(Decoder& decoder) const
    {
        if (!m_hasMetadata)
            return UnlinkedMetadataTable::empty();

        Vector<uint32_t, 16> steps;
        m_steps.decode(decoder, steps);
        Ref<UnlinkedMetadataTable> metadataTable = UnlinkedMetadataTable::create(m_is32Bit, m_numValueProfiles);
        metadataTable->m_isFinalized = true;
        metadataTable->m_isLinked = false;
        metadataTable->m_hasMetadata = m_hasMetadata;
        metadataTable->m_numValueProfiles = m_numValueProfiles;
        if (m_is32Bit)
            expand(steps, metadataTable->offsetTable32());
        else
            expand(steps, metadataTable->offsetTable16());
        return metadataTable;
    }

private:
    template<typename OffsetType>
    static void expand(const Vector<uint32_t, 16>& steps, OffsetType* table)
    {
        uint32_t value = 0;
        unsigned i = 0;
        for (uint32_t step : steps) {
            unsigned end = step >> indexShift;
            RELEASE_ASSERT(end >= i && end < UnlinkedMetadataTable::s_offsetTableEntries); // as the encoder wrote them; a malformed payload stops here rather than past the table
            for (; i < end; ++i)
                table[i] = value;
            value += step & deltaMask;
        }
        for (; i < UnlinkedMetadataTable::s_offsetTableEntries; ++i)
            table[i] = value;
    }

private:
    bool m_hasMetadata;
    bool m_is32Bit;
    unsigned m_numValueProfiles;
    CachedVector<uint32_t, 16> m_steps;
};

class CachedSourceOrigin : public CachedObject<SourceOrigin> {
public:
    void encode(Encoder& encoder, const SourceOrigin& sourceOrigin)
    {
        m_string.encode(encoder, sourceOrigin.url().string());
    }

    SourceOrigin decode(Decoder& decoder) const
    {
        return SourceOrigin { URL({ }, m_string.decode(decoder)) };
    }

private:
    CachedString m_string;
};

class CachedTextPosition : public CachedObject<TextPosition> {
public:
    void encode(Encoder&, TextPosition textPosition)
    {
        m_line = textPosition.m_line.zeroBasedInt();
        m_column = textPosition.m_column.zeroBasedInt();
    }

    TextPosition decode(Decoder&) const
    {
        return TextPosition { OrdinalNumber::fromZeroBasedInt(m_line), OrdinalNumber::fromZeroBasedInt(m_column) };
    }

private:
    int m_line;
    int m_column;
};

template <typename Source, typename CachedType>
class CachedSourceProviderShape : public CachedObject<Source> {
public:
    void encode(Encoder& encoder, const SourceProvider& sourceProvider)
    {
        m_sourceOrigin.encode(encoder, sourceProvider.sourceOrigin());
        m_sourceURL.encode(encoder, sourceProvider.sourceURL());
        m_preRedirectURL.encode(encoder, sourceProvider.preRedirectURL());
        m_sourceURLDirective.encode(encoder, sourceProvider.sourceURLDirective());
        m_sourceMappingURLDirective.encode(encoder, sourceProvider.sourceMappingURLDirective());
        m_startPosition.encode(encoder, sourceProvider.startPosition());
        m_sourceTaintedOrigin = sourceProvider.sourceTaintedOrigin();
    }

    void decode(Decoder& decoder, SourceProvider& sourceProvider) const
    {
        sourceProvider.setSourceURLDirective(m_sourceURLDirective.decode(decoder));
        sourceProvider.setSourceMappingURLDirective(m_sourceMappingURLDirective.decode(decoder));
        sourceProvider.setSourceTaintedOrigin(m_sourceTaintedOrigin);
    }

protected:
    CachedSourceOrigin m_sourceOrigin;
    CachedString m_sourceURL;
    CachedString m_preRedirectURL;
    CachedString m_sourceURLDirective;
    CachedString m_sourceMappingURLDirective;
    CachedTextPosition m_startPosition;
    SourceTaintedOrigin m_sourceTaintedOrigin;
};

class CachedStringSourceProvider : public CachedSourceProviderShape<StringSourceProvider, CachedStringSourceProvider> {
    using Base = CachedSourceProviderShape<StringSourceProvider, CachedStringSourceProvider>;

public:
#if USE(BUN_JSC_ADDITIONS)
    // Takes the base type for the same reason decode() returns it: Bun's runtime
    // provider is a SourceProvider sibling of StringSourceProvider, and only
    // base-class API is used below.
    void encode(Encoder& encoder, const SourceProvider& sourceProvider)
#else
    void encode(Encoder& encoder, const StringSourceProvider& sourceProvider)
#endif
    {
        Base::encode(encoder, sourceProvider);
#if USE(BUN_JSC_ADDITIONS)
        // SourceCodeKey::operator== under BUN_JSC_ADDITIONS does not compare source
        // text, so encoding it here only wastes ~source_size bytes of bytecode and
        // forces a ~source_size heap allocation at decode time. Store length only —
        // the comparison still validates length() and host().
        m_sourceLength = sourceProvider.source().length();
#else
        m_source.encode(encoder, sourceProvider.source().toString());
#endif
    }

#if USE(BUN_JSC_ADDITIONS)
    // The caller (CachedSourceProvider::decode) returns SourceProvider*, so the
    // BUN reuse path can return the runtime provider as its base type without
    // any reinterpret_cast through the StringSourceProvider sibling.
    SourceProvider* decode(Decoder& decoder, SourceProviderSourceType sourceType) const
#else
    StringSourceProvider* decode(Decoder& decoder, SourceProviderSourceType sourceType) const
#endif
    {
#if USE(BUN_JSC_ADDITIONS)
        // Reuse the runtime SourceProvider the Decoder was constructed with rather
        // than allocating a fresh StringSourceProvider holding a heap copy of the
        // source. The decoded key is only used for SourceCodeKey equality, which
        // under BUN_JSC_ADDITIONS does not look at source bytes.
        //
        // Base::decode is intentionally skipped: the runtime provider already has
        // its sourceURLDirective / sourceMappingURLDirective / sourceTaintedOrigin
        // set, and the decoded key only needs sourceOrigin().url().host() and
        // length() for equality. CachedSourceProviderShape fields are offset-based
        // (not stream-based), so leaving them undecoded does not affect later reads.
        if (RefPtr<SourceProvider> provider = decoder.provider()) {
            if (provider->sourceType() == sourceType && provider->source().length() == m_sourceLength)
                return provider.leakRef();
        }
        // Fallback for callers that did not supply a provider: decode without source
        // bytes. SourceCodeKey::operator== ignores string(), but length() is compared,
        // so synthesize a provider whose source() is empty — length() will mismatch
        // and the cache entry will be rejected, which is the conservative behaviour.
        String decodedSource;
#else
        String decodedSource = m_source.decode(decoder);
#endif
        SourceOrigin decodedSourceOrigin = m_sourceOrigin.decode(decoder);
        String decodedSourceURL = m_sourceURL.decode(decoder);
        TextPosition decodedStartPosition = m_startPosition.decode(decoder);

        Ref<StringSourceProvider> sourceProvider = StringSourceProvider::create(decodedSource, decodedSourceOrigin, decodedSourceURL, m_sourceTaintedOrigin, decodedStartPosition, sourceType);
        Base::decode(decoder, sourceProvider.get());
        return &sourceProvider.leakRef();
    }

private:
#if USE(BUN_JSC_ADDITIONS)
    unsigned m_sourceLength;
#else
    CachedString m_source;
#endif
};

#if ENABLE(WEBASSEMBLY)
class CachedWebAssemblySourceProvider : public CachedSourceProviderShape<WebAssemblySourceProvider, CachedWebAssemblySourceProvider> {
    using Base = CachedSourceProviderShape<WebAssemblySourceProvider, CachedWebAssemblySourceProvider>;

public:
    void encode(Encoder& encoder, const WebAssemblySourceProvider& sourceProvider)
    {
        Base::encode(encoder, sourceProvider);
        m_data.encode(encoder, sourceProvider.dataVector());
    }

    WebAssemblySourceProvider* decode(Decoder& decoder) const
    {
        Vector<uint8_t> decodedData;
        SourceOrigin decodedSourceOrigin = m_sourceOrigin.decode(decoder);
        String decodedSourceURL = m_sourceURL.decode(decoder);

        m_data.decode(decoder, decodedData);

        Ref<WebAssemblySourceProvider> sourceProvider = WebAssemblySourceProvider::create(WTF::move(decodedData), decodedSourceOrigin, decodedSourceURL);
        Base::decode(decoder, sourceProvider.get());

        return &sourceProvider.leakRef();
    }

private:
    CachedVector<uint8_t> m_data;
};
#endif

class CachedSourceProvider : public VariableLengthObject<SourceProvider> {
public:
    void encode(Encoder& encoder, const SourceProvider& sourceProvider)
    {
        m_sourceType = sourceProvider.sourceType();
        switch (m_sourceType) {
        case SourceProviderSourceType::Program:
        case SourceProviderSourceType::Module:
#if USE(BUN_JSC_ADDITIONS)
        case SourceProviderSourceType::BunTranspiledModule:
            this->allocate<CachedStringSourceProvider>(encoder)->encode(encoder, sourceProvider);
#else
            this->allocate<CachedStringSourceProvider>(encoder)->encode(encoder, reinterpret_cast<const StringSourceProvider&>(sourceProvider));
#endif
            break;
#if ENABLE(WEBASSEMBLY)
        case SourceProviderSourceType::WebAssembly:
            this->allocate<CachedWebAssemblySourceProvider>(encoder)->encode(encoder, reinterpret_cast<const WebAssemblySourceProvider&>(sourceProvider));
            break;
#endif
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    SourceProvider* decode(Decoder& decoder) const
    {
        switch (m_sourceType) {
        case SourceProviderSourceType::Program:
        case SourceProviderSourceType::Module:
#if USE(BUN_JSC_ADDITIONS)
        case SourceProviderSourceType::BunTranspiledModule:
#endif
            return this->buffer<CachedStringSourceProvider>()->decode(decoder, m_sourceType);
#if ENABLE(WEBASSEMBLY)
        case SourceProviderSourceType::WebAssembly:
            return this->buffer<CachedWebAssemblySourceProvider>()->decode(decoder);
#endif
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

private:
    SourceProviderSourceType m_sourceType;
};

template<typename Source>
class CachedUnlinkedSourceCodeShape : public CachedObject<Source> {
public:
    void encode(Encoder& encoder, const UnlinkedSourceCode& sourceCode)
    {
        m_provider.encode(encoder, sourceCode.m_provider);
        m_startOffset = sourceCode.startOffset();
        m_endOffset = sourceCode.endOffset();
    }

    void decode(Decoder& decoder, UnlinkedSourceCode& sourceCode) const
    {
        sourceCode.m_provider = m_provider.decode(decoder);
        sourceCode.m_startOffset = m_startOffset;
        sourceCode.m_endOffset = m_endOffset;
    }

private:
    CachedRefPtr<CachedSourceProvider> m_provider;
    int m_startOffset;
    int m_endOffset;
};


class CachedUnlinkedSourceCode : public CachedUnlinkedSourceCodeShape<UnlinkedSourceCode> { };

class CachedSourceCode : public CachedUnlinkedSourceCodeShape<SourceCode> {
    using Base = CachedUnlinkedSourceCodeShape<SourceCode>;

public:
    void encode(Encoder& encoder, const SourceCode& sourceCode)
    {
        Base::encode(encoder, sourceCode);
        m_firstLine = sourceCode.firstLine().zeroBasedInt();
        m_startColumn = sourceCode.startColumn().zeroBasedInt();
    }

    void decode(Decoder& decoder, SourceCode& sourceCode) const
    {
        Base::decode(decoder, sourceCode);
        sourceCode.m_firstLine = OrdinalNumber::fromZeroBasedInt(m_firstLine);
        sourceCode.m_startColumn = OrdinalNumber::fromZeroBasedInt(m_startColumn);
    }

private:
    int m_firstLine;
    int m_startColumn;
};

class CachedSourceCodeWithoutProvider : public CachedObject<SourceCode> {
public:
    void encode(Encoder&, const SourceCode& sourceCode)
    {
        m_hasProvider = !!sourceCode.provider();
        m_startOffset = sourceCode.startOffset();
        m_endOffset = sourceCode.endOffset();
        m_firstLine = sourceCode.firstLine().zeroBasedInt();
        m_startColumn = sourceCode.startColumn().zeroBasedInt();
    }

    void decode(Decoder& decoder, SourceCode& sourceCode) const
    {
        if (m_hasProvider)
            sourceCode.m_provider = decoder.provider();
        sourceCode.m_startOffset = m_startOffset;
        sourceCode.m_endOffset = m_endOffset;
        sourceCode.m_firstLine = OrdinalNumber::fromZeroBasedInt(m_firstLine);
        sourceCode.m_startColumn = OrdinalNumber::fromZeroBasedInt(m_startColumn);
    }

private:
    bool m_hasProvider;
    int m_startOffset;
    int m_endOffset;
    int m_firstLine;
    int m_startColumn;
};

class CachedTDZEnvironmentLink : public CachedObject<TDZEnvironmentLink> {
public:
    void encode(Encoder& encoder, const TDZEnvironmentLink& environment)
    {
        m_handle.encode(encoder, environment.m_handle);
        m_parent.encode(encoder, environment.m_parent);
    }

    TDZEnvironmentLink* decode(Decoder& decoder) const
    {
        CompactTDZEnvironmentMap::Handle handle = m_handle.decode(decoder);
        RefPtr<TDZEnvironmentLink> parent = m_parent.decode(decoder);
        return new TDZEnvironmentLink(WTF::move(handle), WTF::move(parent));
    }

private:
    CachedCompactTDZEnvironmentMapHandle m_handle;
    CachedRefPtr<CachedTDZEnvironmentLink> m_parent;
};

class CachedJSTextPosition : public CachedObject<JSTextPosition> {
public:
    void encode(Encoder&, const JSTextPosition& position)
    {
        m_line = position.line;
        m_offset = position.offset;
        m_lineStartOffset = position.lineStartOffset;
    }

    JSTextPosition decode(Decoder&) const
    {
        return JSTextPosition { m_line, m_offset, m_lineStartOffset };
    }

private:
    int m_line;
    int m_offset;
    int m_lineStartOffset;
};

class CachedClassElementDefinition : public CachedObject<UnlinkedFunctionExecutable::ClassElementDefinition> {
public:
    void encode(Encoder& encoder, const UnlinkedFunctionExecutable::ClassElementDefinition& definition)
    {
        m_ident.encode(encoder, definition.ident);
        m_position.encode(encoder, definition.position);
        m_initializerPosition.encode(encoder, definition.initializerPosition);
        m_kind = static_cast<uint8_t>(definition.kind);
    }

    void decode(Decoder& decoder, UnlinkedFunctionExecutable::ClassElementDefinition& definition) const
    {
        definition.ident = m_ident.decode(decoder);
        definition.position = m_position.decode(decoder);
        definition.initializerPosition = m_initializerPosition.decode(decoder);
        definition.kind = static_cast<UnlinkedFunctionExecutable::ClassElementDefinition::Kind>(m_kind);
    }

private:
    CachedIdentifier m_ident;
    CachedJSTextPosition m_position;
    CachedOptional<CachedJSTextPosition> m_initializerPosition;
    uint8_t m_kind;
};

class CachedFunctionExecutableRareData : public CachedObject<UnlinkedFunctionExecutable::RareData> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const UnlinkedFunctionExecutable::RareData& rareData)
    {
        m_classSource.encode(encoder, rareData.m_classSource);
        m_generatorOrAsyncWrapperFunctionParameterNames.encode(encoder, rareData.m_generatorOrAsyncWrapperFunctionParameterNames);
        m_classElementDefinitions.encode(encoder, rareData.m_classElementDefinitions);
        m_parentPrivateNameEnvironment.encode(encoder, rareData.m_parentPrivateNameEnvironment);
    }

    UnlinkedFunctionExecutable::RareData* decode(Decoder& decoder) const
    {
        UnlinkedFunctionExecutable::RareData* rareData = new UnlinkedFunctionExecutable::RareData { };
        m_classSource.decode(decoder, rareData->m_classSource);
        m_generatorOrAsyncWrapperFunctionParameterNames.decode(decoder, rareData->m_generatorOrAsyncWrapperFunctionParameterNames);
        m_classElementDefinitions.decode(decoder, rareData->m_classElementDefinitions);
        m_parentPrivateNameEnvironment.decode(decoder, rareData->m_parentPrivateNameEnvironment);
        return rareData;
    }

private:
    CachedSourceCodeWithoutProvider m_classSource;
    CachedVector<CachedIdentifier> m_generatorOrAsyncWrapperFunctionParameterNames;
    CachedVector<CachedClassElementDefinition> m_classElementDefinitions;
    CachedPrivateNameEnvironment m_parentPrivateNameEnvironment;
};

class CachedFunctionExecutable : public CachedObject<UnlinkedFunctionExecutable> {
    friend struct CachedFunctionExecutableOffsets;

public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    // The fixed part: what CachedBytecode::commitUpdates patches in place and what other records point at. Everything
    // else is a varint tail (see Scalars); a typical record is ~45 bytes instead of 104.
    struct Scalars {
        unsigned firstLineOffset;
        unsigned lineCount;
        unsigned unlinkedFunctionStart;
        unsigned unlinkedBodyStartColumn;
        unsigned unlinkedBodyEndColumn;
        unsigned startOffset;
        unsigned sourceLength;
        unsigned parametersStartOffset;
        unsigned unlinkedFunctionEnd;
        unsigned parameterCount;
        SourceParseMode sourceParseMode;
        ImplementationVisibility implementationVisibility;
        bool isBuiltinFunction;
        bool isBuiltinDefaultClassConstructor;
        unsigned constructAbility;
        unsigned constructorKind;
        unsigned functionMode;
        unsigned scriptMode;
        unsigned superBinding;
        unsigned derivedContextType;
        unsigned evalContextType;
        bool inlineAttribute;
        bool needsClassFieldInitializer;
        unsigned privateBrandRequirement;
        bool hasName;
    };

    static size_t tailSize(const UnlinkedFunctionExecutable& executable)
    {
        VarintWriter writer;
        packScalars(executable, writer);
        return writer.size();
    }

    void encode(Encoder&, const UnlinkedFunctionExecutable&);
    UnlinkedFunctionExecutable* decode(Decoder&) const;

    Scalars scalars() const;

    CodeFeatures NODELETE features() const { return m_mutableMetadata.m_features; }
    LexicallyScopedFeatures NODELETE lexicallyScopedFeatures() const { return m_mutableMetadata.m_lexicallyScopedFeatures; }
    unsigned NODELETE hasCapturedVariables() const { return m_mutableMetadata.m_hasCapturedVariables; }

    Identifier ecmaName(Decoder& decoder) const { return m_ecmaName.decode(decoder); }
    RefPtr<TDZEnvironmentLink> parentScopeTDZVariables(Decoder& decoder) const { return m_parentScopeTDZVariables.decode(decoder); }

    UnlinkedFunctionExecutable::RareData* rareData(Decoder& decoder) const { return m_rareData.decode(decoder); }

    const CachedWriteBarrier<CachedFunctionCodeBlock, UnlinkedFunctionCodeBlock>& NODELETE unlinkedCodeBlockForCall() const { return m_unlinkedCodeBlockForCall; }
    const CachedWriteBarrier<CachedFunctionCodeBlock, UnlinkedFunctionCodeBlock>& NODELETE unlinkedCodeBlockForConstruct() const { return m_unlinkedCodeBlockForConstruct; }

private:
    static void packScalars(const UnlinkedFunctionExecutable&, VarintWriter&);
    const uint8_t* tail() const { return std::bit_cast<const uint8_t*>(this + 1); }
    uint8_t* tail() { return std::bit_cast<uint8_t*>(this + 1); }

    CachedFunctionExecutableMetadata m_mutableMetadata;

    CachedPtr<CachedFunctionExecutableRareData> m_rareData;

    CachedIdentifier m_ecmaName;
    CachedRefPtr<CachedTDZEnvironmentLink> m_parentScopeTDZVariables;

    CachedWriteBarrier<CachedFunctionCodeBlock, UnlinkedFunctionCodeBlock> m_unlinkedCodeBlockForCall;
    CachedWriteBarrier<CachedFunctionCodeBlock, UnlinkedFunctionCodeBlock> m_unlinkedCodeBlockForConstruct;
};

ptrdiff_t CachedFunctionExecutableOffsets::codeBlockForCallOffset()
{
    return OBJECT_OFFSETOF(CachedFunctionExecutable, m_unlinkedCodeBlockForCall);
}

ptrdiff_t CachedFunctionExecutableOffsets::codeBlockForConstructOffset()
{
    return OBJECT_OFFSETOF(CachedFunctionExecutable, m_unlinkedCodeBlockForConstruct);
}

ptrdiff_t CachedFunctionExecutableOffsets::metadataOffset()
{
    return OBJECT_OFFSETOF(CachedFunctionExecutable, m_mutableMetadata);
}

template<typename CodeBlockType> struct CachedCodeBlockRecordFor;

template<typename CodeBlockType>
class CachedCodeBlock : public CachedObject<CodeBlockType> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    // Everything that is a count, register, or flag lives in a varint tail after the most-derived record (see
    // Scalars); the record itself keeps only pointers/vectors and the metadata table header. ~60 bytes -> ~15.
    struct Scalars {
        VirtualRegister thisRegister;
        VirtualRegister scopeRegister;
        unsigned isConstructor : 1;
        unsigned isBuiltinDefaultClassConstructor : 1;
        unsigned hasCapturedVariables : 1;
        unsigned isBuiltinFunction : 1;
        unsigned superBinding : 1;
        unsigned scriptMode : 1;
        unsigned isArrowFunctionContext : 1;
        unsigned isClassContext : 1;
        unsigned constructorKind : 2;
        unsigned derivedContextType : 2;
        unsigned evalContextType : 2;
        unsigned hasTailCalls : 1;
        unsigned codeType : 2;
        unsigned hasCheckpoints : 1;
        CodeFeatures features;
        LexicallyScopedFeatures lexicallyScopedFeatures;
        SourceParseMode parseMode;
        OptionSet<CodeGenerationMode> codeGenerationMode;
        unsigned lineCount;
        unsigned endColumn;
        int numVars;
        int numCalleeLocals;
        int numParameters;
        unsigned numValueProfiles;
        unsigned numArrayProfiles;
        unsigned numBinaryArithProfiles;
        unsigned numUnaryArithProfiles;
    };

    static size_t tailSize(const UnlinkedCodeBlock& codeBlock)
    {
        VarintWriter writer;
        packScalars(codeBlock, writer);
        return writer.size();
    }

    void encode(Encoder&, const UnlinkedCodeBlock&);
    void decode(Decoder&, UnlinkedCodeBlock&) const;

    Scalars scalars() const;

    JSInstructionStream* instructions(Decoder& decoder) const { return m_instructions.decode(decoder); }

    RefPtr<StringImpl> sourceURLDirective(Decoder& decoder) const { return m_sourceURLDirective.decode(decoder); }
    RefPtr<StringImpl> sourceMappingURLDirective(Decoder& decoder) const { return m_sourceMappingURLDirective.decode(decoder); }

    Ref<UnlinkedMetadataTable> metadata(Decoder& decoder) const { return m_metadata.decode(decoder); }

    UnlinkedCodeBlock::RareData* rareData(Decoder& decoder) const { return m_rareData.decode(decoder); }

private:
    static void packScalars(const UnlinkedCodeBlock&, VarintWriter&);
    // The tail follows the most-derived record (CachedProgramCodeBlock etc. add members after this base).
    const uint8_t* tail() const { return std::bit_cast<const uint8_t*>(this) + sizeof(typename CachedCodeBlockRecordFor<CodeBlockType>::type); }
    uint8_t* tail() { return std::bit_cast<uint8_t*>(this) + sizeof(typename CachedCodeBlockRecordFor<CodeBlockType>::type); }

    CachedMetadataTable m_metadata;

    CachedPtr<CachedCodeBlockRareData> m_rareData;

    CachedRefPtr<CachedStringImpl> m_sourceURLDirective;
    CachedRefPtr<CachedStringImpl> m_sourceMappingURLDirective;

    CachedPtr<CachedInstructionStream> m_instructions;
    CachedVector<JSInstructionStream::Offset> m_jumpTargets;
    CachedVector<CachedJSValue> m_constantRegisters;
    CachedVector<SourceCodeRepresentation> m_constantsSourceCodeRepresentation;
    CachedPtr<CachedExpressionInfo> m_expressionInfo;
    CachedHashMap<JSInstructionStream::Offset, int> m_outOfLineJumpTargets;

    CachedVector<CachedIdentifier> m_identifiers;
    CachedVector<CachedWriteBarrier<CachedFunctionExecutable>> m_functionDecls;
    CachedVector<CachedWriteBarrier<CachedFunctionExecutable>> m_functionExprs;
};

class CachedProgramCodeBlock : public CachedCodeBlock<UnlinkedProgramCodeBlock> {
    using Base = CachedCodeBlock<UnlinkedProgramCodeBlock>;

public:
    void encode(Encoder& encoder, const UnlinkedProgramCodeBlock& codeBlock)
    {
        Base::encode(encoder, codeBlock);
        m_varDeclarations.encode(encoder, codeBlock.m_varDeclarations);
        m_lexicalDeclarations.encode(encoder, codeBlock.m_lexicalDeclarations);
    }

    UnlinkedProgramCodeBlock* decode(Decoder& decoder) const
    {
        UnlinkedProgramCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedProgramCodeBlock>(decoder.vm())) UnlinkedProgramCodeBlock(decoder, *this);
        codeBlock->finishCreation(decoder.vm());
        Base::decode(decoder, *codeBlock);
        m_varDeclarations.decode(decoder, codeBlock->m_varDeclarations);
        m_lexicalDeclarations.decode(decoder, codeBlock->m_lexicalDeclarations);
        return codeBlock;
    }

private:
    CachedVariableEnvironment m_varDeclarations;
    CachedVariableEnvironment m_lexicalDeclarations;
};

class CachedModuleCodeBlock : public CachedCodeBlock<UnlinkedModuleProgramCodeBlock> {
    using Base = CachedCodeBlock<UnlinkedModuleProgramCodeBlock>;

public:
    void encode(Encoder& encoder, const UnlinkedModuleProgramCodeBlock& codeBlock)
    {
        Base::encode(encoder, codeBlock);
        m_varDeclarations.encode(encoder, codeBlock.m_varDeclarations);
        m_moduleEnvironmentSymbolTableConstantRegisterOffset = codeBlock.m_moduleEnvironmentSymbolTableConstantRegisterOffset;
    }

    UnlinkedModuleProgramCodeBlock* decode(Decoder& decoder) const
    {
        UnlinkedModuleProgramCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedModuleProgramCodeBlock>(decoder.vm())) UnlinkedModuleProgramCodeBlock(decoder, *this);
        codeBlock->finishCreation(decoder.vm());
        Base::decode(decoder, *codeBlock);
        m_varDeclarations.decode(decoder, codeBlock->m_varDeclarations);
        codeBlock->m_moduleEnvironmentSymbolTableConstantRegisterOffset = m_moduleEnvironmentSymbolTableConstantRegisterOffset;
        return codeBlock;
    }

private:
    CachedVariableEnvironment m_varDeclarations;
    int m_moduleEnvironmentSymbolTableConstantRegisterOffset;
};

class CachedEvalCodeBlock : public CachedCodeBlock<UnlinkedEvalCodeBlock> {
    using Base = CachedCodeBlock<UnlinkedEvalCodeBlock>;

public:
    void encode(Encoder& encoder, const UnlinkedEvalCodeBlock& codeBlock)
    {
        Base::encode(encoder, codeBlock);
        m_variables.encode(encoder, codeBlock.m_variables);
        m_functionHoistingCandidates.encode(encoder, codeBlock.m_functionHoistingCandidates);
    }

    UnlinkedEvalCodeBlock* decode(Decoder& decoder) const
    {
        UnlinkedEvalCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedEvalCodeBlock>(decoder.vm())) UnlinkedEvalCodeBlock(decoder, *this);
        codeBlock->finishCreation(decoder.vm());
        Base::decode(decoder, *codeBlock);
        m_variables.decode(decoder, codeBlock->m_variables);
        m_functionHoistingCandidates.decode(decoder, codeBlock->m_functionHoistingCandidates);
        return codeBlock;
    }

private:
    CachedVector<CachedIdentifier, 0, UnsafeVectorOverflow> m_variables;
    CachedVector<CachedIdentifier, 0, UnsafeVectorOverflow> m_functionHoistingCandidates;
};

class CachedFunctionCodeBlock : public CachedCodeBlock<UnlinkedFunctionCodeBlock> {
    using Base = CachedCodeBlock<UnlinkedFunctionCodeBlock>;

public:
    void encode(Encoder& encoder, const UnlinkedFunctionCodeBlock& codeBlock)
    {
        Base::encode(encoder, codeBlock);
    }

    UnlinkedFunctionCodeBlock* decode(Decoder& decoder) const
    {
        UnlinkedFunctionCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedFunctionCodeBlock>(decoder.vm())) UnlinkedFunctionCodeBlock(decoder, *this);
        codeBlock->finishCreation(decoder.vm());
        Base::decode(decoder, *codeBlock);
        return codeBlock;
    }
};

template<> struct CachedCodeBlockRecordFor<UnlinkedProgramCodeBlock> { using type = CachedProgramCodeBlock; };
template<> struct CachedCodeBlockRecordFor<UnlinkedModuleProgramCodeBlock> { using type = CachedModuleCodeBlock; };
template<> struct CachedCodeBlockRecordFor<UnlinkedEvalCodeBlock> { using type = CachedEvalCodeBlock; };
template<> struct CachedCodeBlockRecordFor<UnlinkedFunctionCodeBlock> { using type = CachedFunctionCodeBlock; };

ALWAYS_INLINE UnlinkedFunctionCodeBlock::UnlinkedFunctionCodeBlock(Decoder& decoder, const CachedFunctionCodeBlock& cachedCodeBlock)
    : Base(decoder, decoder.vm().unlinkedFunctionCodeBlockStructure.get(), cachedCodeBlock)
{
}

template<typename T>
struct CachedCodeBlockTypeImpl;

enum class CachedCodeBlockTag {
    CachedProgramCodeBlockTag,
    CachedModuleCodeBlockTag,
    CachedEvalCodeBlockTag,
};

static CachedCodeBlockTag NODELETE tagFromSourceCodeType(SourceCodeType type)
{
    switch (type) {
    case SourceCodeType::ProgramType:
        return CachedCodeBlockTag::CachedProgramCodeBlockTag;
    case SourceCodeType::EvalType:
        return CachedCodeBlockTag::CachedEvalCodeBlockTag;
    case SourceCodeType::ModuleType:
        return CachedCodeBlockTag::CachedModuleCodeBlockTag;
    case SourceCodeType::FunctionType:
        break;
    }
    ASSERT_NOT_REACHED();
    return static_cast<CachedCodeBlockTag>(-1);
}

template<>
struct CachedCodeBlockTypeImpl<UnlinkedProgramCodeBlock> {
    using type = CachedProgramCodeBlock;
    static constexpr CachedCodeBlockTag tag = CachedCodeBlockTag::CachedProgramCodeBlockTag;
};

template<>
struct CachedCodeBlockTypeImpl<UnlinkedModuleProgramCodeBlock> {
    using type = CachedModuleCodeBlock;
    static constexpr CachedCodeBlockTag tag = CachedCodeBlockTag::CachedModuleCodeBlockTag;
};

template<>
struct CachedCodeBlockTypeImpl<UnlinkedEvalCodeBlock> {
    using type = CachedEvalCodeBlock;
    static constexpr CachedCodeBlockTag tag = CachedCodeBlockTag::CachedEvalCodeBlockTag;
};

template<typename T>
using CachedCodeBlockType = typename CachedCodeBlockTypeImpl<T>::type;

template<typename CodeBlockType>
ALWAYS_INLINE UnlinkedCodeBlock::UnlinkedCodeBlock(Decoder& decoder, Structure* structure, const CachedCodeBlock<CodeBlockType>& cachedCodeBlock)
    : Base(decoder.vm(), structure)
    , m_age(0)
    , m_sourceURLDirective(cachedCodeBlock.sourceURLDirective(decoder))
    , m_sourceMappingURLDirective(cachedCodeBlock.sourceMappingURLDirective(decoder))
    , m_metadata(cachedCodeBlock.metadata(decoder))
    , m_instructions(cachedCodeBlock.instructions(decoder))
    , m_rareData(cachedCodeBlock.rareData(decoder))
{
    auto scalars = cachedCodeBlock.scalars();
    m_thisRegister = scalars.thisRegister;
    m_scopeRegister = scalars.scopeRegister;
    m_numVars = scalars.numVars;
    m_numCalleeLocals = scalars.numCalleeLocals;
    m_isConstructor = scalars.isConstructor;
    m_numParameters = scalars.numParameters;
    m_hasCapturedVariables = scalars.hasCapturedVariables;
    m_isBuiltinFunction = scalars.isBuiltinFunction;
    m_isBuiltinDefaultClassConstructor = scalars.isBuiltinDefaultClassConstructor;
    m_superBinding = scalars.superBinding;
    m_scriptMode = scalars.scriptMode;
    m_isArrowFunctionContext = scalars.isArrowFunctionContext;
    m_isClassContext = scalars.isClassContext;
    m_hasTailCalls = scalars.hasTailCalls;
    m_constructorKind = scalars.constructorKind;
    m_derivedContextType = scalars.derivedContextType;
    m_evalContextType = scalars.evalContextType;
    m_codeType = scalars.codeType;
    m_hasCheckpoints = scalars.hasCheckpoints;
    m_lexicallyScopedFeatures = scalars.lexicallyScopedFeatures;
    m_features = scalars.features;
    m_parseMode = scalars.parseMode;
    m_codeGenerationMode = scalars.codeGenerationMode;
    m_lineCount = scalars.lineCount;
    m_endColumn = scalars.endColumn;
    m_valueProfiles = FixedVector<UnlinkedValueProfile>(scalars.numValueProfiles);
    m_arrayProfiles = FixedVector<UnlinkedArrayProfile>(scalars.numArrayProfiles);
    m_binaryArithProfiles = FixedVector<BinaryArithProfile>(scalars.numBinaryArithProfiles);
    m_unaryArithProfiles = FixedVector<UnaryArithProfile>(scalars.numUnaryArithProfiles);
    m_llintExecuteCounter.setNewThreshold(thresholdForJIT(Options::thresholdForJITAfterWarmUp()));
}

template<typename CodeBlockType>
ALWAYS_INLINE void CachedCodeBlock<CodeBlockType>::decode(Decoder& decoder, UnlinkedCodeBlock& codeBlock) const
{
    m_constantRegisters.decode(decoder, codeBlock.m_constantRegisters, &codeBlock);
    m_constantsSourceCodeRepresentation.decode(decoder, codeBlock.m_constantsSourceCodeRepresentation);
    codeBlock.m_expressionInfo = m_expressionInfo->decode(decoder);
    m_outOfLineJumpTargets.decode(decoder, codeBlock.m_outOfLineJumpTargets);
    m_jumpTargets.decode(decoder, codeBlock.m_jumpTargets);
    m_identifiers.decode(decoder, codeBlock.m_identifiers);
    m_functionDecls.decode(decoder, codeBlock.m_functionDecls, &codeBlock);
    m_functionExprs.decode(decoder, codeBlock.m_functionExprs, &codeBlock);
}

ALWAYS_INLINE UnlinkedProgramCodeBlock::UnlinkedProgramCodeBlock(Decoder& decoder, const CachedProgramCodeBlock& cachedCodeBlock)
    : Base(decoder, decoder.vm().unlinkedProgramCodeBlockStructure.get(), cachedCodeBlock)
{
}

ALWAYS_INLINE UnlinkedModuleProgramCodeBlock::UnlinkedModuleProgramCodeBlock(Decoder& decoder, const CachedModuleCodeBlock& cachedCodeBlock)
    : Base(decoder, decoder.vm().unlinkedModuleProgramCodeBlockStructure.get(), cachedCodeBlock)
{
}

ALWAYS_INLINE UnlinkedEvalCodeBlock::UnlinkedEvalCodeBlock(Decoder& decoder, const CachedEvalCodeBlock& cachedCodeBlock)
    : Base(decoder, decoder.vm().unlinkedEvalCodeBlockStructure.get(), cachedCodeBlock)
{
}

enum CachedFunctionExecutableFlag : uint32_t {
    // one word of 1- and 2-bit fields, written as a varint (the high bits are the rarely-set ones)
    ExecutableScriptModeShift = 0,
    ExecutableSuperBindingShift = 1,
    ExecutableConstructAbilityShift = 2,
    ExecutableHasNameShift = 3,
    ExecutableConstructorKindShift = 4, // 2 bits
    ExecutableFunctionModeShift = 6, // 2
    ExecutableImplementationVisibilityShift = 8, // 2
    ExecutableDerivedContextTypeShift = 10, // 2
    ExecutableEvalContextTypeShift = 12, // 2
    ExecutablePrivateBrandRequirementShift = 14,
    ExecutableInlineAttributeShift = 15,
    ExecutableNeedsClassFieldInitializerShift = 16,
    ExecutableIsBuiltinFunctionShift = 17,
    ExecutableIsBuiltinDefaultClassConstructorShift = 18,
};
static_assert(bitWidthOfImplementationVisibility <= 2);

void CachedFunctionExecutable::packScalars(const UnlinkedFunctionExecutable& executable, VarintWriter& writer)
{
    uint32_t flags = static_cast<uint32_t>(executable.m_scriptMode) << ExecutableScriptModeShift
        | static_cast<uint32_t>(executable.m_superBinding) << ExecutableSuperBindingShift
        | static_cast<uint32_t>(executable.m_constructAbility) << ExecutableConstructAbilityShift
        | static_cast<uint32_t>(executable.m_hasName) << ExecutableHasNameShift
        | static_cast<uint32_t>(executable.m_constructorKind) << ExecutableConstructorKindShift
        | static_cast<uint32_t>(executable.m_functionMode) << ExecutableFunctionModeShift
        | static_cast<uint32_t>(executable.m_implementationVisibility) << ExecutableImplementationVisibilityShift
        | static_cast<uint32_t>(executable.m_derivedContextType) << ExecutableDerivedContextTypeShift
        | static_cast<uint32_t>(executable.m_evalContextType) << ExecutableEvalContextTypeShift
        | static_cast<uint32_t>(executable.m_privateBrandRequirement) << ExecutablePrivateBrandRequirementShift
        | static_cast<uint32_t>(executable.m_inlineAttribute) << ExecutableInlineAttributeShift
        | static_cast<uint32_t>(executable.m_needsClassFieldInitializer) << ExecutableNeedsClassFieldInitializerShift
        | static_cast<uint32_t>(executable.m_isBuiltinFunction) << ExecutableIsBuiltinFunctionShift
        | static_cast<uint32_t>(executable.m_isBuiltinDefaultClassConstructor) << ExecutableIsBuiltinDefaultClassConstructorShift;
    writer.u32(flags);
    writer.u8(static_cast<uint8_t>(executable.m_sourceParseMode));
    // Source positions cluster around the function's start, so all but the first are deltas.
    unsigned start = executable.m_startOffset;
    writer.u32(start);
    writer.i32(static_cast<int32_t>(executable.m_unlinkedFunctionStart - start));
    writer.i32(static_cast<int32_t>(executable.m_parametersStartOffset - start));
    writer.u32(executable.m_sourceLength);
    writer.i32(static_cast<int32_t>(executable.m_unlinkedFunctionEnd - (start + executable.m_sourceLength)));
    writer.u32(executable.m_firstLineOffset);
    writer.u32(executable.m_lineCount);
    writer.i32(static_cast<int32_t>(executable.m_unlinkedBodyStartColumn - executable.m_unlinkedFunctionStart));
    writer.i32(static_cast<int32_t>(executable.m_unlinkedBodyEndColumn - executable.m_unlinkedFunctionEnd));
    writer.u32(executable.m_parameterCount);
}

auto CachedFunctionExecutable::scalars() const -> Scalars
{
    VarintReader reader(tail());
    Scalars s;
    uint32_t flags = reader.u32();
    auto bits = [&](unsigned shift, unsigned width = 1) { return (flags >> shift) & ((1u << width) - 1); };
    s.scriptMode = bits(ExecutableScriptModeShift);
    s.superBinding = bits(ExecutableSuperBindingShift);
    s.constructAbility = bits(ExecutableConstructAbilityShift);
    s.hasName = bits(ExecutableHasNameShift);
    s.constructorKind = bits(ExecutableConstructorKindShift, 2);
    s.functionMode = bits(ExecutableFunctionModeShift, 2);
    s.implementationVisibility = static_cast<ImplementationVisibility>(bits(ExecutableImplementationVisibilityShift, 2));
    s.derivedContextType = bits(ExecutableDerivedContextTypeShift, 2);
    s.evalContextType = bits(ExecutableEvalContextTypeShift, 2);
    s.privateBrandRequirement = bits(ExecutablePrivateBrandRequirementShift);
    s.inlineAttribute = bits(ExecutableInlineAttributeShift);
    s.needsClassFieldInitializer = bits(ExecutableNeedsClassFieldInitializerShift);
    s.isBuiltinFunction = bits(ExecutableIsBuiltinFunctionShift);
    s.isBuiltinDefaultClassConstructor = bits(ExecutableIsBuiltinDefaultClassConstructorShift);
    s.sourceParseMode = static_cast<SourceParseMode>(reader.u8());
    s.startOffset = reader.u32();
    s.unlinkedFunctionStart = s.startOffset + reader.i32();
    s.parametersStartOffset = s.startOffset + reader.i32();
    s.sourceLength = reader.u32();
    s.unlinkedFunctionEnd = s.startOffset + s.sourceLength + reader.i32();
    s.firstLineOffset = reader.u32();
    s.lineCount = reader.u32();
    s.unlinkedBodyStartColumn = s.unlinkedFunctionStart + reader.i32();
    s.unlinkedBodyEndColumn = s.unlinkedFunctionEnd + reader.i32();
    s.parameterCount = reader.u32();
    return s;
}

ALWAYS_INLINE void CachedFunctionExecutable::encode(Encoder& encoder, const UnlinkedFunctionExecutable& executable)
{
    m_mutableMetadata.m_features = executable.m_features;
    m_mutableMetadata.m_lexicallyScopedFeatures = executable.m_lexicallyScopedFeatures;
    m_mutableMetadata.m_hasCapturedVariables = executable.m_hasCapturedVariables;

    {
        VarintWriter writer;
        packScalars(executable, writer);
        writer.copyTo(tail());
    }

    m_rareData.encode(encoder, executable.m_rareData.get());

    m_ecmaName.encode(encoder, executable.ecmaName());
    m_parentScopeTDZVariables.encode(encoder, executable.m_parentScopeTDZVariables);

    if (!executable.m_unlinkedCodeBlockForCall || !executable.m_unlinkedCodeBlockForConstruct)
        encoder.addLeafExecutable(&executable, encoder.offsetOf(this));

    encoder.deferBody([this, &encoder, forCall = executable.m_unlinkedCodeBlockForCall, forConstruct = executable.m_unlinkedCodeBlockForConstruct] {
        m_unlinkedCodeBlockForCall.encode(encoder, forCall);
        m_unlinkedCodeBlockForConstruct.encode(encoder, forConstruct);
    });
}

ALWAYS_INLINE UnlinkedFunctionExecutable* CachedFunctionExecutable::decode(Decoder& decoder) const
{
    UnlinkedFunctionExecutable* executable = new (NotNull, allocateCell<UnlinkedFunctionExecutable>(decoder.vm())) UnlinkedFunctionExecutable(decoder, *this);
    executable->finishCreation(decoder.vm());
    return executable;
}

ALWAYS_INLINE UnlinkedFunctionExecutable::UnlinkedFunctionExecutable(Decoder& decoder, const CachedFunctionExecutable& cachedExecutable)
    : Base(decoder.vm(), decoder.vm().unlinkedFunctionExecutableStructure.get())
    , m_isGeneratedFromCache(true)
    , m_hasCapturedVariables(cachedExecutable.hasCapturedVariables())
    , m_isCached(false)
    , m_singletonHasBeenInvalidated(false)
    , m_features(cachedExecutable.features())
    , m_lexicallyScopedFeatures(cachedExecutable.lexicallyScopedFeatures())
    , m_unlinkedCodeBlockForCall()
    , m_unlinkedCodeBlockForConstruct()

    , m_ecmaName(cachedExecutable.ecmaName(decoder))
    , m_parentScopeTDZVariables(cachedExecutable.parentScopeTDZVariables(decoder))

    , m_rareData(cachedExecutable.rareData(decoder))
{
    auto scalars = cachedExecutable.scalars();
    m_firstLineOffset = scalars.firstLineOffset;
    m_lineCount = scalars.lineCount;
    m_unlinkedFunctionStart = scalars.unlinkedFunctionStart;
    m_isBuiltinFunction = scalars.isBuiltinFunction;
    m_unlinkedBodyStartColumn = scalars.unlinkedBodyStartColumn;
    m_isBuiltinDefaultClassConstructor = scalars.isBuiltinDefaultClassConstructor;
    m_unlinkedBodyEndColumn = scalars.unlinkedBodyEndColumn;
    m_constructAbility = scalars.constructAbility;
    m_startOffset = scalars.startOffset;
    m_scriptMode = scalars.scriptMode;
    m_sourceLength = scalars.sourceLength;
    m_superBinding = scalars.superBinding;
    m_parametersStartOffset = scalars.parametersStartOffset;
    m_unlinkedFunctionEnd = scalars.unlinkedFunctionEnd;
    m_needsClassFieldInitializer = scalars.needsClassFieldInitializer;
    m_parameterCount = scalars.parameterCount;
    m_privateBrandRequirement = scalars.privateBrandRequirement;
    m_constructorKind = scalars.constructorKind;
    m_sourceParseMode = scalars.sourceParseMode;
    m_implementationVisibility = static_cast<unsigned>(scalars.implementationVisibility);
    m_functionMode = scalars.functionMode;
    m_derivedContextType = scalars.derivedContextType;
    m_inlineAttribute = scalars.inlineAttribute;
    m_evalContextType = scalars.evalContextType;
    m_hasName = scalars.hasName;

    uint32_t leafExecutables = 2;
    auto checkBounds = [&](int32_t& codeBlockOffset, auto& cachedPtr) {
        if (!cachedPtr.isEmpty()) {
            ptrdiff_t offset = decoder.offsetOf(&cachedPtr);
            if (static_cast<size_t>(offset) < decoder.size()) {
                codeBlockOffset = offset;
                m_isCached = true;
                leafExecutables--;
                return;
            }
        }

        codeBlockOffset = 0;
    };

    if (!cachedExecutable.unlinkedCodeBlockForCall().isEmpty() || !cachedExecutable.unlinkedCodeBlockForConstruct().isEmpty()) {
        checkBounds(m_cachedCodeBlockForCallOffset, cachedExecutable.unlinkedCodeBlockForCall());
        checkBounds(m_cachedCodeBlockForConstructOffset, cachedExecutable.unlinkedCodeBlockForConstruct());
        if (m_isCached)
            m_decoder = &decoder;
        else
            m_decoder = nullptr;
    }

    if (leafExecutables)
        decoder.addLeafExecutable(this, decoder.offsetOf(&cachedExecutable));
}

enum CachedCodeBlockFlag : uint32_t {
    CodeBlockIsConstructorShift = 0,
    CodeBlockHasCapturedVariablesShift = 1,
    CodeBlockSuperBindingShift = 2,
    CodeBlockScriptModeShift = 3,
    CodeBlockIsArrowFunctionContextShift = 4,
    CodeBlockIsClassContextShift = 5,
    CodeBlockHasTailCallsShift = 6,
    CodeBlockHasCheckpointsShift = 7,
    CodeBlockConstructorKindShift = 8, // 2 bits
    CodeBlockDerivedContextTypeShift = 10, // 2
    CodeBlockEvalContextTypeShift = 12, // 2
    CodeBlockCodeTypeShift = 14, // 2
    CodeBlockIsBuiltinFunctionShift = 16,
    CodeBlockIsBuiltinDefaultClassConstructorShift = 17,
};

template<typename CodeBlockType>
void CachedCodeBlock<CodeBlockType>::packScalars(const UnlinkedCodeBlock& codeBlock, VarintWriter& writer)
{
    uint32_t flags = static_cast<uint32_t>(codeBlock.m_isConstructor) << CodeBlockIsConstructorShift
        | static_cast<uint32_t>(codeBlock.m_hasCapturedVariables) << CodeBlockHasCapturedVariablesShift
        | static_cast<uint32_t>(codeBlock.m_superBinding) << CodeBlockSuperBindingShift
        | static_cast<uint32_t>(codeBlock.m_scriptMode) << CodeBlockScriptModeShift
        | static_cast<uint32_t>(codeBlock.m_isArrowFunctionContext) << CodeBlockIsArrowFunctionContextShift
        | static_cast<uint32_t>(codeBlock.m_isClassContext) << CodeBlockIsClassContextShift
        | static_cast<uint32_t>(codeBlock.m_hasTailCalls) << CodeBlockHasTailCallsShift
        | static_cast<uint32_t>(codeBlock.m_hasCheckpoints) << CodeBlockHasCheckpointsShift
        | static_cast<uint32_t>(codeBlock.m_constructorKind) << CodeBlockConstructorKindShift
        | static_cast<uint32_t>(codeBlock.m_derivedContextType) << CodeBlockDerivedContextTypeShift
        | static_cast<uint32_t>(codeBlock.m_evalContextType) << CodeBlockEvalContextTypeShift
        | static_cast<uint32_t>(codeBlock.m_codeType) << CodeBlockCodeTypeShift
        | static_cast<uint32_t>(codeBlock.m_isBuiltinFunction) << CodeBlockIsBuiltinFunctionShift
        | static_cast<uint32_t>(codeBlock.m_isBuiltinDefaultClassConstructor) << CodeBlockIsBuiltinDefaultClassConstructorShift;
    writer.u32(flags);
    writer.u32(codeBlock.m_features);
    writer.u8(static_cast<uint8_t>(codeBlock.m_lexicallyScopedFeatures));
    writer.u8(static_cast<uint8_t>(codeBlock.m_parseMode));
    writer.u8(codeBlock.m_codeGenerationMode.toRaw());
    writer.i32(codeBlock.m_thisRegister.offset());
    writer.i32(codeBlock.m_scopeRegister.offset());
    writer.i32(codeBlock.m_numVars);
    writer.i32(codeBlock.m_numCalleeLocals);
    writer.i32(codeBlock.m_numParameters);
    writer.u32(codeBlock.m_lineCount);
    writer.u32(codeBlock.m_endColumn);
    writer.u32(codeBlock.m_valueProfiles.size());
    writer.u32(codeBlock.m_arrayProfiles.size());
    writer.u32(codeBlock.m_binaryArithProfiles.size());
    writer.u32(codeBlock.m_unaryArithProfiles.size());
}

template<typename CodeBlockType>
auto CachedCodeBlock<CodeBlockType>::scalars() const -> Scalars
{
    VarintReader reader(tail());
    Scalars s;
    uint32_t flags = reader.u32();
    auto bits = [&](unsigned shift, unsigned width = 1) -> unsigned { return (flags >> shift) & ((1u << width) - 1); };
    s.isConstructor = bits(CodeBlockIsConstructorShift);
    s.hasCapturedVariables = bits(CodeBlockHasCapturedVariablesShift);
    s.superBinding = bits(CodeBlockSuperBindingShift);
    s.scriptMode = bits(CodeBlockScriptModeShift);
    s.isArrowFunctionContext = bits(CodeBlockIsArrowFunctionContextShift);
    s.isClassContext = bits(CodeBlockIsClassContextShift);
    s.hasTailCalls = bits(CodeBlockHasTailCallsShift);
    s.hasCheckpoints = bits(CodeBlockHasCheckpointsShift);
    s.constructorKind = bits(CodeBlockConstructorKindShift, 2);
    s.derivedContextType = bits(CodeBlockDerivedContextTypeShift, 2);
    s.evalContextType = bits(CodeBlockEvalContextTypeShift, 2);
    s.codeType = bits(CodeBlockCodeTypeShift, 2);
    s.isBuiltinFunction = bits(CodeBlockIsBuiltinFunctionShift);
    s.isBuiltinDefaultClassConstructor = bits(CodeBlockIsBuiltinDefaultClassConstructorShift);
    s.features = static_cast<CodeFeatures>(reader.u32());
    s.lexicallyScopedFeatures = static_cast<LexicallyScopedFeatures>(reader.u8());
    s.parseMode = static_cast<SourceParseMode>(reader.u8());
    s.codeGenerationMode = OptionSet<CodeGenerationMode>::fromRaw(reader.u8());
    s.thisRegister = VirtualRegister(reader.i32());
    s.scopeRegister = VirtualRegister(reader.i32());
    s.numVars = reader.i32();
    s.numCalleeLocals = reader.i32();
    s.numParameters = reader.i32();
    s.lineCount = reader.u32();
    s.endColumn = reader.u32();
    s.numValueProfiles = reader.u32();
    s.numArrayProfiles = reader.u32();
    s.numBinaryArithProfiles = reader.u32();
    s.numUnaryArithProfiles = reader.u32();
    return s;
}

template<typename CodeBlockType>
ALWAYS_INLINE void CachedCodeBlock<CodeBlockType>::encode(Encoder& encoder, const UnlinkedCodeBlock& codeBlock)
{
    {
        VarintWriter writer;
        packScalars(codeBlock, writer);
        writer.copyTo(tail());
    }

    m_metadata.encode(encoder, codeBlock.m_metadata.get());
    m_rareData.encode(encoder, codeBlock.m_rareData.get());

    m_sourceURLDirective.encode(encoder, codeBlock.m_sourceURLDirective.get());
    m_sourceMappingURLDirective.encode(encoder, codeBlock.m_sourceMappingURLDirective.get());

    m_instructions.encode(encoder, codeBlock.m_instructions.get());
    m_constantRegisters.encode(encoder, codeBlock.m_constantRegisters);
    m_constantsSourceCodeRepresentation.encode(encoder, codeBlock.m_constantsSourceCodeRepresentation);
    encoder.deferCold([this, &encoder, expressionInfo = codeBlock.m_expressionInfo.get()] {
        m_expressionInfo.encode(encoder, expressionInfo);
    });
    m_jumpTargets.encode(encoder, codeBlock.m_jumpTargets);
    m_outOfLineJumpTargets.encode(encoder, codeBlock.m_outOfLineJumpTargets);

    m_identifiers.encode(encoder, codeBlock.m_identifiers);
    m_functionDecls.encode(encoder, codeBlock.m_functionDecls);
    m_functionExprs.encode(encoder, codeBlock.m_functionExprs);
}

class CachedSourceCodeKey : public CachedObject<SourceCodeKey> {
public:
    void encode(Encoder& encoder, const SourceCodeKey& key)
    {
        m_sourceCode.encode(encoder, key.m_sourceCode);
        m_name.encode(encoder, key.m_name);
        m_flags = key.m_flags.m_flags;
        m_hash = key.hash();
        m_functionConstructorParametersEndPosition = key.m_functionConstructorParametersEndPosition;
    }

    void decode(Decoder& decoder, SourceCodeKey& key) const
    {
        m_sourceCode.decode(decoder, key.m_sourceCode);
        m_name.decode(decoder, key.m_name);
        key.m_flags.m_flags = m_flags;
        key.m_hash = m_hash;
        key.m_functionConstructorParametersEndPosition = m_functionConstructorParametersEndPosition;
    }

private:
    CachedUnlinkedSourceCode m_sourceCode;
    CachedString m_name;
    unsigned m_flags;
    unsigned m_hash;
    int m_functionConstructorParametersEndPosition;
};

class GenericCacheEntry {
public:
    bool decode(Decoder&, std::pair<SourceCodeKey, UnlinkedCodeBlock*>&) const;
    bool decode(Decoder&, SourceCodeKey&) const;
    bool isStillValid(Decoder&, const SourceCodeKey&, CachedCodeBlockTag) const;

protected:
    GenericCacheEntry(Encoder& encoder, CachedCodeBlockTag tag)
        : m_cacheVersion(computeJSCBytecodeCacheVersion())
        , m_tag(tag)
    {
        m_bootSessionUUID.encode(encoder, bootSessionUUIDString());
    }

    CachedCodeBlockTag NODELETE tag() const { return m_tag; }

    bool isUpToDate(Decoder& decoder) const
    {
        if (m_cacheVersion != computeJSCBytecodeCacheVersion())
            return false;
        if (m_bootSessionUUID.decode(decoder) != bootSessionUUIDString())
            return false;
        return true;
    }

private:
    uint32_t m_cacheVersion;
    CachedString m_bootSessionUUID;
    CachedCodeBlockTag m_tag;
};

static_assert(alignof(GenericCacheEntry) <= alignof(std::max_align_t));

template<typename UnlinkedCodeBlockType>
class CacheEntry : public GenericCacheEntry {
public:
    CacheEntry(Encoder& encoder)
        : GenericCacheEntry(encoder, CachedCodeBlockTypeImpl<UnlinkedCodeBlockType>::tag)
    {
    }

    void encode(Encoder& encoder, std::pair<SourceCodeKey, const UnlinkedCodeBlockType*> pair)
    {
        m_key.encode(encoder, pair.first);
        m_codeBlock.encode(encoder, pair.second);
    }

private:
    friend GenericCacheEntry;

    bool isStillValid(Decoder& decoder, const SourceCodeKey& key) const
    {
        SourceCodeKey decodedKey;
        m_key.decode(decoder, decodedKey);
        return decodedKey == key;
    }

    bool decode(Decoder& decoder, std::pair<SourceCodeKey, UnlinkedCodeBlockType*>& result) const
    {
        ASSERT(tag() == CachedCodeBlockTypeImpl<UnlinkedCodeBlockType>::tag);
        SourceCodeKey decodedKey;
        m_key.decode(decoder, decodedKey);
        result = { WTF::move(decodedKey), m_codeBlock.decode(decoder) };
        return true;
    }

    bool decode(Decoder& decoder, SourceCodeKey& key) const
    {
        m_key.decode(decoder, key);
        return true;
    }

    CachedSourceCodeKey m_key;
    CachedPtr<CachedCodeBlockType<UnlinkedCodeBlockType>> m_codeBlock;
};

static_assert(alignof(CacheEntry<UnlinkedProgramCodeBlock>) <= alignof(std::max_align_t));
static_assert(alignof(CacheEntry<UnlinkedModuleProgramCodeBlock>) <= alignof(std::max_align_t));

bool GenericCacheEntry::decode(Decoder& decoder, std::pair<SourceCodeKey, UnlinkedCodeBlock*>& result) const
{
    if (!isUpToDate(decoder))
        return false;

    switch (m_tag) {
    case CachedCodeBlockTag::CachedProgramCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedProgramCodeBlock>*>(this)->decode(decoder, reinterpret_cast<std::pair<SourceCodeKey, UnlinkedProgramCodeBlock*>&>(result));
    case CachedCodeBlockTag::CachedModuleCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedModuleProgramCodeBlock>*>(this)->decode(decoder, reinterpret_cast<std::pair<SourceCodeKey, UnlinkedModuleProgramCodeBlock*>&>(result));
    case CachedCodeBlockTag::CachedEvalCodeBlockTag:
        // We do not cache eval code blocks
        RELEASE_ASSERT_NOT_REACHED();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

bool GenericCacheEntry::decode(Decoder& decoder, SourceCodeKey& key) const
{
    if (!isUpToDate(decoder))
        return false;

    switch (m_tag) {
    case CachedCodeBlockTag::CachedProgramCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedProgramCodeBlock>*>(this)->decode(decoder, key);
    case CachedCodeBlockTag::CachedModuleCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedModuleProgramCodeBlock>*>(this)->decode(decoder, key);
    case CachedCodeBlockTag::CachedEvalCodeBlockTag:
        // We do not cache eval code blocks
        return false;
    }

    return false;
}

bool GenericCacheEntry::isStillValid(Decoder& decoder, const SourceCodeKey& key, CachedCodeBlockTag tag) const
{
    if (!isUpToDate(decoder))
        return false;

    switch (tag) {
    case CachedCodeBlockTag::CachedProgramCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedProgramCodeBlock>*>(this)->isStillValid(decoder, key);
    case CachedCodeBlockTag::CachedModuleCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedModuleProgramCodeBlock>*>(this)->isStillValid(decoder, key);
    case CachedCodeBlockTag::CachedEvalCodeBlockTag:
        // We do not cache eval code blocks
        RELEASE_ASSERT_NOT_REACHED();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

template<typename UnlinkedCodeBlockType>
void encodeCodeBlock(Encoder& encoder, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock)
{
    auto* entry = encoder.template malloc<CacheEntry<UnlinkedCodeBlockType>>(encoder);
    entry->encode(encoder, { key, uncheckedDowncast<UnlinkedCodeBlockType>(codeBlock) });
}

RefPtr<CachedBytecode> encodeCodeBlock(VM& vm, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock, FileSystem::FileHandle& fileHandle, BytecodeCacheError& error)
{
    const ClassInfo* classInfo = codeBlock->classInfo();

    Encoder encoder(vm, fileHandle);
    if (classInfo == UnlinkedProgramCodeBlock::info())
        encodeCodeBlock<UnlinkedProgramCodeBlock>(encoder, key, codeBlock);
    else if (classInfo == UnlinkedModuleProgramCodeBlock::info())
        encodeCodeBlock<UnlinkedModuleProgramCodeBlock>(encoder, key, codeBlock);
    else
        ASSERT(classInfo == UnlinkedEvalCodeBlock::info());
    encoder.encodeDeferred();

    return encoder.release(error);
}

RefPtr<CachedBytecode> encodeCodeBlock(VM& vm, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock)
{
    BytecodeCacheError error;
    FileSystem::FileHandle invalidFileHandle;
    return encodeCodeBlock(vm, key, codeBlock, invalidFileHandle, error);
}

RefPtr<CachedBytecode> encodeFunctionCodeBlock(VM& vm, const UnlinkedFunctionCodeBlock* codeBlock, BytecodeCacheError& error)
{
    FileSystem::FileHandle invalidFileHandle;
    Encoder encoder(vm, invalidFileHandle);
    encoder.mallocFor<CachedFunctionCodeBlock>(*codeBlock)->encode(encoder, *codeBlock);
    encoder.encodeDeferred();
    return encoder.release(error);
}

std::optional<SourceCodeKey> decodeSourceCodeKey(VM& vm, Ref<CachedBytecode> cachedBytecode)
{
    const auto* cachedEntry = std::bit_cast<const GenericCacheEntry*>(cachedBytecode->span().data());
    Ref<Decoder> decoder = Decoder::create(vm, WTF::move(cachedBytecode));

    SourceCodeKey key;
    if (!cachedEntry->decode(decoder.get(), key))
        return std::nullopt;
    return key;
}
UnlinkedCodeBlock* decodeCodeBlockImpl(VM& vm, const SourceCodeKey& key, Ref<CachedBytecode> cachedBytecode)
{
    MonotonicTime before;
    size_t cachedBytecodeSize = cachedBytecode->size();
    if (Options::reportBytecodeCacheDecodeTimes()) [[unlikely]]
        before = MonotonicTime::now();

    auto* cachedEntry = std::bit_cast<const GenericCacheEntry*>(cachedBytecode->span().data());
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode), &key.source().provider());
    std::pair<SourceCodeKey, UnlinkedCodeBlock*> entry;
    {
        DeferGC deferGC(vm);
        if (!cachedEntry->decode(decoder.get(), entry))
            return nullptr;
    }
    if (entry.first != key)
        return nullptr;

    if (Options::reportBytecodeCacheDecodeTimes()) [[unlikely]] {
        MonotonicTime after = MonotonicTime::now();
        dataLogLn("BytecodeCache: decoded ", key.source().provider().sourceURL(), " (", cachedBytecodeSize, " bytes) in ", (after - before).milliseconds(), " ms.");
    }

    return entry.second;
}

bool isCachedBytecodeStillValid(VM& vm, Ref<CachedBytecode> cachedBytecode, const SourceCodeKey& key, SourceCodeType type)
{
    auto span = cachedBytecode->span();
    if (span.empty())
        return false;
    auto* cachedEntry = std::bit_cast<const GenericCacheEntry*>(span.data());
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode));
    return cachedEntry->isStillValid(decoder.get(), key, tagFromSourceCodeType(type));
}

void decodeFunctionCodeBlock(Decoder& decoder, int32_t cachedFunctionCodeBlockOffset, WriteBarrier<UnlinkedFunctionCodeBlock>& codeBlock, const JSCell* owner)
{
    ASSERT(decoder.vm().heap.isDeferred());
    auto* cachedCodeBlock = static_cast<const CachedWriteBarrier<CachedFunctionCodeBlock, UnlinkedFunctionCodeBlock>*>(decoder.ptrForOffsetFromBase(cachedFunctionCodeBlockOffset));
    cachedCodeBlock->decode(decoder, codeBlock, owner);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
