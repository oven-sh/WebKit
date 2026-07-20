/*
 *  Copyright (C) 2024 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include <cstdint>
#include <wtf/HashFunctions.h>
#include <wtf/Packed.h>
#include <wtf/RawPtrTraits.h>
#include <wtf/RefPtr.h>
#include <wtf/text/StringHasher.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

// A UniquedStringImpl* slot may instead hold an inline-encoded fiber word
// (bit 1 set, bit 0 clear; see JSString::isInlineInPointer). Real impls are
// 8-byte aligned so the low 3 bits distinguish. The word is content-unique for
// 2..7 Latin-1 / 1..3 UTF-16 characters, so pointer-identity comparisons and
// PtrHash-style hashing remain sound without dereferencing.
//
// These helpers shim the handful of call sites that dereference a uid for
// isSymbol() / hash() / length() / ref() / deref() so they branch on the tag
// first. Call sites that only compare or store the raw value need no change.

static constexpr uintptr_t inlinePropertyKeyTag = 0x2;
static constexpr uintptr_t inlinePropertyKeyTagMask = 0x3;

// Canonical fiber-word keys cap at 5 chars so the whole word fits in bits 0..47.
// CompactPropertyTableEntry / StructureTransitionTable::Hash::Key pack attributes
// into bits 48..63 of a UniquedStringImpl* slot; a 6/7-char payload there would
// be truncated. 6..7-char names stay atom-backed for D.4 coherence.
static constexpr unsigned maxFiberWordKeyLength = 5;

// Compile-time killswitch for the D.2/D.4 producer paths. Flip to false to
// disable fiber-word Identifiers in one place (debug: 'shims broken' vs
// 'producer reached unguarded site') without reverting commits.
static constexpr bool enableIdentifierFiberWords = true;

ALWAYS_INLINE bool isInlinePropertyKey(const UniquedStringImpl* impl)
{
    return (reinterpret_cast<uintptr_t>(impl) & inlinePropertyKeyTagMask) == inlinePropertyKeyTag;
}

ALWAYS_INLINE bool isInlinePropertyKey(uintptr_t word)
{
    return (word & inlinePropertyKeyTagMask) == inlinePropertyKeyTag;
}

ALWAYS_INLINE uintptr_t inlinePropertyKeyWord(const UniquedStringImpl* impl)
{
    return reinterpret_cast<uintptr_t>(impl);
}

ALWAYS_INLINE UniquedStringImpl* inlinePropertyKeyAsImpl(uintptr_t word)
{
    return reinterpret_cast<UniquedStringImpl*>(word);
}

// Layout matches JSString's inline encoding: bit 2 is is8Bit, bits 3..6 are
// length, payload at byte 1 (8-bit) or byte 2 (16-bit).
ALWAYS_INLINE bool inlinePropertyKeyIs8Bit(uintptr_t word) { return word & 0x4; }
ALWAYS_INLINE unsigned inlinePropertyKeyLength(uintptr_t word) { return static_cast<unsigned>(word >> 3) & 0xf; }

// Span into the caller's fiber-word storage (taken by reference so the returned
// span stays valid for the enclosing full-expression / member lifetime).
ALWAYS_INLINE std::span<const Latin1Character> inlinePropertyKeySpan8(const uintptr_t& word)
{
    ASSERT(isInlinePropertyKey(word));
    ASSERT(inlinePropertyKeyIs8Bit(word));
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&word);
    return std::span<const Latin1Character> { bytes + 1, inlinePropertyKeyLength(word) };
}

ALWAYS_INLINE unsigned inlinePropertyKeyHash(uintptr_t word)
{
    // Must match StringImpl::hash() for equal content so a fiber-word key
    // compares against an AtomStringImpl* key with the same hash.
    unsigned len = inlinePropertyKeyLength(word);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&word);
    if (inlinePropertyKeyIs8Bit(word))
        return StringHasher::computeHashAndMaskTop8Bits(std::span<const Latin1Character> { bytes + 1, len });
    return StringHasher::computeHashAndMaskTop8Bits(std::span<const char16_t> { reinterpret_cast<const char16_t*>(bytes + 2), len });
}

// Shims: safe to call on either a real impl or a fiber word.

ALWAYS_INLINE bool uidIsSymbol(const UniquedStringImpl* impl)
{
    uintptr_t bits = reinterpret_cast<uintptr_t>(impl);
    if (bits & inlinePropertyKeyTag)
        return false;
    return impl->isSymbol();
}

// 64-bit Fibonacci / golden-ratio multiplicative hash constant (2^64 / phi, odd).
// Kept here so the C++ uidHash() and its DFG/FTL/AssemblyHelpers JIT mirrors all
// reference the same value.
static constexpr uint64_t uidHashMultiplier = 0x9E3779B97F4A7C15ULL;

ALWAYS_INLINE unsigned uidHash(const UniquedStringImpl* impl)
{
    // canonicalFiberWordFor guarantees one m_bits value per content, so hashing
    // the raw pointer word is sound for both real impls and fiber words.
    // Cheap branch-free Fibonacci mix: top 32 bits of the 64-bit product depend on
    // all input bits (so fiber-word payload bytes and atom-pointer address bits both
    // spread into the low mask). Emitted identically at every JIT site that mirrors a
    // uidHash()-keyed cache — keep in sync with AssemblyHelpers load/store/has
    // MegamorphicProperty and DFG/FTL compileHasOwnProperty.
    uint64_t bits = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(impl));
    return static_cast<unsigned>((bits * uidHashMultiplier) >> 32);
}

ALWAYS_INLINE unsigned uidLength(const UniquedStringImpl* impl)
{
    if (isInlinePropertyKey(impl)) [[unlikely]]
        return inlinePropertyKeyLength(inlinePropertyKeyWord(impl));
    return impl->length();
}

ALWAYS_INLINE void uidRef(UniquedStringImpl* impl)
{
    if (!isInlinePropertyKey(impl)) [[likely]]
        impl->ref();
}

ALWAYS_INLINE void uidDeref(UniquedStringImpl* impl)
{
    if (!isInlinePropertyKey(impl)) [[likely]]
        impl->deref();
}

// RefDerefTraits for RefPtr<UniquedStringImpl, ...> slots that may hold a
// fiber word instead of a real heap pointer. Matches DefaultRefDerefTraits
// shape so RefPtr/HashTable machinery works unchanged; only ref/deref branch.
struct FiberAwareRefDerefTraits {
    static ALWAYS_INLINE UniquedStringImpl* refIfNotNull(UniquedStringImpl* ptr)
    {
        if (ptr) [[likely]]
            uidRef(ptr);
        return ptr;
    }
    static ALWAYS_INLINE UniquedStringImpl& ref(UniquedStringImpl& r)
    {
        uidRef(&r);
        return r;
    }
    static ALWAYS_INLINE void derefIfNotNull(UniquedStringImpl* ptr)
    {
        if (ptr) [[likely]]
            uidDeref(ptr);
    }
};

using FiberAwareRefPtr = RefPtr<UniquedStringImpl, WTF::RawPtrTraits<UniquedStringImpl>, FiberAwareRefDerefTraits>;
// PackedPtrTraits stores only EFFECTIVE_ADDRESS_WIDTH/8 = 6 bytes; a 6-7 char
// Latin-1 fiber word uses bytes 6-7, so Packed storage would truncate it.
// Alias to the full-width RawPtrTraits RefPtr and accept +2 bytes per entry.
using FiberAwarePackedRefPtr = FiberAwareRefPtr;

} // namespace JSC

#else // USE(BUN_JSC_ADDITIONS)

namespace JSC {
// Transparent fallbacks so bare uidIsSymbol() / isInlinePropertyKey() at call
// sites compile in non-BUN builds without #if guards at each one.
ALWAYS_INLINE bool isInlinePropertyKey(const WTF::UniquedStringImpl*) { return false; }
ALWAYS_INLINE bool uidIsSymbol(const WTF::UniquedStringImpl* impl) { return impl->isSymbol(); }
} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
