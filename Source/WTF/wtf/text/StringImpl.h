/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 * Copyright (C) 2005-2024 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Google Inc. All rights reserved.
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

#include <wtf/Compiler.h>

#include <atomic>
#include <limits.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <wtf/ASCIICType.h>
#include <wtf/Atomics.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/CompactPtr.h>
#include <wtf/DebugHeap.h>
#include <wtf/Expected.h>
#include <wtf/FlipBytes.h>
#include <wtf/MathExtras.h>
#include <wtf/NoVirtualDestructorBase.h>
#include <wtf/Packed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/ConversionMode.h>
#include <wtf/text/StringBuffer.h>
#include <wtf/text/StringCommon.h>
#include <wtf/text/StringHasherInlines.h>
#include <wtf/text/UTF8ConversionError.h>
#include <wtf/unicode/UTF8Conversion.h>
#include <wtf/ForkExtras.h>

#if USE(CF)
typedef const struct __CFString * CFStringRef;
#endif

#ifdef __OBJC__
@class NSString;
#endif

#if HAVE(36BIT_ADDRESS)
#define STRING_IMPL_ALIGNMENT alignas(16)
#else
#define STRING_IMPL_ALIGNMENT
#endif

namespace JSC {
namespace LLInt { class Data; }
class LLIntOffsetsExtractor;
}

namespace WTF {

class SymbolImpl;
class SymbolRegistry;

struct ASCIICaseInsensitiveStringViewHashTranslator;
struct HashedUTF8CharactersTranslator;
struct HashTranslatorASCIILiteral;
struct Latin1BufferTranslator;
struct StringViewHashTranslator;
struct SubstringTranslator;
struct UTF16BufferTranslator;

template<typename> class RetainPtr;

template<typename> struct BufferFromStaticDataTranslator;
template<typename> struct HashAndCharactersTranslator;

// Define STRING_STATS to 1 turn on runtime statistics of string sizes and memory usage.
#define STRING_STATS 0

template<bool isSpecialCharacter(char16_t), typename CharacterType, std::size_t Extent = std::dynamic_extent> bool NODELETE containsOnly(std::span<const CharacterType, Extent>);

#if STRING_STATS

struct StringStats {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StringStats);
    void add8BitString(unsigned length, bool isSubString = false)
    {
        ++m_totalNumberStrings;
        ++m_number8BitStrings;
        if (!isSubString)
            m_total8BitData += length;
    }

    void add16BitString(unsigned length, bool isSubString = false)
    {
        ++m_totalNumberStrings;
        ++m_number16BitStrings;
        if (!isSubString)
            m_total16BitData += length;
    }

    void removeString(StringImpl&);
    void printStats();

    static constexpr unsigned s_printStringStatsFrequency = 5000;
    static std::atomic<unsigned> s_stringRemovesTillPrintStats;

    std::atomic<unsigned> m_refCalls;
    std::atomic<unsigned> m_derefCalls;

    std::atomic<unsigned> m_totalNumberStrings;
    std::atomic<unsigned> m_number8BitStrings;
    std::atomic<unsigned> m_number16BitStrings;
    std::atomic<unsigned long long> m_total8BitData;
    std::atomic<unsigned long long> m_total16BitData;
};

#define STRING_STATS_ADD_8BIT_STRING(length) StringImpl::stringStats().add8BitString(length)
#define STRING_STATS_ADD_8BIT_STRING2(length, isSubString) StringImpl::stringStats().add8BitString(length, isSubString)
#define STRING_STATS_ADD_16BIT_STRING(length) StringImpl::stringStats().add16BitString(length)
#define STRING_STATS_ADD_16BIT_STRING2(length, isSubString) StringImpl::stringStats().add16BitString(length, isSubString)
#define STRING_STATS_REMOVE_STRING(string) StringImpl::stringStats().removeString(string)
#define STRING_STATS_REF_STRING(string) ++StringImpl::stringStats().m_refCalls;
#define STRING_STATS_DEREF_STRING(string) ++StringImpl::stringStats().m_derefCalls;

#else

#define STRING_STATS_ADD_8BIT_STRING(length) ((void)0)
#define STRING_STATS_ADD_8BIT_STRING2(length, isSubString) ((void)0)
#define STRING_STATS_ADD_16BIT_STRING(length) ((void)0)
#define STRING_STATS_ADD_16BIT_STRING2(length, isSubString) ((void)0)
#define STRING_STATS_ADD_UPCONVERTED_STRING(length) ((void)0)
#define STRING_STATS_REMOVE_STRING(string) ((void)0)
#define STRING_STATS_REF_STRING(string) ((void)0)
#define STRING_STATS_DEREF_STRING(string) ((void)0)

#endif

// Process-global shared-atom-table latch (SPEC-vmstate §4). Defined in
// AtomStringTable.cpp; set exactly once by WTF::enableSharedAtomStringTable()
// and immutable after. The canonical accessor is sharedAtomStringTableEnabled()
// (SharedAtomStringTable.h); the variable is redeclared here so the deref()
// fast path can read the latch inline (relaxed load, SPEC-vmstate F4) without
// pulling the table header into this very hot header.
WTF_EXPORT_PRIVATE extern std::atomic<bool> g_sharedAtomStringTableEnabled;

class STRING_IMPL_ALIGNMENT StringImplShape  {
    WTF_MAKE_NONCOPYABLE(StringImplShape);
public:
    static constexpr unsigned MaxLength = std::numeric_limits<int32_t>::max();

protected:
    StringImplShape(uint32_t refCount, std::span<const Latin1Character>, unsigned hashAndFlags);
    StringImplShape(uint32_t refCount, std::span<const char16_t>, unsigned hashAndFlags);

    enum ConstructWithConstExprTag { ConstructWithConstExpr };
    constexpr StringImplShape(uint32_t refCount, ASCIILiteral, unsigned hashAndFlags, ConstructWithConstExprTag);
    template<unsigned characterCount> constexpr StringImplShape(uint32_t refCount, unsigned length, const char16_t (&characters)[characterCount], unsigned hashAndFlags, ConstructWithConstExprTag);

    std::atomic<uint32_t> m_refCount;
    unsigned m_length;
    union {
        const Latin1Character* m_data8;
        const char16_t* m_data16;
        // It seems that reinterpret_cast prevents constexpr's compile time initialization in VC++.
        // These are needed to avoid reinterpret_cast.
        const char* m_data8Char;
        const char16_t* m_data16Char;
    };
    // SPEC-vmstate §4.5: atomic so concurrent flag publication (setIsAtom()
    // under a shared-atom-table shard lock, lazy setHash(), cost reporting) is
    // well-defined when the process-global shared atom-string table is enabled.
    // All accesses are relaxed. Plain stores are permitted only on provably
    // unpublished strings (constructors, translator pre-insert); flag mutations
    // on possibly-published strings MUST use idempotent fetch_or/fetch_and so
    // racing writers never drop each other's bits.
    mutable std::atomic<unsigned> m_hashAndFlags;

    unsigned hashAndFlags() const { return m_hashAndFlags.load(std::memory_order_relaxed); }
};

// JIT'd code reads m_hashAndFlags as a plain 32-bit load at
// StringImpl::flagsOffset(); the atomic must not change size or representation.
static_assert(sizeof(std::atomic<unsigned>) == sizeof(unsigned));
static_assert(std::atomic<unsigned>::is_always_lock_free);

// FIXME: Use of StringImpl and const is rather confused.
// The actual string inside a StringImpl is immutable, so you can't modify a string using a StringImpl&.
// We could mark every member function const and always use "const StringImpl&" and "const StringImpl*".
// Or we could say that "const" doesn't make sense at all and use "StringImpl&" and "StringImpl*" everywhere.
// Right now we use a mix of both, which makes code more confusing and has no benefit.

DECLARE_COMPACT_ALLOCATOR_WITH_HEAP_IDENTIFIER(StringImpl);
class StringImpl : private StringImplShape, public NoVirtualDestructorBase {
    WTF_MAKE_NONCOPYABLE(StringImpl);
    WTF_DEPRECATED_MAKE_FAST_COMPACT_ALLOCATED_WITH_HEAP_IDENTIFIER(StringImpl, StringImpl);

    friend class AtomStringImpl;
    friend class JSC::LLInt::Data;
    friend class JSC::LLIntOffsetsExtractor;
    friend class PrivateSymbolImpl;
    friend class RegisteredSymbolImpl;
    friend class SymbolImpl;
    friend class ExternalStringImpl;

    friend struct WTF::ASCIICaseInsensitiveStringViewHashTranslator;
    friend struct WTF::HashedUTF8CharactersTranslator;
    friend struct WTF::HashTranslatorASCIILiteral;
    friend struct WTF::Latin1BufferTranslator;
    friend struct WTF::StringViewHashTranslator;
    friend struct WTF::SubstringTranslator;
    friend struct WTF::UTF16BufferTranslator;

    template<typename> friend struct WTF::BufferFromStaticDataTranslator;
    template<typename> friend struct WTF::HashAndCharactersTranslator;

    friend WTF_EXPORT_PRIVATE bool equal(const StringImpl&, const StringImpl&);

public:
    enum BufferOwnership { BufferInternal, BufferOwned, BufferSubstring, BufferExternal };

    // Prefer using isValidLength over MaxLength when the character type is known.
    template<typename> static constexpr bool isValidLength(size_t);

    static constexpr unsigned MaxLength = StringImplShape::MaxLength;

    // The bottom 6 bits in the hash are flags, but reserve 8 bits since StringHash only has 24 bits anyway.
    static constexpr const unsigned s_flagCount = 8;

private:
    static constexpr const unsigned s_flagMask = (1u << s_flagCount) - 1;
    static_assert(s_flagCount <= StringHasher::flagCount, "StringHasher reserves enough bits for StringImpl flags");
    static constexpr const unsigned s_flagStringKindCount = 4;
#if USE(BUN_JSC_ADDITIONS)
    // TODO: remove once atomic strings are process-wide.
    static constexpr const unsigned s_hashFlagNeverAtomize = 1u << 6;
#endif

    static constexpr const unsigned s_hashZeroValue = 0;
    static constexpr const unsigned s_hashFlagStringKindIsAtom = 1u << (s_flagStringKindCount);
    static constexpr const unsigned s_hashFlagStringKindIsSymbol = 1u << (s_flagStringKindCount + 1);
    static constexpr const unsigned s_hashMaskStringKind = s_hashFlagStringKindIsAtom | s_hashFlagStringKindIsSymbol;
    static constexpr const unsigned s_hashFlagDidReportCost = 1u << 3;
    static constexpr const unsigned s_hashFlag8BitBuffer = 1u << 2;
    static constexpr const unsigned s_hashMaskBufferOwnership = (1u << 0) | (1u << 1);

public:
    enum StringKind {
        StringNormal = 0u, // non-symbol, non-atomic
        StringAtom = s_hashFlagStringKindIsAtom, // non-symbol, atomic
        StringSymbol = s_hashFlagStringKindIsSymbol, // symbol, non-atomic
    };

private:
    // Create a normal 8-bit string with internal storage (BufferInternal).
    enum Force8Bit { Force8BitConstructor };
    StringImpl(unsigned length, Force8Bit);

