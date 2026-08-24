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
#if CPU(X86_64)
#include <cpuid.h>
#endif

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
    // `end` bounds the read when the bytes have not been checksummed yet; past it every read yields 0 and overran() is set.
    explicit VarintReader(const uint8_t* p, const uint8_t* end = nullptr)
        : m_p(p)
        , m_end(end)
    {
    }
    uint32_t u32()
    {
        uint32_t v = 0;
        for (unsigned shift = 0;; shift += 7) {
            uint8_t b = u8();
            v |= static_cast<uint32_t>(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
            if (shift >= 28) {
                m_overran = true;
                return 0;
            }
        }
    }
    int32_t i32()
    {
        uint32_t v = u32();
        return static_cast<int32_t>((v >> 1) ^ -(v & 1));
    }
    uint8_t u8()
    {
        if (m_end && m_p >= m_end) {
            m_overran = true;
            return 0;
        }
        return *m_p++;
    }
    bool overran() const { return m_overran; }
    const uint8_t* position() const { return m_p; }

private:
    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_overran { false };
};

// CRC-32C of the bytes one code-block decode reads, so a truncated or corrupted payload falls back to generating that
// function from source instead of being trusted. Hardware where the ISA guarantees it, a table elsewhere.
static uint32_t crc32cSoftware(uint32_t crc, std::span<const uint8_t> bytes)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t { };
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0x82F63B78u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (uint8_t byte : bytes)
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return crc;
}

#if CPU(X86_64)
__attribute__((target("sse4.2"))) static uint32_t crc32cHardware(uint32_t crc, std::span<const uint8_t> bytes)
{
    const uint8_t* p = bytes.data();
    size_t n = bytes.size();
    uint64_t c = crc;
    for (; n >= 8; n -= 8, p += 8)
        c = __builtin_ia32_crc32di(c, WTF::unalignedLoad<uint64_t>(p));
    for (; n; --n, ++p)
        c = __builtin_ia32_crc32qi(static_cast<uint32_t>(c), *p);
    return static_cast<uint32_t>(c);
}
#elif CPU(ARM64) && defined(__ARM_FEATURE_CRC32)
static uint32_t crc32cHardware(uint32_t crc, std::span<const uint8_t> bytes)
{
    const uint8_t* p = bytes.data();
    size_t n = bytes.size();
    for (; n >= 8; n -= 8, p += 8)
        crc = __builtin_arm_crc32cd(crc, WTF::unalignedLoad<uint64_t>(p));
    for (; n; --n, ++p)
        crc = __builtin_arm_crc32cb(crc, *p);
    return crc;
}
#endif

static uint32_t crc32c(uint32_t crc, std::span<const uint8_t> bytes)
{
#if CPU(X86_64)
    static const bool hardware = [] {
        // cpuid directly: __builtin_cpu_supports needs compiler-rt's __cpu_model, which not every link provides.
        unsigned eax, ebx, ecx = 0, edx;
        return __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_SSE4_2);
    }();
    if (hardware)
        return crc32cHardware(crc, bytes);
#elif CPU(ARM64) && defined(__ARM_FEATURE_CRC32)
    return crc32cHardware(crc, bytes);
#endif
    return crc32cSoftware(crc, bytes);
}

AtomStringImpl* Decoder::atomForOrdinal(uint32_t ordinal) const
{
    return ordinal < m_atomsByOrdinal.size() ? m_atomsByOrdinal[ordinal] : nullptr;
}

void Decoder::setAtomForOrdinal(uint32_t ordinal, AtomStringImpl& atom)
{
    if (ordinal >= m_atomsByOrdinal.size()) {
        // Payloads are far below 2^32 bytes and every numbered string is a 12+ byte record, so this is bounded by the payload.
        RELEASE_ASSERT(ordinal < m_cachedBytecode->size());
        size_t oldSize = m_atomsByOrdinal.size();
        m_atomsByOrdinal.grow(std::max<size_t>(ordinal + 1, oldSize * 2));
        std::fill(m_atomsByOrdinal.begin() + oldSize, m_atomsByOrdinal.end(), nullptr); // Vector::grow leaves pointers uninitialized
    }
    ASSERT(!m_atomsByOrdinal[ordinal]);
    atom.ref();
    m_atomsByOrdinal[ordinal] = &atom;
}

// 1- and 2-character inline strings are the bulk of minified identifiers: length 1 is SmallStrings' single-character reps; length 2 hits one lazy 65536-entry table on the VM (shared by every Decoder — one 512 KB slab, not one per retained Decoder); length 3 goes to the atom table each time.
Ref<AtomStringImpl> Decoder::atomForInlineString(uint32_t packed)
{
    unsigned length = (packed >> 2) & 3;
    if (length == 1)
        return m_vm.smallStrings.singleCharacterStringRep(static_cast<unsigned char>(packed >> 8));
    if (length == 2) {
        if (!m_twoCharacterAtoms) [[unlikely]]
            m_twoCharacterAtoms = m_vm.ensureCachedBytecodeTwoCharacterAtoms();
        AtomStringImpl*& slot = m_twoCharacterAtoms[(packed >> 8) & 0xffff];
        if (slot) [[likely]]
            return *slot;
        std::array<Latin1Character, 2> characters { static_cast<Latin1Character>(packed >> 8), static_cast<Latin1Character>(packed >> 16) };
        Ref<AtomStringImpl> atom = AtomStringImpl::add(std::span<const Latin1Character>(characters)).releaseNonNull();
        atom->ref();
        slot = atom.ptr();
        return atom;
    }
    std::array<Latin1Character, 3> characters { static_cast<Latin1Character>(packed >> 8), static_cast<Latin1Character>(packed >> 16), static_cast<Latin1Character>(packed >> 24) };
    return AtomStringImpl::add(std::span<const Latin1Character>(characters.data(), length)).releaseNonNull();
}

ALWAYS_INLINE DecoderStringTable& Decoder::externalStrings()
{
    if (!m_externalStrings) [[unlikely]] {
        m_externalStrings = m_vm.clientData ? m_vm.clientData->decoderStringTable() : nullptr;
        RELEASE_ASSERT_WITH_MESSAGE(m_externalStrings, "bytecode payload uses an external string table but the embedder did not provide one");
    }
    return *m_externalStrings;
}

Ref<AtomStringImpl> Decoder::atomForExternalString(uint32_t ordinal)
{
    return externalStrings().atomFor(ordinal);
}

