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
#include <wtf/Deque.h>
#include <wtf/FileHandle.h>
#include <wtf/Function.h>
#include <wtf/MallocSpan.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UUID.h>
#include <wtf/text/AtomStringImpl.h>
#include <array>
#include <bit>
#if CPU(X86_64)
#include <cpuid.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// The bytecode cache format.
//
// A payload is a byte string. Every value in it is written by one of the primitives below and read back by the matching
// one; nothing is ever the memory image of a C++ object, so the format does not depend on the compiler, ABI, or CPU that
// produced it (Bun embeds payloads in cross-compiled executables), and every byte is a function of what was encoded.
//
//   u8 u16 u32 u64    fixed width, little-endian
//   f64               the IEEE-754 bits as a u64
//   varuint / varint  LEB128, at most 5 bytes; varint is zigzag
//   bytes             a length given elsewhere, then that many bytes
//
// Composite values are those primitives in a stated order (each Cached* below is that statement for one type). A
// reference to something elsewhere in the payload is its absolute offset: a varuint when the target was written first
// (string records, shared environments), a u32 written as 0 and patched when the target is written later (a function's
// code blocks, a block's expression info, a code block's child executable records). Offset 0 is the entry header, so 0
// doubles as "none".
//
// Nothing is aligned except the three arrays a decoder may use in place out of a mapped payload instead of copying
// (instruction bytes need nothing; metadata steps and expression-info words are 4-aligned), and those are read in place
// only on a little-endian host, which is asserted.
//
// Layout is the one from before: a code block is a region -- its arrays, then a record with a varint tail saying where
// each array is and holding every count and flag, then its children's executable records; bodies follow breadth-first;
// expression info goes last. Decoding one function reads one contiguous run of a mapped payload.

static_assert(std::endian::native == std::endian::little, "arrays borrowed from a mapped payload are read in place");
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
#if USE(BIGINT32)
#error "a BigInt32 constant would be written as an immediate that a build without them cannot decode"
#endif

// -- Primitives -------------------------------------------------------------------------------------------------------

class Writer {
public:
    void u8(uint8_t v) { m_bytes.append(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u16(uint16_t v)
    {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            u8(static_cast<uint8_t>(v >> shift));
    }
    void u64(uint64_t v)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
            u8(static_cast<uint8_t>(v >> shift));
    }
    void f64(double v) { u64(std::bit_cast<uint64_t>(v)); }
    void varuint(uint32_t v)
    {
        while (v >= 0x80) {
            u8(static_cast<uint8_t>(v) | 0x80);
            v >>= 7;
        }
        u8(static_cast<uint8_t>(v));
    }
    void varint(int32_t v) { varuint((static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31)); }
    void bytes(std::span<const uint8_t> b) { m_bytes.append(b); }
    void zeros(size_t n) { m_bytes.grow(m_bytes.size() + n); memset(m_bytes.mutableSpan().data() + m_bytes.size() - n, 0, n); }

    // A u32 written now as a placeholder and patched once its value is known.
    size_t reserveU32()
    {
        size_t at = size();
        u32(0);
        return at;
    }
    void patchU32(size_t at, uint32_t v)
    {
        uint8_t* p = m_bytes.mutableSpan().data() + at;
        for (unsigned i = 0; i < 4; ++i)
            p[i] = static_cast<uint8_t>(v >> (8 * i));
    }

    size_t size() const { return m_bytes.size(); }
    std::span<const uint8_t> span() const LIFETIME_BOUND { return m_bytes.span(); }
    void clear() { m_bytes.shrink(0); }

private:
    Vector<uint8_t, 128> m_bytes;
};

// Reads never leave [begin, end): past the end every read yields 0 and overran() is set, and whoever is decoding treats
// the record as damaged (the function is generated from source instead).
class Reader {
public:
    Reader(std::span<const uint8_t> payload, size_t offset)
        : m_begin(payload.data())
        , m_p(payload.data() + std::min(offset, payload.size()))
        , m_end(payload.data() + payload.size())
        , m_overran(offset > payload.size())
    {
    }
    uint8_t u8()
    {
        if (m_p >= m_end) [[unlikely]] {
            m_overran = true;
            return 0;
        }
        return *m_p++;
    }
    bool boolean() { return u8(); }
    uint16_t u16()
    {
        uint16_t v = u8();
        return v | static_cast<uint16_t>(u8()) << 8;
    }
    uint32_t u32()
    {
        if (!has(4)) [[unlikely]] {
            m_overran = true;
            m_p = m_end;
            return 0;
        }
        uint32_t v = static_cast<uint32_t>(m_p[0]) | static_cast<uint32_t>(m_p[1]) << 8 | static_cast<uint32_t>(m_p[2]) << 16 | static_cast<uint32_t>(m_p[3]) << 24;
        m_p += 4;
        return v;
    }
    uint64_t u64()
    {
        uint64_t low = u32();
        return low | static_cast<uint64_t>(u32()) << 32;
    }
    double f64() { return std::bit_cast<double>(u64()); }
    uint32_t varuint()
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
    int32_t varint()
    {
        uint32_t v = varuint();
        return static_cast<int32_t>((v >> 1) ^ -(v & 1));
    }
    // `n` bytes in place; empty on overrun.
    std::span<const uint8_t> bytes(size_t n)
    {
        if (!has(n)) [[unlikely]] {
            m_overran = true;
            m_p = m_end;
            return { };
        }
        auto result = std::span { m_p, n };
        m_p += n;
        return result;
    }
    void skip(size_t n) { bytes(n); }
    void alignTo(size_t alignment, const uint8_t* base) { skip((alignment - (m_p - base) % alignment) % alignment); }

    bool has(size_t n) const { return static_cast<size_t>(m_end - m_p) >= n; }
    // A count read from the stream is at most the bytes left (every element takes one or more); a larger one is damage.
    bool checkCount(size_t count)
    {
        if (has(count)) [[likely]]
            return true;
        m_overran = true;
        m_p = m_end;
        return false;
    }
    bool overran() const { return m_overran; }
    // A value this stream refers to (a string record, a shared object) did not decode: the stream is as damaged as if it had run out.
    void setOverran() { m_overran = true; }
    const uint8_t* position() const { return m_p; }
    size_t offset() const { return m_p - m_begin; }
    void seek(size_t offset)
    {
        if (offset > static_cast<size_t>(m_end - m_begin)) {
            m_overran = true;
            m_p = m_end;
        } else
            m_p = m_begin + offset;
    }

private:
    const uint8_t* m_begin;
    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_overran { false };
};

static uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

static void writeU32(uint8_t* p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// -- Checksums --------------------------------------------------------------------------------------------------------

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

// CRC-32C of `bytes` with the 4 bytes at `hole` (where the checksum itself is stored) read as zero.
static uint32_t checksumWithHole(std::span<const uint8_t> bytes, size_t hole)
{
    static const uint8_t zeros[4] = { };
    uint32_t crc = ~0u;
    crc = crc32c(crc, bytes.first(hole));
    crc = crc32c(crc, std::span { zeros, 4 });
    crc = crc32c(crc, bytes.subspan(hole + 4));
    return ~crc;
}

uint32_t bytecodeCacheRecordChecksum(std::span<const uint8_t> record, size_t checksumOffset)
{
    return checksumWithHole(record, checksumOffset);
}

// -- The embedder's shared string table ------------------------------------------------------------------------------

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
    uint8_t* base = out.mutableSpan().data();
    std::memset(base, 0, out.size());
    writeU32(base, count);
    size_t offset = header;
    for (uint32_t i = 0; i < count; ++i) {
        writeU32(base + sizeof(uint32_t) * (1 + i), static_cast<uint32_t>(offset));
        const StringImpl& s = m_strings[i].get();
        uint8_t* record = base + offset;
        writeU32(record, s.length() | (s.is8Bit() ? 1u << 31 : 0));
        writeU32(record + 4, s.hash());
        if (s.is8Bit())
            std::memcpy(record + 8, s.span8().data(), s.length());
        else {
            for (unsigned c = 0; c < s.length(); ++c) {
                record[8 + 2 * c] = static_cast<uint8_t>(s.span16()[c]);
                record[8 + 2 * c + 1] = static_cast<uint8_t>(s.span16()[c] >> 8);
            }
        }
        offset += roundUpToMultipleOf<4>(2 * sizeof(uint32_t) + s.length() * (s.is8Bit() ? sizeof(Latin1Character) : sizeof(char16_t)));
    }
    ASSERT(offset == out.size());
    return out;
}

DecoderStringTable::DecoderStringTable(std::span<const uint8_t> bytes)
    : m_bytes(bytes)
{
    RELEASE_ASSERT(bytes.size() >= sizeof(uint32_t));
    m_count = readU32(bytes.data());
    RELEASE_ASSERT(m_count <= (bytes.size() - sizeof(uint32_t)) / sizeof(uint32_t), m_count, bytes.size());
    if (m_count) {
        m_stringsReservation = roundUpToMultipleOf(WTF::pageSize(), static_cast<size_t>(m_count) * sizeof(StringImpl*));
        m_strings = static_cast<StringImpl**>(OSAllocator::reserveAndCommit(m_stringsReservation, OSAllocator::FastMallocPages));
    }
}

DecoderStringTable::~DecoderStringTable()
{
    // One per VM: a Worker that exits must give back the references it took on its thread's atoms.
    for (uint32_t i = 0; i < m_count; ++i) {
        if (m_strings[i])
            m_strings[i]->deref();
    }
    if (m_strings)
        OSAllocator::decommitAndRelease(m_strings, m_stringsReservation);
}

// The blob comes from an executable users sometimes edit; never read outside it.
DecoderStringTable::Record DecoderStringTable::record(uint32_t ordinal) const
{
    RELEASE_ASSERT(ordinal < m_count);
    size_t offset = readU32(m_bytes.data() + sizeof(uint32_t) * (1 + ordinal));
    RELEASE_ASSERT(!(offset % 4) && offset <= m_bytes.size() && m_bytes.size() - offset >= 2 * sizeof(uint32_t), offset, m_bytes.size());
    const uint8_t* header = m_bytes.data() + offset;
    Record result;
    uint32_t lengthAndWidth = readU32(header);
    result.length = lengthAndWidth & 0x7fffffffu;
    result.is8Bit = lengthAndWidth >> 31;
    result.hash = readU32(header + 4);
    result.characters = header + 8;
    size_t byteLength = static_cast<size_t>(result.length) * (result.is8Bit ? sizeof(Latin1Character) : sizeof(char16_t));
    RELEASE_ASSERT(byteLength <= m_bytes.size() - offset - 2 * sizeof(uint32_t), ordinal, result.length, m_bytes.size());
    return result;
}

// Same threshold as CachedString::minimumLengthToAliasPayload: long strings alias the (persistent) blob.
static constexpr unsigned minimumLengthToAliasPayload = 48; // below this a copy is smaller than pinning part of a page

template<typename CharacterType>
static Ref<AtomStringImpl> atomize(std::span<const CharacterType> characters, uint32_t hash)
{
    if (characters.size() >= minimumLengthToAliasPayload)
        return AtomStringImpl::add(RefPtr<StringImpl> { StringImpl::createWithoutCopying(characters) }).releaseNonNull();
    WTF::HashTranslatorCharBuffer<CharacterType> hashed { characters, hash };
    return AtomStringImpl::add(hashed).releaseNonNull();
}

// A slot holds the one StringImpl this VM uses for that string: an atom once an identifier has asked for it, or the
// plain StringImpl a string constant made first (which atomFor then promotes or replaces).
Ref<AtomStringImpl> DecoderStringTable::atomFor(uint32_t ordinal)
{
    RELEASE_ASSERT(ordinal < m_count);
    StringImpl*& slot = m_strings[ordinal];
    if (slot) [[likely]] {
        if (slot->isAtom()) [[likely]]
            return *static_cast<AtomStringImpl*>(slot);
        Ref<AtomStringImpl> atom = AtomStringImpl::add(slot).releaseNonNull(); // makes `slot` the atom unless one already exists
        if (atom.ptr() != slot) {
            atom->ref();
            std::exchange(slot, atom.ptr())->deref();
        }
        return atom;
    }
    Record r = record(ordinal);
    // 16-bit characters are read in place only where the blob happens to be 2-aligned there; the encoder 4-aligns records.
    Ref<AtomStringImpl> atom = r.is8Bit
        ? atomize(std::span { std::bit_cast<const Latin1Character*>(r.characters), r.length }, r.hash)
        : atomize(std::span { std::bit_cast<const char16_t*>(r.characters), r.length }, r.hash);
    atom->ref();
    slot = atom.ptr();
    return atom;
}

String DecoderStringTable::plainStringFor(uint32_t ordinal)
{
    RELEASE_ASSERT(ordinal < m_count);
    StringImpl*& slot = m_strings[ordinal];
    if (slot)
        return String { slot };
    Record r = record(ordinal);
    RefPtr<StringImpl> string;
    if (r.is8Bit) {
        std::span<const Latin1Character> chars { std::bit_cast<const Latin1Character*>(r.characters), r.length };
        string = r.length >= minimumLengthToAliasPayload ? StringImpl::createWithoutCopying(chars) : StringImpl::create(chars);
    } else {
        std::span<const char16_t> chars { std::bit_cast<const char16_t*>(r.characters), r.length };
        string = r.length >= minimumLengthToAliasPayload ? StringImpl::createWithoutCopying(chars) : StringImpl::create(chars);
    }
    string->ref();
    slot = string.get();
    return String { WTF::move(string) };
}

// -- Encoder ----------------------------------------------------------------------------------------------------------

// The payload being written: an append-only byte string plus the bookkeeping that lets a value written once be referred
// to again (strings by contents, environments by identity, byte-identical arrays), the two deferred passes that give the
// payload its locality (function bodies breadth-first, expression info last), and the checksums sealed at the end.
class Encoder {
    WTF_MAKE_NONCOPYABLE(Encoder);
    WTF_FORBID_HEAP_ALLOCATION;

public:
    // A payload that gets appended to another one (CachedBytecode::addFunctionUpdate) is read by the same Decoder as its
    // base: its offsets are absolute in the combined payload (baseOffset), and it leaves its strings unnumbered rather
    // than collide with numbers the base already handed out.
    enum class NumberStrings : bool { No, Yes };
    Encoder(VM& vm, FileSystem::FileHandle& fileHandle, NumberStrings numberStrings = NumberStrings::Yes, EncoderStringTable* externalStrings = nullptr, BytecodeCacheChecksums checksums = BytecodeCacheChecksums::Yes, BytecodeCacheUpdatable updatable = BytecodeCacheUpdatable::Yes, size_t baseOffset = 0)
        : m_vm(vm)
        , m_fileHandle(fileHandle)
        , m_baseOffset(baseOffset)
        , m_externalStrings(externalStrings)
        , m_numberStrings(numberStrings == NumberStrings::Yes)
        , m_updatable(updatable == BytecodeCacheUpdatable::Yes)
        , m_checksums(m_updatable || checksums == BytecodeCacheChecksums::Yes)
    {
    }

    VM& vm() { return m_vm; }
    EncoderStringTable* NODELETE externalStrings() { return m_externalStrings; }
    bool updatable() const { return m_updatable; }
    bool checksums() const { return m_checksums; }

    // Absolute offset of the next byte appended.
    uint32_t position() const { return safeCast<uint32_t>(m_baseOffset + m_out.size()); }

    uint32_t append(const Writer& writer)
    {
        uint32_t at = position();
        m_out.append(writer.span());
        return at;
    }
    uint32_t appendBytes(std::span<const uint8_t> bytes, size_t alignment = 1)
    {
        alignTo(alignment);
        uint32_t at = position();
        m_out.append(bytes);
        return at;
    }
    uint32_t appendU32Placeholders(unsigned count)
    {
        uint32_t at = position();
        m_out.grow(m_out.size() + 4 * static_cast<size_t>(count));
        memset(m_out.mutableSpan().data() + (at - m_baseOffset), 0, 4 * static_cast<size_t>(count));
        return at;
    }
    void alignTo(size_t alignment)
    {
        while (position() % alignment)
            m_out.append(0);
    }
    void patchU32(uint32_t at, uint32_t value) { writeU32(m_out.mutableSpan().data() + (at - m_baseOffset), value); }
    std::span<const uint8_t> bytesAt(uint32_t at, size_t size) const LIFETIME_BOUND { return m_out.span().subspan(at - m_baseOffset, size); }

    // Non-symbol strings decode to AtomStringImpl::add(characters), so two strings with the same characters decode to
    // the same atom: the record is written once and every user refers to it.
    std::optional<uint32_t> existingString(const StringImpl& string) const
    {
        auto it = m_stringsByContents.find(String(const_cast<StringImpl*>(&string)));
        return it == m_stringsByContents.end() ? std::nullopt : std::optional { it->value };
    }
    void rememberString(const StringImpl& string, uint32_t at) { m_stringsByContents.add(String(const_cast<StringImpl*>(&string)), at); }
    // Distinct strings are numbered in encode order; the decoder keeps the atom for each number it has seen, so only the
    // first block to name a string goes through the atom table.
    static constexpr uint32_t noOrdinal = std::numeric_limits<uint32_t>::max();
    uint32_t nextStringOrdinal() { return m_numberStrings ? m_nextStringOrdinal++ : noOrdinal; }

    // An object several encoded values point at (a TDZ environment shared by sibling functions) is written once; the
    // decoder likewise decodes it once, so what was one object is one object again.
    std::optional<uint32_t> existingObject(const void* object) const
    {
        auto it = m_objects.find(object);
        return it == m_objects.end() ? std::nullopt : std::optional { it->value };
    }
    void rememberObject(const void* object, uint32_t at) { m_objects.add(object, at); }

    // Byte-identical immutable runs (instruction streams, expression info, jump tables of small functions repeat a lot)
    // are stored once; later occurrences point at the first. Decoded objects are per code block either way.
    static unsigned hashBytes(std::span<const uint8_t> bytes) { return StringHasher::computeHashAndMaskTop8Bits(bytes) ^ static_cast<unsigned>(bytes.size()); }
    std::optional<uint32_t> existingBytes(std::span<const uint8_t> bytes, unsigned hash, size_t alignment = 1) const
    {
        auto it = m_bytesByHash.find(hash);
        if (it == m_bytesByHash.end())
            return std::nullopt;
        for (auto [candidate, size] : it->value) {
            if (size == bytes.size() && !(candidate % alignment) && equalSpans(bytesAt(candidate, size), bytes))
                return candidate;
        }
        return std::nullopt;
    }
    void rememberBytes(unsigned hash, uint32_t at, size_t size) { m_bytesByHash.add(hash, Vector<std::pair<uint32_t, size_t>, 1> { }).iterator->value.append({ at, size }); }
    uint32_t appendBytesOnce(std::span<const uint8_t> bytes, size_t alignment = 1)
    {
        unsigned hash = hashBytes(bytes);
        if (auto existing = existingBytes(bytes, hash, alignment))
            return *existing;
        uint32_t at = appendBytes(bytes, alignment);
        rememberBytes(hash, at, bytes.size());
        return at;
    }

    // A code block's arrays may be shared with an identical array an earlier block wrote; one that lies outside the
    // block's own bytes is folded into the block's checksum so it is verified by whoever reads it (see regionIsIntact).
    void beginBlockRegion(uint32_t start) { m_blockRegionStart = start; m_blockExternalArrays.clear(); }
    uint32_t appendBlockArray(std::span<const uint8_t> bytes, size_t alignment = 1)
    {
        unsigned hash = hashBytes(bytes);
        if (auto existing = existingBytes(bytes, hash, alignment)) {
            if (*existing < m_blockRegionStart)
                m_blockExternalArrays.append({ *existing, bytes.size() });
            return *existing;
        }
        uint32_t at = appendBytes(bytes, alignment);
        rememberBytes(hash, at, bytes.size());
        return at;
    }
    Vector<std::pair<uint32_t, size_t>> takeBlockExternalArrays() { return std::exchange(m_blockExternalArrays, { }); }