    // Create a normal 16-bit string with internal storage (BufferInternal).
    explicit StringImpl(unsigned length);

    // Create a StringImpl adopting ownership of the provided buffer (BufferOwned).
    template<typename Malloc> explicit StringImpl(MallocSpan<Latin1Character, Malloc>);
    template<typename Malloc> explicit StringImpl(MallocSpan<char16_t, Malloc>);
    enum ConstructWithoutCopyingTag { ConstructWithoutCopying };
    StringImpl(std::span<const char16_t>, ConstructWithoutCopyingTag);
    StringImpl(std::span<const Latin1Character>, ConstructWithoutCopyingTag);

    // Used to create new strings that are a substring of an existing StringImpl (BufferSubstring).
    StringImpl(std::span<const Latin1Character>, Ref<StringImpl>&&);
    StringImpl(std::span<const char16_t>, Ref<StringImpl>&&);

public:
    WTF_EXPORT_PRIVATE static void destroy(StringImpl*);

    WTF_EXPORT_PRIVATE static Ref<StringImpl> create(std::span<const char16_t>);
    WTF_EXPORT_PRIVATE static Ref<StringImpl> create(std::span<const Latin1Character>);
    ALWAYS_INLINE static Ref<StringImpl> create(std::span<const char> characters) { return create(byteCast<Latin1Character>(characters)); }
    WTF_EXPORT_PRIVATE static Ref<StringImpl> create8BitIfPossible(std::span<const char16_t>);
    WTF_EXPORT_PRIVATE static Ref<StringImpl> create8BitUnconditionally(std::span<const char16_t>);

    // Construct a string with UTF-8 data, null if it contains invalid UTF-8 sequences.
    WTF_EXPORT_PRIVATE static RefPtr<StringImpl> create(std::span<const char8_t>);

    static Ref<StringImpl> createSubstringSharingImpl(StringImpl&, unsigned offset, unsigned length);

    ALWAYS_INLINE static Ref<StringImpl> create(ASCIILiteral literal) { return createWithoutCopying(literal.span8()); }

    static Ref<StringImpl> createWithoutCopying(std::span<const char16_t> characters) { return characters.empty() ?  Ref { *empty() } : createWithoutCopyingNonEmpty(characters); }
    static Ref<StringImpl> createWithoutCopying(std::span<const Latin1Character> characters) { return characters.empty() ? Ref { *empty() } : createWithoutCopyingNonEmpty(characters); }
    ALWAYS_INLINE static Ref<StringImpl> createWithoutCopying(std::span<const char> characters) { return createWithoutCopying(byteCast<Latin1Character>(characters)); }

    WTF_EXPORT_PRIVATE static Ref<StringImpl> createUninitialized(size_t length, std::span<Latin1Character>&);
    WTF_EXPORT_PRIVATE static Ref<StringImpl> createUninitialized(size_t length, std::span<char16_t>&);

    template<typename CharacterType> static RefPtr<StringImpl> tryCreateUninitialized(size_t length, std::span<CharacterType>&);

    static Ref<StringImpl> createByReplacingInCharacters(std::span<const Latin1Character>, char16_t target, char16_t replacement, size_t indexOfFirstTargetCharacter);
    static Ref<StringImpl> createByReplacingInCharacters(std::span<const char16_t>, char16_t target, char16_t replacement, size_t indexOfFirstTargetCharacter);

    static Ref<StringImpl> createStaticStringImpl(std::span<const char> characters)
    {
        ASSERT(charactersAreAllASCII(byteCast<Latin1Character>(characters)));
        return createStaticStringImpl(byteCast<Latin1Character>(characters));
    }
    WTF_EXPORT_PRIVATE static Ref<StringImpl> createStaticStringImpl(std::span<const Latin1Character>);
    WTF_EXPORT_PRIVATE static Ref<StringImpl> createStaticStringImpl(std::span<const char16_t>);

    // Reallocate the StringImpl. The originalString must be only owned by the Ref,
    // and the buffer ownership must be BufferInternal. Just like the input pointer of realloc(),
    // the originalString can't be used after this function.
    static Ref<StringImpl> reallocate(Ref<StringImpl>&& originalString, unsigned length, Latin1Character*& data);
    static Ref<StringImpl> reallocate(Ref<StringImpl>&& originalString, unsigned length, char16_t*& data);
    static Expected<Ref<StringImpl>, UTF8ConversionError> tryReallocate(Ref<StringImpl>&& originalString, unsigned length, Latin1Character*& data);
    static Expected<Ref<StringImpl>, UTF8ConversionError> tryReallocate(Ref<StringImpl>&& originalString, unsigned length, char16_t*& data);

    static constexpr unsigned flagsOffset() { return OBJECT_OFFSETOF(StringImpl, m_hashAndFlags); }
    static constexpr unsigned flagIs8Bit() { return s_hashFlag8BitBuffer; }
    static constexpr unsigned flagIsAtom() { return s_hashFlagStringKindIsAtom; }
    static constexpr unsigned flagIsSymbol() { return s_hashFlagStringKindIsSymbol; }
    static constexpr unsigned maskStringKind() { return s_hashMaskStringKind; }
    static constexpr unsigned dataOffset() { return OBJECT_OFFSETOF(StringImpl, m_data8); }

    template<typename CharacterType, size_t inlineCapacity, typename OverflowHandler, size_t minCapacity, typename Malloc>
    static Ref<StringImpl> adopt(Vector<CharacterType, inlineCapacity, OverflowHandler, minCapacity, Malloc>&&);

    WTF_EXPORT_PRIVATE static Ref<StringImpl> adopt(StringBuffer<char16_t>&&);
    WTF_EXPORT_PRIVATE static Ref<StringImpl> adopt(StringBuffer<Latin1Character>&&);

    // TSAN family rope-stringimpl: a stale lock-free reader (concurrent
    // compiler / GC thread holding a racy snapshot per the object-model
    // ground truth) can probe a recycled allocation while a new StringImpl's
    // constructor stores m_length, so the read must be an annotated relaxed
    // atomic load. Identical plain-load codegen on every supported target;
    // JIT'd code keeps reading the word at lengthMemoryOffset(). The
    // const_cast (rather than making the field mutable) keeps the constexpr
    // ConstructWithConstExpr constructors able to read m_length at compile
    // time; the load never writes, so static/rodata instances are fine.
    unsigned length() const { return WTF::atomicLoad(const_cast<unsigned*>(&m_length), std::memory_order_relaxed); }
    static constexpr ptrdiff_t lengthMemoryOffset() { return OBJECT_OFFSETOF(StringImpl, m_length); }
    bool isEmpty() const { return !length(); }

    bool is8Bit() const { return hashAndFlags() & s_hashFlag8BitBuffer; }
    ALWAYS_INLINE std::span<const Latin1Character> span8() const LIFETIME_BOUND { ASSERT(is8Bit()); return unsafeMakeSpan(m_data8, length()); }
    ALWAYS_INLINE std::span<const char16_t> span16() const LIFETIME_BOUND { ASSERT(!is8Bit() || isEmpty()); return unsafeMakeSpan(m_data16, length()); }

    template<typename CharacterType> std::span<const CharacterType> span() const LIFETIME_BOUND;

    size_t cost() const;
    size_t costDuringGC();

    WTF_EXPORT_PRIVATE size_t NODELETE sizeInBytes() const;

    bool isSymbol() const { return hashAndFlags() & s_hashFlagStringKindIsSymbol; }
    bool isAtom() const { return hashAndFlags() & s_hashFlagStringKindIsAtom; }
    void setIsAtom(bool);
    
    bool isExternal() const { return bufferOwnership() == BufferExternal; }

    bool isSubString() const { return bufferOwnership() == BufferSubstring; }

    static WTF_EXPORT_PRIVATE Expected<CString, UTF8ConversionError> utf8ForCharacters(std::span<const Latin1Character> characters);
    static WTF_EXPORT_PRIVATE Expected<CString, UTF8ConversionError> utf8ForCharacters(std::span<const char16_t> characters, ConversionMode = LenientConversion);
    static WTF_EXPORT_PRIVATE Expected<size_t, UTF8ConversionError> utf8ForCharactersIntoBuffer(std::span<const char16_t> characters, ConversionMode, Vector<char8_t, 1024>&);
    static WTF_EXPORT_PRIVATE size_t utf8LengthFromUTF16(std::span<const char16_t> characters);
    static WTF_EXPORT_PRIVATE size_t tryConvertUTF16ToUTF8(std::span<const char16_t> source, std::span<char8_t> destination);

    template<typename Func>
    static Expected<std::invoke_result_t<Func, std::span<const char8_t>>, UTF8ConversionError> tryGetUTF8ForCharacters(NOESCAPE const Func&, std::span<const Latin1Character> characters);
    template<typename Func>
    static Expected<std::invoke_result_t<Func, std::span<const char8_t>>, UTF8ConversionError> tryGetUTF8ForCharacters(NOESCAPE const Func&, std::span<const char16_t> characters, ConversionMode = LenientConversion);

    template<typename Func>
    Expected<std::invoke_result_t<Func, std::span<const char8_t>>, UTF8ConversionError> tryGetUTF8(NOESCAPE const Func&, ConversionMode = LenientConversion) const;
    WTF_EXPORT_PRIVATE Expected<CString, UTF8ConversionError> tryGetUTF8(ConversionMode = LenientConversion) const;
    WTF_EXPORT_PRIVATE CString utf8(ConversionMode = LenientConversion) const;

#if USE(BUN_JSC_ADDITIONS)
    // ADVISORY in shared-atom-table mode: this is an unlocked read, so a true
    // result can be stale by the time the caller acts on it (setNeverAtomize()
    // takes no lock). Correctness never rests on this check — the in-place
    // atomization path claims atom-hood with trySetIsAtomIfAtomizable(), whose
    // RMW on the same atomic word total-orders against setNeverAtomize()'s CAS
    // (SPEC-vmstate §4.5; round-4 TOCTOU closure). This check only steers the
    // common case to the copying path early.
    bool canBecomeAtom() const { return !(hashAndFlags() & s_hashFlagNeverAtomize); }

