/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompactTable.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTTableInImage.h"
#include "HashMapHelper.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include <wtf/text/StringHasherInlines.h>

namespace JSC { namespace AOT {

static ALWAYS_INLINE const StringImpl* stringImplOrNull(JSValue storedKey)
{
    return storedKey && storedKey.isString() ? asString(storedKey)->tryGetValueImpl() : nullptr;
}

template<typename CharacterType>
ALWAYS_INLINE std::optional<uint32_t> CompactTable::findCharacters(std::span<const CharacterType> characters) const
{
    unsigned hash = StringHasher::computeHashAndMaskTop8Bits(characters);
    Lookup lookup = tryFindCharactersQuickly(characters, hash);
    switch (lookup.result) {
    case QuickLookup::Found:
        return lookup.entry;
    case QuickLookup::Absent:
        return std::nullopt;
    case QuickLookup::Unknown:
        break;
    }
    return findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
        const StringImpl* impl = stringImplOrNull(heapKeyAt(entry));
        return impl && impl->existingHash() == hash && equalsCharacters(*impl, characters);
    });
}

template<typename CharacterType>
static std::optional<uint32_t> findCharactersInImage(const Image& image, const TableInImage& entries, std::span<const CharacterType> characters)
{
    return entries.find(image, StringView { characters }, StringHasher::computeHashAndMaskTop8Bits(characters));
}

std::optional<uint32_t> CompactTable::findInImage(JSGlobalObject* globalObject, JSString* key) const
{
    const Image& image = *Image::current();
    const TableInImage& entries = tableInImage();
    if (const StringImpl* impl = key->tryGetValueImpl()) [[likely]]
        return entries.find(image, StringView { *impl }, impl->hash());
    if (key->isSubstring()) {
        auto characters = key->view(globalObject);
        return characters->is8Bit() ? findCharactersInImage(image, entries, characters->span8()) : findCharactersInImage(image, entries, characters->span16());
    }
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto string = key->value(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    const StringImpl& impl = *string->impl();
    return entries.find(image, StringView { impl }, impl.hash());
}

std::optional<uint32_t> CompactTable::find(JSGlobalObject* globalObject, JSValue key) const
{
    key = normalizeMapKey(key);
    if (key.isString())
        return findString(globalObject, asString(key));
    if (isInImage())
        return std::nullopt;
    return findOtherKey(globalObject, key);
}

std::optional<uint32_t> CompactTable::findString(JSGlobalObject* globalObject, JSString* key) const
{
    if (isInImage())
        return findInImage(globalObject, key);
    if (key->isSubstring()) {
        auto characters = key->view(globalObject);
        auto entry = characters->is8Bit() ? findCharacters(characters->span8()) : findCharacters(characters->span16());
        if (entry) {
            StringImpl& impl = *const_cast<StringImpl*>(asString(heapKeyAt(*entry))->tryGetValueImpl());
            if (impl.is8Bit() || !key->is8Bit())
                uncheckedDowncast<JSRopeString>(key)->resolveSubstringBySharing(impl);
        }
        return entry;
    }

    Lookup lookup = tryFindQuickly(key);
    switch (lookup.result) {
    case QuickLookup::Found:
        return lookup.entry;
    case QuickLookup::Absent:
        return std::nullopt;
    case QuickLookup::Unknown:
        break;
    }
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto string = key->value(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    const StringImpl* impl = string->impl();
    unsigned hash = impl->hash();
    return findWithHash(hash, [&](uint32_t entry) {
        const StringImpl* other = stringImplOrNull(heapKeyAt(entry));
        return other && (other == impl || (other->hash() == hash && equal(other, impl)));
    });
}

std::optional<uint32_t> CompactTable::findOtherKey(JSGlobalObject*, JSValue normalizedKey) const
{
    ASSERT(normalizedKey && !normalizedKey.isString());
    if (normalizedKey.isNumber() && keyWidth() == Width::Narrow)
        return std::nullopt;
    JSBigInt* bigInt = normalizedKey.isHeapBigInt() ? normalizedKey.asHeapBigInt() : nullptr;
    return findWithHash(hashOfStoredKey(normalizedKey), [&](uint32_t entry) {
        JSValue candidate = heapKeyAt(entry);
        if (candidate == normalizedKey)
            return true;
        return bigInt && candidate && candidate.isHeapBigInt() && JSBigInt::equals(candidate.asHeapBigInt(), bigInt);
    });
}

std::optional<uint32_t> CompactTable::find(std::span<const Latin1Character> characters) const
{
    if (isInImage())
        return findCharactersInImage(*Image::current(), tableInImage(), characters);
    return findCharacters(characters);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