    // Layout: a code block's own arrays and its children's executable records are written contiguously; the children's
    // bodies follow breadth-first, and data that is only read on rare paths (expression info) goes after every body.
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
        // Slots inside a checksummed range (a block's expression info offset, its children's records) are filled by the
        // deferred work above, so the checksums are computed only now that every byte is final.
        for (auto& pending : m_pendingChecksums) {
            uint32_t crc = ~checksumWithHole(bytesAt(pending.start, pending.size), pending.checksumAt - pending.start);
            for (auto [at, size] : pending.externalArrays)
                crc = crc32c(crc, bytesAt(at, size));
            patchU32(pending.checksumAt, ~crc);
        }
        m_pendingChecksums.clear();
    }
    void addChecksum(uint32_t start, uint32_t size, uint32_t checksumAt, Vector<std::pair<uint32_t, size_t>>&& externalArrays = { }) { m_pendingChecksums.append({ start, size, checksumAt, WTF::move(externalArrays) }); }

    // CachedBytecode keeps these relative to the payload they are in and rebases them when payloads are combined.
    void addLeafExecutable(const UnlinkedFunctionExecutable* executable, uint32_t at) { m_leafExecutables.add(executable, at - m_baseOffset); }

    RefPtr<CachedBytecode> release(BytecodeCacheError& error)
    {
        if (m_fileHandle)
            return releaseMapped(error);
        auto buffer = MallocSpan<uint8_t, VMMalloc>::malloc(m_out.size());
        memcpySpan(buffer.mutableSpan(), m_out.span());
        return CachedBytecode::create(WTF::move(buffer), WTF::move(m_leafExecutables));
    }

private:
    RefPtr<CachedBytecode> releaseMapped(BytecodeCacheError& error)
    {
        if (!m_fileHandle.truncate(m_out.size())) {
            error = BytecodeCacheError::StandardError(errno);
            return nullptr;
        }
        auto bytesWritten = m_fileHandle.write(m_out.span());
        if (!bytesWritten) {
            error = BytecodeCacheError::StandardError(errno);
            return nullptr;
        }
        if (*bytesWritten != m_out.size()) {
            error = BytecodeCacheError::WriteError(*bytesWritten, m_out.size());
            return nullptr;
        }
        auto mappedFileData = m_fileHandle.map(FileSystem::MappedFileMode::Private);
        if (!mappedFileData) {
            error = BytecodeCacheError::StandardError(errno);
            return nullptr;
        }
        return CachedBytecode::create(WTF::move(*mappedFileData), WTF::move(m_leafExecutables));
    }

    VM& m_vm;
    FileSystem::FileHandle& m_fileHandle;
    size_t m_baseOffset;
    Vector<uint8_t> m_out;
    EncoderStringTable* m_externalStrings;
    bool m_numberStrings;
    bool m_updatable;
    bool m_checksums;
    uint32_t m_nextStringOrdinal { 0 };
    HashMap<String, uint32_t> m_stringsByContents; // keyed by contents (StringHash), not identity
    UncheckedKeyHashMap<const void*, uint32_t> m_objects;
    UncheckedKeyHashMap<unsigned, Vector<std::pair<uint32_t, size_t>, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_bytesByHash;
    uint32_t m_blockRegionStart { 0 };
    Vector<std::pair<uint32_t, size_t>> m_blockExternalArrays;
    Deque<Function<void()>> m_bodies;
    Deque<Function<void()>> m_cold;
    struct PendingChecksum { uint32_t start; uint32_t size; uint32_t checksumAt; Vector<std::pair<uint32_t, size_t>> externalArrays; };
    Vector<PendingChecksum> m_pendingChecksums;
    LeafExecutableMap m_leafExecutables;
};

// -- Decoder ----------------------------------------------------------------------------------------------------------

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

std::span<const uint8_t> Decoder::payload() const
{
    return m_cachedBytecode->span();
}

RefPtr<SourceProvider> Decoder::provider() const
{
    return m_provider;
}

bool Decoder::canBorrowPayload() const
{
#if USE(BUN_JSC_ADDITIONS)
    return Options::useBorrowedBytecodeFromCache() && m_cachedBytecode->payloadIsPersistent();
#else
    return false;
#endif
}

bool Decoder::verifiesChecksums() const
{
#if USE(BUN_JSC_ADDITIONS)
    // A persistent payload is a section of the executable itself: corruption there means the program is already broken, and code signing already covers it. Checksums guard separate on-disk cache files.
    return !m_cachedBytecode->payloadIsPersistent() && Options::verifyBytecodeCacheChecksums();
#else
    return true;
#endif
}

bool Decoder::payloadContains(size_t offset, size_t size) const
{
    size_t payloadSize = payload().size();
    return offset <= payloadSize && size <= payloadSize - offset;
}

bool Decoder::checksumMatches(size_t start, size_t size, size_t checksumAt, std::span<const std::pair<size_t, size_t>> externalArrays) const
{
    if (!verifiesChecksums())
        return true;
    if (!payloadContains(start, size) || checksumAt < start || checksumAt + 4 > start + size)
        return false; // includes a stored size too small to cover the record that holds the checksum
    auto bytes = payload().subspan(start, size);
    uint32_t crc = ~checksumWithHole(bytes, checksumAt - start);
    for (auto [at, arraySize] : externalArrays) {
        if (!payloadContains(at, arraySize))
            return false;
        crc = crc32c(crc, payload().subspan(at, arraySize));
    }
    if (~crc == readU32(payload().data() + checksumAt))
        return true;
    dataLogLnIf(Options::verboseDiskCache(), "[Disk Cache] checksum mismatch; regenerating from source");
    return false;
}

AtomStringImpl* Decoder::atomForOrdinal(uint32_t ordinal) const
{
    return ordinal < m_atomsByOrdinal.size() ? m_atomsByOrdinal[ordinal] : nullptr;
}

void Decoder::setAtomForOrdinal(uint32_t ordinal, AtomStringImpl& atom)
{
    if (ordinal >= m_atomsByOrdinal.size()) {
        // Every numbered string is a record of several bytes, so this is bounded by the payload.
        if (ordinal >= m_cachedBytecode->size())
            return;
        size_t oldSize = m_atomsByOrdinal.size();
        m_atomsByOrdinal.grow(std::max<size_t>(ordinal + 1, oldSize * 2));
        std::fill(m_atomsByOrdinal.begin() + oldSize, m_atomsByOrdinal.end(), nullptr); // Vector::grow leaves pointers uninitialized
    }
    if (m_atomsByOrdinal[ordinal])
        return;
    atom.ref();
    m_atomsByOrdinal[ordinal] = &atom;
}

// 1- and 2-character strings are the bulk of minified identifiers: length 1 is SmallStrings' single-character reps; length 2 hits one lazy 65536-entry table on the VM (shared by every Decoder — one 512 KB slab, not one per retained Decoder); length 3 goes to the atom table each time.
Ref<AtomStringImpl> Decoder::atomForInlineString(std::span<const Latin1Character> characters)
{
    if (characters.size() == 1)
        return m_vm.smallStrings.singleCharacterStringRep(characters[0]);
    if (characters.size() == 2) {
        if (!m_twoCharacterAtoms) [[unlikely]]
            m_twoCharacterAtoms = m_vm.ensureCachedBytecodeTwoCharacterAtoms();
        AtomStringImpl*& slot = m_twoCharacterAtoms[characters[0] | characters[1] << 8];
        if (slot) [[likely]]
            return *slot;
        Ref<AtomStringImpl> atom = AtomStringImpl::add(characters).releaseNonNull();
        atom->ref();
        slot = atom.ptr();
        return atom;
    }
    return AtomStringImpl::add(characters).releaseNonNull();
}

ALWAYS_INLINE DecoderStringTable* Decoder::externalStrings()
{
    if (!m_externalStrings) [[unlikely]] {
        m_externalStrings = m_vm.clientData ? m_vm.clientData->decoderStringTable() : nullptr;
        if (!m_externalStrings)
            dataLogLnIf(Options::verboseDiskCache(), "[Disk Cache] payload names an external string but the embedder provided no table");
    }
    return m_externalStrings;
}

RefPtr<AtomStringImpl> Decoder::atomForExternalString(uint32_t ordinal)
{
    auto* table = externalStrings();
    return table && ordinal < table->size() ? RefPtr { table->atomFor(ordinal) } : nullptr;
}

String Decoder::plainStringForExternalString(uint32_t ordinal)
{
    auto* table = externalStrings();
    return table && ordinal < table->size() ? table->plainStringFor(ordinal) : String();
}

std::optional<std::pair<void*, const void*>> Decoder::sharedObjectAt(uint32_t offset) const
{
    auto it = m_sharedObjects.find(offset);
    return it == m_sharedObjects.end() ? std::nullopt : std::optional { it->value };
}

void Decoder::setSharedObjectAt(uint32_t offset, void* object, const void* type)
{
    m_sharedObjects.add(offset, std::pair { object, type });
}

std::optional<CompactTDZEnvironmentMap::Handle> Decoder::handleForTDZEnvironment(CompactTDZEnvironment* environment) const
{
    auto it = m_environmentToHandleMap.find(environment);
    if (it == m_environmentToHandleMap.end())
        return std::nullopt;
    return it->value;
}

void Decoder::setHandleForTDZEnvironment(CompactTDZEnvironment* environment, const CompactTDZEnvironmentMap::Handle& handle)
{
    auto addResult = m_environmentToHandleMap.add(environment, handle);
    RELEASE_ASSERT(addResult.isNewEntry);
}

void Decoder::addLeafExecutable(const UnlinkedFunctionExecutable* executable, uint32_t offset)
{
#if USE(BUN_JSC_ADDITIONS)
    // Only CachedBytecode::addFunctionUpdate reads this map, and Bun never calls it.
    if (Options::useLeanBytecodeCacheDecoder())
        return;
#endif
    m_cachedBytecode->leafExecutables().add(executable, offset);
}

void Decoder::addFinalizer(Function<void()>&& finalizer)
{
    m_finalizers.append(WTF::move(finalizer));
}

// -- Ordering ---------------------------------------------------------------------------------------------------------

// Hash tables iterate in an order that depends on where their keys hashed to -- for SymbolImpl keys a per-process
// counter, for robin-hood tables the table's own address -- so their entries are written in key order. Keys are
// ordered by contents, then by what kind of StringImpl they decode to; two keys equal in both would decode to the same
// StringImpl and cannot share a table.
struct EncodingOrder {
    static std::strong_ordering compare(unsigned a, unsigned b) { return a <=> b; }
    static unsigned kind(const StringImpl* string)
    {
        if (!string->isSymbol())
            return 0;
        auto& symbol = *static_cast<const SymbolImpl*>(string);
        return 1 + symbol.isRegistered() * 2 + symbol.isPrivate();
    }
    static std::strong_ordering compare(const StringImpl* a, const StringImpl* b)
    {
        if (auto order = codePointCompare(StringView(*a), StringView(*b)); order != 0)
            return order;
        return kind(a) <=> kind(b);
    }
    template<typename T, typename Traits> static std::strong_ordering compare(const RefPtr<T, Traits>& a, const RefPtr<T, Traits>& b) { return compare(a.get(), b.get()); }

    template<typename Entries, typename KeyOf>
    static void sort(Entries& entries, const KeyOf& keyOf)
    {
        std::stable_sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
            return compare(keyOf(a), keyOf(b)) < 0;
        });
    }
    template<typename Map>
    static Vector<const typename Map::KeyValuePairType*> sortedEntries(const Map& map)
    {
        Vector<const typename Map::KeyValuePairType*> entries;
        entries.reserveInitialCapacity(map.size());
        for (auto& entry : map)
            entries.append(&entry);
        sort(entries, [](auto* entry) -> const auto& { return entry->key; });
        return entries;
    }
};

// -- Strings ----------------------------------------------------------------------------------------------------------

// StringRef := varuint head, then by its low two bits:
//   head == 0   null
//   tag 1       inline: length = head >> 2 (1 to 3), then that many Latin-1 characters. Minified code is mostly these.
//   tag 2       external: ordinal = head >> 2, into the embedder's shared DecoderStringTable
//   tag 3       record: absolute offset = head >> 2 of a string record written earlier in this payload
//
// String record := u8 flags, varuint length, u32 hash, varuint ordinal + 1 (0 = unnumbered), characters
//   characters are `length` bytes when Is8Bit, else `length` u16. Well-known symbols are stored by their description
//   minus "Symbol."; Latin-1 contents are stored 8-bit even if this process's atom for them happens to be 16-bit (an
//   equal 16-bit string was atomized first), since that is not a property of the source.
class CachedString {
public:
    enum Tag : uint32_t { Inline = 1, External = 2, Record = 3 };
    enum Flag : uint8_t {
        Is8Bit = 1 << 0,
        IsSymbol = 1 << 1,
        IsRegistered = 1 << 2,
        IsPrivate = 1 << 3,
        IsWellKnownSymbol = 1 << 4,
    };

    static void encode(Writer& writer, Encoder& encoder, const StringImpl* string)
    {
        if (!string) {
            writer.varuint(0);
            return;
        }
        bool isSymbol = string->isSymbol();
        unsigned length = string->length();
        if (!isSymbol && length && length <= 3) {
            Latin1Character characters[3];
            bool allLatin1 = true;
            for (unsigned i = 0; i < length; ++i) {
                char16_t c = (*string)[i];
                allLatin1 &= isLatin1(c);
                characters[i] = static_cast<Latin1Character>(c);
            }
            if (allLatin1) {
                writer.varuint(Inline | length << 2);
                writer.bytes(std::span { characters, length });
                return;
            }
        }
        if (!isSymbol && length && encoder.externalStrings()) {
            uint32_t ordinal = encoder.externalStrings()->ordinalFor(*string);
            if (ordinal <= EncoderStringTable::maxOrdinal) [[likely]] {
                writer.varuint(External | ordinal << 2);
                return;
            }
        }
        // A symbol is one object however many times it is named; other strings are shared by contents.
        std::optional<uint32_t> at = isSymbol ? encoder.existingObject(string) : encoder.existingString(*string);
        if (!at) {
            at = writeRecord(encoder, *string);
            if (isSymbol)
                encoder.rememberObject(string, *at);
            else
                encoder.rememberString(*string, *at);
        }
        RELEASE_ASSERT(*at < 1u << 30);
        writer.varuint(Record | *at << 2);
    }
    static void encode(Writer& writer, Encoder& encoder, const String& string) { encode(writer, encoder, string.impl()); }
    static void encode(Writer& writer, Encoder& encoder, const Identifier& identifier) { encode(writer, encoder, identifier.impl()); }

    // A +1 reference; null only for a null StringRef (a damaged one marks the reader overran and yields the empty atom).
    static RefPtr<UniquedStringImpl> decode(Reader& reader, Decoder& decoder)
    {
        size_t referrerAt = reader.offset();
        uint32_t head = reader.varuint();
        if (!head)
            return nullptr;
        RefPtr<UniquedStringImpl> result;
        switch (head & 3) {
        case Inline:
            if (auto characters = reader.bytes(head >> 2 & 3); !characters.empty())
                result = uniqued(decoder.atomForInlineString(characters));
            break;
        case External:
            if (RefPtr<AtomStringImpl> atom = decoder.atomForExternalString(head >> 2))
                result = uniqued(atom.releaseNonNull());
            break;
        case Record:
            if (head >> 2 < referrerAt) { // records are written before their first use
                Reader record(decoder.payload(), head >> 2);
                result = decodeRecord(record, decoder);
                if (record.overran())
                    result = nullptr;
            }
            break;
        }
        if (!result) {
            reader.setOverran();
            result = static_cast<UniquedStringImpl*>(emptyAtom().impl());
        }
        return result;
    }
    // Where the format has no null (map keys, identifiers): a null StringRef there is damage.
    static RefPtr<UniquedStringImpl> decodeNonNull(Reader& reader, Decoder& decoder)
    {
        RefPtr<UniquedStringImpl> result = decode(reader, decoder);
        if (!result) {
            reader.setOverran();
            result = static_cast<UniquedStringImpl*>(emptyAtom().impl());
        }
        return result;
    }
    static Identifier decodeIdentifier(Reader& reader, Decoder& decoder)
    {
        RefPtr<UniquedStringImpl> string = decode(reader, decoder);
        if (!string)
            return Identifier();
        return Identifier::fromUid(decoder.vm(), string.get());
    }
    static String decodeString(Reader& reader, Decoder& decoder) { return RefPtr<StringImpl> { decode(reader, decoder) }; }

    // For uses that only need the characters (a string constant's JSString), not an atom: no atom table involved.
    static String decodePlainString(Reader& reader, Decoder& decoder)
    {
        size_t referrerAt = reader.offset();
        uint32_t head = reader.varuint();
        if (!head)
            return String();
        String result;
        switch (head & 3) {
        case Inline:
            if (auto characters = reader.bytes(head >> 2 & 3); !characters.empty())
                result = decoder.atomForInlineString(characters);
            break;
        case External:
            result = decoder.plainStringForExternalString(head >> 2);
            break;
        case Record:
            if (head >> 2 < referrerAt) {
                Reader record(decoder.payload(), head >> 2);
                result = decodePlainRecord(record, decoder);
                if (record.overran())
                    result = String();
            }
            break;
        }
        if (result.isNull()) {
            reader.setOverran();
            result = emptyString();
        }
        return result;
    }

private:
    static RefPtr<UniquedStringImpl> uniqued(Ref<AtomStringImpl>&& atom) { return adoptRef(static_cast<UniquedStringImpl*>(static_cast<StringImpl*>(&atom.leakRef()))); }

    static uint32_t writeRecord(Encoder& encoder, const StringImpl& string)
    {
        RefPtr<StringImpl> characters = const_cast<StringImpl*>(&string);
        uint8_t flags = 0;
        if (string.isSymbol()) {
            auto& symbol = static_cast<const SymbolImpl&>(string);
            flags |= IsSymbol;
            if (symbol.isRegistered())
                flags |= IsRegistered;
            if (symbol.isPrivate())
                flags |= IsPrivate;
            if (!symbol.isNullSymbol() && !symbol.isPrivate()) {
                flags |= IsWellKnownSymbol;
                characters = const_cast<SymbolImpl&>(symbol).substring(strlen("Symbol."));
            }
        }
        if (!characters->is8Bit())
            characters = StringImpl::create8BitIfPossible(characters->span16());
        if (characters->is8Bit())
            flags |= Is8Bit;

        Writer writer;
        writer.u8(flags);
        writer.varuint(characters->length());
        writer.u32(characters->hash()); // what StringImpl::hash() / the atom table use, so decode never rehashes
        writer.varuint((flags & IsSymbol) || !characters->length() ? 0 : encoder.nextStringOrdinal() + 1);
        if (flags & Is8Bit)
            writer.bytes(characters->span8());
        else {
            for (char16_t c : characters->span16())
                writer.u16(c);
        }
        return encoder.append(writer);
    }

    struct RecordHead {
        uint8_t flags;
        unsigned length;
        uint32_t hash;
        uint32_t ordinal; // Encoder::noOrdinal when unnumbered
    };
    static RecordHead readHead(Reader& reader)
    {
        RecordHead head;
        head.flags = reader.u8();
        head.length = reader.varuint();
        head.hash = reader.u32();
        uint32_t ordinal = reader.varuint();
        head.ordinal = ordinal ? ordinal - 1 : Encoder::noOrdinal;
        return head;
    }
    static Vector<char16_t, 32> read16(Reader& reader, unsigned length)
    {
        Vector<char16_t, 32> characters;
        characters.grow(length);
        for (unsigned i = 0; i < length; ++i)
            characters[i] = reader.u16();
        if (reader.overran())
            characters.shrink(0);
        return characters;
    }