String Decoder::plainStringForExternalString(uint32_t ordinal)
{
    return externalStrings().plainStringFor(ordinal);
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(EncoderStringTable);
WTF_MAKE_TZONE_ALLOCATED_IMPL(DecoderStringTable);

EncoderStringTable::~EncoderStringTable() = default;

uint32_t EncoderStringTable::ordinalFor(const StringImpl& string)
{
    ASSERT(!string.isSymbol() && string.length());
    auto result = m_ordinals.add(String { const_cast<StringImpl*>(&string) }, static_cast<uint32_t>(m_strings.size()));
    if (result.isNewEntry)
        m_strings.append(const_cast<StringImpl&>(string));
    return result.iterator->value;
}

// [u32 count][u32 offsets[count]][records: {u32 length|is8Bit<<31, u32 hash, chars, pad-to-4}...]; offsets are from the start of the blob.
Vector<uint8_t> EncoderStringTable::serialize() const
{
    Vector<uint8_t> out;
    uint32_t count = static_cast<uint32_t>(m_strings.size());
    size_t header = sizeof(uint32_t) * (1 + static_cast<size_t>(count));
    size_t body = 0;
    for (auto& s : m_strings)
        body += roundUpToMultipleOf<4>(2 * sizeof(uint32_t) + s->length() * (s->is8Bit() ? sizeof(Latin1Character) : sizeof(char16_t)));
    out.grow(header + body);
    std::memset(out.mutableSpan().data(), 0, out.size());
    uint32_t* words = std::bit_cast<uint32_t*>(out.mutableSpan().data());
    words[0] = count;
    size_t offset = header;
    for (uint32_t i = 0; i < count; ++i) {
        words[1 + i] = static_cast<uint32_t>(offset);
        const StringImpl& s = m_strings[i].get();
        uint32_t* record = std::bit_cast<uint32_t*>(out.mutableSpan().data() + offset);
        record[0] = s.length() | (s.is8Bit() ? 1u << 31 : 0);
        record[1] = s.hash();
        if (s.is8Bit())
            std::memcpy(record + 2, s.span8().data(), s.length());
        else
            std::memcpy(record + 2, s.span16().data(), s.length() * sizeof(char16_t));
        offset += roundUpToMultipleOf<4>(2 * sizeof(uint32_t) + s.length() * (s.is8Bit() ? sizeof(Latin1Character) : sizeof(char16_t)));
    }
    ASSERT(offset == out.size());
    return out;
}

DecoderStringTable::DecoderStringTable(std::span<const uint8_t> bytes)
    : m_bytes(bytes)
{
    RELEASE_ASSERT(bytes.size() >= sizeof(uint32_t) && !(std::bit_cast<uintptr_t>(bytes.data()) % alignof(uint32_t)));
    m_count = *std::bit_cast<const uint32_t*>(bytes.data());
    RELEASE_ASSERT(m_count <= (bytes.size() - sizeof(uint32_t)) / sizeof(uint32_t), m_count, bytes.size());
    if (m_count) {
        m_atomsReservation = roundUpToMultipleOf(WTF::pageSize(), static_cast<size_t>(m_count) * sizeof(AtomStringImpl*));
        m_atoms = static_cast<AtomStringImpl**>(OSAllocator::reserveAndCommit(m_atomsReservation, OSAllocator::FastMallocPages));
    }
}

DecoderStringTable::~DecoderStringTable()
{
    // One per VM: a Worker that exits must give back the references it took on its thread's atoms.
    for (uint32_t i = 0; i < m_count; ++i) {
        if (m_atoms[i])
            m_atoms[i]->deref();
    }
    if (m_atoms)
        OSAllocator::decommitAndRelease(m_atoms, m_atomsReservation);
}

// The blob comes from an executable users sometimes edit; never read outside it.
DecoderStringTable::Record DecoderStringTable::record(uint32_t ordinal) const
{
    RELEASE_ASSERT(ordinal < m_count);
    const uint32_t* offsets = std::bit_cast<const uint32_t*>(m_bytes.data() + sizeof(uint32_t));
    size_t offset = offsets[ordinal];
    RELEASE_ASSERT(!(offset % 4) && offset <= m_bytes.size() && m_bytes.size() - offset >= 2 * sizeof(uint32_t), offset, m_bytes.size());
    const uint32_t* header = std::bit_cast<const uint32_t*>(m_bytes.data() + offset);
    Record result;
    result.length = header[0] & 0x7fffffffu;
    result.is8Bit = header[0] >> 31;
    result.hash = header[1];
    result.characters = std::bit_cast<const uint8_t*>(header + 2);
    size_t byteLength = static_cast<size_t>(result.length) * (result.is8Bit ? sizeof(Latin1Character) : sizeof(char16_t));
    RELEASE_ASSERT(byteLength <= m_bytes.size() - offset - 2 * sizeof(uint32_t), ordinal, result.length, m_bytes.size());
    return result;
}

template<typename CharacterType>
static Ref<AtomStringImpl> atomize(std::span<const CharacterType> characters, uint32_t hash)
{
    // Same threshold as CachedUniquedStringImplBase::minimumLengthToAliasPayload: long strings alias the (persistent) blob.
    if (characters.size() >= 48)
        return AtomStringImpl::add(RefPtr<StringImpl> { StringImpl::createWithoutCopying(characters) }).releaseNonNull();
    WTF::HashTranslatorCharBuffer<CharacterType> hashed { characters, hash };
    return AtomStringImpl::add(hashed).releaseNonNull();
}

Ref<AtomStringImpl> DecoderStringTable::atomFor(uint32_t ordinal)
{
    RELEASE_ASSERT(ordinal < m_count);
    if (AtomStringImpl* known = m_atoms[ordinal]) [[likely]]
        return *known;
    Record r = record(ordinal);
    Ref<AtomStringImpl> atom = r.is8Bit
        ? atomize(std::span { std::bit_cast<const Latin1Character*>(r.characters), r.length }, r.hash)
        : atomize(std::span { std::bit_cast<const char16_t*>(r.characters), r.length }, r.hash);
    atom->ref();
    m_atoms[ordinal] = atom.ptr();
    return atom;
}

String DecoderStringTable::plainStringFor(uint32_t ordinal)
{
    RELEASE_ASSERT(ordinal < m_count);
    if (AtomStringImpl* known = m_atoms[ordinal])
        return String { known };
    Record r = record(ordinal);
    if (r.is8Bit) {
        std::span<const Latin1Character> chars { std::bit_cast<const Latin1Character*>(r.characters), r.length };
        return r.length >= 48 ? StringImpl::createWithoutCopying(chars) : StringImpl::create(chars);
    }
    std::span<const char16_t> chars { std::bit_cast<const char16_t*>(r.characters), r.length };
    return r.length >= 48 ? StringImpl::createWithoutCopying(chars) : StringImpl::create(chars);
}

bool Decoder::payloadContains(const void* start, size_t size) const
{
    auto payload = m_cachedBytecode->span();
    auto* begin = static_cast<const uint8_t*>(start);
    return begin >= payload.data() && size <= payload.size() && begin + size <= payload.data() + payload.size();
}

bool Decoder::recordAndArrayChecksumMatches(const void* record, size_t recordSize, const uint32_t* storedChecksum, const void* array, size_t arraySize) const
{
#if USE(BUN_JSC_ADDITIONS)
    if (!Options::verifyBytecodeCacheChecksums())
        return true;
#endif
    auto* begin = static_cast<const uint8_t*>(record);
    auto* hole = reinterpret_cast<const uint8_t*>(storedChecksum);
    if (!payloadContains(record, recordSize) || (arraySize && !payloadContains(array, arraySize)) || hole < begin || hole + 4 > begin + recordSize)
        return false;
    static const uint8_t zeros[4] = { };
    uint32_t crc = ~0u;
    crc = crc32c(crc, std::span { begin, hole });
    crc = crc32c(crc, std::span { zeros, 4 });
    crc = crc32c(crc, std::span { hole + 4, begin + recordSize });
    if (arraySize)
        crc = crc32c(crc, std::span { static_cast<const uint8_t*>(array), arraySize });
    if (~crc == *storedChecksum)
        return true;
    dataLogLnIf(Options::verboseDiskCache(), "[Disk Cache] expression info checksum mismatch; dropping it");
    return false;
}

bool Decoder::regionChecksumMatches(const void* start, uint32_t size, const uint32_t* storedChecksum, std::span<const std::span<const uint8_t>> externalArrays) const
{
#if USE(BUN_JSC_ADDITIONS)
    if (!Options::verifyBytecodeCacheChecksums())
        return true;
#endif
    auto* begin = static_cast<const uint8_t*>(start);
    auto* hole = reinterpret_cast<const uint8_t*>(storedChecksum);
    if (!payloadContains(start, size) || hole < begin || hole + 4 > begin + size)
        return false; // includes a stored size too small to cover the record that holds the checksum
    static const uint8_t zeros[4] = { };
    uint32_t crc = ~0u;
    crc = crc32c(crc, std::span { begin, hole });
    crc = crc32c(crc, std::span { zeros, 4 });
    crc = crc32c(crc, std::span { hole + 4, begin + size });
    for (auto external : externalArrays) {
        if (!payloadContains(external.data(), external.size()))
            return false;
        crc = crc32c(crc, external);
    }
    if (~crc == *storedChecksum)
        return true;
    dataLogLnIf(Options::verboseDiskCache(), "[Disk Cache] code block checksum mismatch; regenerating from source");
    return false;
}

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

    // A payload that gets appended to another one (CachedBytecode::addFunctionUpdate) is read by the same Decoder as its
    // base, so it leaves its strings unnumbered rather than collide with numbers the base already handed out.
    enum class NumberStrings : bool { No, Yes };
    Encoder(VM& vm, FileSystem::FileHandle& fileHandle, NumberStrings numberStrings = NumberStrings::Yes, EncoderStringTable* externalStrings = nullptr)
        : m_vm(vm)
        , m_fileHandle(fileHandle)
        , m_baseOffset(0)
        , m_currentPage(nullptr)
        , m_externalStrings(externalStrings)
        , m_numberStrings(numberStrings == NumberStrings::Yes)
    {
        allocateNewPage();
    }

    EncoderStringTable* NODELETE externalStrings() { return m_externalStrings; }

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

    ptrdiff_t currentOffset() const { return m_baseOffset + m_currentPage->size(); }

    // CRC-32C of [offset, offset + size) as it will appear in the payload, with the 4 bytes at `hole` read as zero
    // (that is where the checksum itself is stored).
    uint32_t checksumOfRange(ptrdiff_t offset, size_t size, ptrdiff_t hole)
    {
        uint32_t crc = ~0u;
        ptrdiff_t baseOffset = 0;
        ptrdiff_t end = offset + size;
        for (const auto& page : m_pages) {
            ptrdiff_t pageEnd = baseOffset + page.size();
            ptrdiff_t from = std::max(offset, baseOffset);
            ptrdiff_t to = std::min(end, pageEnd);
            for (ptrdiff_t cursor = from; cursor < to;) {
                ptrdiff_t stop = to;
                if (cursor < hole)
                    stop = std::min(stop, hole);
                else if (cursor < hole + 4) {
                    static const uint8_t zeros[4] = { };
                    ptrdiff_t skip = std::min<ptrdiff_t>(hole + 4, to) - cursor;
                    crc = crc32c(crc, std::span { zeros, static_cast<size_t>(skip) });
                    cursor += skip;
                    continue;
                }
                crc = crc32c(crc, page.span().subspan(cursor - baseOffset, stop - cursor));
                cursor = stop;
            }
            baseOffset = pageEnd;
            if (baseOffset >= end)
                break;
        }
        return ~crc;
    }

    std::span<const uint8_t> bytesAt(ptrdiff_t offset, size_t size) { return mutableBytesAt(offset, size); }
    std::span<uint8_t> mutableBytesAt(ptrdiff_t offset, size_t size)
    {
        ptrdiff_t baseOffset = 0;
        for (auto& page : m_pages) {
            if (offset - baseOffset < static_cast<ptrdiff_t>(page.size()))
                return page.mutableSpan().subspan(offset - baseOffset, size);
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
    std::optional<ptrdiff_t> existingIdenticalArray(std::span<const uint8_t> bytes, unsigned hash, size_t alignment)
    {
        auto it = m_arraysByHash.find(hash);
        if (it == m_arraysByHash.end())
            return std::nullopt;
        for (auto [candidate, size] : it->value) {
            // An earlier copy made for a less-aligned element type may sit at an offset this one cannot use.
            if (size == bytes.size() && !(candidate % alignment) && equalSpans(bytesAt(candidate, size), bytes))
                return candidate;
        }
        return std::nullopt;
    }
    void registerArray(unsigned hash, ptrdiff_t offset, size_t size)
    {
        m_arraysByHash.add(hash, Vector<std::pair<ptrdiff_t, size_t>, 1> { }).iterator->value.append({ offset, size });
    }

    // Non-symbol strings decode to AtomStringImpl::add(characters), so two records with the same characters decode to the
    // same atom: write the characters once and point every user at them.
    std::optional<ptrdiff_t> cachedOffsetForStringContents(const StringImpl& string)
    {
        if (string.isSymbol() || !string.length())
            return std::nullopt;
        auto it = m_stringsByContents.find(String(const_cast<StringImpl*>(&string)));
        if (it == m_stringsByContents.end())
            return std::nullopt;
        return it->value;
    }
    void cacheStringContents(const StringImpl& string, ptrdiff_t offset)
    {
        if (string.isSymbol() || !string.length())
            return;
        m_stringsByContents.add(String(const_cast<StringImpl*>(&string)), offset);
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
        // Slots inside a checksummed region (a block's ExpressionInfo, its children's records) are filled by the deferred
        // work above, so the checksums are computed only now that every byte is final.
        for (auto& pending : m_pendingChecksums) {
            uint32_t crc = ~checksumOfRange(pending.start, pending.size, pending.checksumOffset);
            for (auto [offset, size] : pending.externalArrays)
                crc = crc32c(crc, bytesAt(offset, size));
            uint32_t checksum = ~crc;
            memcpySpan(mutableBytesAt(pending.checksumOffset, sizeof(checksum)), std::span { reinterpret_cast<const uint8_t*>(&checksum), sizeof(checksum) });
        }
        m_pendingChecksums.clear();
    }
    void addChecksum(ptrdiff_t start, size_t size, ptrdiff_t checksumOffset, Vector<std::pair<ptrdiff_t, size_t>>&& externalArrays = { }) { m_pendingChecksums.append({ start, size, checksumOffset, WTF::move(externalArrays) }); }
    uint32_t nextStringOrdinal() { return m_numberStrings ? m_nextStringOrdinal++ : std::numeric_limits<uint32_t>::max(); }

    // Content-sharing of arrays is only on while a code block encodes the few arrays its checksum knows how to follow
    // (decoder side: CachedCodeBlock::regionIsIntact); an array shared from outside the block's own bytes is folded into
    // the block's checksum so it is verified by whoever reads it, not only by whoever wrote it first.
    class ShareableArrayScope {
    public:
        ShareableArrayScope(Encoder& encoder)
            : m_encoder(encoder)
            , m_previous(std::exchange(encoder.m_arraySharingEnabled, true))
        {
        }
        ~ShareableArrayScope() { m_encoder.m_arraySharingEnabled = m_previous; }

    private:
        Encoder& m_encoder;
        bool m_previous;
    };
    bool arraySharingEnabled() const { return m_arraySharingEnabled; }
    void beginBlockRegion(ptrdiff_t start) { m_blockRegionStart = start; m_blockExternalArrays.clear(); }
    void noteSharedArray(ptrdiff_t offset, size_t size)
    {
        if (offset < m_blockRegionStart)
            m_blockExternalArrays.append({ offset, size });
    }
    Vector<std::pair<ptrdiff_t, size_t>> takeBlockExternalArrays() { return std::exchange(m_blockExternalArrays, { }); }
    uint32_t checksumOfRecordAndArray(ptrdiff_t record, size_t recordSize, ptrdiff_t checksumOffset, ptrdiff_t array, size_t arraySize)
    {
        uint32_t crc = ~checksumOfRange(record, recordSize, checksumOffset); // un-finalize to keep accumulating
        if (arraySize)
            crc = crc32c(crc, bytesAt(array, arraySize));
        return ~crc;
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
            : m_buffer(MallocSpan<uint8_t, VMMalloc>::zeroedMalloc(size)) // alignment gaps end up in the file: keep them deterministic
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
        // Grow geometrically so offsetOf()/bytesAt(), which walk the page list, stay cheap on large payloads.
        size_t preferred = minPageSize << std::min<size_t>(m_pages.size() + 4, 14);
        if (size < preferred)
            size = preferred;
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
    HashMap<String, ptrdiff_t> m_stringsByContents; // keyed by contents (StringHash), not identity
    LeafExecutableMap m_leafExecutables;
    Deque<Function<void()>> m_bodies;
    Deque<Function<void()>> m_cold;
    struct PendingChecksum { ptrdiff_t start; size_t size; ptrdiff_t checksumOffset; Vector<std::pair<ptrdiff_t, size_t>> externalArrays; };
    Vector<PendingChecksum> m_pendingChecksums;
    uint32_t m_nextStringOrdinal { 0 };
    EncoderStringTable* m_externalStrings;
    bool m_numberStrings;
    bool m_arraySharingEnabled { false };
    ptrdiff_t m_blockRegionStart { 0 };
    Vector<std::pair<ptrdiff_t, size_t>> m_blockExternalArrays;
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
    for (AtomStringImpl* atom : m_atomsByOrdinal) {
        if (atom)
            atom->deref();
    }
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

    // Encoder side: where this object's payload landed, as a payload offset (encoder pages are not contiguous in memory,
    // so `this + m_offset` is only meaningful once decoded).
    ptrdiff_t payloadOffsetInEncoder(Encoder& encoder) const { return encoder.offsetOf(&this->m_offset) + this->m_offset; }
    // Encoder side: point at something already written instead of allocating.
    void pointAtPayloadOffset(Encoder& encoder, ptrdiff_t offset) { this->m_offset = safeCast<Offset>(offset - encoder.offsetOf(&this->m_offset)); }

    // A 1-3 character Latin-1 string that decodes to an atom fits in the 4-byte slot that would otherwise hold the offset
    // of its record: low two bits 01 (record offsets are multiples of 4 and the empty sentinel ends in 11), then the
    // length, then the characters. Minified code is mostly such names.
    static constexpr uint32_t inlineStringTag = 1;
    static constexpr uint32_t inlineStringTagMask = 3;
    static constexpr unsigned inlineStringMaxLength = 3;
    bool tryEncodeInlineString(const StringImpl& string)
    {
        if (string.isSymbol() || !string.is8Bit() || !string.length() || string.length() > inlineStringMaxLength)
            return false;
        uint32_t packed = inlineStringTag | string.length() << 2;
        for (unsigned i = 0; i < string.length(); ++i)
            packed |= static_cast<uint32_t>(string.span8()[i]) << (8 * (i + 1));
        m_offset = std::bit_cast<Offset>(packed);
        return true;
    }
    bool NODELETE hasInlineString() const { return (static_cast<uint32_t>(m_offset) & inlineStringTagMask) == inlineStringTag; }
    Ref<AtomStringImpl> inlineString(Decoder& decoder) const { return decoder.atomForInlineString(std::bit_cast<uint32_t>(m_offset)); }

    // A ≥4-char non-symbol string held in the embedder's shared EncoderStringTable/DecoderStringTable: the slot is an ordinal into that one process-wide table, so every chunk's payload carries 4 bytes instead of a full record. Tag 10 is the value low-two-bits neither a 4-aligned record offset (00), an inline string (01), nor the empty sentinel (11) can produce.
    static constexpr uint32_t externalStringTag = 2;
    bool NODELETE hasExternalString() const { return (static_cast<uint32_t>(m_offset) & inlineStringTagMask) == externalStringTag; }
    uint32_t NODELETE externalStringOrdinal() const { return static_cast<uint32_t>(std::bit_cast<uint32_t>(m_offset)) >> 2; }
    bool tryEncodeExternalString(Encoder& encoder, const StringImpl& string)
    {
        if (!encoder.externalStrings() || string.isSymbol() || !string.length())
            return false;
        uint32_t ordinal = encoder.externalStrings()->ordinalFor(string);
        if (ordinal > EncoderStringTable::maxOrdinal) [[unlikely]]
            return false;
        m_offset = std::bit_cast<Offset>(externalStringTag | ordinal << 2);
        return true;
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
        if (encoder.arraySharingEnabled()) {
            if (auto existing = encoder.existingIdenticalArray(bytes, hash, alignment)) {
                m_offset = safeCast<Offset>(*existing - encoder.offsetOf(&m_offset));
                encoder.noteSharedArray(*existing, bytes.size());
                return;
            }
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
    const void* rawElements() const { return this->isEmpty() ? nullptr : this->buffer(); } // decoded side only
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

class CachedUniquedStringImpl;
class CachedStringImpl;

template<typename T, typename Source = SourceType<T>>
class CachedPtr : public VariableLengthObject<Source*> {
    template<typename, typename, typename>
    friend class CachedRefPtr;

    friend struct CachedPtrOffsets;

public:
    static constexpr bool holdsString = std::is_same_v<T, CachedUniquedStringImpl> || std::is_same_v<T, CachedStringImpl>;

    void encode(Encoder& encoder, const Source* src)
    {
        if (!src)
            return;
        if constexpr (holdsString) {
            if (this->tryEncodeInlineString(*src))
                return;
            if (this->tryEncodeExternalString(encoder, *src))
                return;
        }

        if constexpr (requires (Encoder& e, const Source& s) { T::create(e, s); }) {
            // Code blocks write their arrays first and their record after, so they place themselves.
            T* record = T::create(encoder, *src);
            this->m_offset = safeCast<VariableLengthObjectBase::Offset>(encoder.offsetOf(record) - encoder.offsetOf(&this->m_offset));
            return;
        } else
#if USE(BUN_JSC_ADDITIONS)
        if constexpr (isSingleOwnerCachedType<T>) {
            ASSERT(!encoder.cachedOffsetForPtr(src));
            this->template allocateFor<T>(encoder, *src)->encode(encoder, *src);
            return;
        } else
#endif
        {

        if (std::optional<ptrdiff_t> offset = encoder.cachedOffsetForPtr(src)) {
            this->m_offset = safeCast<VariableLengthObjectBase::Offset>(*offset - encoder.offsetOf(&this->m_offset));
            return;
        }
        if constexpr (holdsString) {
            if (std::optional<ptrdiff_t> offset = encoder.cachedOffsetForStringContents(*src)) {
                this->m_offset = safeCast<VariableLengthObjectBase::Offset>(*offset - encoder.offsetOf(&this->m_offset));
                encoder.cachePtr(src, *offset);
                return;
            }
        }

        T* cachedObject = this->template allocateFor<T>(encoder, *src);
        cachedObject->encode(encoder, *src);
        encoder.cachePtr(src, encoder.offsetOf(cachedObject));
        if constexpr (holdsString)
            encoder.cacheStringContents(*src, encoder.offsetOf(cachedObject));
        }
    }

    template<typename... Args>
    Source* decode(Decoder& decoder, bool& isNewAllocation, Args&&... args) const
    {
        if (this->isEmpty()) {
            isNewAllocation = false;
            return nullptr;
        }
        if constexpr (holdsString) {
            if (this->hasInlineString()) {
                isNewAllocation = true;
                return static_cast<Source*>(&this->inlineString(decoder).leakRef());
            }
            if (this->hasExternalString()) {
                isNewAllocation = true;
                return static_cast<Source*>(&decoder.atomForExternalString(this->externalStringOrdinal()).leakRef());
            }
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

    // For integrity checks before anything is decoded: the target if it lies inside the payload, else null.
    const T* getIfInPayload(Decoder& decoder) const
    {
        if (this->isEmpty())
            return nullptr;
        if constexpr (holdsString) {
            if (this->hasInlineString() || this->hasExternalString())
                return nullptr;
        }
        const T* target = this->template buffer<T>();
        return decoder.payloadContains(target, sizeof(T)) ? target : nullptr;
    }

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
                if constexpr (CachedPtr<T, Source>::holdsString) {
                    if (m_ptr.hasInlineString())
                        return adoptRef<Source, PtrTraits>(static_cast<Source*>(&m_ptr.inlineString(decoder).leakRef()));
                    if (m_ptr.hasExternalString())
                        return adoptRef<Source, PtrTraits>(static_cast<Source*>(&decoder.atomForExternalString(m_ptr.externalStringOrdinal()).leakRef()));
                }
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
    const CachedPtr<T, Source>& ptr() const { return m_ptr; }

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

    // Allocate the element slots now and let the caller encode into them later (used to keep a code block's own bytes
    // ahead of its children's records).
    template<typename VectorContainer>
    std::span<T> allocateElements(Encoder& encoder, const VectorContainer& vector)
    {
        m_size = vector.size();
        if (!m_size)
            return { };
        return { this->template allocate<T>(encoder, m_size), m_size };
    }

    // Encoder side: the slots allocateElements() made.
    std::span<T> mutableElements(Encoder& encoder)
    {
        if (!m_size)
            return { };
        auto bytes = encoder.mutableBytesAt(this->payloadOffsetInEncoder(encoder), sizeof(T) * m_size);
        return { reinterpret_cast<T*>(bytes.data()), m_size };
    }

    // Where the encoded elements are (decoded side), whether or not they are inside the payload; empty if none.
    std::span<const uint8_t> rawBytes() const
    {
        if (!m_size)
            return { };
        return { this->buffer(), sizeof(T) * m_size };
    }

    // The encoded elements themselves, bounds-checked, for integrity checks before decoding.
    std::span<const T> elementsIfInPayload(Decoder& decoder) const
    {
        if (!m_size)
            return { };
        const T* elements = this->template buffer<T>();
        if (!decoder.payloadContains(elements, sizeof(T) * m_size))
            return { };
        return { elements, m_size };
    }
    unsigned size() const { return m_size; }

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

    // The characters follow this 12-byte header (length/flags, precomputed hash, ordinal) directly (see tailSize), instead
    // of a separately aligned allocation reached through an offset.
    static size_t tailSize(const StringImpl& string) { return Shape(string).byteLength(); }

    void encode(Encoder& encoder, const StringImpl& string)
    {
        Shape shape(string);
        m_isSymbol = shape.isSymbol;
        m_isRegistered = shape.isRegistered;
        m_isWellKnownSymbol = shape.isWellKnownSymbol;
        m_isPrivate = shape.isPrivate;
        m_is8Bit = shape.characters->is8Bit();
        m_length = shape.characters->length();
        RELEASE_ASSERT(m_length == shape.characters->length()); // fits the bitfield
        m_hash = shape.characters->hash(); // what StringImpl::hash() / the atom table use, so decode never rehashes
        m_ordinal = m_isSymbol || !m_length ? noOrdinal : encoder.nextStringOrdinal(); // see Decoder::atomForOrdinal
        if (m_is8Bit)
            memcpy(tail(), shape.characters->span8().data(), shape.byteLength());
        else
            memcpy(tail(), shape.characters->span16().data(), shape.byteLength());
    }

    UniquedStringImpl* decode(Decoder& decoder) const
    {
        if (m_ordinal != noOrdinal) {
            if (AtomStringImpl* known = decoder.atomForOrdinal(m_ordinal)) {
                known->ref();
                return static_cast<UniquedStringImpl*>(static_cast<StringImpl*>(known));
            }
        }
        auto create = [&](auto buffer) -> UniquedStringImpl* {
            if (!m_isSymbol) {
                RefPtr<AtomStringImpl> atom;
                // Long strings out of a persistent payload keep their characters in the mapping (clean, shared pages) and
                // only allocate the StringImpl header; AtomStringImpl::add adopts it in place unless the atom already exists.
                if (buffer.size() >= minimumLengthToAliasPayload && decoder.canBorrowPayload())
                    atom = AtomStringImpl::add(RefPtr<StringImpl> { StringImpl::createWithoutCopying(buffer) });
                else {
                    WTF::HashTranslatorCharBuffer<std::remove_const_t<typename decltype(buffer)::element_type>> hashed { buffer, m_hash };
                    atom = AtomStringImpl::add(hashed);
                }
                if (m_ordinal != noOrdinal)
                    decoder.setAtomForOrdinal(m_ordinal, *atom);
                return static_cast<UniquedStringImpl*>(static_cast<StringImpl*>(atom.leakRef()));
            }

            SymbolImpl* symbol;
            VM& vm = decoder.vm();
            if (m_isRegistered) {
                String str(buffer);
                if (m_isPrivate)
                    symbol = static_cast<SymbolImpl*>(&protect(vm.privateSymbolRegistry())->symbolForKey(str).leakRef());
                else
                    symbol = static_cast<SymbolImpl*>(&protect(vm.symbolRegistry())->symbolForKey(str).leakRef());
            } else {
                if (m_isWellKnownSymbol)
                    symbol = vm.propertyNames->builtinNames().lookUpWellKnownSymbol(buffer);
                else
                    symbol = vm.propertyNames->builtinNames().lookUpPrivateName(buffer);
                RELEASE_ASSERT(symbol);
                symbol->ref();
            }
            ASSERT(m_isWellKnownSymbol != symbol->isPrivate());
            return symbol;
        };

        if (!m_length) {
            if (m_isSymbol)
                return &SymbolImpl::createNullSymbol().leakRef();
            return RefPtr { emptyAtom().impl() }.leakRef();
        }

        return m_is8Bit ? create(span8()) : create(span16());
    }

    // For uses that only need the characters (a string constant's JSString), not an atom: no atom table involved.
    String decodePlainString(Decoder& decoder) const
    {
        if (m_isSymbol)
            return String { adoptRef(*static_cast<StringImpl*>(decode(decoder))) };
        if (!m_length)
            return emptyString();
        if (m_ordinal != noOrdinal) {
            if (AtomStringImpl* known = decoder.atomForOrdinal(m_ordinal))
                return String { known };
        }
        if (m_is8Bit) {
            if (m_length >= minimumLengthToAliasPayload && decoder.canBorrowPayload())
                return StringImpl::createWithoutCopying(span8());
            return StringImpl::create(span8());
        }
        if (m_length >= minimumLengthToAliasPayload && decoder.canBorrowPayload())
            return StringImpl::createWithoutCopying(span16());
        return StringImpl::create(span16());
    }

    static constexpr unsigned minimumLengthToAliasPayload = 48; // below this a copy is smaller than pinning part of a page
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
    uint32_t m_hash { 0 };
    // Distinct (non-symbol) strings are numbered in encode order; the decoder keeps the atom for each number it has seen,
    // so only the first block to name a string goes through the atom table.
    static constexpr uint32_t noOrdinal = std::numeric_limits<uint32_t>::max();
    uint32_t m_ordinal { noOrdinal };
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
        ptrdiff_t self = encoder.offsetOf(this);
        m_checksum = encoder.checksumOfRecordAndArray(self, sizeof(*this), encoder.offsetOf(&m_checksum), info.payloadSize() ? m_storage.payloadOffsetInEncoder(encoder) : 0, payloadBytes());
    }

    // Lives in the cold tail, outside its code block's checksummed region, so it carries its own; a damaged one decodes as
    // "no expression info" (stack traces lose line/column for that function) rather than failing the function.
    std::unique_ptr<ExpressionInfo> decode(Decoder& decoder) const
    {
        if (!decoder.recordAndArrayChecksumMatches(this, sizeof(*this), &m_checksum, m_storage.rawElements(), payloadBytes()))
            return ExpressionInfo::createUninitialized(0, 0, 0);
        if (decoder.canBorrowPayload() && !m_storage.isEmpty())
            return ExpressionInfo::createBorrowed(m_numberOfChapters, m_numberOfEncodedInfo, m_numberOfEncodedInfoExtensions, m_storage.borrow());
        auto info = ExpressionInfo::createUninitialized(m_numberOfChapters, m_numberOfEncodedInfo, m_numberOfEncodedInfoExtensions);
        m_storage.decode(decoder, info->payload(), info->payloadSize());
        return info;
    }

private:
    size_t payloadBytes() const { return ExpressionInfo::payloadSizeInBytes(m_numberOfChapters, m_numberOfEncodedInfo, m_numberOfEncodedInfoExtensions); }

    uint32_t m_checksum { 0 };
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
            auto str = string->tryGetValue();
            RELEASE_ASSERT(str.data.impl()); // constants are never unresolved ropes; a failed resolution must not be encoded as garbage
            StringImpl& impl = *str.data.impl();
            if (this->tryEncodeInlineString(impl))
                return;
            if (this->tryEncodeExternalString(encoder, impl))
                return;
            if (auto existing = encoder.cachedOffsetForStringContents(impl)) {
                this->pointAtPayloadOffset(encoder, *existing);
                return;
            }
            auto* record = this->allocateFor<CachedUniquedStringImpl>(encoder, impl);
            record->encode(encoder, impl);
            encoder.cacheStringContents(impl, encoder.offsetOf(record));
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
            if (this->hasInlineString()) {
                v = jsString(decoder.vm(), String { this->inlineString(decoder) });
                break;
            }
            if (this->hasExternalString()) {
                v = jsString(decoder.vm(), decoder.plainStringForExternalString(this->externalStringOrdinal()));
                break;
            }
            // A constant becomes a JSString; it does not have to be an atom, so skip the atom table.
            v = jsString(decoder.vm(), this->buffer<CachedUniquedStringImpl>()->decodePlainString(decoder));
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


// UnlinkedMetadataTable's offset table is cumulative and most opcodes have no metadata in a given function, so a code
// block stores only the entries where the running offset changes: (index << 24 | delta). A typical function has a handful
// instead of 51.
struct CachedMetadataSteps {
    static constexpr unsigned indexShift = UnlinkedMetadataTable::stepIndexShift;
    static constexpr uint32_t deltaMask = UnlinkedMetadataTable::stepDeltaMask;
    static_assert(UnlinkedMetadataTable::s_offsetTableEntries < (1u << (32 - indexShift)));

    static Vector<uint32_t, 16> compute(const UnlinkedMetadataTable& metadataTable)
    {
        ASSERT(metadataTable.m_isFinalized && metadataTable.m_hasMetadata);
        Vector<uint32_t, 16> steps;
        if (metadataTable.m_steps && !metadataTable.m_isLinked) {
            steps.append(std::span { metadataTable.m_steps, metadataTable.m_stepsCount });
            return steps;
        }
        uint32_t previous = 0;
        for (unsigned i = 0; i < UnlinkedMetadataTable::s_offsetTableEntries; ++i) {
            uint32_t value = metadataTable.m_is32Bit ? metadataTable.offsetTable32()[i] : metadataTable.offsetTable16()[i];
            if (value == previous)
                continue;
            RELEASE_ASSERT(value > previous && value - previous <= deltaMask);
            steps.append(i << indexShift | (value - previous));
            previous = value;
        }
        return steps;
    }

    static Ref<UnlinkedMetadataTable> build(bool is32Bit, unsigned numValueProfiles, std::span<const uint32_t> steps)
    {
        Ref<UnlinkedMetadataTable> metadataTable = UnlinkedMetadataTable::create(is32Bit, numValueProfiles);
        metadataTable->m_isFinalized = true;
        metadataTable->m_isLinked = false;
        metadataTable->m_hasMetadata = true;
        metadataTable->m_numValueProfiles = numValueProfiles;
        if (is32Bit)
            UnlinkedMetadataTable::expandSteps(steps, metadataTable->offsetTable32());
        else
            UnlinkedMetadataTable::expandSteps(steps, metadataTable->offsetTable16());
        return metadataTable;
    }
};

// Arrays a code block refers to from its varint tail by (count, offset) instead of through an 8-byte CachedVector member.
// Plain arrays may be shared with an identical one written earlier (see Encoder::ShareableArrayScope).
template<typename T, typename Container>
static ptrdiff_t encodeArrayForTail(Encoder& encoder, const Container& container)
{
    unsigned size = container.size();
    ASSERT(size);
    if constexpr (std::is_same_v<T, SourceType<T>> && std::is_trivially_copyable_v<T>) {
        auto bytes = std::span { std::bit_cast<const uint8_t*>(container.span().data()), sizeof(T) * size };
        unsigned hash = StringHasher::computeHashAndMaskTop8Bits(bytes) ^ static_cast<unsigned>(bytes.size());
        if (encoder.arraySharingEnabled()) {
            if (auto existing = encoder.existingIdenticalArray(bytes, hash, alignof(T))) {
                encoder.noteSharedArray(*existing, bytes.size());
                return *existing;
            }
        }
        auto result = encoder.malloc(bytes.size(), alignof(T));
        memcpySpan(std::span { result.buffer(), bytes.size() }, bytes);
        encoder.registerArray(hash, result.offset(), bytes.size());
        return result.offset();
    } else {
        auto result = encoder.malloc(sizeof(T) * size, alignof(T));
        T* buffer = new (result.buffer()) T[size];
        for (unsigned i = 0; i < size; ++i)
            ::JSC::encode(encoder, buffer[i], container[i]);
        return result.offset();
    }
}

template<typename T, typename Container, typename... Args>
static void decodeArrayFromTail(Decoder& decoder, const void* elements, unsigned size, Container& out, Args... args)
{
    if (!size)
        return;
    out = Container(size);
    const T* buffer = static_cast<const T*>(elements);
    for (unsigned i = 0; i < size; ++i)
        ::JSC::decode(decoder, buffer[i], out[i], args...);
}

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

    // Covers the whole record and its tail (CachedBytecode::commitUpdates re-seals a record it patches); checked by the owning
    // code block before it decodes anything.
    bool isIntact(Decoder& decoder) const
    {
        return decoder.regionChecksumMatches(this, m_extent, &m_checksum);
    }

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

    // Rewritable in place by CachedBytecode::commitUpdates (jsc shell cache updates), which then re-seals m_checksum.
    CachedFunctionExecutableMetadata m_mutableMetadata;
    CachedWriteBarrier<CachedFunctionCodeBlock, UnlinkedFunctionCodeBlock> m_unlinkedCodeBlockForCall;
    CachedWriteBarrier<CachedFunctionCodeBlock, UnlinkedFunctionCodeBlock> m_unlinkedCodeBlockForConstruct;

    uint32_t m_checksum { 0 };
    uint32_t m_extent { 0 }; // record + tail + whatever rare data / name / TDZ environment this record was the first to write
    CachedPtr<CachedFunctionExecutableRareData> m_rareData;
    CachedIdentifier m_ecmaName;
    CachedRefPtr<CachedTDZEnvironmentLink> m_parentScopeTDZVariables;
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

ptrdiff_t CachedFunctionExecutableOffsets::checksumOffset()
{
    return OBJECT_OFFSETOF(CachedFunctionExecutable, m_checksum);
}

ptrdiff_t CachedFunctionExecutableOffsets::extentOffset()
{
    return OBJECT_OFFSETOF(CachedFunctionExecutable, m_extent);
}

size_t CachedFunctionExecutableOffsets::fixedSize()
{
    return sizeof(CachedFunctionExecutable);
}

uint32_t bytecodeCacheRecordChecksum(std::span<const uint8_t> record, size_t checksumOffset)
{
    static const uint8_t zeros[4] = { };
    uint32_t crc = ~0u;
    crc = crc32c(crc, record.first(checksumOffset));
    crc = crc32c(crc, std::span { zeros, 4 });
    crc = crc32c(crc, record.subspan(checksumOffset + 4));
    return ~crc;
}

template<typename CodeBlockType> struct CachedCodeBlockRecordFor;

class CachedProgramCodeBlock;
class CachedModuleCodeBlock;
class CachedEvalCodeBlock;
class CachedFunctionCodeBlock;
template<> struct CachedCodeBlockRecordFor<UnlinkedProgramCodeBlock> { using type = CachedProgramCodeBlock; };
template<> struct CachedCodeBlockRecordFor<UnlinkedModuleProgramCodeBlock> { using type = CachedModuleCodeBlock; };
template<> struct CachedCodeBlockRecordFor<UnlinkedEvalCodeBlock> { using type = CachedEvalCodeBlock; };
template<> struct CachedCodeBlockRecordFor<UnlinkedFunctionCodeBlock> { using type = CachedFunctionCodeBlock; };

// The few members most code blocks never have; written (before the record, like everything else) only when one is set.
struct CachedCodeBlockExtras {
    void encode(Encoder& encoder, const UnlinkedCodeBlock& codeBlock)
    {
        rareData.encode(encoder, codeBlock.m_rareData.get());
        sourceURLDirective.encode(encoder, codeBlock.m_sourceURLDirective.get());
        sourceMappingURLDirective.encode(encoder, codeBlock.m_sourceMappingURLDirective.get());
        outOfLineJumpTargets.encode(encoder, codeBlock.m_outOfLineJumpTargets);
    }
    static bool isNeeded(const UnlinkedCodeBlock& codeBlock)
    {
        return codeBlock.m_rareData || codeBlock.m_sourceURLDirective || codeBlock.m_sourceMappingURLDirective || !codeBlock.m_outOfLineJumpTargets.isEmpty();
    }

    CachedPtr<CachedCodeBlockRareData> rareData;
    CachedRefPtr<CachedStringImpl> sourceURLDirective;
    CachedRefPtr<CachedStringImpl> sourceMappingURLDirective;
    CachedHashMap<JSInstructionStream::Offset, int> outOfLineJumpTargets;
};

// A code block is written as one region: its arrays (metadata steps, instructions, constants, identifiers, jump targets,
// child slots, extras), then a 16-byte record followed by a varint tail that says where in the region each array is and
// holds every count/register/flag, then whatever the derived record adds, then the children's executable records.
// Offsets in the tail are relative to the start of the region, so they are 1-2 bytes for nearly every function.
template<typename CodeBlockType>
class CachedCodeBlock : public CachedObject<CodeBlockType> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif
    using Record = typename CachedCodeBlockRecordFor<CodeBlockType>::type;

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

    enum LayoutFlag : uint8_t {
        LayoutHasMetadata = 1 << 0,
        LayoutMetadataIs32Bit = 1 << 1,
        LayoutHasExtras = 1 << 2,
    };
    struct Array {
        unsigned count { 0 };
        int32_t at { 0 }; // relative to the region start; only meaningful when count is non-zero
    };
    struct Layout {
        uint8_t flags { 0 };
        unsigned metadataValueProfiles { 0 };
        Array steps;
        Array instructions; // count is in bytes
        Array constants;
        Array constantsSourceCodeRepresentation;
        Array identifiers;
        Array jumpTargets;
        Array functionDecls;
        Array functionExprs;
        int32_t extrasAt { 0 };
    };
    struct Tail {
        Layout layout;
        Scalars scalars;
        bool intact { true };
    };

    static Record* create(Encoder&, const CodeBlockType&);
    void decode(Decoder&, UnlinkedCodeBlock&, const Tail&) const;

    // `limit` bounds the parse for the integrity check; once the region is verified it is read unbounded.
    Tail readTail(const uint8_t* limit = nullptr) const;
    // The tail the decode in progress already parsed (see ActiveTailScope), else a fresh parse.
    const Tail& tail(Decoder& decoder, Tail& storage) const
    {
        if (auto* active = static_cast<const Tail*>(decoder.activeCodeBlockTail(this)))
            return *active;
        storage = readTail();
        return storage;
    }
    struct ActiveTailScope {
        ActiveTailScope(Decoder& decoder, const void* record, const Tail& tail)
            : m_decoder(decoder)
        {
            decoder.setActiveCodeBlockTail(record, &tail);
        }
        ~ActiveTailScope() { m_decoder.setActiveCodeBlockTail(nullptr, nullptr); }
        Decoder& m_decoder;
    };
    Scalars scalars(Decoder& decoder) const { Tail storage; return tail(decoder, storage).scalars; }

    const uint8_t* regionBegin() const { return std::bit_cast<const uint8_t*>(this) - m_recordOffsetInRegion; }
    template<typename T> const T* at(const Array& array) const { return array.count ? reinterpret_cast<const T*>(regionBegin() + array.at) : nullptr; }
    const CachedCodeBlockExtras* extras(const Layout& layout) const { return layout.flags & LayoutHasExtras ? reinterpret_cast<const CachedCodeBlockExtras*>(regionBegin() + layout.extrasAt) : nullptr; }

    JSInstructionStream* instructions(Decoder& decoder) const
    {
        Tail storage;
        const Layout& layout = tail(decoder, storage).layout;
        std::span<const uint8_t> bytes { at<uint8_t>(layout.instructions), layout.instructions.count };
        if (decoder.canBorrowPayload())
            return new JSInstructionStream(bytes, JSInstructionStream::Borrow);
        Vector<uint8_t, 0, UnsafeVectorOverflow, 16, InstructionStreamBufferMalloc> copy;
        copy.append(bytes);
        return new JSInstructionStream(WTF::move(copy));
    }

    Ref<UnlinkedMetadataTable> metadata(Decoder& decoder) const
    {
        Tail storage;
        const Layout& layout = tail(decoder, storage).layout;
        if (!(layout.flags & LayoutHasMetadata))
            return UnlinkedMetadataTable::empty();
        std::span<const uint32_t> steps { at<uint32_t>(layout.steps), layout.steps.count };
        if (decoder.canBorrowPayload())
            return UnlinkedMetadataTable::createFromPersistentSteps(layout.flags & LayoutMetadataIs32Bit, layout.metadataValueProfiles, steps);
        return CachedMetadataSteps::build(layout.flags & LayoutMetadataIs32Bit, layout.metadataValueProfiles, steps);
    }

    RefPtr<StringImpl> sourceURLDirective(Decoder& decoder) const
    {
        Tail storage;
        auto* e = extras(tail(decoder, storage).layout);
        return e ? e->sourceURLDirective.decode(decoder) : nullptr;
    }
    RefPtr<StringImpl> sourceMappingURLDirective(Decoder& decoder) const
    {
        Tail storage;
        auto* e = extras(tail(decoder, storage).layout);
        return e ? e->sourceMappingURLDirective.decode(decoder) : nullptr;
    }
    UnlinkedCodeBlock::RareData* rareData(Decoder& decoder) const
    {
        Tail storage;
        auto* e = extras(tail(decoder, storage).layout);
        return e ? e->rareData.decode(decoder) : nullptr;
    }

    // The region (arrays, record, tail, derived members, child slots) is checksummed; a mismatch on decode means the
    // payload is damaged and the block is generated from source instead.
    bool regionIsIntact(Decoder& decoder, Tail& tail) const
    {
        // `this` came from a slot that CachedBytecode::commitUpdates may rewrite and is therefore not itself checksummed.
        if (!decoder.payloadContains(this, sizeof(Record)))
            return false;
        const uint8_t* begin = regionBegin();
        const uint8_t* end = begin + m_regionSize;
        if (!decoder.payloadContains(begin, m_regionSize) || begin > std::bit_cast<const uint8_t*>(this) || std::bit_cast<const uint8_t*>(this) + sizeof(Record) > end)
            return false;
        tail = readTail(end);
        if (!tail.intact)
            return false;
        const Layout& layout = tail.layout;

        // Every array must lie inside the region, except the four the encoder may have shared from an earlier block,
        // which are folded into the checksum instead (in encoder order).
        std::array<std::span<const uint8_t>, 4> external;
        unsigned externalCount = 0;
        auto covered = [&](const Array& array, size_t elementSize, bool shareable) {
            if (!array.count)
                return true;
            size_t bytes = elementSize * array.count;
            const uint8_t* p = begin + array.at;
            if (array.at >= 0 && p + bytes <= end && p + bytes >= p)
                return true;
            if (!shareable)
                return false;
            external[externalCount++] = { p, bytes };
            return true;
        };
        if (!covered(layout.steps, sizeof(uint32_t), true)
            || !covered(layout.instructions, 1, true)
            || !covered(layout.constantsSourceCodeRepresentation, sizeof(SourceCodeRepresentation), true)
            || !covered(layout.jumpTargets, sizeof(JSInstructionStream::Offset), true)
            || !covered(layout.constants, sizeof(CachedJSValue), false)
            || !covered(layout.identifiers, sizeof(CachedIdentifier), false)
            || !covered(layout.functionDecls, sizeof(CachedWriteBarrier<CachedFunctionExecutable>), false)
            || !covered(layout.functionExprs, sizeof(CachedWriteBarrier<CachedFunctionExecutable>), false))
            return false;
        if ((layout.flags & LayoutHasExtras) && (layout.extrasAt < 0 || begin + layout.extrasAt + sizeof(CachedCodeBlockExtras) > end))
            return false;
        if (!decoder.regionChecksumMatches(begin, m_regionSize, &m_regionChecksum, std::span { external.data(), externalCount }))
            return false;

        for (const Array* children : { &layout.functionDecls, &layout.functionExprs }) {
            auto* slots = at<CachedWriteBarrier<CachedFunctionExecutable>>(*children);
            for (unsigned i = 0; i < children->count; ++i) {
                auto* record = slots[i].ptr().getIfInPayload(decoder);
                if (!record || !record->isIntact(decoder))
                    return false;
            }
        }
        return true;
    }

protected:
    // Derived records with nothing of their own use these.
    void encodeOwnMembers(Encoder&, const CodeBlockType&) { }
    void decodeOwnMembers(Decoder&, CodeBlockType&) const { }

private:
    static void packScalars(const UnlinkedCodeBlock&, VarintWriter&);
    static void packLayout(const Layout&, VarintWriter&);
    const uint8_t* tailBytes() const { return std::bit_cast<const uint8_t*>(this) + sizeof(Record); }
    uint8_t* tailBytes() { return std::bit_cast<uint8_t*>(this) + sizeof(Record); }

    uint32_t m_regionChecksum { 0 };
    uint32_t m_recordOffsetInRegion { 0 };
    uint32_t m_regionSize { 0 };
    CachedPtr<CachedExpressionInfo> m_expressionInfo; // written by the deferred cold pass, so it stays a fixed slot
};

class CachedProgramCodeBlock : public CachedCodeBlock<UnlinkedProgramCodeBlock> {
    using Base = CachedCodeBlock<UnlinkedProgramCodeBlock>;
    friend Base;

public:
    UnlinkedProgramCodeBlock* decode(Decoder&) const;

private:
    void encodeOwnMembers(Encoder& encoder, const UnlinkedProgramCodeBlock& codeBlock)
    {
        m_varDeclarations.encode(encoder, codeBlock.m_varDeclarations);
        m_lexicalDeclarations.encode(encoder, codeBlock.m_lexicalDeclarations);
    }
    void decodeOwnMembers(Decoder& decoder, UnlinkedProgramCodeBlock& codeBlock) const
    {
        m_varDeclarations.decode(decoder, codeBlock.m_varDeclarations);
        m_lexicalDeclarations.decode(decoder, codeBlock.m_lexicalDeclarations);
    }

    CachedVariableEnvironment m_varDeclarations;
    CachedVariableEnvironment m_lexicalDeclarations;
};

class CachedModuleCodeBlock : public CachedCodeBlock<UnlinkedModuleProgramCodeBlock> {
    using Base = CachedCodeBlock<UnlinkedModuleProgramCodeBlock>;
    friend Base;

public:
    UnlinkedModuleProgramCodeBlock* decode(Decoder&) const;

private:
    void encodeOwnMembers(Encoder& encoder, const UnlinkedModuleProgramCodeBlock& codeBlock)
    {
        m_varDeclarations.encode(encoder, codeBlock.m_varDeclarations);
        m_moduleEnvironmentSymbolTableConstantRegisterOffset = codeBlock.m_moduleEnvironmentSymbolTableConstantRegisterOffset;
    }
    void decodeOwnMembers(Decoder& decoder, UnlinkedModuleProgramCodeBlock& codeBlock) const
    {
        m_varDeclarations.decode(decoder, codeBlock.m_varDeclarations);
        codeBlock.m_moduleEnvironmentSymbolTableConstantRegisterOffset = m_moduleEnvironmentSymbolTableConstantRegisterOffset;
    }

    CachedVariableEnvironment m_varDeclarations;
    int m_moduleEnvironmentSymbolTableConstantRegisterOffset;
};

class CachedEvalCodeBlock : public CachedCodeBlock<UnlinkedEvalCodeBlock> {
    using Base = CachedCodeBlock<UnlinkedEvalCodeBlock>;
    friend Base;

public:
    UnlinkedEvalCodeBlock* decode(Decoder&) const;

private:
    void encodeOwnMembers(Encoder& encoder, const UnlinkedEvalCodeBlock& codeBlock)
    {
        m_variables.encode(encoder, codeBlock.m_variables);
        m_functionHoistingCandidates.encode(encoder, codeBlock.m_functionHoistingCandidates);
    }
    void decodeOwnMembers(Decoder& decoder, UnlinkedEvalCodeBlock& codeBlock) const
    {
        m_variables.decode(decoder, codeBlock.m_variables);
        m_functionHoistingCandidates.decode(decoder, codeBlock.m_functionHoistingCandidates);
    }

    CachedVector<CachedIdentifier, 0, UnsafeVectorOverflow> m_variables;
    CachedVector<CachedIdentifier, 0, UnsafeVectorOverflow> m_functionHoistingCandidates;
};

class CachedFunctionCodeBlock : public CachedCodeBlock<UnlinkedFunctionCodeBlock> {
    using Base = CachedCodeBlock<UnlinkedFunctionCodeBlock>;
    friend Base;

public:
    UnlinkedFunctionCodeBlock* decode(Decoder&) const;
};


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
    CachedBuiltinFunctionTag, // a root UnlinkedFunctionExecutable created by BuiltinExecutables (an embedder's JS builtins)
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
    auto scalars = cachedCodeBlock.scalars(decoder);
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
ALWAYS_INLINE void CachedCodeBlock<CodeBlockType>::decode(Decoder& decoder, UnlinkedCodeBlock& codeBlock, const Tail& tail) const
{
    const Layout& layout = tail.layout;
    // Most identifiers and many constants become atoms; let the table grow once for this block rather than as they trickle in.
    if (unsigned expected = layout.identifiers.count + layout.constants.count; expected >= 64)
        AtomStringImpl::reserveCapacityForCurrentThread(expected);
    decodeArrayFromTail<CachedJSValue>(decoder, at<CachedJSValue>(layout.constants), layout.constants.count, codeBlock.m_constantRegisters, &codeBlock);
    decodeArrayFromTail<SourceCodeRepresentation>(decoder, at<SourceCodeRepresentation>(layout.constantsSourceCodeRepresentation), layout.constantsSourceCodeRepresentation.count, codeBlock.m_constantsSourceCodeRepresentation);
    codeBlock.m_expressionInfo = m_expressionInfo->decode(decoder);
    if (auto* e = extras(layout))
        e->outOfLineJumpTargets.decode(decoder, codeBlock.m_outOfLineJumpTargets);
    decodeArrayFromTail<JSInstructionStream::Offset>(decoder, at<JSInstructionStream::Offset>(layout.jumpTargets), layout.jumpTargets.count, codeBlock.m_jumpTargets);
    decodeArrayFromTail<CachedIdentifier>(decoder, at<CachedIdentifier>(layout.identifiers), layout.identifiers.count, codeBlock.m_identifiers);
    decodeArrayFromTail<CachedWriteBarrier<CachedFunctionExecutable>>(decoder, at<CachedWriteBarrier<CachedFunctionExecutable>>(layout.functionDecls), layout.functionDecls.count, codeBlock.m_functionDecls, &codeBlock);
    decodeArrayFromTail<CachedWriteBarrier<CachedFunctionExecutable>>(decoder, at<CachedWriteBarrier<CachedFunctionExecutable>>(layout.functionExprs), layout.functionExprs.count, codeBlock.m_functionExprs, &codeBlock);
}

UnlinkedProgramCodeBlock* CachedProgramCodeBlock::decode(Decoder& decoder) const
{
    Tail tail;
    if (!regionIsIntact(decoder, tail))
        return nullptr;
    ActiveTailScope activeTail(decoder, this, tail);
    UnlinkedProgramCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedProgramCodeBlock>(decoder.vm())) UnlinkedProgramCodeBlock(decoder, *this);
    codeBlock->finishCreation(decoder.vm());
    Base::decode(decoder, *codeBlock, tail);
    decodeOwnMembers(decoder, *codeBlock);
    return codeBlock;
}

UnlinkedModuleProgramCodeBlock* CachedModuleCodeBlock::decode(Decoder& decoder) const
{
    Tail tail;
    if (!regionIsIntact(decoder, tail))
        return nullptr;
    ActiveTailScope activeTail(decoder, this, tail);
    UnlinkedModuleProgramCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedModuleProgramCodeBlock>(decoder.vm())) UnlinkedModuleProgramCodeBlock(decoder, *this);
    codeBlock->finishCreation(decoder.vm());
    Base::decode(decoder, *codeBlock, tail);
    decodeOwnMembers(decoder, *codeBlock);
    return codeBlock;
}

UnlinkedEvalCodeBlock* CachedEvalCodeBlock::decode(Decoder& decoder) const
{
    Tail tail;
    if (!regionIsIntact(decoder, tail))
        return nullptr;
    ActiveTailScope activeTail(decoder, this, tail);
    UnlinkedEvalCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedEvalCodeBlock>(decoder.vm())) UnlinkedEvalCodeBlock(decoder, *this);
    codeBlock->finishCreation(decoder.vm());
    Base::decode(decoder, *codeBlock, tail);
    decodeOwnMembers(decoder, *codeBlock);
    return codeBlock;
}

UnlinkedFunctionCodeBlock* CachedFunctionCodeBlock::decode(Decoder& decoder) const
{
    Tail tail;
    if (!regionIsIntact(decoder, tail))
        return nullptr;
    ActiveTailScope activeTail(decoder, this, tail);
    UnlinkedFunctionCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedFunctionCodeBlock>(decoder.vm())) UnlinkedFunctionCodeBlock(decoder, *this);
    codeBlock->finishCreation(decoder.vm());
    Base::decode(decoder, *codeBlock, tail);
    decodeOwnMembers(decoder, *codeBlock);
    return codeBlock;
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

    ptrdiff_t start = encoder.offsetOf(this);
    m_extent = safeCast<uint32_t>(encoder.currentOffset() - start);
    encoder.addChecksum(start, m_extent, encoder.offsetOf(&m_checksum));

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
void CachedCodeBlock<CodeBlockType>::packLayout(const Layout& layout, VarintWriter& writer)
{
    writer.u8(layout.flags);
    if (layout.flags & LayoutHasMetadata)
        writer.u32(layout.metadataValueProfiles);
    auto array = [&](const Array& a) {
        writer.u32(a.count);
        if (a.count)
            writer.i32(a.at);
    };
    array(layout.steps);
    array(layout.instructions);
    array(layout.constants);
    array(layout.constantsSourceCodeRepresentation);
    array(layout.identifiers);
    array(layout.jumpTargets);
    array(layout.functionDecls);
    array(layout.functionExprs);
    if (layout.flags & LayoutHasExtras)
        writer.i32(layout.extrasAt);
}

template<typename CodeBlockType>
auto CachedCodeBlock<CodeBlockType>::readTail(const uint8_t* limit) const -> Tail
{
    Tail tail;
    VarintReader reader(tailBytes(), limit);
    Layout& layout = tail.layout;
    layout.flags = reader.u8();
    if (layout.flags & LayoutHasMetadata)
        layout.metadataValueProfiles = reader.u32();
    auto array = [&](Array& a) {
        a.count = reader.u32();
        if (a.count)
            a.at = reader.i32();
    };
    array(layout.steps);
    array(layout.instructions);
    array(layout.constants);
    array(layout.constantsSourceCodeRepresentation);
    array(layout.identifiers);
    array(layout.jumpTargets);
    array(layout.functionDecls);
    array(layout.functionExprs);
    if (layout.flags & LayoutHasExtras)
        layout.extrasAt = reader.i32();

    Scalars& s = tail.scalars;
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
    tail.intact = !reader.overran();
    return tail;
}

template<typename CodeBlockType>
auto CachedCodeBlock<CodeBlockType>::create(Encoder& encoder, const CodeBlockType& codeBlock) -> Record*
{
    ptrdiff_t regionStart = encoder.currentOffset();
    encoder.beginBlockRegion(regionStart);
    Layout layout;
    auto place = [&](Array& array, unsigned count, auto&& write) {
        array.count = count;
        if (count)
            array.at = safeCast<int32_t>(write() - regionStart);
    };

    // These four may be shared with an identical array written earlier; regionIsIntact() follows them in this order.
    {
        Encoder::ShareableArrayScope shareable(encoder);
        const UnlinkedMetadataTable& metadata = codeBlock.m_metadata.get();
        if (metadata.m_hasMetadata) {
            layout.flags |= LayoutHasMetadata | (metadata.m_is32Bit ? LayoutMetadataIs32Bit : 0);
            layout.metadataValueProfiles = metadata.m_numValueProfiles;
            auto steps = CachedMetadataSteps::compute(metadata);
            place(layout.steps, steps.size(), [&] { return encodeArrayForTail<uint32_t>(encoder, steps); });
        }
        const JSInstructionStream& instructions = *codeBlock.m_instructions;
        RELEASE_ASSERT(!instructions.isBorrowed()); // a borrowed stream's bytes live in the payload being read
        place(layout.instructions, instructions.m_instructions.size(), [&] { return encodeArrayForTail<uint8_t>(encoder, instructions.m_instructions); });
        place(layout.constantsSourceCodeRepresentation, codeBlock.m_constantsSourceCodeRepresentation.size(), [&] { return encodeArrayForTail<SourceCodeRepresentation>(encoder, codeBlock.m_constantsSourceCodeRepresentation); });
        place(layout.jumpTargets, codeBlock.m_jumpTargets.size(), [&] { return encodeArrayForTail<JSInstructionStream::Offset>(encoder, codeBlock.m_jumpTargets); });
    }
    place(layout.constants, codeBlock.m_constantRegisters.size(), [&] { return encodeArrayForTail<CachedJSValue>(encoder, codeBlock.m_constantRegisters); });
    place(layout.identifiers, codeBlock.m_identifiers.size(), [&] { return encodeArrayForTail<CachedIdentifier>(encoder, codeBlock.m_identifiers); });
    // The children's slots are part of this block's bytes; the records they point at are written after the region.
    auto allocateSlots = [&](unsigned count) {
        auto result = encoder.malloc(sizeof(CachedWriteBarrier<CachedFunctionExecutable>) * count, alignof(CachedWriteBarrier<CachedFunctionExecutable>));
        new (result.buffer()) CachedWriteBarrier<CachedFunctionExecutable>[count];
        return result.offset();
    };
    place(layout.functionDecls, codeBlock.m_functionDecls.size(), [&] { return allocateSlots(codeBlock.m_functionDecls.size()); });
    place(layout.functionExprs, codeBlock.m_functionExprs.size(), [&] { return allocateSlots(codeBlock.m_functionExprs.size()); });
    if (CachedCodeBlockExtras::isNeeded(codeBlock)) {
        layout.flags |= LayoutHasExtras;
        auto result = encoder.malloc(sizeof(CachedCodeBlockExtras), alignof(CachedCodeBlockExtras));
        layout.extrasAt = safeCast<int32_t>(result.offset() - regionStart);
        (new (result.buffer()) CachedCodeBlockExtras())->encode(encoder, codeBlock);
    }

    VarintWriter writer;
    packLayout(layout, writer);
    packScalars(codeBlock, writer);
    auto result = encoder.malloc(sizeof(Record) + writer.size(), alignof(Record));
    Record* record = new (result.buffer()) Record();
    record->m_recordOffsetInRegion = safeCast<uint32_t>(result.offset() - regionStart);
    writer.copyTo(record->tailBytes());
    encoder.deferCold([record, &encoder, expressionInfo = codeBlock.m_expressionInfo.get()] {
        Encoder::ShareableArrayScope shareable(encoder); // self-checksummed over its actual storage, so sharing is safe
        record->m_expressionInfo.encode(encoder, expressionInfo);
    });
    record->encodeOwnMembers(encoder, codeBlock);

    record->m_regionSize = safeCast<uint32_t>(encoder.currentOffset() - regionStart);
    encoder.addChecksum(regionStart, record->m_regionSize, encoder.offsetOf(&record->m_regionChecksum), encoder.takeBlockExternalArrays());

    auto encodeChildren = [&](const Array& slots, const auto& executables) {
        if (!slots.count)
            return;
        auto bytes = encoder.mutableBytesAt(regionStart + slots.at, sizeof(CachedWriteBarrier<CachedFunctionExecutable>) * slots.count);
        auto* slot = reinterpret_cast<CachedWriteBarrier<CachedFunctionExecutable>*>(bytes.data());
        for (unsigned i = 0; i < slots.count; ++i)
            slot[i].encode(encoder, executables[i]);
    };
    encodeChildren(layout.functionDecls, codeBlock.m_functionDecls);
    encodeChildren(layout.functionExprs, codeBlock.m_functionExprs);
    return record;
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
        // The entry, its boot session string and its source code key, up to where the code block starts.
        if (!decoder.regionChecksumMatches(this, m_headerSize, &m_headerChecksum))
            return false;
        if (m_bootSessionUUID.decode(decoder) != bootSessionUUIDString())
            return false;
        return true;
    }

    void sealHeader(Encoder& encoder)
    {
        m_headerSize = safeCast<uint32_t>(encoder.currentOffset()); // the entry is at offset 0
        encoder.addChecksum(0, m_headerSize, encoder.offsetOf(&m_headerChecksum));
    }

private:
    uint32_t m_cacheVersion;
    uint32_t m_headerSize { 0 };
    uint32_t m_headerChecksum { 0 };
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
        sealHeader(encoder);
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
    case CachedCodeBlockTag::CachedBuiltinFunctionTag:
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
    case CachedCodeBlockTag::CachedBuiltinFunctionTag:
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
    case CachedCodeBlockTag::CachedBuiltinFunctionTag:
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

// A builtin function (BuiltinExecutables::createExecutable) and, lazily, its body and nested functions. The embedder
// supplies the source it was created from and a stamp identifying that source's contents; nothing is hashed at load.
class BuiltinFunctionCacheEntry : public GenericCacheEntry {
public:
    BuiltinFunctionCacheEntry(Encoder& encoder)
        : GenericCacheEntry(encoder, CachedCodeBlockTag::CachedBuiltinFunctionTag)
    {
    }

    void encode(Encoder& encoder, const UnlinkedFunctionExecutable& executable, unsigned sourceLength, unsigned embedderStamp)
    {
        m_sourceLength = sourceLength;
        m_embedderStamp = embedderStamp;
        sealHeader(encoder);
        m_executable.encode(encoder, &executable);
    }

    UnlinkedFunctionExecutable* decode(Decoder& decoder, unsigned sourceLength, unsigned embedderStamp) const
    {
        if (tag() != CachedCodeBlockTag::CachedBuiltinFunctionTag || !isUpToDate(decoder))
            return nullptr;
        if (m_sourceLength != sourceLength || m_embedderStamp != embedderStamp)
            return nullptr;
        auto* record = m_executable.getIfInPayload(decoder);
        if (!record || !record->isIntact(decoder))
            return nullptr;
        return m_executable.decode(decoder);
    }

private:
    unsigned m_sourceLength { 0 };
    unsigned m_embedderStamp { 0 };
    CachedPtr<CachedFunctionExecutable> m_executable;
};

RefPtr<CachedBytecode> encodeBuiltinFunction(VM& vm, const UnlinkedFunctionExecutable* executable, unsigned sourceLength, unsigned embedderStamp)
{
    BytecodeCacheError error;
    FileSystem::FileHandle invalidFileHandle;
    Encoder encoder(vm, invalidFileHandle);
    encoder.template malloc<BuiltinFunctionCacheEntry>(encoder)->encode(encoder, *executable, sourceLength, embedderStamp);
    encoder.encodeDeferred();
    return encoder.release(error);
}

UnlinkedFunctionExecutable* decodeBuiltinFunction(VM& vm, Ref<CachedBytecode> cachedBytecode, SourceProvider& provider, unsigned embedderStamp)
{
    if (cachedBytecode->span().size() < sizeof(BuiltinFunctionCacheEntry))
        return nullptr;
    unsigned sourceLength = provider.source().length();
    auto* entry = std::bit_cast<const BuiltinFunctionCacheEntry*>(cachedBytecode->span().data());
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode), &provider);
    DeferGC deferGC(vm);
    return entry->decode(decoder.get(), sourceLength, embedderStamp);
}

RefPtr<CachedBytecode> encodeCodeBlock(VM& vm, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock, FileSystem::FileHandle& fileHandle, BytecodeCacheError& error, EncoderStringTable* externalStrings)
{
    const ClassInfo* classInfo = codeBlock->classInfo();

    Encoder encoder(vm, fileHandle, Encoder::NumberStrings::Yes, externalStrings);
    if (classInfo == UnlinkedProgramCodeBlock::info())
        encodeCodeBlock<UnlinkedProgramCodeBlock>(encoder, key, codeBlock);
    else if (classInfo == UnlinkedModuleProgramCodeBlock::info())
        encodeCodeBlock<UnlinkedModuleProgramCodeBlock>(encoder, key, codeBlock);
    else
        ASSERT(classInfo == UnlinkedEvalCodeBlock::info());
    encoder.encodeDeferred();

    return encoder.release(error);
}

RefPtr<CachedBytecode> encodeCodeBlock(VM& vm, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock, EncoderStringTable* externalStrings)
{
    BytecodeCacheError error;
    FileSystem::FileHandle invalidFileHandle;
    return encodeCodeBlock(vm, key, codeBlock, invalidFileHandle, error, externalStrings);
}

RefPtr<CachedBytecode> encodeFunctionCodeBlock(VM& vm, const UnlinkedFunctionCodeBlock* codeBlock, BytecodeCacheError& error)
{
    FileSystem::FileHandle invalidFileHandle;
    Encoder encoder(vm, invalidFileHandle, Encoder::NumberStrings::No);
    ptrdiff_t rootOffset = encoder.offsetOf(CachedFunctionCodeBlock::create(encoder, *codeBlock));
    encoder.encodeDeferred();
    RefPtr<CachedBytecode> result = encoder.release(error);
    if (result)
        result->setRootOffset(rootOffset);
    return result;
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
