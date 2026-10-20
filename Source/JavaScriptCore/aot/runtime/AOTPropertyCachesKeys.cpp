/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesKeys.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTObject.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include <algorithm>

namespace JSC { namespace AOT {

KeyName nameOfKeyWithoutAllocating(JSGlobalObject* globalObject, JSCell* key, KeyAtomization atomization)
{
    if (key->isSymbol()) {
        SymbolImpl& uid = asSymbol(key)->uid();
        if (uid.isPrivate())
            return { };
        return { KeyNameKind::Name, &uid };
    }
    if (!key->isString())
        return { };
    constexpr unsigned maxIndexLength = 10;
    JSString* string = asString(key);
    StringImpl* text = string->tryGetValueImpl();
    bool isRope = !text;
    if (isRope || !text->isAtom()) {
        if (string->length() > JSString::maxLengthForOnStackResolve)
            return { };
        bool isIndex = text && parseIndex(*text);
        if (!isIndex)
            text = atomization == KeyAtomization::Atomize ? string->toAtomString(globalObject).data : string->toExistingAtomString(globalObject).data;
        if (!text)
            return { isRope && string->length() <= maxIndexLength ? KeyNameKind::NotAName : KeyNameKind::HasNoAtom };
    }
    if (std::optional<uint32_t> index = parseIndex(*text))
        return { KeyNameKind::Index, nullptr, *index };
    if (text == globalObject->vm().propertyNames->underscoreProto.impl())
        return { };
    return { KeyNameKind::Name, static_cast<UniquedStringImpl*>(text) };
}

std::optional<uint32_t> indexOfKey(JSValue key)
{
    if (std::optional<uint32_t> index = key.tryGetAsUint32Index())
        return index;
    StringImpl* text = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
    return text ? parseIndex(*text) : std::nullopt;
}

std::optional<Identifier> nameForKeyedCache(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = globalObject->vm();
    if (key.isSymbol())
        return Identifier::fromUid(asSymbol(key)->privateName());
    if (!key.isString())
        return std::nullopt;
    auto existingAtomString = asString(key)->toExistingAtomString(globalObject);
    if (!existingAtomString || parseIndex(*existingAtomString.data))
        return std::nullopt;
    return Identifier::fromUid(vm, existingAtomString.data);
}

UniquedStringImpl* atomOfKey(JSValue key)
{
    StringImpl* text = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
    return text && text->isAtom() ? static_cast<UniquedStringImpl*>(text) : nullptr;
}

UniquedStringImpl* uidOfKey(JSCell* key)
{
    return key->isSymbol() ? &asSymbol(key)->uid() : atomOfKey(key);
}

struct StringComparison {
    StringView name;
    uint64_t remainingPieces;
    bool isEqual { true };
};

template<typename Character>
static void compareStringPiece(jsstring_iterator* iterator, const void* characters, uint32_t length, uint32_t offset)
{
    auto& comparison = *static_cast<StringComparison*>(iterator->data);
    auto isEqualToName = [&] {
        std::span piece { static_cast<const Character*>(characters), length };
        if constexpr (std::is_same_v<Character, Latin1Character>) {
            if (comparison.name.is8Bit() && offset <= comparison.name.length() && length <= comparison.name.length() - offset)
                return equalSpans(comparison.name.span8().subspan(offset, length), piece);
        }
        return comparison.name.substring(offset, length) == StringView(piece);
    };
    if (comparison.remainingPieces && isEqualToName()) {
        comparison.remainingPieces--;
        return;
    }
    comparison.isEqual = false;
    iterator->stop = 1;
}

template<typename Character>
static void compareWholeString(jsstring_iterator* iterator, const void* characters, uint32_t length)
{
    compareStringPiece<Character>(iterator, characters, length, 0);
}

static bool stringEqualsName(JSString* string, StringView name, uint64_t maxPieces, bool& hasTooManyPieces)
{
    StringComparison comparison { name, maxPieces };
    jsstring_iterator iterator { };
    iterator.data = &comparison;
    iterator.append8 = compareWholeString<Latin1Character>;
    iterator.append16 = compareWholeString<char16_t>;
    iterator.write8 = compareStringPiece<Latin1Character>;
    iterator.write16 = compareStringPiece<char16_t>;
    string->value(&iterator);
    hasTooManyPieces |= !comparison.remainingPieces;
    return comparison.isEqual;
}

static bool mayHaveFieldWithNameOfLength(const Object& object, unsigned length)
{
    if (!object.state().tables.removedFields().empty())
        return true;
    const ImageText& text = Image::current()->text();
    return std::ranges::any_of(object.layout().fields(), [&](const FieldData& field) {
        StringID name = text.identifier(field.name).text();
        return name && text.view(name).length() == length;
    });
}

std::optional<UniquedStringImpl*> findNameEqualToLongString(JSValue base, JSValue key, bool checksPrototypes)
{
    if (!key.isString() || asString(key)->length() <= JSString::maxLengthForOnStackResolve || !base.isObject())
        return std::nullopt;
    JSString* string = asString(key);
    if (StringImpl* text = string->tryGetValueImpl(); text && text->isAtom())
        return std::nullopt;
    VM& vm = string->vm();
    unsigned length = string->length();
    uint64_t maxPieces = length / std::max<uint64_t>(runtimeLimitValue(vm, LimitID::MinLongKeyUnitsPerPiece), 1) + runtimeLimitValue(vm, LimitID::LongKeyExtraPieces);
    uint64_t remainingNames = std::max<uint64_t>(length / 4, runtimeLimitValue(vm, LimitID::MaxNamesComparedWithLongKey));
    for (JSObject* holder = asObject(base);;) {
        JSValue prototype = holder->getPrototypeDirect();
        if (Object* compiled = compiledObjectOrNull(holder)) {
            if (mayHaveFieldWithNameOfLength(*compiled, length))
                return std::nullopt;
            holder = compiled->extraProperties();
        }
        if (!holder) {
            if (!checksPrototypes || !prototype.isObject())
                return nullptr;
            holder = asObject(prototype);
            continue;
        }
        Structure* structure = holder->structure();
        TypeInfo info = structure->typeInfo();
        if (info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || TypeInfo::hasStaticPropertyTable(holder->inlineTypeFlags()) || structure->isCompiledStructure())
            return std::nullopt;
        unsigned numberOfNames = structure->isDictionary() ? structure->dictionaryPropertyCount() : numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity());
        if (numberOfNames > remainingNames) {
            countLimitHit(vm, LimitID::MaxNamesComparedWithLongKey);
            return std::nullopt;
        }
        remainingNames -= numberOfNames;
        UniquedStringImpl* equalName = nullptr;
        bool hasTooManyPieces = false;
        structure->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
            UniquedStringImpl* name = entry.key();
            if (name->length() == length && !name->isSymbol() && stringEqualsName(string, name, maxPieces, hasTooManyPieces))
                equalName = name;
            return !equalName && !hasTooManyPieces;
        });
        if (hasTooManyPieces) {
            countLimitHit(vm, LimitID::MinLongKeyUnitsPerPiece);
            return std::nullopt;
        }
        if (equalName)
            return equalName;
        if (!checksPrototypes || !prototype.isObject())
            return nullptr;
        holder = asObject(prototype);
    }
}

ChainLookups classifyChainLookups(JSObject* base)
{
    for (JSObject* object = base;;) {
        Structure* structure = object->structure();
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) || TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) || structure->typeInfo().overridesGetPrototype() || structure->isCompiledStructure())
            return ChainLookups::Unknown;
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return ChainLookups::Ordinary;
        object = asObject(prototype);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