    static RefPtr<UniquedStringImpl> decodeRecord(Reader& reader, Decoder& decoder)
    {
        RecordHead head = readHead(reader);
        if (head.ordinal != Encoder::noOrdinal) {
            if (AtomStringImpl* known = decoder.atomForOrdinal(head.ordinal); known && known->hash() == head.hash)
                return static_cast<UniquedStringImpl*>(static_cast<StringImpl*>(known));
        }
        if (!head.length) {
            if (head.flags & IsSymbol)
                return RefPtr<UniquedStringImpl> { SymbolImpl::createNullSymbol() };
            return static_cast<UniquedStringImpl*>(emptyAtom().impl());
        }
        auto create = [&](auto characters) -> RefPtr<UniquedStringImpl> {
            if (characters.empty())
                return nullptr; // overran
            if (!(head.flags & IsSymbol)) {
                RefPtr<AtomStringImpl> atom;
                // Long strings out of a persistent payload keep their characters in the mapping (clean, shared pages) and
                // only allocate the StringImpl header; AtomStringImpl::add adopts it in place unless the atom already exists.
                if constexpr (sizeof(characters[0]) == 1) {
                    if (characters.size() >= minimumLengthToAliasPayload && decoder.canBorrowPayload())
                        atom = AtomStringImpl::add(RefPtr<StringImpl> { StringImpl::createWithoutCopying(characters) });
                }
                if (!atom) {
                    WTF::HashTranslatorCharBuffer<std::remove_const_t<typename decltype(characters)::element_type>> hashed { characters, head.hash };
                    atom = AtomStringImpl::add(hashed);
                }
                if (head.ordinal != Encoder::noOrdinal)
                    decoder.setAtomForOrdinal(head.ordinal, *atom);
                return static_cast<UniquedStringImpl*>(static_cast<StringImpl*>(atom.get()));
            }
            VM& vm = decoder.vm();
            if (head.flags & IsRegistered) {
                String key(characters);
                if (head.flags & IsPrivate)
                    return RefPtr<UniquedStringImpl> { protect(vm.privateSymbolRegistry())->symbolForKey(key) };
                return RefPtr<UniquedStringImpl> { protect(vm.symbolRegistry())->symbolForKey(key) };
            }
            SymbolImpl* symbol = head.flags & IsWellKnownSymbol
                ? vm.propertyNames->builtinNames().lookUpWellKnownSymbol(characters)
                : vm.propertyNames->builtinNames().lookUpPrivateName(characters);
            return symbol; // null if this VM has no such symbol, which the caller treats as damage
        };
        if (head.flags & Is8Bit) {
            auto bytes = reader.bytes(head.length);
            return create(std::span { std::bit_cast<const Latin1Character*>(bytes.data()), bytes.size() });
        }
        auto characters = read16(reader, head.length);
        return create(std::span<const char16_t> { characters.span() });
    }

    static String decodePlainRecord(Reader& reader, Decoder& decoder)
    {
        Reader start = reader;
        RecordHead head = readHead(reader);
        if (head.flags & IsSymbol)
            return decodeRecord(start, decoder).get();
        if (!head.length)
            return emptyString();
        if (head.ordinal != Encoder::noOrdinal) {
            if (AtomStringImpl* known = decoder.atomForOrdinal(head.ordinal); known && known->hash() == head.hash)
                return known;
        }
        if (head.flags & Is8Bit) {
            auto bytes = reader.bytes(head.length);
            if (bytes.size() != head.length)
                return String();
            std::span characters { std::bit_cast<const Latin1Character*>(bytes.data()), bytes.size() };
            if (characters.size() >= minimumLengthToAliasPayload && decoder.canBorrowPayload())
                return StringImpl::createWithoutCopying(characters);
            return StringImpl::create(characters);
        }
        auto characters = read16(reader, head.length);
        if (reader.overran())
            return String();
        return StringImpl::create(characters.span());
    }
};

// A value several others point at, written once at its own offset and referred to by that offset; decoded once, so the
// sharing survives the round trip. SharedRef := varuint offset (0 = null).
template<typename T>
struct CachedShared {
    template<typename Source>
    static void encode(Writer& writer, Encoder& encoder, const Source* object)
    {
        if (!object) {
            writer.varuint(0);
            return;
        }
        std::optional<uint32_t> at = encoder.existingObject(object);
        if (!at) {
            Writer record;
            T::encode(record, encoder, *object);
            at = encoder.append(record);
            encoder.rememberObject(object, *at);
        }
        writer.varuint(*at);
    }

    // What T::decode returned for this offset the first time; T decides how that is owned.
    template<typename... Args>
    static auto decode(Reader& reader, Decoder& decoder, bool& isNew, Args&&... args) -> decltype(T::decode(reader, decoder, args...))
    {
        using Result = decltype(T::decode(reader, decoder, args...));
        isNew = false;
        size_t referrerAt = reader.offset();
        uint32_t at = reader.varuint();
        if (!at)
            return nullptr;
        // A shared value is written before anything that refers to it; this also keeps a damaged payload from sending us in circles.
        if (at >= referrerAt) {
            reader.setOverran();
            return nullptr;
        }
        static const char type = 0;
        if (auto existing = decoder.sharedObjectAt(at)) {
            if (existing->second == &type)
                return static_cast<Result>(existing->first);
            reader.setOverran(); // an offset that decoded as something else
            return nullptr;
        }
        Reader record(decoder.payload(), at);
        Result result = T::decode(record, decoder, std::forward<Args>(args)...);
        if (record.overran())
            reader.setOverran();
        if (result) {
            decoder.setSharedObjectAt(at, result, &type);
            isNew = true;
        }
        return result;
    }
};

// -- Environments ------------------------------------------------------------------------------------------------------

// VariableEnvironment := u8 flags, varuint count, count × { StringRef name, varuint entry bits }, [PrivateNameEnvironment]
class CachedVariableEnvironment {
public:
    enum Flag : uint8_t { IsEverythingCaptured = 1 << 0, HasAwaitUsingDeclaration = 1 << 1, HasPrivateNames = 1 << 2 };

    static void encode(Writer& writer, Encoder& encoder, const VariableEnvironment& environment)
    {
        uint8_t flags = 0;
        if (environment.m_isEverythingCaptured)
            flags |= IsEverythingCaptured;
        if (environment.m_hasAwaitUsingDeclaration)
            flags |= HasAwaitUsingDeclaration;
        if (environment.m_rareData)
            flags |= HasPrivateNames;
        writer.u8(flags);
        Vector<std::pair<const UniquedStringImpl*, uint16_t>> entries;
        entries.reserveInitialCapacity(environment.m_map.size());
        for (auto& entry : environment.m_map)
            entries.append({ entry.key.get(), entry.value.bits() });
        EncodingOrder::sort(entries, [](auto& entry) { return entry.first; });
        writer.varuint(entries.size());
        for (auto [name, bits] : entries) {
            CachedString::encode(writer, encoder, name);
            writer.varuint(bits);
        }
        if (environment.m_rareData)
            encodePrivateNames(writer, encoder, environment.m_rareData->m_privateNames);
    }

    static void decode(Reader& reader, Decoder& decoder, VariableEnvironment& environment)
    {
        uint8_t flags = reader.u8();
        environment.m_isEverythingCaptured = flags & IsEverythingCaptured;
        environment.m_hasAwaitUsingDeclaration = flags & HasAwaitUsingDeclaration;
        unsigned count = reader.varuint();
        if (!reader.checkCount(count))
            return;
        environment.m_map.reserveInitialCapacity(count);
        for (unsigned i = 0; i < count; ++i) {
            RefPtr<UniquedStringImpl> name = CachedString::decodeNonNull(reader, decoder);
            VariableEnvironmentEntry entry;
            entry.m_bits = reader.varuint();
            environment.m_map.add(WTF::move(name), entry);
        }
        if (flags & HasPrivateNames) {
            environment.m_rareData = WTF::makeUnique<VariableEnvironment::RareData>();
            decodePrivateNames(reader, decoder, environment.m_rareData->m_privateNames);
        }
    }

    // PrivateNameEnvironment := varuint offset of { varuint count, count × { StringRef name, varuint entry bits } }
    // Every function nested in a class with private names carries a copy of the class's environment, so the entries are
    // written once per distinct environment and each copy is a reference to them.
    static void encodePrivateNames(Writer& writer, Encoder& encoder, const PrivateNameEnvironment& environment)
    {
        Writer entries;
        auto sorted = EncodingOrder::sortedEntries(environment);
        entries.varuint(sorted.size());
        for (auto* entry : sorted) {
            CachedString::encode(entries, encoder, entry->key.get());
            entries.varuint(entry->value.bits());
        }
        writer.varuint(encoder.appendBytesOnce(entries.span()));
    }
    static void decodePrivateNames(Reader& reader, Decoder& decoder, PrivateNameEnvironment& environment)
    {
        size_t referrerAt = reader.offset();
        uint32_t at = reader.varuint();
        if (at >= referrerAt)
            return reader.setOverran();
        Reader entries(decoder.payload(), at);
        unsigned count = entries.varuint();
        if (entries.checkCount(count)) {
            for (unsigned i = 0; i < count; ++i) {
                RefPtr<UniquedStringImpl> name = CachedString::decodeNonNull(entries, decoder);
                PrivateNameEntry entry(entries.varuint());
                environment.add(WTF::move(name), entry);
            }
        }
        if (entries.overran())
            reader.setOverran();
    }
};

// CompactTDZEnvironment := varuint count, count × StringRef       (shared: one object per distinct environment)
class CachedCompactTDZEnvironment {
public:
    static void encode(Writer& writer, Encoder& encoder, const CompactTDZEnvironment& environment)
    {
        // A Compact is sorted by StringImpl address; decode() sorts again.
        CompactTDZEnvironment::Compact names;
        if (std::holds_alternative<CompactTDZEnvironment::Compact>(environment.m_variables))
            names = std::get<CompactTDZEnvironment::Compact>(environment.m_variables);
        else {
            for (auto& name : std::get<CompactTDZEnvironment::Inflated>(environment.m_variables))
                names.append(name);
        }
        EncodingOrder::sort(names, [](const auto& name) -> const auto& { return name; });
        writer.varuint(names.size());
        for (auto& name : names)
            CachedString::encode(writer, encoder, name.get());
    }

    static CompactTDZEnvironment* decode(Reader& reader, Decoder& decoder)
    {
        auto* environment = new CompactTDZEnvironment;
        CompactTDZEnvironment::Compact names;
        unsigned count = reader.varuint();
        if (reader.checkCount(count)) {
            names.reserveInitialCapacity(count);
            for (unsigned i = 0; i < count; ++i)
                names.append(CachedString::decodeNonNull(reader, decoder));
        }
        environment->m_hash = 0;
        for (auto& name : names)
            environment->m_hash ^= name->hash(); // as CompactTDZEnvironment's constructor computes it
        CompactTDZEnvironment::sortCompact(names);
        environment->m_variables = CompactTDZEnvironment::Variables(WTF::move(names));
        return environment;
    }
};

class CachedCompactTDZEnvironmentMapHandle {
public:
    static void encode(Writer& writer, Encoder& encoder, const CompactTDZEnvironmentMap::Handle& handle)
    {
        CachedShared<CachedCompactTDZEnvironment>::encode(writer, encoder, handle.m_environment);
    }

    static CompactTDZEnvironmentMap::Handle decode(Reader& reader, Decoder& decoder)
    {
        bool isNew;
        CompactTDZEnvironment* environment = CachedShared<CachedCompactTDZEnvironment>::decode(reader, decoder, isNew);
        if (!environment)
            return CompactTDZEnvironmentMap::Handle();
        if (!isNew) {
            if (auto handle = decoder.handleForTDZEnvironment(environment))
                return WTF::move(*handle);
            reader.setOverran();
            return CompactTDZEnvironmentMap::Handle();
        }
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
};

// TDZEnvironmentLink := SharedRef environment, SharedRef parent link      (shared)
class CachedTDZEnvironmentLink {
public:
    static void encode(Writer& writer, Encoder& encoder, const TDZEnvironmentLink& link)
    {
        CachedCompactTDZEnvironmentMapHandle::encode(writer, encoder, link.m_handle);
        encodeRef(writer, encoder, link.m_parent.get());
    }
    static void encodeRef(Writer& writer, Encoder& encoder, const TDZEnvironmentLink* link)
    {
        CachedShared<CachedTDZEnvironmentLink>::encode(writer, encoder, link);
    }

    static TDZEnvironmentLink* decode(Reader& reader, Decoder& decoder)
    {
        CompactTDZEnvironmentMap::Handle handle = CachedCompactTDZEnvironmentMapHandle::decode(reader, decoder);
        RefPtr<TDZEnvironmentLink> parent = decodeRef(reader, decoder);
        if (reader.overran())
            return nullptr;
        return new TDZEnvironmentLink(WTF::move(handle), WTF::move(parent));
    }
    static RefPtr<TDZEnvironmentLink> decodeRef(Reader& reader, Decoder& decoder)
    {
        bool isNew;
        TDZEnvironmentLink* link = CachedShared<CachedTDZEnvironmentLink>::decode(reader, decoder, isNew);
        if (isNew) {
            // The map's entry holds the reference decode() was born with until the decoder dies.
            decoder.addFinalizer([=] {
                link->deref();
            });
        }
        return link;
    }
};

// -- Symbol tables ----------------------------------------------------------------------------------------------------

// A slim SymbolTableEntry is a 32-bit raw VarOffset (zero-extended) above six flag bits. Offsets are far below 2^25 in
// magnitude, so the two fit one varint: the offset's low 26 bits, sign-extended, above the flags.
class CachedSymbolTableEntry {
    static constexpr intptr_t flagsMask = (intptr_t { 1 } << SymbolTableEntry::FlagBits) - 1;

public:
    static int32_t pack(const SymbolTableEntry& entry)
    {
        intptr_t bits = entry.bits() | SymbolTableEntry::SlimFlag;
        int32_t rawOffset = static_cast<int32_t>(bits >> SymbolTableEntry::FlagBits);
        RELEASE_ASSERT((rawOffset << SymbolTableEntry::FlagBits) >> SymbolTableEntry::FlagBits == rawOffset);
        return (rawOffset << SymbolTableEntry::FlagBits) | static_cast<int32_t>(bits & flagsMask);
    }
    static void unpack(SymbolTableEntry& entry, int32_t packed)
    {
        unsigned rawOffset = static_cast<unsigned>(packed >> SymbolTableEntry::FlagBits);
        entry.m_bits = (static_cast<intptr_t>(rawOffset) << SymbolTableEntry::FlagBits) | (packed & flagsMask) | SymbolTableEntry::SlimFlag;
    }
};

class CachedScopedArgumentsTable {
public:
    static void encode(Writer& writer, const ScopedArgumentsTable* arguments)
    {
        if (!arguments)
            return writer.varuint(0);
        writer.varuint(arguments->m_arguments.size() + 1);
        for (ScopeOffset offset : arguments->m_arguments)
            writer.varuint(offset.offsetUnchecked());
    }
    static ScopedArgumentsTable* decode(Reader& reader, VM& vm)
    {
        unsigned length = reader.varuint();
        if (!length--)
            return nullptr;
        if (!reader.checkCount(length))
            return nullptr;
        ScopedArgumentsTable* arguments = ScopedArgumentsTable::tryCreate(vm, length);
        RELEASE_ASSERT(arguments);
        for (unsigned i = 0; i < length; ++i)
            arguments->m_arguments[i] = ScopeOffset(reader.varuint());
        return arguments;
    }
};

// SymbolTable := varuint count, count × { StringRef name, varint entry }, varuint maxScopeOffset, u8 flags, u8 scopeType,
//                varuint argumentsLength + 1 (0 = none), argumentsLength × varuint scopeOffset, [PrivateNameEnvironment]
class CachedSymbolTable {
public:
    enum Flag : uint8_t { UsesSloppyEval = 1 << 0, NestedLexicalScope = 1 << 1, HasPrivateNames = 1 << 2 };

    static void encode(Writer& writer, Encoder& encoder, const SymbolTable& symbolTable)
    {
        auto entries = EncodingOrder::sortedEntries(symbolTable.m_map);
        writer.varuint(entries.size());
        for (auto* entry : entries) {
            CachedString::encode(writer, encoder, entry->key.get());
            writer.varint(CachedSymbolTableEntry::pack(entry->value));
        }
        writer.varuint(symbolTable.m_maxScopeOffset.offsetUnchecked());
        uint8_t flags = 0;
        if (symbolTable.m_usesSloppyEval)
            flags |= UsesSloppyEval;
        if (symbolTable.m_nestedLexicalScope)
            flags |= NestedLexicalScope;
        if (symbolTable.m_rareData)
            flags |= HasPrivateNames;
        writer.u8(flags);
        writer.u8(symbolTable.m_scopeType);
        CachedScopedArgumentsTable::encode(writer, symbolTable.m_arguments.get());
        if (symbolTable.m_rareData)
            CachedVariableEnvironment::encodePrivateNames(writer, encoder, symbolTable.m_rareData->m_privateNames);
    }

    static SymbolTable* decode(Reader& reader, Decoder& decoder)
    {
        VM& vm = decoder.vm();
        SymbolTable* symbolTable = SymbolTable::create(vm);
        unsigned count = reader.varuint();
        if (!reader.checkCount(count))
            return symbolTable;
        for (unsigned i = 0; i < count; ++i) {
            RefPtr<UniquedStringImpl> name = CachedString::decodeNonNull(reader, decoder);
            SymbolTableEntry entry;
            CachedSymbolTableEntry::unpack(entry, reader.varint());
            symbolTable->m_map.add(WTF::move(name), WTF::move(entry));
        }
        symbolTable->m_maxScopeOffset = ScopeOffset(reader.varuint());
        uint8_t flags = reader.u8();
        symbolTable->m_usesSloppyEval = flags & UsesSloppyEval;
        symbolTable->m_nestedLexicalScope = flags & NestedLexicalScope;
        symbolTable->m_scopeType = reader.u8();
        if (ScopedArgumentsTable* arguments = CachedScopedArgumentsTable::decode(reader, vm))
            symbolTable->m_arguments.set(vm, symbolTable, arguments);
        if (flags & HasPrivateNames) {
            symbolTable->m_rareData = WTF::makeUnique<SymbolTable::SymbolTableRareData>();
            CachedVariableEnvironment::decodePrivateNames(reader, decoder, symbolTable->m_rareData->m_privateNames);
        }
        return symbolTable;
    }
};

// -- A code block's rare data ------------------------------------------------------------------------------------------

// BitVector := varuint numBits, ceil(numBits / 8) bytes, bit i in byte i / 8 at position i % 8
class CachedBitVector {
public:
    static void encode(Writer& writer, const BitVector& bits)
    {
        size_t numBits = bits.size();
        writer.varuint(numBits);
        for (size_t byte = 0; byte < (numBits + 7) / 8; ++byte) {
            uint8_t value = 0;
            for (unsigned bit = 0; bit < 8 && byte * 8 + bit < numBits; ++bit)
                value |= bits.get(byte * 8 + bit) << bit;
            writer.u8(value);
        }
    }
    static void decode(Reader& reader, BitVector& bits)
    {
        size_t numBits = reader.varuint();
        auto bytes = reader.bytes((numBits + 7) / 8);
        if (bytes.size() < (numBits + 7) / 8)
            return;
        bits.ensureSize(numBits);
        for (size_t i = 0; i < numBits; ++i) {
            if (bytes[i / 8] >> (i % 8) & 1)
                bits.quickSet(i);
        }
    }
};

// SimpleJumpTable := varint min, varint defaultOffset, u8 isList, varuint count, count × varint branchOffset
class CachedSimpleJumpTable {
public:
    static void encode(Writer& writer, const UnlinkedSimpleJumpTable& jumpTable)
    {
        writer.varint(jumpTable.m_min);
        writer.varint(jumpTable.m_defaultOffset);
        writer.boolean(jumpTable.m_isList);
        writer.varuint(jumpTable.m_branchOffsets.size());
        for (int32_t offset : jumpTable.m_branchOffsets)
            writer.varint(offset);
    }
    static void decode(Reader& reader, UnlinkedSimpleJumpTable& jumpTable)
    {
        jumpTable.m_min = reader.varint();
        jumpTable.m_defaultOffset = reader.varint();
        jumpTable.m_isList = reader.boolean();
        unsigned count = reader.varuint();
        if (!reader.checkCount(count))
            return;
        jumpTable.m_branchOffsets = FixedVector<int32_t>(count);
        for (unsigned i = 0; i < count; ++i)
            jumpTable.m_branchOffsets[i] = reader.varint();
    }
};

// StringJumpTable := varuint count, count × { StringRef, varint branchOffset, varuint indexInTable }, varuint minLength, varuint maxLength, varint defaultOffset
class CachedStringJumpTable {
public:
    static void encode(Writer& writer, Encoder& encoder, const UnlinkedStringJumpTable& jumpTable)
    {
        auto entries = EncodingOrder::sortedEntries(jumpTable.m_offsetTable);
        writer.varuint(entries.size());
        for (auto* entry : entries) {
            CachedString::encode(writer, encoder, entry->key.get());
            writer.varint(entry->value.m_branchOffset);
            writer.varuint(entry->value.m_indexInTable);
        }
        writer.varuint(jumpTable.m_minLength);
        writer.varuint(jumpTable.m_maxLength);
        writer.varint(jumpTable.m_defaultOffset);
    }
    static void decode(Reader& reader, Decoder& decoder, UnlinkedStringJumpTable& jumpTable)
    {
        unsigned count = reader.varuint();
        if (!reader.checkCount(count))
            return;
        Vector<std::pair<RefPtr<StringImpl>, UnlinkedStringJumpTable::OffsetLocation>> entries;
        entries.reserveInitialCapacity(count);
        for (unsigned i = 0; i < count; ++i) {
            RefPtr<StringImpl> string = CachedString::decodeNonNull(reader, decoder);
            UnlinkedStringJumpTable::OffsetLocation location;
            location.m_branchOffset = reader.varint();
            location.m_indexInTable = reader.varuint();
            entries.append({ WTF::move(string), location });
        }
        for (auto& [string, location] : entries)
            jumpTable.m_offsetTable.add(WTF::move(string), location);
        jumpTable.m_minLength = reader.varuint();
        jumpTable.m_maxLength = reader.varuint();
        jumpTable.m_defaultOffset = reader.varint();
    }
};

// CodeBlockRareData :=
//   varuint count, count × { varuint start, varuint end, varuint target, u8 type }        exception handlers
//   varuint count, count × SimpleJumpTable
//   varuint count, count × StringJumpTable
//   varuint count, count × { varuint bytecodeOffset, varuint startDivot, varuint endDivot } type profiler ranges
//   varuint count, count × varuint                                                          control flow profiler bytecode offsets
//   varuint count, count × BitVector
//   varuint count, count × { varuint count, count × StringRef }                            constant identifier sets
//   u8 needsClassFieldInitializer, u8 privateBrandRequirement
class CachedCodeBlockRareData {
public:
    static void encode(Writer& writer, Encoder& encoder, const UnlinkedCodeBlock::RareData& rareData)
    {
        writer.varuint(rareData.m_exceptionHandlers.size());
        for (auto& handler : rareData.m_exceptionHandlers) {
            writer.varuint(handler.start);
            writer.varuint(handler.end);
            writer.varuint(handler.target);
            writer.u8(static_cast<uint8_t>(handler.type()));
        }
        writer.varuint(rareData.m_unlinkedSwitchJumpTables.size());
        for (auto& table : rareData.m_unlinkedSwitchJumpTables)
            CachedSimpleJumpTable::encode(writer, table);
        writer.varuint(rareData.m_unlinkedStringSwitchJumpTables.size());
        for (auto& table : rareData.m_unlinkedStringSwitchJumpTables)
            CachedStringJumpTable::encode(writer, encoder, table);
        {
            Vector<std::pair<unsigned, UnlinkedCodeBlock::RareData::TypeProfilerExpressionRange>> ranges;
            for (auto& entry : rareData.m_typeProfilerInfoMap)
                ranges.append({ entry.key, entry.value });
            EncodingOrder::sort(ranges, [](auto& entry) { return entry.first; });
            writer.varuint(ranges.size());
            for (auto& [offset, range] : ranges) {
                writer.varuint(offset);
                writer.varuint(range.m_startDivot);
                writer.varuint(range.m_endDivot);
            }
        }
        writer.varuint(rareData.m_opProfileControlFlowBytecodeOffsets.size());
        for (auto offset : rareData.m_opProfileControlFlowBytecodeOffsets)
            writer.varuint(offset);
        writer.varuint(rareData.m_bitVectors.size());
        for (auto& bits : rareData.m_bitVectors)
            CachedBitVector::encode(writer, bits);
        writer.varuint(rareData.m_constantIdentifierSets.size());
        for (auto& set : rareData.m_constantIdentifierSets) {
            Vector<RefPtr<UniquedStringImpl>> names;
            for (auto& name : set)
                names.append(name);
            EncodingOrder::sort(names, [](auto& name) -> const auto& { return name; });
            writer.varuint(names.size());
            for (auto& name : names)
                CachedString::encode(writer, encoder, name.get());
        }
        writer.boolean(rareData.m_needsClassFieldInitializer);
        writer.u8(rareData.m_privateBrandRequirement);
    }