    // Marks the string never-atomizable (e.g. an ExternalStringImpl whose
    // buffer the embedder may free early via releaseBufferEarly()) UNLESS it
    // is already an atom. Returns true if the flag was set: the string is not
    // in any atom table now and never will be parked in one (every in-place
    // atomization claims atom-hood via trySetIsAtomIfAtomizable(), which
    // refuses once this flag is visible in the same atomic word). Returns
    // false if the string is already an atom — the flag is NOT set, and the
    // caller MUST NOT free or release the character buffer early: the string
    // is (or is becoming) table-resident and probes read its characters under
    // the table/shard lock. In shared-atom-table mode losing this race to a
    // concurrent in-place atomization is legal (the two RMWs on
    // m_hashAndFlags are total-ordered; exactly one side wins — SPEC-vmstate
    // §4.5/§4.4.4); the embedder must check the return value before any early
    // buffer release (cross-WS item 17, INTEGRATE-vmstate.md). In legacy mode
    // a false return still means the historical caller bug (marking a live
    // atom), now reported instead of asserted-then-corrupted.
    bool setNeverAtomize()
    {
        unsigned old = hashAndFlags();
        while (true) {
            if (old & s_hashFlagStringKindIsAtom) {
                // Legacy mode (per-thread tables): no legal cross-thread race
                // exists, so an already-atom argument is the historical caller
                // bug — keep the old ASSERT(!isAtom()) fail-stop for it.
                // Shared mode: a lost race is legal; report via the return.
                ASSERT(g_sharedAtomStringTableEnabled.load(std::memory_order_relaxed));
                return false;
            }
            // CAS (not fetch_or): the bit must not be set once the isAtom bit
            // is observed in the same word, and racing flag publication
            // (setHash, cost bit, a winning trySetIsAtomIfAtomizable) just
            // refreshes `old` and retries — no flag bits are ever dropped.
            if (m_hashAndFlags.compare_exchange_weak(old, old | s_hashFlagNeverAtomize, std::memory_order_relaxed))
                return true;
        }
    }
#endif

    // SPEC-vmstate §4.4.4, shared-atom-table publication (round-4 TOCTOU
    // closure): atomically set the isAtom bit unless s_hashFlagNeverAtomize is
    // already set in the same atomic word. Returns false — leaving the string
    // a non-atom — if it lost the race with a concurrent setNeverAtomize();
    // the caller (addOwnedStringToSharedStringTable) must then back its table
    // insert out and fall back to a copying atomization. Because this RMW and
    // setNeverAtomize()'s CAS target the same std::atomic, they are
    // total-ordered: exactly one side wins, with no window in which a
    // NeverAtomize (early-buffer-releasable) string can end up table-resident.
    bool trySetIsAtomIfAtomizable()
    {
        ASSERT(!isStatic());
        ASSERT(!isSymbol());
        unsigned old = hashAndFlags();
        while (true) {
            if (old & s_hashFlagNeverAtomize)
                return false;
            if (m_hashAndFlags.compare_exchange_weak(old, old | s_hashFlagStringKindIsAtom, std::memory_order_relaxed))
                return true;
        }
    }

private:
    // The high bits of 'hash' are always empty, but we prefer to store our flags
    // in the low bits because it makes them slightly more efficient to access.
    // So, we shift left and right when setting and getting our hash code.
    void setHash(unsigned) const;

    unsigned rawHash() const { return hashAndFlags() >> s_flagCount; }

public:
    bool hasHash() const { return !!rawHash(); }

    unsigned existingHash() const { ASSERT(hasHash()); return rawHash(); }
    unsigned hash() const { return hasHash() ? rawHash() : hashSlowCase(); }

    WTF_EXPORT_PRIVATE unsigned concurrentHash() const;

    unsigned symbolAwareHash() const;
    unsigned existingSymbolAwareHash() const;

    SUPPRESS_TSAN bool isStatic() const { return m_refCount.load(std::memory_order_relaxed) & s_refCountFlagIsStaticString; }

    uint32_t refCount() const { return m_refCount.load(std::memory_order_relaxed) / s_refCountIncrement; }
    bool hasOneRef() const { return m_refCount.load(std::memory_order_relaxed) == s_refCountIncrement; }
    bool hasAtLeastOneRef() const { return m_refCount.load(std::memory_order_relaxed); } // For assertions.

    void ref();
    void deref();

    // SPEC-vmstate §4.4.2 — shared-atom-table hit path. Takes a reference only
    // if the refcount is non-zero: refcount 0 is final (§4.4), so a dying
    // string is never revived (a hit on it is treated as a miss / dead entry by
    // the caller). Statics always succeed (they rest at masked refcount 0 and
    // never die). When used on a table entry, MUST be called under the owning
    // shard's lock; the returned reference is adopted in-lock.
    bool tryRefAtom();

    class StaticStringImpl : private StringImplShape {
        WTF_MAKE_NONCOPYABLE(StaticStringImpl);
    public:
        // Used to construct static strings, which have an special refCount that can never hit zero.
        // This means that the static string will never be destroyed, which is important because
        // static strings will be shared across threads & ref-counted in a non-threadsafe manner.
        //
        // In order to make StaticStringImpl thread safe, we also need to ensure that the rest of
        // the fields are never mutated by threads. We have this guarantee because:
        //
        // 1. m_length is only set on construction and never mutated thereafter.
        //
        // 2. m_data8 and m_data16 are only set on construction and never mutated thereafter.
        //    We also know that a StringImpl never changes from 8 bit to 16 bit because there
        //    is no way to set/clear the s_hashFlag8BitBuffer flag other than at construction.
        //
        // 3. m_hashAndFlags will not be mutated by different threads because:
        //
        //    a. StaticStringImpl's constructor sets the s_hashFlagDidReportCost flag to ensure
        //       that StringImpl::cost() returns early.
        //       This means StaticStringImpl costs are not counted. But since there should only
        //       be a finite set of StaticStringImpls, their cost can be aggregated into a single
        //       system cost if needed.
        //    b. setIsAtom() is never called on a StaticStringImpl.
        //       setIsAtom() asserts !isStatic().
        //    c. setHash() is never called on a StaticStringImpl.
        //       StaticStringImpl's constructor sets the hash on construction.
        //       StringImpl::hash() only sets a new hash iff !hasHash().
        //       Additionally, StringImpl::setHash() asserts hasHash() and !isStatic().

        explicit constexpr StaticStringImpl(ASCIILiteral, StringKind = StringNormal);
        template<unsigned characterCount> explicit constexpr StaticStringImpl(const char16_t (&characters)[characterCount], StringKind = StringNormal);
        operator StringImpl&();
        operator const StringImpl&() const;
        ASCIILiteral literal() const { return ASCIILiteral::fromLiteralUnsafe(m_data8Char); }
    };

    WTF_EXPORT_PRIVATE static StaticStringImpl s_emptyAtomString;
    ALWAYS_INLINE static StringImpl* empty() { SUPPRESS_MEMORY_UNSAFE_CAST return reinterpret_cast<StringImpl*>(&s_emptyAtomString); }


    template<typename SourceCharacterType> static void iterCharacters(jsstring_iterator* iter, unsigned start, const SourceCharacterType* source, unsigned numCharacters);
    template<typename CharacterType>
    ALWAYS_INLINE static void copyCharacters(std::span<CharacterType> destination, std::span<const CharacterType> source)
    {
        return copyElements(destination, source);
    }

    ALWAYS_INLINE static void copyCharacters(std::span<char16_t> destination, std::span<const Latin1Character> source)
    {
        static_assert(sizeof(char16_t) == sizeof(uint16_t));
        static_assert(sizeof(Latin1Character) == sizeof(uint8_t));
        return copyElements(spanReinterpretCast<uint16_t>(destination), source);
    }

    ALWAYS_INLINE static void copyCharacters(std::span<Latin1Character> destination, std::span<const char16_t> source)
    {
        static_assert(sizeof(char16_t) == sizeof(uint16_t));
        static_assert(sizeof(Latin1Character) == sizeof(uint8_t));
#if ASSERT_ENABLED
        for (auto character : source)
            ASSERT(isLatin1(character));
#endif
        return copyElements(destination, spanReinterpretCast<const uint16_t>(source));
    }

    // Some string features, like reference counting and the atomicity flag, are not
    // thread-safe. We achieve thread safety by isolation, giving each thread
    // its own copy of the string.
    Ref<StringImpl> isolatedCopy() const;

    WTF_EXPORT_PRIVATE Ref<StringImpl> substring(unsigned position, unsigned length = MaxLength);

    char16_t at(unsigned) const;
    char16_t operator[](unsigned i) const { return at(i); }
    WTF_EXPORT_PRIVATE char32_t NODELETE codePointAt(unsigned);

    // FIXME: Like the strict functions above, these give false for "ok" when there is trailing garbage.
    // Like the non-strict functions above, these return the value when there is trailing garbage.
    // It would be better if these were more consistent with the above functions instead.
    double toDouble(bool* ok = nullptr);
    float toFloat(bool* ok = nullptr);

    WTF_EXPORT_PRIVATE Ref<StringImpl> convertToASCIILowercase();
    WTF_EXPORT_PRIVATE Ref<StringImpl> convertToASCIIUppercase();
    WTF_EXPORT_PRIVATE Ref<StringImpl> convertToLowercaseWithoutLocale();
    WTF_EXPORT_PRIVATE Ref<StringImpl> convertToLowercaseWithoutLocaleStartingAtFailingIndex8Bit(unsigned);
    WTF_EXPORT_PRIVATE Ref<StringImpl> convertToUppercaseWithoutLocale();
    WTF_EXPORT_PRIVATE Ref<StringImpl> convertToUppercaseWithoutLocaleStartingAtFailingIndex8Bit(unsigned);
    WTF_EXPORT_PRIVATE Ref<StringImpl> convertToLowercaseWithLocale(const AtomString& localeIdentifier);
    WTF_EXPORT_PRIVATE Ref<StringImpl> convertToUppercaseWithLocale(const AtomString& localeIdentifier);

    Ref<StringImpl> foldCase();

    WTF_EXPORT_PRIVATE Ref<StringImpl> simplifyWhiteSpace(CodeUnitMatchFunction);

    WTF_EXPORT_PRIVATE Ref<StringImpl> trim(CodeUnitMatchFunction);
    template<typename Predicate> Ref<StringImpl> removeCharacters(const Predicate&);

    bool containsOnlyASCII() const;
    bool containsOnlyLatin1() const;
    template<bool isSpecialCharacter(char16_t)> bool NODELETE containsOnly() const;

    size_t NODELETE find(Latin1Character, size_t start = 0);
    size_t NODELETE find(char, size_t start = 0);
    size_t NODELETE find(char16_t, size_t start = 0);
    template<typename CodeUnitMatchFunction>
        requires (std::is_invocable_r_v<bool, CodeUnitMatchFunction, char16_t>)
    size_t NODELETE find(CodeUnitMatchFunction, size_t start = 0);
    ALWAYS_INLINE size_t find(ASCIILiteral literal, size_t start = 0) { return find(literal.span8(), start); }
    WTF_EXPORT_PRIVATE size_t NODELETE find(StringView);
    WTF_EXPORT_PRIVATE size_t NODELETE find(StringView, size_t start);
    WTF_EXPORT_PRIVATE size_t NODELETE findIgnoringASCIICase(StringView) const;
    WTF_EXPORT_PRIVATE size_t NODELETE findIgnoringASCIICase(StringView, size_t start) const;

    WTF_EXPORT_PRIVATE size_t NODELETE reverseFind(char16_t, size_t start = MaxLength);
    WTF_EXPORT_PRIVATE size_t NODELETE reverseFind(StringView, size_t start = MaxLength);
    ALWAYS_INLINE size_t reverseFind(ASCIILiteral literal, size_t start = MaxLength) { return reverseFind(literal.span8(), start); }

    WTF_EXPORT_PRIVATE bool NODELETE startsWith(StringView) const;
    WTF_EXPORT_PRIVATE bool NODELETE startsWithIgnoringASCIICase(StringView) const;
    WTF_EXPORT_PRIVATE bool NODELETE startsWith(char16_t) const;
    WTF_EXPORT_PRIVATE bool NODELETE startsWith(std::span<const char>) const;
    WTF_EXPORT_PRIVATE bool NODELETE hasInfixStartingAt(StringView, size_t start) const;

    WTF_EXPORT_PRIVATE bool NODELETE endsWith(StringView);
    WTF_EXPORT_PRIVATE bool NODELETE endsWithIgnoringASCIICase(StringView) const;
    WTF_EXPORT_PRIVATE bool NODELETE endsWith(char16_t) const;
    WTF_EXPORT_PRIVATE bool NODELETE endsWith(std::span<const char>) const;
    WTF_EXPORT_PRIVATE bool NODELETE hasInfixEndingAt(StringView, size_t end) const;

    WTF_EXPORT_PRIVATE Ref<StringImpl> replace(char16_t, char16_t);
    WTF_EXPORT_PRIVATE Ref<StringImpl> replace(char16_t, StringView);
    ALWAYS_INLINE Ref<StringImpl> replace(char16_t pattern, std::span<const char> replacement) { return replace(pattern, byteCast<Latin1Character>(replacement)); }
    WTF_EXPORT_PRIVATE Ref<StringImpl> replace(char16_t, std::span<const Latin1Character>);
    Ref<StringImpl> replace(char16_t, std::span<const char16_t>);
    WTF_EXPORT_PRIVATE Ref<StringImpl> replace(StringView, StringView);
    WTF_EXPORT_PRIVATE Ref<StringImpl> replace(size_t start, size_t length, StringView);

    WTF_EXPORT_PRIVATE std::optional<UCharDirection> defaultWritingDirection();

#if USE(CF)
    RetainPtr<CFStringRef> createCFString();
#endif

#ifdef __OBJC__
    WTF_EXPORT_PRIVATE operator NSString *();
    WTF_EXPORT_PRIVATE RetainPtr<NSString> createNSString();
#endif

#if STRING_STATS
    ALWAYS_INLINE static StringStats& stringStats() { return m_stringStats; }
#endif

    BufferOwnership bufferOwnership() const { return static_cast<BufferOwnership>(hashAndFlags() & s_hashMaskBufferOwnership); }

    template<typename T> static constexpr size_t headerSize() { return tailOffset<T>(); }
    
protected:
    ~StringImpl();

    // Used to create new symbol string that holds an existing [[Description]] string as a substring buffer (BufferSubstring).
    enum CreateSymbolTag { CreateSymbol };
    StringImpl(CreateSymbolTag, std::span<const Latin1Character>);
    StringImpl(CreateSymbolTag, std::span<const char16_t>);

    // Null symbol.
    explicit StringImpl(CreateSymbolTag);

private:
    template<typename> static size_t allocationSize(Checked<size_t> tailElementCount);
    template<typename> static constexpr size_t tailOffset();

    WTF_EXPORT_PRIVATE size_t NODELETE find(std::span<const Latin1Character>, size_t start);
    WTF_EXPORT_PRIVATE size_t NODELETE reverseFind(std::span<const Latin1Character>, size_t start);

    bool requiresCopy() const;
    template<typename T> const T* tailPointer() const;
    template<typename T> T* tailPointer();
    StringImpl* const& substringBuffer() const;
    StringImpl*& substringBuffer();

    enum class CaseConvertType { Upper, Lower };
    template<CaseConvertType, typename CharacterType> static Ref<StringImpl> convertASCIICase(StringImpl&, std::span<const CharacterType>);

    WTF_EXPORT_PRIVATE static Ref<StringImpl> createWithoutCopyingNonEmpty(std::span<const Latin1Character>);
    WTF_EXPORT_PRIVATE static Ref<StringImpl> createWithoutCopyingNonEmpty(std::span<const char16_t>);

    template<typename CharacterType, class CodeUnitPredicate> Ref<StringImpl> trimMatchedCharacters(CodeUnitPredicate);
    template<typename CharacterType, typename Predicate> ALWAYS_INLINE Ref<StringImpl> removeCharactersImpl(std::span<const CharacterType> characters, const Predicate&);
    template<typename CharacterType, class CodeUnitPredicate> Ref<StringImpl> simplifyMatchedCharactersToSpace(CodeUnitPredicate);
    template<typename CharacterType, typename Malloc> static ALWAYS_INLINE MallocSpan<CharacterType, StringImplMalloc> toStringImplMallocSpan(MallocSpan<CharacterType, Malloc>);
    template<typename CharacterType> static Ref<StringImpl> constructInternal(StringImpl&, unsigned);
    template<typename CharacterType> static Ref<StringImpl> createUninitializedInternal(size_t, std::span<CharacterType>&);
    template<typename CharacterType> static Ref<StringImpl> createUninitializedInternalNonEmpty(size_t, std::span<CharacterType>&);
    template<typename CharacterType> static Expected<Ref<StringImpl>, UTF8ConversionError> reallocateInternal(Ref<StringImpl>&&, unsigned, CharacterType*&);
    template<typename CharacterType> static Ref<StringImpl> createInternal(std::span<const CharacterType>);
    WTF_EXPORT_PRIVATE NEVER_INLINE unsigned hashSlowCase() const;
    // Shared-atom-table mode only (SPEC-vmstate §4.4.3): zero-transition tail
    // of deref(); routes live-table atoms through AtomStringImpl::removeDeadAtom().
    WTF_EXPORT_PRIVATE NEVER_INLINE void derefSharedZero();
    Ref<StringImpl> convertToUppercaseWithoutLocaleUpconvert();

    // The bottom bit in the ref count indicates a static (immortal) string.
    static constexpr uint32_t s_refCountFlagIsStaticString = 0x1;
    static constexpr uint32_t s_refCountIncrement = 0x2; // This allows us to ref / deref without disturbing the static string flag.

#if STRING_STATS
    WTF_EXPORT_PRIVATE static StringStats m_stringStats;
#endif

public:
    void assertHashIsCorrect() const;
};

using StaticStringImpl = StringImpl::StaticStringImpl;

static_assert(sizeof(StringImpl) == sizeof(StaticStringImpl));

template<typename CharacterType>
struct HashTranslatorCharBuffer {
    std::span<const CharacterType> characters;
    unsigned hash;

    HashTranslatorCharBuffer(std::span<const CharacterType> characters)
        : characters(characters)
        , hash(StringHasher::computeHashAndMaskTop8Bits(characters))
    {
    }

    HashTranslatorCharBuffer(std::span<const CharacterType> characters, unsigned hash)
        : characters(characters)
        , hash(hash)
    {
    }
};

#if ASSERT_ENABLED

// StringImpls created from StaticStringImpl will ASSERT in the generic ValueCheck<T>::checkConsistency
// as they are not allocated by fastMalloc. We don't currently have any way to detect that case
// so we ignore the consistency check for all StringImpl*.
template<> struct ValueCheck<StringImpl*> {
    static void checkConsistency(const StringImpl*) { }
};

#endif // ASSERT_ENABLED

WTF_EXPORT_PRIVATE bool NODELETE equal(const StringImpl*, const StringImpl*);
WTF_EXPORT_PRIVATE bool NODELETE equal(const StringImpl*, std::span<const Latin1Character>);
SUPPRESS_NODELETE inline bool NODELETE equal(const StringImpl* a, const char* b) { return equal(a, byteCast<Latin1Character>(unsafeSpan(b))); }
WTF_EXPORT_PRIVATE bool NODELETE equal(const StringImpl*, std::span<const char16_t>);
ALWAYS_INLINE bool NODELETE equal(const StringImpl* a, ASCIILiteral b) { return equal(a, b.span8()); }
inline bool NODELETE equal(const StringImpl* a, std::span<const char> b) { return equal(a, byteCast<Latin1Character>(b)); }
SUPPRESS_NODELETE inline bool NODELETE equal(const char* a, StringImpl* b) { return equal(b, byteCast<Latin1Character>(unsafeSpan(a))); }
WTF_EXPORT_PRIVATE bool NODELETE equal(const StringImpl& a, const StringImpl& b);

WTF_EXPORT_PRIVATE bool NODELETE equalIgnoringNullity(StringImpl*, StringImpl*);
WTF_EXPORT_PRIVATE bool NODELETE equalIgnoringNullity(std::span<const char16_t>, StringImpl*);

bool NODELETE equalIgnoringASCIICase(const StringImpl&, const StringImpl&);
WTF_EXPORT_PRIVATE bool NODELETE equalIgnoringASCIICase(const StringImpl*, const StringImpl*);
bool NODELETE equalIgnoringASCIICase(const StringImpl&, ASCIILiteral);
bool NODELETE equalIgnoringASCIICase(const StringImpl*, ASCIILiteral);

WTF_EXPORT_PRIVATE bool NODELETE equalIgnoringASCIICaseNonNull(const StringImpl*, const StringImpl*);

bool NODELETE equalLettersIgnoringASCIICase(const StringImpl&, ASCIILiteral);
bool NODELETE equalLettersIgnoringASCIICase(const StringImpl*, ASCIILiteral);

template<typename CodeUnit, typename CodeUnitMatchFunction>
    requires (std::is_invocable_r_v<bool, CodeUnitMatchFunction, CodeUnit>)
size_t NODELETE find(std::span<const CodeUnit>, CodeUnitMatchFunction&&, size_t start = 0);