    static UnlinkedCodeBlock::RareData* decode(Reader& reader, Decoder& decoder)
    {
        auto* rareData = new UnlinkedCodeBlock::RareData { };
        auto counted = [&](auto&& each) {
            unsigned count = reader.varuint();
            if (!reader.checkCount(count))
                return 0u;
            each(count);
            return count;
        };
        counted([&](unsigned count) {
            rareData->m_exceptionHandlers = FixedVector<UnlinkedHandlerInfo>(count);
            for (auto& handler : rareData->m_exceptionHandlers) {
                uint32_t start = reader.varuint();
                uint32_t end = reader.varuint();
                uint32_t target = reader.varuint();
                handler = UnlinkedHandlerInfo(start, end, target, static_cast<HandlerType>(reader.u8() & 3));
            }
        });
        counted([&](unsigned count) {
            rareData->m_unlinkedSwitchJumpTables = FixedVector<UnlinkedSimpleJumpTable>(count);
            for (auto& table : rareData->m_unlinkedSwitchJumpTables)
                CachedSimpleJumpTable::decode(reader, table);
        });
        counted([&](unsigned count) {
            rareData->m_unlinkedStringSwitchJumpTables = FixedVector<UnlinkedStringJumpTable>(count);
            for (auto& table : rareData->m_unlinkedStringSwitchJumpTables)
                CachedStringJumpTable::decode(reader, decoder, table);
        });
        counted([&](unsigned count) {
            for (unsigned i = 0; i < count; ++i) {
                unsigned offset = reader.varuint();
                UnlinkedCodeBlock::RareData::TypeProfilerExpressionRange range;
                range.m_startDivot = reader.varuint();
                range.m_endDivot = reader.varuint();
                rareData->m_typeProfilerInfoMap.set(offset, range);
            }
        });
        counted([&](unsigned count) {
            rareData->m_opProfileControlFlowBytecodeOffsets = FixedVector<JSInstructionStream::Offset>(count);
            for (auto& offset : rareData->m_opProfileControlFlowBytecodeOffsets)
                offset = reader.varuint();
        });
        counted([&](unsigned count) {
            rareData->m_bitVectors = FixedVector<BitVector>(count);
            for (auto& bits : rareData->m_bitVectors)
                CachedBitVector::decode(reader, bits);
        });
        counted([&](unsigned count) {
            rareData->m_constantIdentifierSets = FixedVector<IdentifierSet>(count);
            for (auto& set : rareData->m_constantIdentifierSets) {
                unsigned size = reader.varuint();
                if (!reader.checkCount(size))
                    return;
                for (unsigned i = 0; i < size; ++i)
                    set.add(CachedString::decodeNonNull(reader, decoder));
            }
        });
        rareData->m_needsClassFieldInitializer = reader.boolean();
        rareData->m_privateBrandRequirement = reader.u8() & 1;
        return rareData;
    }

};

// OutOfLineJumpTargets := varuint count, count × { varuint bytecodeOffset, varint target }
struct CachedCodeBlockExtras {
    static void encodeOutOfLineJumpTargets(Writer& writer, const UnlinkedCodeBlock& codeBlock)
    {
        auto& targets = codeBlock.m_outOfLineJumpTargets;
        Vector<std::pair<JSInstructionStream::Offset, int>> entries;
        for (auto& entry : targets)
            entries.append({ entry.key, entry.value });
        EncodingOrder::sort(entries, [](auto& entry) { return entry.first; });
        writer.varuint(entries.size());
        for (auto [offset, target] : entries) {
            writer.varuint(offset);
            writer.varint(target);
        }
    }
    static void decodeOutOfLineJumpTargets(Reader& reader, UnlinkedCodeBlock& codeBlock)
    {
        auto& targets = codeBlock.m_outOfLineJumpTargets;
        unsigned count = reader.varuint();
        if (!reader.checkCount(count))
            return;
        for (unsigned i = 0; i < count; ++i) {
            JSInstructionStream::Offset offset = reader.varuint();
            targets.set(offset, reader.varint());
        }
    }
};

// -- Constants --------------------------------------------------------------------------------------------------------

// JSValue := u8 kind, then by kind:
//   Undefined, Null, True, False, Empty    nothing
//   Int32                                  varint
//   Double                                 f64
//   String                                 StringRef
//   SymbolTable                            SymbolTable
//   ImmutableButterfly                     u8 indexingMode, varuint length, length × (f64 if a double array, else JSValue)
//   RegExp                                 StringRef pattern, u16 flags
//   TemplateObjectDescriptor               varuint count, count × StringRef raw, count × { u8 present, [StringRef cooked] }, varint endOffset
//   BigInt                                 u8 sign, varuint length, length × u64 digit
class CachedJSValue {
public:
    enum class Kind : uint8_t {
        Undefined,
        Null,
        True,
        False,
        Empty,
        Int32,
        Double,
        SymbolTable,
        String,
        ImmutableButterfly,
        RegExp,
        TemplateObjectDescriptor,
        BigInt,
    };