template<typename CharacterType> size_t NODELETE reverseFindLineTerminator(std::span<const CharacterType>, size_t start = StringImpl::MaxLength);
template<typename CharacterType> size_t NODELETE reverseFind(std::span<const CharacterType>, CharacterType matchCharacter, size_t start = StringImpl::MaxLength);
size_t NODELETE reverseFind(std::span<const char16_t>, Latin1Character matchCharacter, size_t start = StringImpl::MaxLength);
size_t NODELETE reverseFind(std::span<const Latin1Character>, char16_t matchCharacter, size_t start = StringImpl::MaxLength);

template<size_t inlineCapacity> bool NODELETE equalIgnoringNullity(const Vector<char16_t, inlineCapacity>&, StringImpl*);

template<typename CharacterType1, typename CharacterType2>
SUPPRESS_NODELETE std::strong_ordering NODELETE codePointCompare(std::span<const CharacterType1> characters1, std::span<const CharacterType2> characters2);

bool NODELETE isUnicodeWhitespace(char16_t);

// Deprecated as this excludes U+0085 and U+00A0 which are part of Unicode's White_Space definition:
// https://www.unicode.org/Public/UCD/latest/ucd/PropList.txt
bool NODELETE deprecatedIsSpaceOrNewline(char16_t);

// Inverse of deprecatedIsSpaceOrNewline for predicates
bool NODELETE deprecatedIsNotSpaceOrNewline(char16_t);

// StringHash is the default hash for StringImpl* and RefPtr<StringImpl>
template<typename> struct DefaultHash;
template<> struct DefaultHash<StringImpl*>;
template<> struct DefaultHash<RefPtr<StringImpl>>;
template<> struct DefaultHash<PackedPtr<StringImpl>>;
template<> struct DefaultHash<CompactPtr<StringImpl>>;

#define MAKE_STATIC_STRING_IMPL(characters) ([] { \
        static StaticStringImpl impl(characters); \
        return &impl; \
    }())

template<> ALWAYS_INLINE Ref<StringImpl> StringImpl::constructInternal<Latin1Character>(StringImpl& string, unsigned length)
{
    return adoptRef(*new (NotNull, &string) StringImpl { length, Force8BitConstructor });
}

template<> ALWAYS_INLINE Ref<StringImpl> StringImpl::constructInternal<char16_t>(StringImpl& string, unsigned length)
{
    return adoptRef(*new (NotNull, &string) StringImpl { length });
}

template<> ALWAYS_INLINE std::span<const Latin1Character> StringImpl::span<Latin1Character>() const LIFETIME_BOUND
{
    return span8();
}

template<> ALWAYS_INLINE std::span<const char16_t> StringImpl::span<char16_t>() const LIFETIME_BOUND
{
    return span16();
}

template<typename CodeUnit, typename CodeUnitMatchFunction>
    requires (std::is_invocable_r_v<bool, CodeUnitMatchFunction, CodeUnit>)
inline size_t find(std::span<const CodeUnit> characters, CodeUnitMatchFunction&& matchFunction, size_t start)
{
    while (start < characters.size()) {
        // FIXME: support NODELETE predicates (rdar://178355573).
        SUPPRESS_NODELETE if (matchFunction(characters[start]))
            return start;
        ++start;
    }
    return notFound;
}

template<typename CharacterType> inline size_t reverseFindLineTerminator(std::span<const CharacterType> characters, size_t start)
{
    if (characters.empty())
        return notFound;
    if (start >= characters.size())
        start = characters.size() - 1;
    auto character = characters[start];
    while (character != '\n' && character != '\r') {
        if (!start--)
            return notFound;
        character = characters[start];
    }
    return start;
}

template<typename CharacterType> inline size_t reverseFind(std::span<const CharacterType> characters, CharacterType matchCharacter, size_t start)
{
    if (characters.empty())
        return notFound;
    if (start >= characters.size())
        start = characters.size() - 1;
    size_t searchLength = start + 1;
    const CharacterType* result;
    if constexpr (sizeof(CharacterType) == 1)
        result = std::bit_cast<const CharacterType*>(reverseFind8(std::bit_cast<const uint8_t*>(characters.data()), static_cast<uint8_t>(matchCharacter), searchLength));
    else if constexpr (sizeof(CharacterType) == 2)
        result = std::bit_cast<const CharacterType*>(reverseFind16(std::bit_cast<const uint16_t*>(characters.data()), static_cast<uint16_t>(matchCharacter), searchLength));
    else
        result = std::bit_cast<const CharacterType*>(reverseFind32(std::bit_cast<const uint32_t*>(characters.data()), static_cast<uint32_t>(matchCharacter), searchLength));
    if (!result)
        return notFound;
    return result - characters.data();
}

ALWAYS_INLINE size_t reverseFind(std::span<const char16_t> characters, Latin1Character matchCharacter, size_t start)
{
    return reverseFind(characters, static_cast<char16_t>(matchCharacter), start);
}

inline size_t reverseFind(std::span<const Latin1Character> characters, char16_t matchCharacter, size_t start)
{
    if (!isLatin1(matchCharacter))
        return notFound;
    return reverseFind(characters, static_cast<Latin1Character>(matchCharacter), start);
}

inline size_t StringImpl::find(Latin1Character character, size_t start)
{
    if (is8Bit())
        return WTF::find(span8(), character, start);
    return WTF::find(span16(), character, start);
}

ALWAYS_INLINE size_t StringImpl::find(char character, size_t start)
{
    return find(byteCast<Latin1Character>(character), start);
}

inline size_t StringImpl::find(char16_t character, size_t start)
{
    if (is8Bit())
        return WTF::find(span8(), character, start);
    return WTF::find(span16(), character, start);
}

template<typename CodeUnitMatchFunction>
    requires (std::is_invocable_r_v<bool, CodeUnitMatchFunction, char16_t>)
size_t StringImpl::find(CodeUnitMatchFunction matchFunction, size_t start)
{
    if (is8Bit())
        return WTF::find(span8(), matchFunction, start);
    return WTF::find(span16(), matchFunction, start);
}