    static void encode(Writer& writer, Encoder& encoder, JSValue value)
    {
        if (value.isEmpty())
            return writer.u8(static_cast<uint8_t>(Kind::Empty));
        if (!value.isCell()) {
            if (value.isInt32()) {
                writer.u8(static_cast<uint8_t>(Kind::Int32));
                return writer.varint(value.asInt32());
            }
            if (value.isUndefined())
                return writer.u8(static_cast<uint8_t>(Kind::Undefined));
            if (value.isNull())
                return writer.u8(static_cast<uint8_t>(Kind::Null));
            if (value.isTrue())
                return writer.u8(static_cast<uint8_t>(Kind::True));
            if (value.isFalse())
                return writer.u8(static_cast<uint8_t>(Kind::False));
            RELEASE_ASSERT(value.isDouble());
            writer.u8(static_cast<uint8_t>(Kind::Double));
            return writer.f64(value.asDouble());
        }

        JSCell* cell = value.asCell();
        if (auto* symbolTable = dynamicDowncast<SymbolTable>(cell)) {
            writer.u8(static_cast<uint8_t>(Kind::SymbolTable));
            return CachedSymbolTable::encode(writer, encoder, *symbolTable);
        }
        if (auto* string = dynamicDowncast<JSString>(cell)) {
            auto value = string->tryGetValue();
            RELEASE_ASSERT(value.data.impl()); // constants are never unresolved ropes; a failed resolution must not be encoded as garbage
            writer.u8(static_cast<uint8_t>(Kind::String));
            return CachedString::encode(writer, encoder, value.data.impl());
        }
        if (auto* butterfly = dynamicDowncast<JSCellButterfly>(cell)) {
            writer.u8(static_cast<uint8_t>(Kind::ImmutableButterfly));
            IndexingType indexingMode = butterfly->indexingMode(); // not indexingTypeAndMisc(): the rest of that byte is cell-lock state
            unsigned length = butterfly->length();
            writer.u8(indexingMode);
            writer.varuint(length);
            for (unsigned i = 0; i < length; ++i) {
                if (hasDouble(indexingMode))
                    writer.f64(butterfly->toButterfly()->contiguousDouble().at(butterfly, i));
                else
                    encode(writer, encoder, butterfly->toButterfly()->contiguous().at(butterfly, i).get());
            }
            return;
        }
        if (auto* regExp = dynamicDowncast<RegExp>(cell)) {
            writer.u8(static_cast<uint8_t>(Kind::RegExp));
            CachedString::encode(writer, encoder, regExp->pattern());
            return writer.u16(regExp->flags().toRaw());
        }
        if (auto* descriptor = dynamicDowncast<JSTemplateObjectDescriptor>(cell)) {
            writer.u8(static_cast<uint8_t>(Kind::TemplateObjectDescriptor));
            auto& rawStrings = descriptor->descriptor().rawStrings();
            auto& cookedStrings = descriptor->descriptor().cookedStrings();
            ASSERT(rawStrings.size() == cookedStrings.size());
            writer.varuint(rawStrings.size());
            for (auto& raw : rawStrings)
                CachedString::encode(writer, encoder, raw);
            for (auto& cooked : cookedStrings) {
                writer.boolean(cooked.has_value());
                if (cooked)
                    CachedString::encode(writer, encoder, *cooked);
            }
            return writer.varint(descriptor->endOffset());
        }
        if (auto* bigInt = dynamicDowncast<JSBigInt>(cell)) {
            writer.u8(static_cast<uint8_t>(Kind::BigInt));
            writer.boolean(bigInt->sign());
            writer.varuint(bigInt->length());
            static_assert(sizeof(JSBigInt::Digit) == sizeof(uint64_t));
            for (unsigned i = 0; i < bigInt->length(); ++i)
                writer.u64(bigInt->digit(i));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static JSValue decode(Reader& reader, Decoder& decoder)
    {
        VM& vm = decoder.vm();
        switch (static_cast<Kind>(reader.u8())) {
        case Kind::Undefined:
            return jsUndefined();
        case Kind::Null:
            return jsNull();
        case Kind::True:
            return jsBoolean(true);
        case Kind::False:
            return jsBoolean(false);
        case Kind::Empty:
            return JSValue();
        case Kind::Int32:
            return jsNumber(reader.varint());
        case Kind::Double:
            return JSValue(JSValue::EncodeAsDouble, reader.f64());
        case Kind::SymbolTable:
            return CachedSymbolTable::decode(reader, decoder);
        case Kind::String:
            // A constant becomes a JSString; it does not have to be an atom, so skip the atom table.
            return jsString(vm, CachedString::decodePlainString(reader, decoder));
        case Kind::ImmutableButterfly: {
            IndexingType indexingMode = reader.u8();
            unsigned length = reader.varuint();
            if (!reader.checkCount(length))
                return JSValue();
            JSCellButterfly* butterfly = JSCellButterfly::create(vm, indexingMode, length);
            for (unsigned i = 0; i < length; ++i) {
                if (hasDouble(indexingMode))
                    butterfly->toButterfly()->contiguousDouble().at(butterfly, i) = reader.f64();
                else
                    butterfly->toButterfly()->contiguous().at(butterfly, i).set(vm, butterfly, decode(reader, decoder));
            }
            return butterfly;
        }
        case Kind::RegExp: {
            String pattern = CachedString::decodeString(reader, decoder);
            auto flags = OptionSet<Yarr::Flags>::fromRaw(reader.u16());
            if (reader.overran())
                return JSValue();
            return RegExp::create(vm, pattern, flags);
        }
        case Kind::TemplateObjectDescriptor: {
            unsigned count = reader.varuint();
            if (!reader.checkCount(count))
                return JSValue();
            TemplateObjectDescriptor::StringVector rawStrings;
            TemplateObjectDescriptor::OptionalStringVector cookedStrings;
            rawStrings.reserveInitialCapacity(count);
            cookedStrings.reserveInitialCapacity(count);
            for (unsigned i = 0; i < count; ++i)
                rawStrings.append(CachedString::decodeString(reader, decoder));
            for (unsigned i = 0; i < count; ++i) {
                if (reader.boolean())
                    cookedStrings.append(CachedString::decodeString(reader, decoder));
                else
                    cookedStrings.append(std::nullopt);
            }
            int endOffset = reader.varint();
            return JSTemplateObjectDescriptor::create(vm, TemplateObjectDescriptor::create(WTF::move(rawStrings), WTF::move(cookedStrings)), endOffset);
        }
        case Kind::BigInt: {
            bool sign = reader.boolean();
            unsigned length = reader.varuint();
            if (!length)
                return vm.heapBigIntConstantZero.get();
            if (!reader.checkCount(length))
                return JSValue();
            JSBigInt* bigInt = JSBigInt::tryCreateWithLength(vm, length);
            RELEASE_ASSERT(bigInt);
            bigInt->setSign(sign);
            for (unsigned i = 0; i < length; ++i)
                bigInt->setDigit(i, reader.u64());
            return bigInt;
        }
        }
        reader.setOverran();
        return JSValue();
    }
};

// -- Expression info --------------------------------------------------------------------------------------------------

// ExpressionInfo := u32 numberOfEncodedInfo, u8 flags, varuint chapters, varuint extensions, zeros to a 4-byte boundary,
//                   words × u32, [u32 checksum]
// where words is the number ExpressionInfo derives from the three counts. Written 4-aligned so the words can be used
// in place; self-contained and position-independent, so identical ones (every async wrapper, say) are written once.
// A damaged one decodes as "no expression info" (stack traces lose line/column for that function) rather than failing
// the function.
class CachedExpressionInfo {
public:
    enum Flag : uint8_t { HasChecksum = 1 << 0 };

    static Vector<uint8_t, 64> pack(const ExpressionInfo& info, bool checksum)
    {
        Writer writer;
        writer.u32(info.m_numberOfEncodedInfo);
        writer.u8(checksum ? HasChecksum : 0);
        writer.varuint(info.m_numberOfChapters);
        writer.varuint(info.m_numberOfEncodedInfoExtensions);
        writer.zeros(roundUpToMultipleOf<4>(writer.size()) - writer.size());
        static_assert(sizeof(*info.payload()) == sizeof(uint32_t));
        for (size_t i = 0; i < info.payloadSize(); ++i)
            writer.u32(info.payload()[i]);
        if (checksum)
            writer.u32(~crc32c(~0u, writer.span()));
        return Vector<uint8_t, 64>(writer.span());
    }

    static std::unique_ptr<ExpressionInfo> decode(Decoder& decoder, uint32_t at)
    {
        Reader reader(decoder.payload(), at);
        const uint8_t* base = reader.position();
        unsigned encodedInfo = reader.u32();
        uint8_t flags = reader.u8();
        unsigned chapters = reader.varuint();
        unsigned extensions = reader.varuint();
        reader.alignTo(4, base);
        if (reader.overran() || at % 4)
            return ExpressionInfo::createUninitialized(0, 0, 0);
        size_t words = ExpressionInfo::payloadSizeInBytes(chapters, encodedInfo, extensions) / sizeof(uint32_t);
        auto wordBytes = reader.bytes(words * sizeof(uint32_t));
        if (wordBytes.size() != words * sizeof(uint32_t))
            return ExpressionInfo::createUninitialized(0, 0, 0);
        if (flags & HasChecksum) {
            size_t covered = reader.position() - base;
            uint32_t stored = reader.u32();
            if (reader.overran() || (decoder.verifiesChecksums() && stored != ~crc32c(~0u, std::span { base, covered }))) {
                dataLogLnIf(Options::verboseDiskCache(), "[Disk Cache] expression info checksum mismatch; dropping it");
                return ExpressionInfo::createUninitialized(0, 0, 0);
            }
        }
        const unsigned* wordsInPlace = reinterpret_cast<const unsigned*>(wordBytes.data()); // little-endian host, 4-aligned: see the top of the file
        if (decoder.canBorrowPayload() && words)
            return ExpressionInfo::createBorrowed(chapters, encodedInfo, extensions, wordsInPlace);
        auto info = ExpressionInfo::createUninitialized(chapters, encodedInfo, extensions);
        for (size_t i = 0; i < words; ++i)
            info->payload()[i] = readU32(wordBytes.data() + 4 * i);
        return info;
    }
};

// -- Metadata ---------------------------------------------------------------------------------------------------------

// UnlinkedMetadataTable's offset table is cumulative and most opcodes have no metadata in a given function, so a code
// block stores only the opcodes that have entries: (opcode << 24 | entry count). A typical function has a handful
// instead of 51. The decoder lays the table out with its own sizeof(Op::Metadata), which is why counts and not offsets.
struct CachedMetadataSteps {
    static constexpr unsigned indexShift = UnlinkedMetadataTable::stepIndexShift;
    static constexpr uint32_t countMask = UnlinkedMetadataTable::stepCountMask;
    static_assert(UnlinkedMetadataTable::s_offsetTableEntries < (1u << (32 - indexShift)));

    // The inverse of UnlinkedMetadataTable::finalize(): (opcode << 24 | entry count) back out of the offset table.
    static Vector<uint32_t, 16> compute(const UnlinkedMetadataTable& metadataTable)
    {
        ASSERT(metadataTable.m_isFinalized && metadataTable.m_hasMetadata);
        Vector<uint32_t, 16> steps;
        if (metadataTable.m_steps && !metadataTable.m_isLinked) {
            steps.append(std::span { metadataTable.m_steps, metadataTable.m_stepsCount });
            return steps;
        }
        auto offsetAt = [&](unsigned i) -> uint32_t { return metadataTable.m_is32Bit ? metadataTable.offsetTable32()[i] : metadataTable.offsetTable16()[i]; };
        for (unsigned i = 0; i < UnlinkedMetadataTable::s_offsetTableEntries - 1; ++i) {
            auto opcode = static_cast<OpcodeID>(i);
            uint32_t start = roundUpToMultipleOf(metadataAlignment(opcode), offsetAt(i));
            uint32_t end = offsetAt(i + 1);
            if (end <= start)
                continue;
            uint32_t count = (end - start) / metadataSize(opcode);
            ASSERT(count && start + count * metadataSize(opcode) == end && count <= countMask);
            steps.append(i << indexShift | count);
        }
#if ASSERT_ENABLED
        std::array<UnlinkedMetadataTable::Offset32, UnlinkedMetadataTable::s_offsetTableEntries> check;
        UnlinkedMetadataTable::expandSteps(steps.span(), check.data());
        for (unsigned i = 0; i < UnlinkedMetadataTable::s_offsetTableEntries; ++i)
            ASSERT(check[i] == offsetAt(i));
#endif
        return steps;
    }

    static Ref<UnlinkedMetadataTable> build(unsigned numValueProfiles, std::span<const uint32_t> steps)
    {
        Ref<UnlinkedMetadataTable> metadataTable = UnlinkedMetadataTable::create(UnlinkedMetadataTable::stepsNeed32BitOffsets(steps), numValueProfiles);
        metadataTable->m_isFinalized = true;
        metadataTable->m_isLinked = false;
        metadataTable->m_hasMetadata = true;
        metadataTable->m_numValueProfiles = numValueProfiles;
        if (metadataTable->m_is32Bit)
            UnlinkedMetadataTable::expandSteps(steps, metadataTable->offsetTable32());
        else
            UnlinkedMetadataTable::expandSteps(steps, metadataTable->offsetTable16());
        return metadataTable;
    }
};

// -- Source code ------------------------------------------------------------------------------------------------------

// JSTextPosition := varint line, varint offset, varint lineStartOffset
class CachedJSTextPosition {
public:
    static void encode(Writer& writer, const JSTextPosition& position)
    {
        writer.varint(position.line);
        writer.varint(position.offset);
        writer.varint(position.lineStartOffset);
    }
    static JSTextPosition decode(Reader& reader)
    {
        JSTextPosition position;
        position.line = reader.varint();
        position.offset = reader.varint();
        position.lineStartOffset = reader.varint();
        return position;
    }
};

// SourceProvider := u8 sourceType, StringRef sourceOrigin, StringRef sourceURL, StringRef preRedirectURL,
//                   StringRef sourceURLDirective, StringRef sourceMappingURLDirective, varint startLine, varint startColumn,
//                   u8 taintedOrigin, then by sourceType: varuint sourceLength (Bun: the source text is not stored) or
//                   StringRef source; WebAssembly: varuint size, bytes
class CachedSourceProvider {
public:
    static void encode(Writer& writer, Encoder& encoder, const SourceProvider& provider)
    {
        SourceProviderSourceType sourceType = provider.sourceType();
        writer.u8(static_cast<uint8_t>(sourceType));
        CachedString::encode(writer, encoder, provider.sourceOrigin().url().string());
        CachedString::encode(writer, encoder, provider.sourceURL());
        CachedString::encode(writer, encoder, provider.preRedirectURL());
        CachedString::encode(writer, encoder, provider.sourceURLDirective());
        CachedString::encode(writer, encoder, provider.sourceMappingURLDirective());
        writer.varint(provider.startPosition().m_line.zeroBasedInt());
        writer.varint(provider.startPosition().m_column.zeroBasedInt());
        writer.u8(static_cast<uint8_t>(provider.sourceTaintedOrigin()));
        switch (sourceType) {
        case SourceProviderSourceType::Program:
        case SourceProviderSourceType::Module:
#if USE(BUN_JSC_ADDITIONS)
        case SourceProviderSourceType::BunTranspiledModule:
            // SourceCodeKey::operator== under BUN_JSC_ADDITIONS does not compare source text; length() and host() are compared.
            writer.varuint(provider.source().length());
#else
            CachedString::encode(writer, encoder, provider.source().toString());
#endif
            break;
#if ENABLE(WEBASSEMBLY)
        case SourceProviderSourceType::WebAssembly: {
            auto& data = static_cast<const WebAssemblySourceProvider&>(provider).dataVector();
            writer.varuint(data.size());
            writer.bytes(data.span());
            break;
        }
#endif
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    static RefPtr<SourceProvider> decode(Reader& reader, Decoder& decoder)
    {
        auto sourceType = static_cast<SourceProviderSourceType>(reader.u8());
        SourceOrigin sourceOrigin { URL({ }, CachedString::decodeString(reader, decoder)) };
        String sourceURL = CachedString::decodeString(reader, decoder);
        String preRedirectURL = CachedString::decodeString(reader, decoder);
        String sourceURLDirective = CachedString::decodeString(reader, decoder);
        String sourceMappingURLDirective = CachedString::decodeString(reader, decoder);
        int startLine = reader.varint();
        int startColumn = reader.varint();
        TextPosition startPosition { OrdinalNumber::fromZeroBasedInt(startLine), OrdinalNumber::fromZeroBasedInt(startColumn) };
        auto taintedOrigin = static_cast<SourceTaintedOrigin>(reader.u8());
        RefPtr<SourceProvider> provider;
        switch (sourceType) {
        case SourceProviderSourceType::Program:
        case SourceProviderSourceType::Module:
#if USE(BUN_JSC_ADDITIONS)
        case SourceProviderSourceType::BunTranspiledModule: {
            unsigned sourceLength = reader.varuint();
            // Reuse the runtime SourceProvider the Decoder was constructed with rather than allocating one holding a copy
            // of the source: the decoded key is only used for SourceCodeKey equality, which does not look at source bytes.
            if (RefPtr<SourceProvider> runtimeProvider = decoder.provider()) {
                if (runtimeProvider->sourceType() == sourceType && runtimeProvider->source().length() == sourceLength)
                    return runtimeProvider;
            }
            // No provider supplied: one whose source() is empty, so length() mismatches and the cache entry is rejected.
            provider = StringSourceProvider::create(String(), sourceOrigin, sourceURL, taintedOrigin, startPosition, sourceType);
            break;
        }
#else
        {
            String source = CachedString::decodeString(reader, decoder);
            provider = StringSourceProvider::create(source, sourceOrigin, sourceURL, taintedOrigin, startPosition, sourceType);
            break;
        }
#endif
#if ENABLE(WEBASSEMBLY)
        case SourceProviderSourceType::WebAssembly: {
            auto bytes = reader.bytes(reader.varuint());
            provider = WebAssemblySourceProvider::create(Vector<uint8_t>(bytes), sourceOrigin, sourceURL);
            break;
        }
#endif
        default:
            return nullptr;
        }
        provider->setSourceURLDirective(sourceURLDirective);
        provider->setSourceMappingURLDirective(sourceMappingURLDirective);
        return provider;
    }
};

// UnlinkedSourceCode := SourceProvider, varint startOffset, varint endOffset
template<typename SourceType>
class CachedUnlinkedSourceCodeShape {
public:
    static void encode(Writer& writer, Encoder& encoder, const SourceType& source)
    {
        CachedSourceProvider::encode(writer, encoder, *source.m_provider);
        writer.varint(source.m_startOffset);
        writer.varint(source.m_endOffset);
    }
    static bool decode(Reader& reader, Decoder& decoder, SourceType& source)
    {
        source.m_provider = CachedSourceProvider::decode(reader, decoder);
        source.m_startOffset = reader.varint();
        source.m_endOffset = reader.varint();
        return source.m_provider;
    }
};
using CachedUnlinkedSourceCode = CachedUnlinkedSourceCodeShape<UnlinkedSourceCode>;

// SourceCodeKey := UnlinkedSourceCode, StringRef name, varuint flags, u32 hash, varint functionConstructorParametersEndPosition
class CachedSourceCodeKey {
public:
    static void encode(Writer& writer, Encoder& encoder, const SourceCodeKey& key)
    {
        CachedUnlinkedSourceCode::encode(writer, encoder, key.m_sourceCode);
        CachedString::encode(writer, encoder, key.m_name);
        writer.varuint(key.m_flags.m_flags);
        writer.u32(key.m_hash);
        writer.varint(key.m_functionConstructorParametersEndPosition);
    }
    static bool decode(Reader& reader, Decoder& decoder, SourceCodeKey& key)
    {
        bool hasProvider = CachedUnlinkedSourceCode::decode(reader, decoder, key.m_sourceCode);
        key.m_name = CachedString::decodeString(reader, decoder);
        key.m_flags.m_flags = reader.varuint();
        key.m_hash = reader.u32();
        key.m_functionConstructorParametersEndPosition = reader.varint();
        return hasProvider && !reader.overran();
    }
};

// -- Function executables ---------------------------------------------------------------------------------------------

// FunctionExecutableRareData := varuint bits, then only the members that are set:
//   [varuint count, count × StringRef]                                            generator/async wrapper parameter names
//   [varuint count, count × { StringRef ident, JSTextPosition, u8 hasInitializer, [JSTextPosition], u8 kind }]  class element definitions
//   [PrivateNameEnvironment]                                                       parent private-name environment
//   [varuint startOffset, varuint length, varint firstLine, varint startColumn]   class source
// Most rare data is an async function's (empty) wrapper parameter names.
class CachedFunctionExecutableRareData {
public:
    enum Bit : uint32_t {
        HasClassSource = 1 << 0,
        HasWrapperParameterNames = 1 << 1,
        HasClassElementDefinitions = 1 << 2,
        HasParentPrivateNameEnvironment = 1 << 3,
    };

    static void encode(Writer& writer, Encoder& encoder, const UnlinkedFunctionExecutable::RareData& rareData)
    {
        uint32_t bits = 0;
        if (!rareData.m_classSource.isNull())
            bits |= HasClassSource;
        if (!rareData.m_generatorOrAsyncWrapperFunctionParameterNames.isEmpty())
            bits |= HasWrapperParameterNames;
        if (!rareData.m_classElementDefinitions.isEmpty())
            bits |= HasClassElementDefinitions;
        if (!rareData.m_parentPrivateNameEnvironment.isEmpty())
            bits |= HasParentPrivateNameEnvironment;
        writer.varuint(bits);
        if (bits & HasWrapperParameterNames) {
            writer.varuint(rareData.m_generatorOrAsyncWrapperFunctionParameterNames.size());
            for (auto& name : rareData.m_generatorOrAsyncWrapperFunctionParameterNames)
                CachedString::encode(writer, encoder, name);
        }
        if (bits & HasClassElementDefinitions) {
            writer.varuint(rareData.m_classElementDefinitions.size());
            for (auto& definition : rareData.m_classElementDefinitions) {
                CachedString::encode(writer, encoder, definition.ident);
                CachedJSTextPosition::encode(writer, definition.position);
                writer.boolean(definition.initializerPosition.has_value());
                if (definition.initializerPosition)
                    CachedJSTextPosition::encode(writer, *definition.initializerPosition);
                writer.u8(static_cast<uint8_t>(definition.kind));
            }
        }
        if (bits & HasParentPrivateNameEnvironment)
            CachedVariableEnvironment::encodePrivateNames(writer, encoder, rareData.m_parentPrivateNameEnvironment);
        if (bits & HasClassSource) {
            const SourceCode& source = rareData.m_classSource;
            writer.varuint(source.startOffset());
            writer.varuint(source.endOffset() - source.startOffset());
            writer.varint(source.firstLine().zeroBasedInt());
            writer.varint(source.startColumn().zeroBasedInt());
        }
    }

    static std::unique_ptr<UnlinkedFunctionExecutable::RareData> decode(Reader& reader, Decoder& decoder)
    {
        auto rareData = makeUnique<UnlinkedFunctionExecutable::RareData>();
        uint32_t bits = reader.varuint();
        if (bits & HasWrapperParameterNames) {
            unsigned count = reader.varuint();
            if (reader.checkCount(count)) {
                rareData->m_generatorOrAsyncWrapperFunctionParameterNames = FixedVector<Identifier>(count);
                for (auto& name : rareData->m_generatorOrAsyncWrapperFunctionParameterNames)
                    name = CachedString::decodeIdentifier(reader, decoder);
            }
        }
        if (bits & HasClassElementDefinitions) {
            unsigned count = reader.varuint();
            if (reader.checkCount(count)) {
                rareData->m_classElementDefinitions = FixedVector<UnlinkedFunctionExecutable::ClassElementDefinition>(count);
                for (auto& definition : rareData->m_classElementDefinitions) {
                    definition.ident = CachedString::decodeIdentifier(reader, decoder);
                    definition.position = CachedJSTextPosition::decode(reader);
                    if (reader.boolean())
                        definition.initializerPosition = CachedJSTextPosition::decode(reader);
                    definition.kind = static_cast<UnlinkedFunctionExecutable::ClassElementDefinition::Kind>(reader.u8());
                }
            }
        }
        if (bits & HasParentPrivateNameEnvironment)
            CachedVariableEnvironment::decodePrivateNames(reader, decoder, rareData->m_parentPrivateNameEnvironment);
        if (bits & HasClassSource) {
            SourceCode& source = rareData->m_classSource;
            source.m_provider = decoder.provider();
            source.m_startOffset = reader.varuint();
            source.m_endOffset = source.m_startOffset + reader.varuint();
            source.m_firstLine = OrdinalNumber::fromZeroBasedInt(reader.varint());
            source.m_startColumn = OrdinalNumber::fromZeroBasedInt(reader.varint());
        }
        return rareData;
    }
};

static uint32_t encodeFunctionCodeBlockRecord(Encoder&, const UnlinkedFunctionCodeBlock&);

// ExecutableRecord :=
//   u32 header                                              what is present (Header), parse mode << 16
//   if Updatable:       u32 features, u8 lexicallyScopedFeatures, u8 hasCapturedVariables, u16 0
//   if HasChecksum:     u32 checksum, u32 extent           the checksum covers [record, record + extent)
//   if HasCallBlock:    u32 callBlockAt                    offset of the code block record; 0 until its body is written
//   if HasConstructBlock: u32 constructBlockAt
//   varuint flags, varuint features, u8 lexicallyScopedFeatures, varuint startOffset, varint × 6 (positions as deltas),
//   varuint parameterCount, [varuint firstLineOffset, varuint lineCount]
//   if HasName:         StringRef
//   if HasTDZ:          SharedRef TDZEnvironmentLink
//   if HasRareData:     FunctionExecutableRareData
// An updatable record (the jsc shell's disk cache patches it when a lazily compiled function joins the cache later)
// always has the first three rows, at fixed offsets; a persistent payload (bun --compile) has only what it uses.
class CachedFunctionExecutable {
public:
    enum Header : uint32_t {
        HasCallBlock = 1 << 0,
        HasConstructBlock = 1 << 1,
        HasName = 1 << 2,
        HasTDZ = 1 << 3,
        HasRareData = 1 << 4,
        HasLines = 1 << 5,
        HasChecksum = 1 << 6,
        Updatable = 1 << 7, // implies HasChecksum and both code block slots
        HasCapturedVariables = 1 << 8,
        ParseModeShift = 16, // 8 bits
    };
    enum Flag : uint32_t {
        // one word of 1- and 2-bit fields, written as a varuint (the high bits are the rarely-set ones)
        ScriptModeShift = 0,
        SuperBindingShift = 1,
        ConstructAbilityShift = 2,
        HasNameFlagShift = 3,
        ConstructorKindShift = 4, // 2 bits
        FunctionModeShift = 6, // 2
        ImplementationVisibilityShift = 8, // 2
        DerivedContextTypeShift = 10, // 2
        EvalContextTypeShift = 12, // 2
        PrivateBrandRequirementShift = 14,
        InlineAttributeShift = 15,
        NeedsClassFieldInitializerShift = 16,
        IsBuiltinFunctionShift = 17,
        IsBuiltinDefaultClassConstructorShift = 18,
    };
    static_assert(bitWidthOfImplementationVisibility <= 2);

    // Offsets of the fixed fields of an updatable record.
    static constexpr size_t metadataOffset = 4;
    static constexpr size_t checksumOffset = metadataOffset + 8;
    static constexpr size_t extentOffset = checksumOffset + 4;
    static constexpr size_t callBlockOffset = extentOffset + 4;
    static constexpr size_t constructBlockOffset = callBlockOffset + 4;
    static constexpr size_t updatableFixedSize = constructBlockOffset + 4;

    static uint32_t encode(Encoder&, const UnlinkedFunctionExecutable&);

    // The record at `at`, parsed and its references decoded; `intact` is false if it does not parse or its checksum is off.
    CachedFunctionExecutable(Decoder&, uint32_t at);
    UnlinkedFunctionExecutable* decode(Decoder&) const;
    // Whether the record at `at` parses and checks out, without decoding anything.
    static bool isIntact(Decoder&, uint32_t at);

    bool intact { false };
    uint32_t at { 0 };
    uint32_t header { 0 };
    uint32_t callBlockAt { 0 };
    uint32_t constructBlockAt { 0 };
    unsigned firstLineOffset { 0 };
    unsigned lineCount { 0 };
    unsigned unlinkedFunctionStart { 0 };
    unsigned unlinkedBodyStartColumn { 0 };
    unsigned unlinkedBodyEndColumn { 0 };
    unsigned startOffset { 0 };
    unsigned sourceLength { 0 };
    unsigned parametersStartOffset { 0 };
    unsigned unlinkedFunctionEnd { 0 };
    unsigned parameterCount { 0 };
    SourceParseMode sourceParseMode { };
    ImplementationVisibility implementationVisibility { };
    bool isBuiltinFunction { false };
    bool isBuiltinDefaultClassConstructor { false };
    unsigned constructAbility { 0 };
    unsigned constructorKind { 0 };
    unsigned functionMode { 0 };
    unsigned scriptMode { 0 };
    unsigned superBinding { 0 };
    unsigned derivedContextType { 0 };
    unsigned evalContextType { 0 };
    bool inlineAttribute { false };
    bool needsClassFieldInitializer { false };
    unsigned privateBrandRequirement { 0 };
    bool hasName { false };
    CodeFeatures features { 0 };
    LexicallyScopedFeatures lexicallyScopedFeatures { 0 };
    bool hasCapturedVariables { false };
    Identifier name;
    RefPtr<TDZEnvironmentLink> parentScopeTDZVariables;
    mutable std::unique_ptr<UnlinkedFunctionExecutable::RareData> rareData; // moved into the executable decoded from this

private:
    enum class Mode { Check, Decode };
    CachedFunctionExecutable(Decoder&, uint32_t at, Mode);
};

bool CachedFunctionExecutableOffsets::isUpdatable(std::span<const uint8_t> record)
{
    return record.size() >= 4 && (readU32(record.data()) & CachedFunctionExecutable::Updatable);
}
ptrdiff_t CachedFunctionExecutableOffsets::metadataOffset() { return CachedFunctionExecutable::metadataOffset; }
ptrdiff_t CachedFunctionExecutableOffsets::checksumOffset() { return CachedFunctionExecutable::checksumOffset; }
ptrdiff_t CachedFunctionExecutableOffsets::extentOffset() { return CachedFunctionExecutable::extentOffset; }
ptrdiff_t CachedFunctionExecutableOffsets::codeBlockForCallOffset() { return CachedFunctionExecutable::callBlockOffset; }
ptrdiff_t CachedFunctionExecutableOffsets::codeBlockForConstructOffset() { return CachedFunctionExecutable::constructBlockOffset; }
size_t CachedFunctionExecutableOffsets::fixedSize() { return CachedFunctionExecutable::updatableFixedSize; }

uint32_t CachedFunctionExecutable::encode(Encoder& encoder, const UnlinkedFunctionExecutable& executable)
{
    uint32_t header = static_cast<uint32_t>(executable.m_sourceParseMode) << ParseModeShift;
    if (executable.m_hasCapturedVariables)
        header |= HasCapturedVariables;
    if (executable.m_firstLineOffset || executable.m_lineCount)
        header |= HasLines;
    if (!executable.ecmaName().isNull())
        header |= HasName;
    if (executable.m_parentScopeTDZVariables)
        header |= HasTDZ;
    if (executable.m_rareData)
        header |= HasRareData;
    if (executable.m_unlinkedCodeBlockForCall)
        header |= HasCallBlock;
    if (executable.m_unlinkedCodeBlockForConstruct)
        header |= HasConstructBlock;
    if (encoder.checksums())
        header |= HasChecksum;
    if (encoder.updatable())
        header |= Updatable | HasChecksum | HasCallBlock | HasConstructBlock;

    Writer writer;
    writer.u32(header);
    if (header & Updatable) {
        writer.u32(executable.m_features);
        writer.u8(executable.m_lexicallyScopedFeatures);
        writer.boolean(executable.m_hasCapturedVariables);
        writer.u16(0);
    }
    size_t checksumAt = 0;
    size_t extentAt = 0;
    if (header & HasChecksum) {
        checksumAt = writer.reserveU32();
        extentAt = writer.reserveU32();
    }
    size_t callBlockAt = header & HasCallBlock ? writer.reserveU32() : 0;
    size_t constructBlockAt = header & HasConstructBlock ? writer.reserveU32() : 0;

    uint32_t flags = static_cast<uint32_t>(executable.m_scriptMode) << ScriptModeShift
        | static_cast<uint32_t>(executable.m_superBinding) << SuperBindingShift
        | static_cast<uint32_t>(executable.m_constructAbility) << ConstructAbilityShift
        | static_cast<uint32_t>(executable.m_hasName) << HasNameFlagShift
        | static_cast<uint32_t>(executable.m_constructorKind) << ConstructorKindShift
        | static_cast<uint32_t>(executable.m_functionMode) << FunctionModeShift
        | static_cast<uint32_t>(executable.m_implementationVisibility) << ImplementationVisibilityShift
        | static_cast<uint32_t>(executable.m_derivedContextType) << DerivedContextTypeShift
        | static_cast<uint32_t>(executable.m_evalContextType) << EvalContextTypeShift
        | static_cast<uint32_t>(executable.m_privateBrandRequirement) << PrivateBrandRequirementShift
        | static_cast<uint32_t>(executable.m_inlineAttribute) << InlineAttributeShift
        | static_cast<uint32_t>(executable.m_needsClassFieldInitializer) << NeedsClassFieldInitializerShift
        | static_cast<uint32_t>(executable.m_isBuiltinFunction) << IsBuiltinFunctionShift
        | static_cast<uint32_t>(executable.m_isBuiltinDefaultClassConstructor) << IsBuiltinDefaultClassConstructorShift;
    writer.varuint(flags);
    writer.varuint(executable.m_features);
    writer.u8(static_cast<uint8_t>(executable.m_lexicallyScopedFeatures));
    // Source positions cluster around the function's start, so all but the first are deltas.
    unsigned start = executable.m_startOffset;
    writer.varuint(start);
    writer.varint(static_cast<int32_t>(executable.m_unlinkedFunctionStart - start));
    writer.varint(static_cast<int32_t>(executable.m_parametersStartOffset - start));
    writer.varuint(executable.m_sourceLength);
    writer.varint(static_cast<int32_t>(executable.m_unlinkedFunctionEnd - (start + executable.m_sourceLength)));
    // Columns are offsets from a line start; on one line the two differ by the same constant, so the second is a delta of the first.
    int32_t bodyStartColumnDelta = static_cast<int32_t>(executable.m_unlinkedBodyStartColumn - executable.m_unlinkedFunctionStart);
    writer.varint(bodyStartColumnDelta);
    writer.varint(static_cast<int32_t>(executable.m_unlinkedBodyEndColumn - executable.m_unlinkedFunctionEnd) - bodyStartColumnDelta);
    writer.varuint(executable.m_parameterCount);
    if (header & HasLines) {
        writer.varuint(executable.m_firstLineOffset);
        writer.varuint(executable.m_lineCount);
    }
    if (header & HasName)
        CachedString::encode(writer, encoder, executable.ecmaName());
    if (header & HasTDZ)
        CachedTDZEnvironmentLink::encodeRef(writer, encoder, executable.m_parentScopeTDZVariables.get());
    if (header & HasRareData)
        CachedFunctionExecutableRareData::encode(writer, encoder, *executable.m_rareData);

    if (header & HasChecksum)
        writer.patchU32(extentAt, writer.size());
    uint32_t at = encoder.append(writer);
    if (header & HasChecksum)
        encoder.addChecksum(at, writer.size(), at + checksumAt);
    if (!executable.m_unlinkedCodeBlockForCall || !executable.m_unlinkedCodeBlockForConstruct)
        encoder.addLeafExecutable(&executable, at);

    encoder.deferBody([&encoder, at, callBlockAt, constructBlockAt, forCall = executable.m_unlinkedCodeBlockForCall.get(), forConstruct = executable.m_unlinkedCodeBlockForConstruct.get()] {
        if (forCall)
            encoder.patchU32(at + callBlockAt, encodeFunctionCodeBlockRecord(encoder, *forCall));
        if (forConstruct)
            encoder.patchU32(at + constructBlockAt, encodeFunctionCodeBlockRecord(encoder, *forConstruct));
    });
    return at;
}

CachedFunctionExecutable::CachedFunctionExecutable(Decoder& decoder, uint32_t at)
    : CachedFunctionExecutable(decoder, at, Mode::Decode)
{
}

bool CachedFunctionExecutable::isIntact(Decoder& decoder, uint32_t at)
{
    return CachedFunctionExecutable(decoder, at, Mode::Check).intact;
}

CachedFunctionExecutable::CachedFunctionExecutable(Decoder& decoder, uint32_t at, Mode mode)
    : at(at)
{
    Reader reader(decoder.payload(), at);
    header = reader.u32();
    uint32_t checksumFieldAt = 0;
    uint32_t extent = 0;
    if (header & Updatable) {
        // The jsc shell's disk cache patches these after a lazily compiled function joins the cache.
        features = reader.u32();
        lexicallyScopedFeatures = reader.u8();
        hasCapturedVariables = reader.boolean();
        reader.u16();
    }
    if (header & HasChecksum) {
        checksumFieldAt = reader.offset();
        reader.u32();
        extent = reader.u32();
    }
    if (header & HasCallBlock)
        callBlockAt = reader.u32();
    if (header & HasConstructBlock)
        constructBlockAt = reader.u32();
    if (reader.overran())
        return;
    if (mode == Mode::Check && (header & HasChecksum)) {
        // Nothing past the fixed fields is read before the checksum has been verified.
        intact = decoder.checksumMatches(at, extent, checksumFieldAt);
        return;
    }

    uint32_t flags = reader.varuint();
    auto bits = [&](unsigned shift, unsigned width = 1) { return (flags >> shift) & ((1u << width) - 1); };
    scriptMode = bits(ScriptModeShift);
    superBinding = bits(SuperBindingShift);
    constructAbility = bits(ConstructAbilityShift);
    hasName = bits(HasNameFlagShift);
    constructorKind = bits(ConstructorKindShift, 2);
    functionMode = bits(FunctionModeShift, 2);
    implementationVisibility = static_cast<ImplementationVisibility>(bits(ImplementationVisibilityShift, 2));
    derivedContextType = bits(DerivedContextTypeShift, 2);
    evalContextType = bits(EvalContextTypeShift, 2);
    privateBrandRequirement = bits(PrivateBrandRequirementShift);
    inlineAttribute = bits(InlineAttributeShift);
    needsClassFieldInitializer = bits(NeedsClassFieldInitializerShift);
    isBuiltinFunction = bits(IsBuiltinFunctionShift);
    isBuiltinDefaultClassConstructor = bits(IsBuiltinDefaultClassConstructorShift);
    CodeFeatures generatedFeatures = reader.varuint();
    LexicallyScopedFeatures generatedLexicallyScopedFeatures = reader.u8();
    if (!(header & Updatable)) {
        features = generatedFeatures;
        lexicallyScopedFeatures = generatedLexicallyScopedFeatures;
        hasCapturedVariables = header & HasCapturedVariables;
    }
    sourceParseMode = static_cast<SourceParseMode>((header >> ParseModeShift) & 0xff);
    startOffset = reader.varuint();
    unlinkedFunctionStart = startOffset + reader.varint();
    parametersStartOffset = startOffset + reader.varint();
    sourceLength = reader.varuint();
    unlinkedFunctionEnd = startOffset + sourceLength + reader.varint();
    int32_t bodyStartColumnDelta = reader.varint();
    unlinkedBodyStartColumn = unlinkedFunctionStart + bodyStartColumnDelta;
    unlinkedBodyEndColumn = unlinkedFunctionEnd + bodyStartColumnDelta + reader.varint();
    parameterCount = reader.varuint();
    if (header & HasLines) {
        firstLineOffset = reader.varuint();
        lineCount = reader.varuint();
    }
    if (mode == Mode::Check) {
        intact = !reader.overran();
        return;
    }
    if (header & HasName)
        name = Identifier::fromUid(decoder.vm(), CachedString::decodeNonNull(reader, decoder).get());
    if (header & HasTDZ)
        parentScopeTDZVariables = CachedTDZEnvironmentLink::decodeRef(reader, decoder);
    if (header & HasRareData)
        rareData = CachedFunctionExecutableRareData::decode(reader, decoder);
    intact = !reader.overran();
}

ALWAYS_INLINE UnlinkedFunctionExecutable* CachedFunctionExecutable::decode(Decoder& decoder) const
{
    UnlinkedFunctionExecutable* executable = new (NotNull, allocateCell<UnlinkedFunctionExecutable>(decoder.vm())) UnlinkedFunctionExecutable(decoder, *this);
    executable->finishCreation(decoder.vm());
    return executable;
}

ALWAYS_INLINE UnlinkedFunctionExecutable::UnlinkedFunctionExecutable(Decoder& decoder, const CachedFunctionExecutable& record)
    : Base(decoder.vm(), decoder.vm().unlinkedFunctionExecutableStructure.get())
    , m_firstLineOffset(record.firstLineOffset)
    , m_isGeneratedFromCache(true)
    , m_lineCount(record.lineCount)
    , m_hasCapturedVariables(record.hasCapturedVariables)
    , m_unlinkedFunctionStart(record.unlinkedFunctionStart)
    , m_isBuiltinFunction(record.isBuiltinFunction)
    , m_unlinkedBodyStartColumn(record.unlinkedBodyStartColumn)
    , m_isBuiltinDefaultClassConstructor(record.isBuiltinDefaultClassConstructor)
    , m_unlinkedBodyEndColumn(record.unlinkedBodyEndColumn)
    , m_constructAbility(record.constructAbility)
    , m_startOffset(record.startOffset)
    , m_scriptMode(record.scriptMode)
    , m_sourceLength(record.sourceLength)
    , m_superBinding(record.superBinding)
    , m_parametersStartOffset(record.parametersStartOffset)
    , m_isCached(false)
    , m_unlinkedFunctionEnd(record.unlinkedFunctionEnd)
    , m_needsClassFieldInitializer(record.needsClassFieldInitializer)
    , m_parameterCount(record.parameterCount)
    , m_singletonHasBeenInvalidated(false)
    , m_privateBrandRequirement(record.privateBrandRequirement)
    , m_features(record.features)
    , m_constructorKind(record.constructorKind)
    , m_sourceParseMode(record.sourceParseMode)
    , m_implementationVisibility(static_cast<unsigned>(record.implementationVisibility))
    , m_lexicallyScopedFeatures(record.lexicallyScopedFeatures)
    , m_functionMode(record.functionMode)
    , m_derivedContextType(record.derivedContextType)
    , m_inlineAttribute(record.inlineAttribute)
    , m_evalContextType(record.evalContextType)
    , m_hasName(record.hasName)
    , m_unlinkedCodeBlockForCall()
    , m_unlinkedCodeBlockForConstruct()
    , m_ecmaName(record.name)
    , m_parentScopeTDZVariables(record.parentScopeTDZVariables)
    , m_rareData(WTF::move(record.rareData))
{
    if (record.callBlockAt || record.constructBlockAt) {
        m_isCached = true;
        m_decoder = &decoder;
        m_cachedCodeBlockForCallOffset = record.callBlockAt;
        m_cachedCodeBlockForConstructOffset = record.constructBlockAt;
    }
    if (!record.callBlockAt || !record.constructBlockAt)
        decoder.addLeafExecutable(this, record.at);
}

// -- Code blocks ------------------------------------------------------------------------------------------------------

class CachedProgramCodeBlock;
class CachedModuleCodeBlock;
class CachedEvalCodeBlock;
class CachedFunctionCodeBlock;
template<typename CodeBlockType> struct CachedCodeBlockTypeFor;
template<> struct CachedCodeBlockTypeFor<UnlinkedProgramCodeBlock> { using type = CachedProgramCodeBlock; };
template<> struct CachedCodeBlockTypeFor<UnlinkedModuleProgramCodeBlock> { using type = CachedModuleCodeBlock; };
template<> struct CachedCodeBlockTypeFor<UnlinkedEvalCodeBlock> { using type = CachedEvalCodeBlock; };
template<> struct CachedCodeBlockTypeFor<UnlinkedFunctionCodeBlock> { using type = CachedFunctionCodeBlock; };
template<typename CodeBlockType> using CachedCodeBlockType = typename CachedCodeBlockTypeFor<CodeBlockType>::type;

// A code block is a region: its arrays, then the record everything points at, then its children's executable records.
//
//   [steps]            4-aligned, count × u32        the metadata table as (opcode << 24 | entries); see CachedMetadataSteps
//   [instructions]     count bytes
//   [representations]  count × u8                    constants' SourceCodeRepresentation
//   [constants]        count × JSValue
//   [identifiers]      count × StringRef
//   [rare data]        CodeBlockRareData
//   [jump targets]     OutOfLineJumpTargets
//   [own members]      what the kind of code block adds (GlobalCodeBlock below; nothing for a function)
//   [child slots]      (functionDecls + functionExprs) × u32   offsets of the children's executable records, patched
//   record:
//     u32 expressionInfoAt                            patched in the cold pass
//     Layout: u8 flags, varuint recordOffsetInRegion, [varuint metadataValueProfiles], 7 × Array, [varint rareDataAt],
//             [varint jumpTargetsAt], [varint ownAt]     Array := varuint count, [varint at]; offsets are from the region's start
//     Scalars: varuint flags, u8 parseMode, u8 codeGenerationMode, varint thisRegister, varint scopeRegister,
//              varint numVars, varint numCalleeLocals, varint numParameters, varuint × 4 profile counts
//     [u32 regionSize, u32 checksum]                  the checksum covers [region, region + regionSize)
//
// The first three arrays may be shared with an identical one an earlier block wrote (their `at` is then negative);
// regionIsIntact() folds those into this block's checksum in that order.
template<typename CodeBlockType>
class CachedCodeBlock {
public:
    enum LayoutFlag : uint8_t {
        LayoutHasMetadata = 1 << 0,
        LayoutHasRareData = 1 << 1,
        LayoutHasJumpTargets = 1 << 2,
        LayoutHasOwnMembers = 1 << 3,
        LayoutHasChecksum = 1 << 4,
    };
    enum Flag : uint32_t {
        IsConstructorShift = 0,
        SuperBindingShift = 1,
        ScriptModeShift = 2,
        IsArrowFunctionContextShift = 3,
        IsClassContextShift = 4,
        HasTailCallsShift = 5,
        HasCheckpointsShift = 6,
        ConstructorKindShift = 7, // 2 bits
        DerivedContextTypeShift = 9, // 2
        EvalContextTypeShift = 11, // 2
        CodeTypeShift = 13, // 2
        IsBuiltinFunctionShift = 15,
        IsBuiltinDefaultClassConstructorShift = 16,
    };
    struct Array {
        unsigned count { 0 };
        int32_t at { 0 }; // from the region start; only meaningful when count is non-zero
    };
    struct Layout {
        uint8_t flags { 0 };
        unsigned recordOffsetInRegion { 0 };
        unsigned metadataValueProfiles { 0 };
        Array steps;
        Array instructions; // count is in bytes
        Array representations;
        Array constants;
        Array identifiers;
        Array functionDecls;
        Array functionExprs;
        int32_t rareDataAt { 0 };
        int32_t jumpTargetsAt { 0 };
        int32_t ownAt { 0 };
    };
    struct Scalars {
        VirtualRegister thisRegister;
        VirtualRegister scopeRegister;
        bool isConstructor { false };
        bool isBuiltinDefaultClassConstructor { false };
        bool isBuiltinFunction { false };
        bool superBinding { false };
        bool scriptMode { false };
        bool isArrowFunctionContext { false };
        bool isClassContext { false };
        bool hasTailCalls { false };
        bool hasCheckpoints { false };
        unsigned constructorKind { 0 };
        unsigned derivedContextType { 0 };
        unsigned evalContextType { 0 };
        unsigned codeType { 0 };
        SourceParseMode parseMode { };
        OptionSet<CodeGenerationMode> codeGenerationMode;
        int numVars { 0 };
        int numCalleeLocals { 0 };
        int numParameters { 0 };
        unsigned numValueProfiles { 0 };
        unsigned numArrayProfiles { 0 };
        unsigned numBinaryArithProfiles { 0 };
        unsigned numUnaryArithProfiles { 0 };
    };

    // Writes the region and returns the record's offset.
    static uint32_t encode(Encoder&, const CodeBlockType&);

    // The record at `at`, parsed. Nothing is decoded until regionIsIntact() has passed.
    CachedCodeBlock(Decoder&, uint32_t at);
    bool regionIsIntact(Decoder&);
    CodeBlockType* decode(Decoder&) const;

    const Scalars& scalars() const { return m_scalars; }
    JSInstructionStream* instructions(Decoder&) const;
    Ref<UnlinkedMetadataTable> metadata(Decoder&) const;
    UnlinkedCodeBlock::RareData* rareData(Decoder&) const;

protected:
    Reader arrayReader(Decoder& decoder, const Array& array) const { return Reader(decoder.payload(), m_regionAt + array.at); }
    Reader ownMembersReader(Decoder& decoder) const { return Reader(decoder.payload(), m_regionAt + m_layout.ownAt); }
    // What the kind of code block adds; a function block has nothing.
    static void encodeOwnMembers(Writer&, Encoder&, const CodeBlockType&) { }
    bool decodeOwnMembers(Decoder&, CodeBlockType&) const { return true; }

private:
    bool decodeArrays(Decoder&, UnlinkedCodeBlock&) const; // false if anything read past its bounds

    Layout m_layout;
    Scalars m_scalars;
    uint32_t m_recordAt { 0 };
    uint32_t m_regionAt { 0 };
    uint32_t m_expressionInfoAt { 0 };
    uint32_t m_trailerAt { 0 }; // where the region size and checksum are, when present
    bool m_parsed { false };
    bool m_verified { false };
};

// GlobalCodeBlock own members := varuint features, u8 lexicallyScopedFeatures, u8 hasCapturedVariables, varuint lineCount,
//                                varuint endColumn, StringRef sourceURLDirective, StringRef sourceMappingURLDirective
//   Program adds:  VariableEnvironment varDeclarations, VariableEnvironment lexicalDeclarations
//   Module adds:   VariableEnvironment varDeclarations, varint moduleEnvironmentSymbolTableConstantRegisterOffset
//   Eval adds:     varuint count, count × StringRef variables, varuint count, count × StringRef functionHoistingCandidates
template<typename CodeBlockType>
class CachedGlobalCodeBlock : public CachedCodeBlock<CodeBlockType> {
public:
    using CachedCodeBlock<CodeBlockType>::CachedCodeBlock;

protected:
    static void encodeOwnMembers(Writer& writer, Encoder& encoder, const UnlinkedGlobalCodeBlock& codeBlock)
    {
        writer.varuint(codeBlock.m_features);
        writer.u8(codeBlock.m_lexicallyScopedFeatures);
        writer.boolean(codeBlock.m_hasCapturedVariables);
        writer.varuint(codeBlock.m_lineCount);
        writer.varuint(codeBlock.m_endColumn);
        CachedString::encode(writer, encoder, codeBlock.m_sourceURLDirective.get());
        CachedString::encode(writer, encoder, codeBlock.m_sourceMappingURLDirective.get());
    }
    void decodeOwnMembers(Reader& reader, Decoder& decoder, UnlinkedGlobalCodeBlock& codeBlock) const
    {
        codeBlock.m_features = reader.varuint();
        codeBlock.m_lexicallyScopedFeatures = reader.u8();
        codeBlock.m_hasCapturedVariables = reader.boolean();
        codeBlock.m_lineCount = reader.varuint();
        codeBlock.m_endColumn = reader.varuint();
        codeBlock.m_sourceURLDirective = CachedString::decodeString(reader, decoder).releaseImpl();
        codeBlock.m_sourceMappingURLDirective = CachedString::decodeString(reader, decoder).releaseImpl();
    }
};

class CachedProgramCodeBlock final : public CachedGlobalCodeBlock<UnlinkedProgramCodeBlock> {
    using Base = CachedGlobalCodeBlock<UnlinkedProgramCodeBlock>;
    friend CachedCodeBlock<UnlinkedProgramCodeBlock>;

public:
    using Base::Base;

private:
    UnlinkedProgramCodeBlock* construct(Decoder& decoder) const { return new (NotNull, allocateCell<UnlinkedProgramCodeBlock>(decoder.vm())) UnlinkedProgramCodeBlock(decoder, *this); }
    static void encodeOwnMembers(Writer& writer, Encoder& encoder, const UnlinkedProgramCodeBlock& codeBlock)
    {
        Base::encodeOwnMembers(writer, encoder, codeBlock);
        CachedVariableEnvironment::encode(writer, encoder, codeBlock.m_varDeclarations);
        CachedVariableEnvironment::encode(writer, encoder, codeBlock.m_lexicalDeclarations);
    }
    bool decodeOwnMembers(Decoder& decoder, UnlinkedProgramCodeBlock& codeBlock) const
    {
        Reader reader = ownMembersReader(decoder);
        Base::decodeOwnMembers(reader, decoder, codeBlock);
        CachedVariableEnvironment::decode(reader, decoder, codeBlock.m_varDeclarations);
        CachedVariableEnvironment::decode(reader, decoder, codeBlock.m_lexicalDeclarations);
        return !reader.overran();
    }
};

class CachedModuleCodeBlock final : public CachedGlobalCodeBlock<UnlinkedModuleProgramCodeBlock> {
    using Base = CachedGlobalCodeBlock<UnlinkedModuleProgramCodeBlock>;
    friend CachedCodeBlock<UnlinkedModuleProgramCodeBlock>;

public:
    using Base::Base;

private:
    UnlinkedModuleProgramCodeBlock* construct(Decoder& decoder) const { return new (NotNull, allocateCell<UnlinkedModuleProgramCodeBlock>(decoder.vm())) UnlinkedModuleProgramCodeBlock(decoder, *this); }
    static void encodeOwnMembers(Writer& writer, Encoder& encoder, const UnlinkedModuleProgramCodeBlock& codeBlock)
    {
        Base::encodeOwnMembers(writer, encoder, codeBlock);
        CachedVariableEnvironment::encode(writer, encoder, codeBlock.m_varDeclarations);
        writer.varint(codeBlock.m_moduleEnvironmentSymbolTableConstantRegisterOffset);
    }
    bool decodeOwnMembers(Decoder& decoder, UnlinkedModuleProgramCodeBlock& codeBlock) const
    {
        Reader reader = ownMembersReader(decoder);
        Base::decodeOwnMembers(reader, decoder, codeBlock);
        CachedVariableEnvironment::decode(reader, decoder, codeBlock.m_varDeclarations);
        codeBlock.m_moduleEnvironmentSymbolTableConstantRegisterOffset = reader.varint();
        return !reader.overran();
    }
};

class CachedEvalCodeBlock final : public CachedGlobalCodeBlock<UnlinkedEvalCodeBlock> {
    using Base = CachedGlobalCodeBlock<UnlinkedEvalCodeBlock>;
    friend CachedCodeBlock<UnlinkedEvalCodeBlock>;

public:
    using Base::Base;

private:
    UnlinkedEvalCodeBlock* construct(Decoder& decoder) const { return new (NotNull, allocateCell<UnlinkedEvalCodeBlock>(decoder.vm())) UnlinkedEvalCodeBlock(decoder, *this); }
    static void encodeOwnMembers(Writer& writer, Encoder& encoder, const UnlinkedEvalCodeBlock& codeBlock)
    {
        Base::encodeOwnMembers(writer, encoder, codeBlock);
        writer.varuint(codeBlock.m_variables.size());
        for (auto& variable : codeBlock.m_variables)
            CachedString::encode(writer, encoder, variable);
        writer.varuint(codeBlock.m_functionHoistingCandidates.size());
        for (auto& candidate : codeBlock.m_functionHoistingCandidates)
            CachedString::encode(writer, encoder, candidate);
    }
    bool decodeOwnMembers(Decoder& decoder, UnlinkedEvalCodeBlock& codeBlock) const
    {
        Reader reader = ownMembersReader(decoder);
        Base::decodeOwnMembers(reader, decoder, codeBlock);
        auto identifiers = [&](auto& out) {
            unsigned count = reader.varuint();
            if (!reader.checkCount(count))
                return;
            out = std::remove_reference_t<decltype(out)>(count);
            for (auto& identifier : out)
                identifier = CachedString::decodeIdentifier(reader, decoder);
        };
        identifiers(codeBlock.m_variables);
        identifiers(codeBlock.m_functionHoistingCandidates);
        return !reader.overran();
    }
};

class CachedFunctionCodeBlock final : public CachedCodeBlock<UnlinkedFunctionCodeBlock> {
    friend CachedCodeBlock<UnlinkedFunctionCodeBlock>;

public:
    using CachedCodeBlock<UnlinkedFunctionCodeBlock>::CachedCodeBlock;

private:
    UnlinkedFunctionCodeBlock* construct(Decoder& decoder) const { return new (NotNull, allocateCell<UnlinkedFunctionCodeBlock>(decoder.vm())) UnlinkedFunctionCodeBlock(decoder, *this); }
};

static uint32_t encodeFunctionCodeBlockRecord(Encoder& encoder, const UnlinkedFunctionCodeBlock& codeBlock)
{
    return CachedFunctionCodeBlock::encode(encoder, codeBlock);
}

template<typename CodeBlockType>
uint32_t CachedCodeBlock<CodeBlockType>::encode(Encoder& encoder, const CodeBlockType& codeBlock)
{
    using Record = CachedCodeBlockType<CodeBlockType>;
    uint32_t regionAt = encoder.position();
    encoder.beginBlockRegion(regionAt);
    Layout layout;
    auto place = [&](Array& array, unsigned count, auto&& write) {
        array.count = count;
        if (count)
            array.at = safeCast<int32_t>(static_cast<int64_t>(write()) - regionAt);
    };
    // Values a stream refers to (string records, shared environments) are written before the stream; the stream itself
    // is composed aside and appended whole, so it is contiguous.
    auto placeStream = [&](int32_t& at, auto&& write) {
        Writer writer;
        write(writer);
        at = safeCast<int32_t>(static_cast<int64_t>(encoder.append(writer)) - regionAt);
    };

    const UnlinkedMetadataTable& metadata = codeBlock.m_metadata.get();
    if (metadata.m_hasMetadata) {
        layout.flags |= LayoutHasMetadata;
        layout.metadataValueProfiles = metadata.m_numValueProfiles;
        Writer steps;
        for (uint32_t step : CachedMetadataSteps::compute(metadata))
            steps.u32(step);
        place(layout.steps, steps.size() / 4, [&] { return encoder.appendBlockArray(steps.span(), 4); });
    }
    const JSInstructionStream& instructions = *codeBlock.m_instructions;
    RELEASE_ASSERT(!instructions.isBorrowed()); // a borrowed stream's bytes live in the payload being read
    place(layout.instructions, instructions.m_instructions.size(), [&] { return encoder.appendBlockArray(instructions.m_instructions.span()); });
    {
        static_assert(sizeof(SourceCodeRepresentation) == 1);
        auto& representations = codeBlock.m_constantsSourceCodeRepresentation;
        place(layout.representations, representations.size(), [&] { return encoder.appendBlockArray(std::span { reinterpret_cast<const uint8_t*>(representations.span().data()), representations.size() }); });
    }
    place(layout.constants, codeBlock.m_constantRegisters.size(), [&] {
        Writer writer;
        for (auto& constant : codeBlock.m_constantRegisters)
            CachedJSValue::encode(writer, encoder, constant.get());
        return encoder.append(writer);
    });
    place(layout.identifiers, codeBlock.m_identifiers.size(), [&] {
        Writer writer;
        for (auto& identifier : codeBlock.m_identifiers)
            CachedString::encode(writer, encoder, identifier);
        return encoder.append(writer);
    });
    if (codeBlock.m_rareData) {
        layout.flags |= LayoutHasRareData;
        placeStream(layout.rareDataAt, [&](Writer& writer) { CachedCodeBlockRareData::encode(writer, encoder, *codeBlock.m_rareData); });
    }
    if (!codeBlock.m_outOfLineJumpTargets.isEmpty()) {
        layout.flags |= LayoutHasJumpTargets;
        placeStream(layout.jumpTargetsAt, [&](Writer& writer) { CachedCodeBlockExtras::encodeOutOfLineJumpTargets(writer, codeBlock); });
    }
    if constexpr (!std::is_same_v<CodeBlockType, UnlinkedFunctionCodeBlock>) {
        layout.flags |= LayoutHasOwnMembers;
        placeStream(layout.ownAt, [&](Writer& writer) { Record::encodeOwnMembers(writer, encoder, codeBlock); });
    }
    // The children's slots are part of this block's bytes; the records they point at are written after the region.
    place(layout.functionDecls, codeBlock.m_functionDecls.size(), [&] { return encoder.appendU32Placeholders(codeBlock.m_functionDecls.size()); });
    place(layout.functionExprs, codeBlock.m_functionExprs.size(), [&] { return encoder.appendU32Placeholders(codeBlock.m_functionExprs.size()); });

    if (encoder.checksums())
        layout.flags |= LayoutHasChecksum;
    uint32_t recordAt = encoder.position();
    layout.recordOffsetInRegion = recordAt - regionAt;

    Writer record;
    size_t expressionInfoSlot = record.reserveU32();
    record.u8(layout.flags);
    record.varuint(layout.recordOffsetInRegion);
    if (layout.flags & LayoutHasMetadata)
        record.varuint(layout.metadataValueProfiles);
    for (const Array* array : { &layout.steps, &layout.instructions, &layout.representations, &layout.constants, &layout.identifiers, &layout.functionDecls, &layout.functionExprs }) {
        record.varuint(array->count);
        if (array->count)
            record.varint(array->at);
    }
    if (layout.flags & LayoutHasRareData)
        record.varint(layout.rareDataAt);
    if (layout.flags & LayoutHasJumpTargets)
        record.varint(layout.jumpTargetsAt);
    if (layout.flags & LayoutHasOwnMembers)
        record.varint(layout.ownAt);

    uint32_t flags = static_cast<uint32_t>(codeBlock.m_isConstructor) << IsConstructorShift
        | static_cast<uint32_t>(codeBlock.m_superBinding) << SuperBindingShift
        | static_cast<uint32_t>(codeBlock.m_scriptMode) << ScriptModeShift
        | static_cast<uint32_t>(codeBlock.m_isArrowFunctionContext) << IsArrowFunctionContextShift
        | static_cast<uint32_t>(codeBlock.m_isClassContext) << IsClassContextShift
        | static_cast<uint32_t>(codeBlock.m_hasTailCalls) << HasTailCallsShift
        | static_cast<uint32_t>(codeBlock.m_hasCheckpoints) << HasCheckpointsShift
        | static_cast<uint32_t>(codeBlock.m_constructorKind) << ConstructorKindShift
        | static_cast<uint32_t>(codeBlock.m_derivedContextType) << DerivedContextTypeShift
        | static_cast<uint32_t>(codeBlock.m_evalContextType) << EvalContextTypeShift
        | static_cast<uint32_t>(codeBlock.m_codeType) << CodeTypeShift
        | static_cast<uint32_t>(codeBlock.m_isBuiltinFunction) << IsBuiltinFunctionShift
        | static_cast<uint32_t>(codeBlock.m_isBuiltinDefaultClassConstructor) << IsBuiltinDefaultClassConstructorShift;
    record.varuint(flags);
    record.u8(static_cast<uint8_t>(codeBlock.m_parseMode));
    record.u8(codeBlock.m_codeGenerationMode.toRaw());
    record.varint(codeBlock.m_thisRegister.offset());
    record.varint(codeBlock.m_scopeRegister.offset());
    record.varint(codeBlock.m_numVars);
    record.varint(codeBlock.m_numCalleeLocals);
    record.varint(codeBlock.m_numParameters);
    record.varuint(codeBlock.m_valueProfiles.size());
    record.varuint(codeBlock.m_arrayProfiles.size());
    record.varuint(codeBlock.m_binaryArithProfiles.size());
    record.varuint(codeBlock.m_unaryArithProfiles.size());
    size_t trailer = 0;
    if (layout.flags & LayoutHasChecksum) {
        trailer = record.reserveU32();
        record.reserveU32();
    }
    encoder.append(record);
    uint32_t regionSize = encoder.position() - regionAt;
    if (layout.flags & LayoutHasChecksum) {
        encoder.patchU32(recordAt + trailer, regionSize);
        encoder.addChecksum(regionAt, regionSize, recordAt + trailer + 4, encoder.takeBlockExternalArrays());
    } else
        encoder.takeBlockExternalArrays();

    encoder.deferCold([&encoder, slot = recordAt + expressionInfoSlot, expressionInfo = codeBlock.m_expressionInfo.get()] {
        auto bytes = CachedExpressionInfo::pack(*expressionInfo, encoder.checksums());
        encoder.patchU32(slot, encoder.appendBytesOnce(bytes.span(), 4));
    });

    auto encodeChildren = [&](const Array& slots, const auto& executables) {
        for (unsigned i = 0; i < slots.count; ++i)
            encoder.patchU32(regionAt + slots.at + 4 * i, CachedFunctionExecutable::encode(encoder, *executables[i].get()));
    };
    encodeChildren(layout.functionDecls, codeBlock.m_functionDecls);
    encodeChildren(layout.functionExprs, codeBlock.m_functionExprs);
    return recordAt;
}

template<typename CodeBlockType>
CachedCodeBlock<CodeBlockType>::CachedCodeBlock(Decoder& decoder, uint32_t at)
    : m_recordAt(at)
{
    Reader reader(decoder.payload(), at);
    m_expressionInfoAt = reader.u32();
    Layout& layout = m_layout;
    layout.flags = reader.u8();
    layout.recordOffsetInRegion = reader.varuint();
    if (layout.flags & LayoutHasMetadata)
        layout.metadataValueProfiles = reader.varuint();
    for (Array* array : { &layout.steps, &layout.instructions, &layout.representations, &layout.constants, &layout.identifiers, &layout.functionDecls, &layout.functionExprs }) {
        array->count = reader.varuint();
        if (array->count)
            array->at = reader.varint();
    }
    if (layout.flags & LayoutHasRareData)
        layout.rareDataAt = reader.varint();
    if (layout.flags & LayoutHasJumpTargets)
        layout.jumpTargetsAt = reader.varint();
    if (layout.flags & LayoutHasOwnMembers)
        layout.ownAt = reader.varint();

    Scalars& s = m_scalars;
    uint32_t flags = reader.varuint();
    auto bits = [&](unsigned shift, unsigned width = 1) -> unsigned { return (flags >> shift) & ((1u << width) - 1); };
    s.isConstructor = bits(IsConstructorShift);
    s.superBinding = bits(SuperBindingShift);
    s.scriptMode = bits(ScriptModeShift);
    s.isArrowFunctionContext = bits(IsArrowFunctionContextShift);
    s.isClassContext = bits(IsClassContextShift);
    s.hasTailCalls = bits(HasTailCallsShift);
    s.hasCheckpoints = bits(HasCheckpointsShift);
    s.constructorKind = bits(ConstructorKindShift, 2);
    s.derivedContextType = bits(DerivedContextTypeShift, 2);
    s.evalContextType = bits(EvalContextTypeShift, 2);
    s.codeType = bits(CodeTypeShift, 2);
    s.isBuiltinFunction = bits(IsBuiltinFunctionShift);
    s.isBuiltinDefaultClassConstructor = bits(IsBuiltinDefaultClassConstructorShift);
    s.parseMode = static_cast<SourceParseMode>(reader.u8());
    s.codeGenerationMode = OptionSet<CodeGenerationMode>::fromRaw(reader.u8());
    s.thisRegister = VirtualRegister(reader.varint());
    s.scopeRegister = VirtualRegister(reader.varint());
    s.numVars = reader.varint();
    s.numCalleeLocals = reader.varint();
    s.numParameters = reader.varint();
    s.numValueProfiles = reader.varuint();
    s.numArrayProfiles = reader.varuint();
    s.numBinaryArithProfiles = reader.varuint();
    s.numUnaryArithProfiles = reader.varuint();
    m_trailerAt = reader.offset();
    m_parsed = !reader.overran() && layout.recordOffsetInRegion <= at;
    m_regionAt = at - layout.recordOffsetInRegion;
}

// The region is checksummed when the payload carries checksums; a mismatch means the payload is damaged and the block
// is generated from source instead. Without them the check is that everything the record locates lies inside the payload.
template<typename CodeBlockType>
bool CachedCodeBlock<CodeBlockType>::regionIsIntact(Decoder& decoder)
{
    if (!m_parsed)
        return false;
    const Layout& layout = m_layout;
    size_t payloadSize = decoder.payload().size();
    size_t regionEnd = payloadSize; // bound for the arrays when there is no stored region size
    if (layout.flags & LayoutHasChecksum) {
        Reader trailer(decoder.payload(), m_trailerAt);
        uint32_t regionSize = trailer.u32();
        uint32_t checksumAt = m_trailerAt + 4;
        if (trailer.overran() || regionSize < checksumAt + 4 - m_regionAt)
            return false;
        regionEnd = m_regionAt + regionSize;
    }

    // Every array must lie inside the region, except the three the encoder may have shared from an earlier block,
    // which are folded into the checksum instead (in encoder order).
    std::array<std::pair<size_t, size_t>, 3> external;
    unsigned externalCount = 0;
    auto located = [&](const Array& array, size_t bytes, bool shareable) {
        if (!array.count)
            return true;
        int64_t start = static_cast<int64_t>(m_regionAt) + array.at;
        if (start >= m_regionAt && start + static_cast<int64_t>(bytes) <= static_cast<int64_t>(regionEnd))
            return true;
        if (!shareable || start < 0 || !decoder.payloadContains(start, bytes))
            return false;
        external[externalCount++] = { static_cast<size_t>(start), bytes };
        return true;
    };
    // A stream's length is only known by reading it; its start is checked here and every read is bounds-checked.
    auto streamLocated = [&](bool present, int32_t at) {
        return !present || (at >= 0 && m_regionAt + at < regionEnd);
    };
    if (!located(layout.steps, 4 * static_cast<size_t>(layout.steps.count), true)
        || !located(layout.instructions, layout.instructions.count, true)
        || !located(layout.representations, layout.representations.count, true)
        || !streamLocated(layout.constants.count, layout.constants.at)
        || !streamLocated(layout.identifiers.count, layout.identifiers.at)
        || !located(layout.functionDecls, 4 * static_cast<size_t>(layout.functionDecls.count), false)
        || !located(layout.functionExprs, 4 * static_cast<size_t>(layout.functionExprs.count), false)
        || !streamLocated(layout.flags & LayoutHasRareData, layout.rareDataAt)
        || !streamLocated(layout.flags & LayoutHasJumpTargets, layout.jumpTargetsAt)
        || !streamLocated(layout.flags & LayoutHasOwnMembers, layout.ownAt)
        || (layout.steps.count && (m_regionAt + layout.steps.at) % 4))
        return false;
    if ((layout.flags & LayoutHasChecksum) && !decoder.checksumMatches(m_regionAt, regionEnd - m_regionAt, m_trailerAt + 4, std::span { external.data(), externalCount }))
        return false;

    for (const Array* children : { &layout.functionDecls, &layout.functionExprs }) {
        Reader slots = arrayReader(decoder, *children);
        for (unsigned i = 0; i < children->count; ++i) {
            uint32_t recordAt = slots.u32();
            if (!recordAt || !CachedFunctionExecutable::isIntact(decoder, recordAt))
                return false;
        }
    }
    m_verified = true;
    return true;
}

template<typename CodeBlockType>
JSInstructionStream* CachedCodeBlock<CodeBlockType>::instructions(Decoder& decoder) const
{
    auto bytes = arrayReader(decoder, m_layout.instructions).bytes(m_layout.instructions.count);
    if (decoder.canBorrowPayload())
        return new JSInstructionStream(bytes, JSInstructionStream::Borrow);
    Vector<uint8_t, 0, UnsafeVectorOverflow, 16, InstructionStreamBufferMalloc> copy;
    copy.append(bytes);
    return new JSInstructionStream(WTF::move(copy));
}

template<typename CodeBlockType>
Ref<UnlinkedMetadataTable> CachedCodeBlock<CodeBlockType>::metadata(Decoder& decoder) const
{
    const Layout& layout = m_layout;
    if (!(layout.flags & LayoutHasMetadata))
        return UnlinkedMetadataTable::empty();
    auto bytes = arrayReader(decoder, layout.steps).bytes(4 * static_cast<size_t>(layout.steps.count));
    std::span<const uint32_t> steps { reinterpret_cast<const uint32_t*>(bytes.data()), layout.steps.count }; // little-endian host, 4-aligned: see the top of the file
    if (decoder.canBorrowPayload())
        return UnlinkedMetadataTable::createFromPersistentSteps(layout.metadataValueProfiles, steps);
    return CachedMetadataSteps::build(layout.metadataValueProfiles, steps);
}

template<typename CodeBlockType>
UnlinkedCodeBlock::RareData* CachedCodeBlock<CodeBlockType>::rareData(Decoder& decoder) const
{
    if (!(m_layout.flags & LayoutHasRareData))
        return nullptr;
    Reader reader(decoder.payload(), m_regionAt + m_layout.rareDataAt);
    return CachedCodeBlockRareData::decode(reader, decoder);
}

template<typename CodeBlockType>
ALWAYS_INLINE UnlinkedCodeBlock::UnlinkedCodeBlock(Decoder& decoder, Structure* structure, const CachedCodeBlock<CodeBlockType>& cachedCodeBlock)
    : Base(decoder.vm(), structure)
    , m_age(0)
    , m_metadata(cachedCodeBlock.metadata(decoder))
    , m_instructions(cachedCodeBlock.instructions(decoder))
    , m_rareData(cachedCodeBlock.rareData(decoder))
{
    const auto& scalars = cachedCodeBlock.scalars();
    m_thisRegister = scalars.thisRegister;
    m_scopeRegister = scalars.scopeRegister;
    m_numVars = scalars.numVars;
    m_numCalleeLocals = scalars.numCalleeLocals;
    m_isConstructor = scalars.isConstructor;
    m_numParameters = scalars.numParameters;
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
    m_parseMode = scalars.parseMode;
    m_codeGenerationMode = scalars.codeGenerationMode;
    m_valueProfiles = FixedVector<UnlinkedValueProfile>(scalars.numValueProfiles);
    m_arrayProfiles = FixedVector<UnlinkedArrayProfile>(scalars.numArrayProfiles);
    m_binaryArithProfiles = FixedVector<BinaryArithProfile>(scalars.numBinaryArithProfiles);
    m_unaryArithProfiles = FixedVector<UnaryArithProfile>(scalars.numUnaryArithProfiles);
    m_llintExecuteCounter.setNewThreshold(thresholdForJIT(Options::thresholdForJITAfterWarmUp()));
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

ALWAYS_INLINE UnlinkedFunctionCodeBlock::UnlinkedFunctionCodeBlock(Decoder& decoder, const CachedFunctionCodeBlock& cachedCodeBlock)
    : Base(decoder, decoder.vm().unlinkedFunctionCodeBlockStructure.get(), cachedCodeBlock)
{
}

template<typename CodeBlockType>
bool CachedCodeBlock<CodeBlockType>::decodeArrays(Decoder& decoder, UnlinkedCodeBlock& codeBlock) const
{
    VM& vm = decoder.vm();
    const Layout& layout = m_layout;
    bool intact = true;
    // Most identifiers and many constants become atoms; let the table grow once for this block rather than as they trickle in.
    if (unsigned expected = layout.identifiers.count + layout.constants.count; expected >= 64)
        AtomStringImpl::reserveCapacityForCurrentThread(expected);
    if (layout.constants.count) {
        Reader reader = arrayReader(decoder, layout.constants);
        codeBlock.m_constantRegisters = FixedVector<WriteBarrier<Unknown>>(layout.constants.count);
        for (auto& constant : codeBlock.m_constantRegisters)
            constant.set(vm, &codeBlock, CachedJSValue::decode(reader, decoder));
        intact &= !reader.overran();
    }
    if (layout.representations.count) {
        auto bytes = arrayReader(decoder, layout.representations).bytes(layout.representations.count);
        codeBlock.m_constantsSourceCodeRepresentation = FixedVector<SourceCodeRepresentation>(bytes.size());
        for (size_t i = 0; i < bytes.size(); ++i)
            codeBlock.m_constantsSourceCodeRepresentation[i] = static_cast<SourceCodeRepresentation>(bytes[i]);
    }
    codeBlock.m_expressionInfo = CachedExpressionInfo::decode(decoder, m_expressionInfoAt);
    if (layout.flags & LayoutHasJumpTargets) {
        Reader reader(decoder.payload(), m_regionAt + layout.jumpTargetsAt);
        CachedCodeBlockExtras::decodeOutOfLineJumpTargets(reader, codeBlock);
        intact &= !reader.overran();
    }
    if (layout.identifiers.count) {
        Reader reader = arrayReader(decoder, layout.identifiers);
        codeBlock.m_identifiers = FixedVector<Identifier>(layout.identifiers.count);
        for (auto& identifier : codeBlock.m_identifiers)
            identifier = Identifier::fromUid(vm, CachedString::decodeNonNull(reader, decoder).get());
        intact &= !reader.overran();
    }
    auto children = [&](const Array& slots, auto& executables, bool declarations) {
        if (!slots.count)
            return;
        Reader reader = arrayReader(decoder, slots);
        executables = FixedVector<WriteBarrier<UnlinkedFunctionExecutable>>(slots.count);
        for (auto& executable : executables) {
            CachedFunctionExecutable record(decoder, reader.u32());
            intact &= record.intact && (!declarations || !record.name.isNull());
            executable.set(vm, &codeBlock, record.decode(decoder));
        }
    };
    children(layout.functionDecls, codeBlock.m_functionDecls, true);
    children(layout.functionExprs, codeBlock.m_functionExprs, false);
    return intact;
}

template<typename CodeBlockType>
CodeBlockType* CachedCodeBlock<CodeBlockType>::decode(Decoder& decoder) const
{
    RELEASE_ASSERT(m_verified);
    auto& record = static_cast<const CachedCodeBlockType<CodeBlockType>&>(*this);
    CodeBlockType* codeBlock = record.construct(decoder);
    codeBlock->finishCreation(decoder.vm());
    if (!decodeArrays(decoder, *codeBlock) || !record.decodeOwnMembers(decoder, *codeBlock)) {
        dataLogLnIf(Options::verboseDiskCache(), "[Disk Cache] code block record damaged; regenerating from source");
        return nullptr;
    }
    return codeBlock;
}

template<typename CodeBlockType>
static CodeBlockType* decodeCodeBlockRecord(Decoder& decoder, uint32_t at)
{
    CachedCodeBlockType<CodeBlockType> record(decoder, at);
    if (!record.regionIsIntact(decoder))
        return nullptr;
    return record.decode(decoder);
}

void decodeFunctionCodeBlock(Decoder& decoder, uint32_t at, WriteBarrier<UnlinkedFunctionCodeBlock>& codeBlock, const JSCell* owner)
{
    ASSERT(decoder.vm().heap.isDeferred());
    if (UnlinkedFunctionCodeBlock* decoded = decodeCodeBlockRecord<UnlinkedFunctionCodeBlock>(decoder, at))
        codeBlock.set(decoder.vm(), owner, decoded);
}

// -- Entries ----------------------------------------------------------------------------------------------------------

enum class CachedCodeBlockTag : uint8_t {
    Program,
    Module,
    Eval,
    BuiltinFunction, // a root UnlinkedFunctionExecutable created by BuiltinExecutables (an embedder's JS builtins)
};

template<typename CodeBlockType> static constexpr CachedCodeBlockTag tagFor = CachedCodeBlockTag::Eval;
template<> constexpr CachedCodeBlockTag tagFor<UnlinkedProgramCodeBlock> = CachedCodeBlockTag::Program;
template<> constexpr CachedCodeBlockTag tagFor<UnlinkedModuleProgramCodeBlock> = CachedCodeBlockTag::Module;

static CachedCodeBlockTag tagFromSourceCodeType(SourceCodeType type)
{
    switch (type) {
    case SourceCodeType::ProgramType:
        return CachedCodeBlockTag::Program;
    case SourceCodeType::EvalType:
        return CachedCodeBlockTag::Eval;
    case SourceCodeType::ModuleType:
        return CachedCodeBlockTag::Module;
    case SourceCodeType::FunctionType:
        break;
    }
    ASSERT_NOT_REACHED();
    return static_cast<CachedCodeBlockTag>(-1);
}

// The payload starts with an entry:
//   u32 cacheVersion, u8 tag, u8 reservedCalleeLocals, u32 headerSize, u32 headerChecksum, u32 keyAt, u32 rootAt
// followed (after the strings it names) by the key record keyAt points at:
//   varuint length, length bytes                 boot session
//   Program / Module:  SourceCodeKey
//   BuiltinFunction:   varuint sourceLength, varuint embedderStamp
// headerSize is where the key record ends; the checksum covers [0, headerSize). rootAt is the root code block's record
// (Program / Module) or the root executable record (BuiltinFunction).
class CacheEntry {
public:
    static constexpr size_t versionAt = 0;
    static constexpr size_t tagAt = 4;
    static constexpr size_t reservedCalleeLocalsAt = 5;
    static constexpr size_t headerSizeAt = 6;
    static constexpr size_t headerChecksumAt = 10;
    static constexpr size_t keyRecordAt = 14;
    static constexpr size_t rootRecordAt = 18;
    static constexpr size_t fixedSize = 22;

    template<typename WriteKey, typename WriteRoot>
    static void encode(Encoder& encoder, CachedCodeBlockTag tag, const WriteKey& writeKey, const WriteRoot& writeRoot)
    {
        Writer fixed;
        fixed.u32(computeJSCBytecodeCacheVersion());
        fixed.u8(static_cast<uint8_t>(tag));
        // The one property of the encoding CPU that generated bytecode depends on: BytecodeGenerator numbers a code block's
        // locals after the LLInt/baseline callee-save area. Equal on every CPU JSC targets today; a port where it differed
        // would produce foreign payloads, not portable ones.
        fixed.u8(CodeBlock::llintBaselineCalleeSaveSpaceAsVirtualRegisters());
        fixed.zeros(fixedSize - fixed.size());
        uint32_t at = encoder.append(fixed);
        RELEASE_ASSERT(!at);

        Writer key;
        String bootSession = bootSessionUUIDString();
        key.varuint(bootSession.length());
        for (unsigned i = 0; i < bootSession.length(); ++i)
            key.u8(static_cast<uint8_t>(bootSession[i]));
        writeKey(key);
        encoder.patchU32(keyRecordAt, encoder.append(key));
        uint32_t headerSize = encoder.position();
        encoder.patchU32(headerSizeAt, headerSize);
        encoder.addChecksum(0, headerSize, headerChecksumAt);
        encoder.patchU32(rootRecordAt, writeRoot());
    }

    CacheEntry(Decoder& decoder)
    {
        Reader reader(decoder.payload(), 0);
        m_version = reader.u32();
        m_tag = static_cast<CachedCodeBlockTag>(reader.u8());
        m_reservedCalleeLocals = reader.u8();
        m_headerSize = reader.u32();
        reader.u32();
        m_keyAt = reader.u32();
        m_rootAt = reader.u32();
        m_parsed = !reader.overran();
    }

    CachedCodeBlockTag tag() const { return m_tag; }
    uint32_t rootAt() const { return m_rootAt; }

    bool isUpToDate(Decoder& decoder) const
    {
        if (!m_parsed || m_version != computeJSCBytecodeCacheVersion())
            return false;
        if (m_reservedCalleeLocals != CodeBlock::llintBaselineCalleeSaveSpaceAsVirtualRegisters())
            return false;
        if (m_keyAt < fixedSize || m_keyAt >= m_headerSize || m_rootAt < m_headerSize)
            return false;
        return decoder.checksumMatches(0, m_headerSize, headerChecksumAt);
    }

    // Positioned after the boot session, which has been checked; overran() if it did not match.
    Reader keyReader(Decoder& decoder) const
    {
        Reader reader(decoder.payload(), m_keyAt);
        String bootSession = bootSessionUUIDString();
        auto stored = reader.bytes(reader.varuint());
        if (!equalSpans(stored, bootSession.span8()))
            reader.seek(decoder.payload().size() + 1);
        return reader;
    }

private:
    uint32_t m_version { 0 };
    CachedCodeBlockTag m_tag { };
    uint8_t m_reservedCalleeLocals { 0 };
    uint32_t m_headerSize { 0 };
    uint32_t m_keyAt { 0 };
    uint32_t m_rootAt { 0 };
    bool m_parsed { false };
};

template<typename UnlinkedCodeBlockType>
static void encodeCodeBlock(Encoder& encoder, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock)
{
    CacheEntry::encode(encoder, tagFor<UnlinkedCodeBlockType>,
        [&](Writer& writer) { CachedSourceCodeKey::encode(writer, encoder, key); },
        [&] { return CachedCodeBlockType<UnlinkedCodeBlockType>::encode(encoder, *uncheckedDowncast<UnlinkedCodeBlockType>(codeBlock)); });
}

RefPtr<CachedBytecode> encodeCodeBlock(VM& vm, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock, FileSystem::FileHandle& fileHandle, BytecodeCacheError& error, EncoderStringTable* externalStrings, BytecodeCacheChecksums checksums, BytecodeCacheUpdatable updatable)
{
    const ClassInfo* classInfo = codeBlock->classInfo();
    Encoder encoder(vm, fileHandle, Encoder::NumberStrings::Yes, externalStrings, checksums, updatable);
    if (classInfo == UnlinkedProgramCodeBlock::info())
        encodeCodeBlock<UnlinkedProgramCodeBlock>(encoder, key, codeBlock);
    else if (classInfo == UnlinkedModuleProgramCodeBlock::info())
        encodeCodeBlock<UnlinkedModuleProgramCodeBlock>(encoder, key, codeBlock);
    else
        ASSERT(classInfo == UnlinkedEvalCodeBlock::info());
    encoder.encodeDeferred();
    return encoder.release(error);
}

RefPtr<CachedBytecode> encodeCodeBlock(VM& vm, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock, EncoderStringTable* externalStrings, BytecodeCacheChecksums checksums, BytecodeCacheUpdatable updatable)
{
    BytecodeCacheError error;
    FileSystem::FileHandle invalidFileHandle;
    return encodeCodeBlock(vm, key, codeBlock, invalidFileHandle, error, externalStrings, checksums, updatable);
}

// A builtin function (BuiltinExecutables::createExecutable) and, lazily, its body and nested functions. The embedder
// supplies the source it was created from and a stamp identifying that source's contents; nothing is hashed at load.
RefPtr<CachedBytecode> encodeBuiltinFunction(VM& vm, const UnlinkedFunctionExecutable* executable, unsigned sourceLength, unsigned embedderStamp, EncoderStringTable* externalStrings, BytecodeCacheChecksums checksums, BytecodeCacheUpdatable updatable)
{
    BytecodeCacheError error;
    FileSystem::FileHandle invalidFileHandle;
    Encoder encoder(vm, invalidFileHandle, Encoder::NumberStrings::Yes, externalStrings, checksums, updatable);
    CacheEntry::encode(encoder, CachedCodeBlockTag::BuiltinFunction,
        [&](Writer& writer) {
            writer.varuint(sourceLength);
            writer.varuint(embedderStamp);
        },
        [&] { return CachedFunctionExecutable::encode(encoder, *executable); });
    encoder.encodeDeferred();
    return encoder.release(error);
}

UnlinkedFunctionExecutable* decodeBuiltinFunction(VM& vm, Ref<CachedBytecode> cachedBytecode, SourceProvider& provider, unsigned embedderStamp)
{
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode), &provider);
    CacheEntry entry(decoder.get());
    if (entry.tag() != CachedCodeBlockTag::BuiltinFunction || !entry.isUpToDate(decoder.get()))
        return nullptr;
    Reader key = entry.keyReader(decoder.get());
    unsigned sourceLength = key.varuint();
    unsigned stamp = key.varuint();
    if (key.overran() || sourceLength != provider.source().length() || stamp != embedderStamp)
        return nullptr;
    if (!CachedFunctionExecutable::isIntact(decoder.get(), entry.rootAt()))
        return nullptr;
    DeferGC deferGC(vm);
    CachedFunctionExecutable record(decoder.get(), entry.rootAt());
    if (!record.intact)
        return nullptr;
    return record.decode(decoder.get());
}

// A function's code block on its own, to be appended to the payload it was left out of (CachedBytecode::addFunctionUpdate).
RefPtr<CachedBytecode> encodeFunctionCodeBlock(VM& vm, const UnlinkedFunctionCodeBlock* codeBlock, size_t baseOffset, BytecodeCacheError& error)
{
    FileSystem::FileHandle invalidFileHandle;
    Encoder encoder(vm, invalidFileHandle, Encoder::NumberStrings::No, nullptr, BytecodeCacheChecksums::Yes, BytecodeCacheUpdatable::Yes, baseOffset);
    uint32_t recordAt = CachedFunctionCodeBlock::encode(encoder, *codeBlock);
    encoder.encodeDeferred();
    RefPtr<CachedBytecode> result = encoder.release(error);
    if (result)
        result->setRootOffset(recordAt - baseOffset);
    return result;
}

std::optional<SourceCodeKey> decodeSourceCodeKey(VM& vm, Ref<CachedBytecode> cachedBytecode)
{
    Ref<Decoder> decoder = Decoder::create(vm, WTF::move(cachedBytecode));
    CacheEntry entry(decoder.get());
    if (!entry.isUpToDate(decoder.get()) || (entry.tag() != CachedCodeBlockTag::Program && entry.tag() != CachedCodeBlockTag::Module))
        return std::nullopt;
    Reader reader = entry.keyReader(decoder.get());
    SourceCodeKey key;
    if (!CachedSourceCodeKey::decode(reader, decoder.get(), key))
        return std::nullopt;
    return key;
}

UnlinkedCodeBlock* decodeCodeBlockImpl(VM& vm, const SourceCodeKey& key, Ref<CachedBytecode> cachedBytecode)
{
    MonotonicTime before;
    size_t cachedBytecodeSize = cachedBytecode->size();
    if (Options::reportBytecodeCacheDecodeTimes()) [[unlikely]]
        before = MonotonicTime::now();

    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode), &key.source().provider());
    CacheEntry entry(decoder.get());
    if (!entry.isUpToDate(decoder.get()))
        return nullptr;
    UnlinkedCodeBlock* codeBlock = nullptr;
    {
        DeferGC deferGC(vm);
        Reader reader = entry.keyReader(decoder.get());
        SourceCodeKey decodedKey;
        if (!CachedSourceCodeKey::decode(reader, decoder.get(), decodedKey) || decodedKey != key)
            return nullptr;
        switch (entry.tag()) {
        case CachedCodeBlockTag::Program:
            codeBlock = decodeCodeBlockRecord<UnlinkedProgramCodeBlock>(decoder.get(), entry.rootAt());
            break;
        case CachedCodeBlockTag::Module:
            codeBlock = decodeCodeBlockRecord<UnlinkedModuleProgramCodeBlock>(decoder.get(), entry.rootAt());
            break;
        case CachedCodeBlockTag::Eval:
        case CachedCodeBlockTag::BuiltinFunction:
            return nullptr;
        }
    }

    if (Options::reportBytecodeCacheDecodeTimes()) [[unlikely]] {
        MonotonicTime after = MonotonicTime::now();
        dataLogLn("BytecodeCache: decoded ", key.source().provider().sourceURL(), " (", cachedBytecodeSize, " bytes) in ", (after - before).milliseconds(), " ms.");
    }
    return codeBlock;
}

bool isCachedBytecodeStillValid(VM& vm, Ref<CachedBytecode> cachedBytecode, const SourceCodeKey& key, SourceCodeType type)
{
    if (cachedBytecode->span().empty())
        return false;
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode));
    CacheEntry entry(decoder.get());
    if (!entry.isUpToDate(decoder.get()) || entry.tag() != tagFromSourceCodeType(type))
        return false;
    Reader reader = entry.keyReader(decoder.get());
    SourceCodeKey decodedKey;
    return CachedSourceCodeKey::decode(reader, decoder.get(), decodedKey) && decodedKey == key;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