template<size_t inlineCapacity> inline bool equalIgnoringNullity(const Vector<char16_t, inlineCapacity>& a, StringImpl* b)
{
    return equalIgnoringNullity(a.data(), a.size(), b);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
template<typename CharacterType1, typename CharacterType2>
inline std::strong_ordering codePointCompare(std::span<const CharacterType1> characters1, std::span<const CharacterType2> characters2)
{
    size_t commonLength = std::min(characters1.size(), characters2.size());

    auto* characters1Ptr = characters1.data();
    auto* characters2Ptr = characters2.data();
    size_t position = 0;

#if CPU(REGISTER64) && !CPU(NEEDS_ALIGNED_ACCESS) && CPU(LITTLE_ENDIAN)
    if constexpr (sizeof(CharacterType1) == sizeof(CharacterType2) && (sizeof(CharacterType1) == 1 || sizeof(CharacterType1) == 2)) {
        using ChunkType = std::conditional_t<sizeof(CharacterType1) == 1, uint32_t, uint64_t>;
        constexpr size_t stride = sizeof(ChunkType) / sizeof(CharacterType1);
        for (; position + (stride - 1) < commonLength;) {
            // Even though CPU does not need aligned access, we cannot
            // simply dereference ChunkType* or the compiler may perform
            // optimizations based on the assumption that all ChunkType
            // objects are naturally aligned.
            auto lhs = unalignedLoad<ChunkType>(characters1Ptr);
            auto rhs = unalignedLoad<ChunkType>(characters2Ptr);
            if (lhs != rhs) {
                if constexpr (sizeof(CharacterType1) == 1)
                    return (flipBytes(lhs) > flipBytes(rhs)) ? std::strong_ordering::greater : std::strong_ordering::less;

#if CPU(ARM64)
                if constexpr (sizeof(CharacterType1) == 2) {
                    auto rev16 = [](uint64_t value) ALWAYS_INLINE_LAMBDA {
                        uint64_t result;
                        __asm__("rev16 %x0, %x1" : "=r"(result) : "r"(value));
                        return result;
                    };
                    return (rev16(flipBytes(lhs)) > rev16(flipBytes(rhs))) ? std::strong_ordering::greater : std::strong_ordering::less;
                }
#endif

                break;
            }

            characters1Ptr += stride;
            characters2Ptr += stride;
            position += stride;
        }
    }
#endif

    while (position < commonLength && *characters1Ptr == *characters2Ptr) {
        ++characters1Ptr;
        ++characters2Ptr;
        ++position;
    }

    if (position < commonLength)
        return (characters1Ptr[0] > characters2Ptr[0]) ? std::strong_ordering::greater : std::strong_ordering::less;

    if (characters1.size() == characters2.size())
        return std::strong_ordering::equal;
    return (characters1.size() > characters2.size()) ? std::strong_ordering::greater : std::strong_ordering::less;
}
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

// FIXME: For Latin1Character, isUnicodeCompatibleASCIIWhitespace(character) || character == 0x0085 || character == noBreakSpace would be enough
SUPPRESS_NODELETE inline bool isUnicodeWhitespace(char16_t character)
{
    return isASCII(character) ? isUnicodeCompatibleASCIIWhitespace(character) : u_isUWhiteSpace(character);
}

SUPPRESS_NODELETE inline bool deprecatedIsSpaceOrNewline(char16_t character)
{
    // Use isUnicodeCompatibleASCIIWhitespace() for all Latin-1 characters, which is incorrect as it
    // excludes U+0085 and U+00A0.
    return isLatin1(character) ? isUnicodeCompatibleASCIIWhitespace(character) : u_charDirection(character) == U_WHITE_SPACE_NEUTRAL;
}

SUPPRESS_NODELETE inline bool deprecatedIsNotSpaceOrNewline(char16_t character)
{
    return !deprecatedIsSpaceOrNewline(character);
}

// TSAN family rope-stringimpl: a freed StringImpl's memory can be recycled by
// the allocator while a stale lock-free reader (concurrent compiler / GC
// thread holding a racy snapshot per the object-model ground truth) still
// probes m_refCount/m_hashAndFlags. The std::atomic member-initializer is a
// plain (non-atomic) construction store, so the atomic members of the
// runtime-constructed shapes are initialized with annotated relaxed stores in
// the body instead — identical codegen, defined behavior against stale
// readers. The ConstructWithConstExpr constructors below are compile-time
// (static/literal storage, never recycled) and must stay constexpr
// member-inits.
inline StringImplShape::StringImplShape(uint32_t refCount, std::span<const Latin1Character> data, unsigned hashAndFlags)
    : m_data8(data.data())
{
    // TSAN family rope-stringimpl: m_length joins m_refCount/m_hashAndFlags as
    // an annotated relaxed constructor store (see the comment above) so a
    // stale lock-free reader's StringImpl::length() on this recycled
    // allocation is defined. Identical plain-store codegen.
    RELEASE_ASSERT(data.size() <= MaxLength);
    m_refCount.store(refCount, std::memory_order_relaxed);
    atomicStore(&m_length, static_cast<unsigned>(data.size()), std::memory_order_relaxed);
    m_hashAndFlags.store(hashAndFlags, std::memory_order_relaxed);
}

inline StringImplShape::StringImplShape(uint32_t refCount, std::span<const char16_t> data, unsigned hashAndFlags)
    : m_data16(data.data())
{
    // TSAN family rope-stringimpl: see the Latin1 constructor above.
    RELEASE_ASSERT(data.size() <= MaxLength);
    m_refCount.store(refCount, std::memory_order_relaxed);
    atomicStore(&m_length, static_cast<unsigned>(data.size()), std::memory_order_relaxed);
    m_hashAndFlags.store(hashAndFlags, std::memory_order_relaxed);
}

constexpr StringImplShape::StringImplShape(uint32_t refCount, ASCIILiteral literal, unsigned hashAndFlags, ConstructWithConstExprTag)
    : m_refCount(refCount)
    , m_length(literal.length())
    , m_data8Char(literal.characters())
    , m_hashAndFlags(hashAndFlags)
{
    RELEASE_ASSERT(m_length <= MaxLength);
}

template<unsigned characterCount> constexpr StringImplShape::StringImplShape(uint32_t refCount, unsigned length, const char16_t (&characters)[characterCount], unsigned hashAndFlags, ConstructWithConstExprTag)
    : m_refCount(refCount)
    , m_length(length)
    , m_data16Char(characters)
    , m_hashAndFlags(hashAndFlags)
{
    RELEASE_ASSERT(length <= MaxLength);
}

inline Ref<StringImpl> StringImpl::isolatedCopy() const
{
    if (!requiresCopy()) {
        if (is8Bit())
            return StringImpl::createWithoutCopying(span8());
        return StringImpl::createWithoutCopying(span16());
    }

    if (is8Bit())
        return create(span8());
    return create(span16());
}

inline bool StringImpl::containsOnlyASCII() const
{
    if (is8Bit())
        return charactersAreAllASCII(span8());
    return charactersAreAllASCII(span16());
}

inline bool StringImpl::containsOnlyLatin1() const
{
    if (is8Bit())
        return true;
    auto characters = span16();
    char16_t mergedCharacterBits = 0;
    for (auto character : characters)
        mergedCharacterBits |= character;
    return isLatin1(mergedCharacterBits);
}

template<bool isSpecialCharacter(char16_t), typename CharacterType, std::size_t Extent> inline bool containsOnly(std::span<const CharacterType, Extent> characters)
{
    for (auto character : characters) {
        if (!isSpecialCharacter(character))
            return false;
    }
    return true;
}

template<bool isSpecialCharacter(char16_t)> inline bool StringImpl::containsOnly() const
{
    if (is8Bit())
        return WTF::containsOnly<isSpecialCharacter>(span8());
    return WTF::containsOnly<isSpecialCharacter>(span16());
}

inline StringImpl::StringImpl(unsigned length, Force8Bit)
    : StringImplShape(s_refCountIncrement, unsafeMakeSpan(tailPointer<Latin1Character>(), length), s_hashFlag8BitBuffer | StringNormal | BufferInternal)
{
    ASSERT(m_data8);
    ASSERT(m_length);

    STRING_STATS_ADD_8BIT_STRING(m_length);
}

inline StringImpl::StringImpl(unsigned length)
    : StringImplShape(s_refCountIncrement, unsafeMakeSpan(tailPointer<char16_t>(), length), s_hashZeroValue | StringNormal | BufferInternal)
{
    ASSERT(m_data16);
    ASSERT(m_length);

    STRING_STATS_ADD_16BIT_STRING(m_length);
}

template<typename CharacterType, typename Malloc>
MallocSpan<CharacterType, StringImplMalloc> StringImpl::toStringImplMallocSpan(MallocSpan<CharacterType, Malloc> characters)
{
    if constexpr (std::is_same_v<Malloc, StringImplMalloc>)
        return characters;
    else {
        auto buffer = MallocSpan<CharacterType, StringImplMalloc>::malloc(characters.sizeInBytes());
        copyCharacters(buffer.mutableSpan(), characters.span());
        return buffer;
    }
}

template<typename Malloc>
inline StringImpl::StringImpl(MallocSpan<Latin1Character, Malloc> characters)
    : StringImplShape(s_refCountIncrement, toStringImplMallocSpan(WTF::move(characters)).leakSpan(), s_hashFlag8BitBuffer | StringNormal | BufferOwned)
{
    ASSERT(m_data8);
    ASSERT(m_length);

    STRING_STATS_ADD_8BIT_STRING(m_length);
}

inline StringImpl::StringImpl(std::span<const char16_t> characters, ConstructWithoutCopyingTag)
    : StringImplShape(s_refCountIncrement, characters, s_hashZeroValue | StringNormal | BufferInternal)
{
    ASSERT(m_data16);
    ASSERT(m_length);

    STRING_STATS_ADD_16BIT_STRING(m_length);
}

inline StringImpl::StringImpl(std::span<const Latin1Character> characters, ConstructWithoutCopyingTag)
    : StringImplShape(s_refCountIncrement, characters, s_hashFlag8BitBuffer | StringNormal | BufferInternal)
{
    ASSERT(m_data8);
    ASSERT(m_length);

    STRING_STATS_ADD_8BIT_STRING(m_length);
}

template<typename Malloc>
inline StringImpl::StringImpl(MallocSpan<char16_t, Malloc> characters)
    : StringImplShape(s_refCountIncrement, toStringImplMallocSpan(WTF::move(characters)).leakSpan(), s_hashZeroValue | StringNormal | BufferOwned)
{
    ASSERT(m_data16);
    ASSERT(m_length);

    STRING_STATS_ADD_16BIT_STRING(m_length);
}

inline StringImpl::StringImpl(std::span<const Latin1Character> characters, Ref<StringImpl>&& base)
    : StringImplShape(s_refCountIncrement, characters, s_hashFlag8BitBuffer | StringNormal | BufferSubstring)
{
    ASSERT(is8Bit());
    ASSERT(m_data8);
    ASSERT(m_length);
    ASSERT(base->bufferOwnership() != BufferSubstring);

    substringBuffer() = &base.leakRef();

    STRING_STATS_ADD_8BIT_STRING2(m_length, true);
}

inline StringImpl::StringImpl(std::span<const char16_t> characters, Ref<StringImpl>&& base)
    : StringImplShape(s_refCountIncrement, characters, s_hashZeroValue | StringNormal | BufferSubstring)
{
    ASSERT(!is8Bit());
    ASSERT(m_data16);
    ASSERT(m_length);
    ASSERT(base->bufferOwnership() != BufferSubstring);

    substringBuffer() = &base.leakRef();

    STRING_STATS_ADD_16BIT_STRING2(m_length, true);
}

ALWAYS_INLINE Ref<StringImpl> StringImpl::createSubstringSharingImpl(StringImpl& rep, unsigned offset, unsigned length)
{
    ASSERT(length <= rep.length());

    if (!length)
        return *empty();

    // Copying the thing would save more memory sometimes, largely due to the size of pointer.
    size_t substringSize = allocationSize<StringImpl*>(1);
    if (rep.is8Bit()) {
        if (substringSize >= allocationSize<Latin1Character>(length))
            return create(rep.span8().subspan(offset, length));
    } else {
        auto span = rep.span16().subspan(offset, length);
        if (substringSize >= allocationSize<Latin1Character>(length) && charactersAreAllLatin1(span))
            return create8BitUnconditionally(span);

        if (substringSize >= allocationSize<char16_t>(length))
            return create(span);
    }

    SUPPRESS_UNCOUNTED_LOCAL auto* ownerRep = ((rep.bufferOwnership() == BufferSubstring) ? rep.substringBuffer() : &rep);

    // We allocate a buffer that contains both the StringImpl struct as well as the pointer to the owner string.
    SUPPRESS_UNCOUNTED_LOCAL auto* stringImpl = static_cast<StringImpl*>(StringImplMalloc::malloc(substringSize));
    if (rep.is8Bit())
        return adoptRef(*new (NotNull, stringImpl) StringImpl(rep.span8().subspan(offset, length), *ownerRep));
    return adoptRef(*new (NotNull, stringImpl) StringImpl(rep.span16().subspan(offset, length), *ownerRep));
}

template<typename CharacterType> ALWAYS_INLINE RefPtr<StringImpl> StringImpl::tryCreateUninitialized(size_t length, std::span<CharacterType>& output)
{
    if (!length) {
        output = { };
        return empty();
    }

    if (!isValidLength<CharacterType>(length)) {
        output = { };
        return nullptr;
    }
    SUPPRESS_UNCOUNTED_LOCAL StringImpl* result = (StringImpl*)StringImplMalloc::tryMalloc(allocationSize<CharacterType>(length));
    if (!result) [[unlikely]] {
        output = { };
        return nullptr;
    }
    output = unsafeMakeSpan(result->tailPointer<CharacterType>(), length);

    return constructInternal<CharacterType>(*result, length);
}

template<typename CharacterType, size_t inlineCapacity, typename OverflowHandler, size_t minCapacity, typename Malloc>
inline Ref<StringImpl> StringImpl::adopt(Vector<CharacterType, inlineCapacity, OverflowHandler, minCapacity, Malloc>&& vector)
{
    if constexpr (std::is_same_v<Malloc, StringImplMalloc>) {
        if (!vector.size())
            return *empty();
        return adoptRef(*new StringImpl(vector.releaseBuffer()));
    } else
        return create(vector.span());
}

inline size_t StringImpl::cost() const
{
    // For substrings, return the cost of the base string.
    if (bufferOwnership() == BufferSubstring)
        return substringBuffer()->cost();

    // Note: we must not alter the m_hashAndFlags field in instances of StaticStringImpl.
    // We ensure this by pre-setting the s_hashFlagDidReportCost bit in all instances of
    // StaticStringImpl. As a result, StaticStringImpl instances will always return a cost of
    // 0 here and avoid modifying m_hashAndFlags.
    if (hashAndFlags() & s_hashFlagDidReportCost)
        return 0;

    // fetch_or: possibly published (SPEC-vmstate §4.5). A racing pair may both
    // report the cost (benign, same as the pre-atomic code), but neither can
    // drop a concurrently published flag bit.
    m_hashAndFlags.fetch_or(s_hashFlagDidReportCost, std::memory_order_relaxed);
    size_t result = length();
    if (!is8Bit())
        result <<= 1;
    return result;
}

inline size_t StringImpl::costDuringGC()
{
    if (isStatic())
        return 0;

    if (bufferOwnership() == BufferSubstring)
        return divideRoundedUp<size_t>(substringBuffer()->costDuringGC(), refCount());

    // TSAN family rope-stringimpl: GC-thread cost accounting can race a
    // recycled allocation's constructor store of m_length; read through the
    // annotated relaxed accessor.
    size_t result = length();
    if (!is8Bit())
        result <<= 1;
    return divideRoundedUp<size_t>(result, refCount());
}

inline void StringImpl::setIsAtom(bool isAtom)
{
    ASSERT(!isStatic());
    ASSERT(!isSymbol());
    // SPEC-vmstate §4.5: RMW because the string may already be published
    // (e.g. setIsAtom(true) under a shared-atom-table shard lock while another
    // thread lazily publishes the hash via setHash()).
    if (isAtom)
        m_hashAndFlags.fetch_or(s_hashFlagStringKindIsAtom, std::memory_order_relaxed);
    else
        m_hashAndFlags.fetch_and(~s_hashFlagStringKindIsAtom, std::memory_order_relaxed);
}

inline void StringImpl::setHash(unsigned hash) const
{
    // The high bits of 'hash' are always empty, but we prefer to store our flags
    // in the low bits because it makes them slightly more efficient to access.
    // So, we shift left and right when setting and getting our hash code.

    ASSERT(!isStatic());
    // Multiple clients assume that StringHasher is the canonical string hash function.
    ASSERT(hash == (is8Bit() ? StringHasher::computeHashAndMaskTop8Bits(span8()) : StringHasher::computeHashAndMaskTop8Bits(span16())));
    ASSERT(!(hash & (s_flagMask << (8 * sizeof(hash) - s_flagCount)))); // Verify that enough high bits are empty.

    hash <<= s_flagCount;
    ASSERT(hash); // Verify that 0 is a valid sentinel hash value.

    // SPEC-vmstate §4.5: lazy-hash publication may race with itself (every
    // writer computes the identical hash from the immutable characters, so
    // fetch_or is value-idempotent) and with concurrent flag publication
    // (fetch_or never drops flag bits a racing writer just set; a plain
    // read-modify-write would). Store hash with flags in low bits.
    unsigned old = m_hashAndFlags.fetch_or(hash, std::memory_order_relaxed);
    // If a racing setHash() got here first, it must have stored the same hash.
    ASSERT_UNUSED(old, !(old & ~s_flagMask) || (old & ~s_flagMask) == hash);
}

inline void StringImpl::ref()
{
    STRING_STATS_REF_STRING(*this);

#if TSAN_ENABLED
    if (isStatic())
        return;
#endif

    m_refCount.fetch_add(s_refCountIncrement, std::memory_order_relaxed);
}

ALWAYS_INLINE bool StringImpl::tryRefAtom()
{
    uint32_t value = m_refCount.load(std::memory_order_relaxed);
    if (value & s_refCountFlagIsStaticString) {
        // Statics rest at masked refcount 0 and never die: plain ref(), no
        // zero check, no CAS (SPEC-vmstate §4.4.2).
        ref();
        return true;
    }
    while (true) {
        if (!(value & ~s_refCountFlagIsStaticString))
            return false; // Refcount 0 is final (§4.4): never revive a dying string.
        // Relaxed is sufficient: table-hit callers hold the shard lock that
        // ordered the entry's publication (SPEC-vmstate F1).
        if (m_refCount.compare_exchange_weak(value, value + s_refCountIncrement, std::memory_order_relaxed)) {
            STRING_STATS_REF_STRING(*this);
            return true;
        }
    }
}

inline void StringImpl::deref()
{
    STRING_STATS_DEREF_STRING(*this);

#if TSAN_ENABLED
    if (isStatic())
        return;
#endif

    // Relaxed latch read is sound even on threads created before the latch:
    // the §4.8 contract (SPEC-vmstate; normative text in
    // SharedAtomStringTable.h) requires a pre-latch thread to synchronize-with
    // the initializing thread before its next ref/deref of ANY WTF::String —
    // explicitly including strings it already owned pre-latch, because
    // AtomStringImpl::addSlowCase can atomize a co-owned StringImpl in place
    // into a shard after the latch. With that edge, write-read coherence
    // forbids this load from reading a stale 'false'; threads created after
    // the latch get the edge from thread creation itself. Full argument: F4
    // comment at sharedAtomStringTableEnabled() in SharedAtomStringTable.h.
    if (g_sharedAtomStringTableEnabled.load(std::memory_order_relaxed)) [[unlikely]] {
        // Shared-atom-table mode (SPEC-vmstate §4.4.3 / F3): the release
        // decrement plus the acquire fence on the zero transition order every
        // other thread's prior accesses to this string before its destruction
        // (deliberately NOT seq_cst). Refcount 0 is final: tryRefAtom() fails
        // at 0, so no table hit can revive the string and exactly one thread
        // reaches the zero transition and destroys it.
#if TSAN_ENABLED
        // TSAN r12 (reports 1/2/5): TSAN does not model
        // std::atomic_thread_fence, so the acquire fence below is invisible
        // and the destroying thread's free() pairs against other threads'
        // release decrements. acq_rel RMW under TSAN only; production keeps
        // release + acquire-fence-on-zero (identical ordering guarantees).
        auto oldRefCount = m_refCount.fetch_sub(s_refCountIncrement, std::memory_order_acq_rel);
#else
        auto oldRefCount = m_refCount.fetch_sub(s_refCountIncrement, std::memory_order_release);
#endif
        if (oldRefCount != s_refCountIncrement)
            return;
        std::atomic_thread_fence(std::memory_order_acquire);
        derefSharedZero();
        return;
    }

    // Legacy mode (per-thread atom tables): today's relaxed path, kept
    // verbatim — no flag-off RMW upgrade (SPEC-vmstate R3(b)).
    auto oldRefCount = m_refCount.fetch_sub(s_refCountIncrement, std::memory_order_relaxed);
    if (oldRefCount != s_refCountIncrement)
        return;

    // §4.8 ref/deref backstop (debug-only, zero release cost). This arm was
    // selected because the latch load above read false; if a re-read on the
    // zero transition now reads true, the latch flipped without this thread
    // having the happens-before edge the §4.8 contract requires before any
    // ref/deref by a pre-latch thread — a stale-latch breach caught in
    // flight. This matters because the release-mode failure is SILENT, not
    // fail-stop: if this string was meanwhile atomized in place into a shard
    // (addSlowCase atomizes co-owned strings), destroy() below would skip
    // removeDeadAtom and leave a dangling shard entry (UAF on the next
    // same-bucket probe). I17's ~AtomStringTable RELEASE_ASSERT backstops
    // only the ATOMIZE half of the contract (a pre-latch atomizer leaves a
    // non-empty per-thread table); this assert is the only detector for the
    // ref/deref half — heuristic (catches breaches whose deref overlaps or
    // follows the flip), with cross-WS item 15 (Bun embedder audit,
    // INTEGRATE-vmstate.md) as the release-mode mitigation.
    ASSERT(!g_sharedAtomStringTableEnabled.load(std::memory_order_relaxed));

    StringImpl::destroy(this);
}

template<typename SourceCharacterType>
inline void StringImpl::iterCharacters(jsstring_iterator* iter, unsigned start, const SourceCharacterType* source, unsigned numCharacters)
{
    static_assert(std::is_same_v<SourceCharacterType, Latin1Character> || std::is_same_v<SourceCharacterType, char16_t>);

    if constexpr (std::is_same_v<SourceCharacterType, Latin1Character>) {
       iter->write8(iter, (const void*) source, numCharacters, start);
    } else {
       iter->write16(iter, (const void*) source, numCharacters, start);
    }
}

inline char16_t StringImpl::at(unsigned i) const
{
    return is8Bit() ? span8()[i] : span16()[i];
}

inline StringImpl::StringImpl(CreateSymbolTag, std::span<const Latin1Character> characters)
    : StringImplShape(s_refCountIncrement, characters, s_hashFlag8BitBuffer | StringSymbol | BufferSubstring)
{
    ASSERT(is8Bit());
    ASSERT(m_data8);
    STRING_STATS_ADD_8BIT_STRING2(m_length, true);
}

inline StringImpl::StringImpl(CreateSymbolTag, std::span<const char16_t> characters)
    : StringImplShape(s_refCountIncrement, characters, s_hashZeroValue | StringSymbol | BufferSubstring)
{
    ASSERT(!is8Bit());
    ASSERT(m_data16);
    STRING_STATS_ADD_16BIT_STRING2(m_length, true);
}

inline StringImpl::StringImpl(CreateSymbolTag)
    : StringImplShape(s_refCountIncrement, empty()->span8(), s_hashFlag8BitBuffer | StringSymbol | BufferSubstring)
{
    ASSERT(is8Bit());
    ASSERT(m_data8);
    STRING_STATS_ADD_8BIT_STRING2(m_length, true);
}

template<typename T> inline size_t StringImpl::allocationSize(Checked<size_t> tailElementCount)
{
    return tailOffset<T>() + tailElementCount * sizeof(T);
}

template<typename CharacterType>
inline constexpr bool StringImpl::isValidLength(size_t length)
{
    // In order to not overflow the unsigned length, the check for (std::numeric_limits<unsigned>::max() - sizeof(StringImpl)) is needed when sizeof(CharacterType) == 2.
    constexpr size_t max = std::min(static_cast<size_t>(MaxLength), (std::numeric_limits<unsigned>::max() - sizeof(StringImpl)) / sizeof(CharacterType));
    return length <= max;
}

template<typename T> constexpr size_t StringImpl::tailOffset()
{
    return roundUpToMultipleOf<alignof(T)>(offsetof(StringImpl, m_hashAndFlags) + sizeof(StringImpl::m_hashAndFlags));
}

inline bool StringImpl::requiresCopy() const
{
    if (bufferOwnership() != BufferInternal)
        return true;

    if (is8Bit())
        return m_data8 == tailPointer<Latin1Character>();
    return m_data16 == tailPointer<char16_t>();
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
template<typename T> inline const T* StringImpl::tailPointer() const
{
    return reinterpret_cast_ptr<const T*>(reinterpret_cast<const uint8_t*>(this) + tailOffset<T>());
}

template<typename T> inline T* StringImpl::tailPointer()
{
    return reinterpret_cast_ptr<T*>(reinterpret_cast<uint8_t*>(this) + tailOffset<T>());
}
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

template<typename CharacterType> inline Ref<StringImpl> StringImpl::createUninitializedInternalNonEmpty(size_t length, std::span<CharacterType>& data)
{
    ASSERT(length);

    // Allocate a single buffer large enough to contain the StringImpl
    // struct as well as the data which it contains. This removes one
    // heap allocation from this call.
    if (!isValidLength<CharacterType>(length))
        CRASH();

    SUPPRESS_UNCOUNTED_LOCAL StringImpl* string = static_cast<StringImpl*>(StringImplMalloc::malloc(allocationSize<CharacterType>(length)));
    data = unsafeMakeSpan(string->tailPointer<CharacterType>(), length);
    return constructInternal<CharacterType>(*string, length);
}

inline StringImpl* const& StringImpl::substringBuffer() const
{
    ASSERT(bufferOwnership() == BufferSubstring);

    return *tailPointer<StringImpl*>();
}

inline StringImpl*& StringImpl::substringBuffer()
{
    ASSERT(bufferOwnership() == BufferSubstring);

    return *tailPointer<StringImpl*>();
}

inline void StringImpl::assertHashIsCorrect() const
{
    ASSERT(existingHash() == StringHasher::computeHashAndMaskTop8Bits(span8()));
}

constexpr StringImpl::StaticStringImpl::StaticStringImpl(ASCIILiteral literal, StringKind stringKind)
    : StringImplShape(s_refCountFlagIsStaticString, literal,
        s_hashFlag8BitBuffer | s_hashFlagDidReportCost | stringKind | BufferInternal | (StringHasher::computeLiteralHashAndMaskTop8Bits(literal) << s_flagCount), ConstructWithConstExpr)
{
}

template<unsigned characterCount> constexpr StringImpl::StaticStringImpl::StaticStringImpl(const char16_t (&characters)[characterCount], StringKind stringKind)
    : StringImplShape(s_refCountFlagIsStaticString, characterCount - 1, characters,
        s_hashFlagDidReportCost | stringKind | BufferInternal | (StringHasher::computeLiteralHashAndMaskTop8Bits(characters) << s_flagCount), ConstructWithConstExpr)
{
}

inline StringImpl::StaticStringImpl::operator StringImpl&()
{
    SUPPRESS_MEMORY_UNSAFE_CAST return *reinterpret_cast<StringImpl*>(this);
}

inline StringImpl::StaticStringImpl::operator const StringImpl&() const
{
    SUPPRESS_MEMORY_UNSAFE_CAST return *reinterpret_cast<const StringImpl*>(this);
}

inline bool equalIgnoringASCIICase(const StringImpl& a, const StringImpl& b)
{
    return equalIgnoringASCIICaseCommon(a, b);
}

inline bool equalIgnoringASCIICase(const StringImpl& a, ASCIILiteral b)
{
    return equalIgnoringASCIICaseCommon(a, b.characters());
}

inline bool equalIgnoringASCIICase(const StringImpl* a, ASCIILiteral b)
{
    return a && equalIgnoringASCIICase(*a, b);
}

inline bool startsWithLettersIgnoringASCIICase(const StringImpl& string, ASCIILiteral literal)
{
    return startsWithLettersIgnoringASCIICaseCommon(string, literal);
}

inline bool startsWithLettersIgnoringASCIICase(const StringImpl* string, ASCIILiteral literal)
{
    return string && startsWithLettersIgnoringASCIICase(*string, literal);
}

inline bool equalLettersIgnoringASCIICase(const StringImpl& string, ASCIILiteral literal)
{
    return equalLettersIgnoringASCIICaseCommon(string, literal);
}

inline bool equalLettersIgnoringASCIICase(const StringImpl* string, ASCIILiteral literal)
{
    return string && equalLettersIgnoringASCIICase(*string, literal);
}

template<typename CharacterType, typename Predicate> ALWAYS_INLINE Ref<StringImpl> StringImpl::removeCharactersImpl(std::span<const CharacterType> characters, const Predicate& findMatch)
{
    auto from = characters;

    // Assume the common case will not remove any characters
    while (!from.empty() && !findMatch(from[0]))
        skip(from, 1);
    if (from.empty())
        return *this;

    StringBuffer<CharacterType> data(length());
    auto to = data.span();
    unsigned outc = from.data() - characters.data();

    copyCharacters(to, characters.first(outc));

    do {
        while (!from.empty() && findMatch(from[0]))
            skip(from, 1);
        while (!from.empty() && !findMatch(from[0]))
            to[outc++] = consume(from);
    } while (!from.empty());

    data.shrink(outc);

    return adopt(WTF::move(data));
}

template<typename Predicate>
inline Ref<StringImpl> StringImpl::removeCharacters(const Predicate& findMatch)
{
    static_assert(!std::is_function_v<Predicate>, "Passing a lambda instead of a function pointer helps the compiler with inlining");
    if (is8Bit())
        return removeCharactersImpl(span8(), findMatch);
    return removeCharactersImpl(span16(), findMatch);
}

inline Ref<StringImpl> StringImpl::createByReplacingInCharacters(std::span<const Latin1Character> characters, char16_t target, char16_t replacement, size_t indexOfFirstTargetCharacter)
{
    ASSERT(indexOfFirstTargetCharacter < characters.size());
    if (isLatin1(replacement)) {
        std::span<Latin1Character> data;
        Latin1Character oldChar = target;
        Latin1Character newChar = replacement;
        auto newImpl = createUninitializedInternalNonEmpty(characters.size(), data);
        memcpySpan(data, characters.first(indexOfFirstTargetCharacter));
        for (size_t i = indexOfFirstTargetCharacter; i != characters.size(); ++i) {
            Latin1Character character = characters[i];
            data[i] = character == oldChar ? newChar : character;
        }
        return newImpl;
    }

    std::span<char16_t> data;
    auto newImpl = createUninitializedInternalNonEmpty(characters.size(), data);
    size_t i = 0;
    for (auto character : characters)
        data[i++] = character == target ? replacement : character;
    return newImpl;
}

inline Ref<StringImpl> StringImpl::createByReplacingInCharacters(std::span<const char16_t> characters, char16_t target, char16_t replacement, size_t indexOfFirstTargetCharacter)
{
    ASSERT(indexOfFirstTargetCharacter < characters.size());
    std::span<char16_t> data;
    auto newImpl = createUninitializedInternalNonEmpty(characters.size(), data);
    copyCharacters(data, characters.first(indexOfFirstTargetCharacter));
    for (size_t i = indexOfFirstTargetCharacter; i != characters.size(); ++i) {
        char16_t character = characters[i];
        data[i] = character == target ? replacement : character;
    }
    return newImpl;
}

template<typename Func>
inline Expected<std::invoke_result_t<Func, std::span<const char8_t>>, UTF8ConversionError> StringImpl::tryGetUTF8(NOESCAPE const Func& function, ConversionMode mode) const
{
    if (is8Bit())
        return tryGetUTF8ForCharacters(function, span8());
    return tryGetUTF8ForCharacters(function, span16(), mode);
}

static inline std::span<const char8_t> nonNullEmptyUTF8Span()
{
    static constexpr char8_t empty = 0;
    return { &empty, 0 };
}

template<typename Func>
inline Expected<std::invoke_result_t<Func, std::span<const char8_t>>, UTF8ConversionError> StringImpl::tryGetUTF8ForCharacters(NOESCAPE const Func& function, std::span<const Latin1Character> characters)
{
    if (characters.empty())
        return function(nonNullEmptyUTF8Span());

    // Allocate a buffer big enough to hold all the characters
    // (an individual Latin1Character can only expand to 2 UTF-8 bytes).
    // Optimization ideas, if we find this function is hot:
    //  * We could speculatively create a CStringBuffer to contain 'length'
    //    characters, and resize if necessary (i.e. if the buffer contains
    //    non-ascii characters). (Alternatively, scan the buffer first for
    //    ascii characters, so we know this will be sufficient).
    //  * We could allocate a CStringBuffer with an appropriate size to
    //    have a good chance of being able to write the string into the
    //    buffer without reallocing (say, 1.5 x length).

    if (productOverflows<size_t>(characters.size(), 2) || !isValidCapacityForVector<char8_t>(characters.size() * 2)) [[unlikely]]
        return makeUnexpected(UTF8ConversionError::OutOfMemory);

#if CPU(ARM64)
    if (auto* firstNonASCII = find8NonASCII(characters)) {
        size_t prefixLength = firstNonASCII - characters.data();
        size_t remainingLength = characters.size() - prefixLength;
        Vector<char8_t, 1024> buffer(prefixLength + remainingLength * 2);
        memcpySpan(buffer.mutableSpan(), characters.first(prefixLength));
        auto result = Unicode::convert(characters.subspan(prefixLength), buffer.mutableSpan().subspan(prefixLength));
        ASSERT(result.code == Unicode::ConversionResultCode::Success); // 2x is sufficient for any conversion from Latin1
        return function(buffer.span().first(prefixLength + result.buffer.size()));
    }
    return function(byteCast<char8_t>(characters));
#else
    Vector<char8_t, 1024> buffer(characters.size() * 2);
    auto result = Unicode::convert(characters, buffer.mutableSpan());
    ASSERT(result.code == Unicode::ConversionResultCode::Success); // 2x is sufficient for any conversion from Latin1
    return function(result.buffer);
#endif
}

template<typename Func>
inline Expected<std::invoke_result_t<Func, std::span<const char8_t>>, UTF8ConversionError> StringImpl::tryGetUTF8ForCharacters(NOESCAPE const Func& function, std::span<const char16_t> characters, ConversionMode mode)
{
    if (characters.empty())
        return function(nonNullEmptyUTF8Span());

    size_t utf8Length = utf8LengthFromUTF16(characters);
    if (!isValidCapacityForVector<char8_t>(utf8Length)) [[unlikely]]
        return makeUnexpected(UTF8ConversionError::OutOfMemory);

    Vector<char8_t, 1024> bufferVector(utf8Length);
    size_t simdConvertedSize = tryConvertUTF16ToUTF8(characters, bufferVector.mutableSpan());
    if (simdConvertedSize != notFound)
        return function(bufferVector.span().first(simdConvertedSize));

    if (productOverflows<size_t>(characters.size(), 3) || !isValidCapacityForVector<char8_t>(characters.size() * 3)) [[unlikely]]
        return makeUnexpected(UTF8ConversionError::OutOfMemory);

    size_t bufferSize = characters.size() * 3;
    bufferVector.resize(bufferSize);
    auto convertedSize = utf8ForCharactersIntoBuffer(characters, mode, bufferVector);
    if (!convertedSize)
        return makeUnexpected(convertedSize.error());
    return function(bufferVector.span().first(*convertedSize));
}

} // namespace WTF

using WTF::StaticStringImpl;
using WTF::StringImpl;
using WTF::equal;
using WTF::isUnicodeWhitespace;
using WTF::deprecatedIsSpaceOrNewline;
using WTF::deprecatedIsNotSpaceOrNewline;
