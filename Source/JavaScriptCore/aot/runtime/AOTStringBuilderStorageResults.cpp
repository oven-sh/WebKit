/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderStorage.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "NumberPrototype.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

template<typename Functor>
void StringBuilderStorage::forEachPiece(const Functor& functor) const
{
    uint32_t taken = 0;
    for (const Segment& segment : m_segments) {
        if (segment.offsetInBuffer > taken)
            functor(Piece { nullptr, taken, segment.offsetInBuffer - taken });
        if (segment.length)
            functor(Piece { segment.string.get(), 0, segment.length });
        taken = segment.offsetInBuffer;
    }
    if (m_position > taken)
        functor(Piece { nullptr, taken, m_position - taken });
}

RefPtr<StringImpl> StringBuilderStorage::tryShareOrGiveAwayBuffer(bool mayGiveBufferAway)
{
    ASSERT(m_segments.isEmpty());
    if (m_position < m_limits.minLengthAdoptedByFinish)
        return nullptr;
    if (!mayGiveBufferAway) {
        m_sharedLength = std::max(m_sharedLength, m_position);
        return StringImpl::createSubstringSharingImpl(*m_buffer, 0, m_position);
    }
    if (capacity() <= m_limits.maxRetainedCapacity)
        return nullptr;
    countLimitHit(vm(), LimitID::MaxRetainedStringBuilderCapacity);
    RefPtr<StringImpl> impl = m_position == capacity() ? m_buffer : RefPtr<StringImpl> { StringImpl::createSubstringSharingImpl(*m_buffer, 0, m_position) };
    dropBuffer();
    return impl;
}

static JSString* createIndexKey(VM& vm, StringKeyCache& keys, uint32_t index, std::span<const Latin1Character> characters)
{
    ASSERT(index <= INT32_MAX);
    if (index < NumericStrings::cacheSize) {
        JSString* string = int32ToString(vm, static_cast<int32_t>(index), 10);
        string->tryGetValueImpl()->hash();
        return string;
    }
    if (JSString* string = keys.tryFindIntegerKey(index))
        return string;
    JSString* string = keys.makeIntegerKey(vm, index, characters);
    vm.numericStrings.setJSString(static_cast<int32_t>(index), string->tryGetValue(), string);
    return string;
}

static JSString* tryCreateIndexKey(VM& vm, StringKeyCache& keys, std::span<const Latin1Character> characters)
{
    constexpr size_t maxLengthOfIndex = 10;
    if (characters.empty() || characters.size() > maxLengthOfIndex || !isASCIIDigit(characters.back()))
        return nullptr;
    std::optional<uint32_t> index = parseIndex(characters);
    if (!index || *index > INT32_MAX)
        return nullptr;
    return createIndexKey(vm, keys, *index, characters);
}

ALWAYS_INLINE JSString* StringBuilderStorage::copyBufferToString(JSGlobalObject* globalObject)
{
    ASSERT(m_position > 1);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<Latin1Character> output;
    RefPtr<StringImpl> impl = StringImpl::tryCreateUninitialized(m_position, output);
    if (!impl) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    StringImpl::copyCharacters(output, std::span<const Latin1Character> { m_characters, m_position });
    return jsNontrivialString(vm, String { impl.releaseNonNull() });
}

static JSString* tryCreateKey(VM& vm, Instance& instance, std::span<const Latin1Character> characters)
{
    StringKeyCache& keys = instance.state->propertyCaches->stringKeys();
    if (JSString* key = tryCreateIndexKey(vm, keys, characters))
        return key;
    if (characters.size() <= keys.maxCachedKeyLength())
        return keys.findOrAdd(vm, characters);
    countLimitHit(vm, LimitID::MaxCachedKeyLength);
    return nullptr;
}

JSString* StringBuilderStorage::createStringFromBuffer(JSGlobalObject* globalObject, Instance& instance, StringUse use, bool mayGiveBufferAway)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const Latin1Character> characters { m_characters, m_position };
    if (m_position == 1)
        return jsSingleCharacterString(vm, characters[0]);
    if (use == StringUse::LikelyKey) {
        if (JSString* key = tryCreateKey(vm, instance, characters))
            return key;
    }
    if (RefPtr<StringImpl> impl = tryShareOrGiveAwayBuffer(mayGiveBufferAway))
        return jsNontrivialString(vm, String { impl.releaseNonNull() });
    RELEASE_AND_RETURN(scope, copyBufferToString(globalObject));
}

JSString* StackStringBuilder::finish(JSGlobalObject* globalObject, Instance& instance, const Latin1Character* cursor, StringBuilderStorage::StringUse use)
{
    VM& vm = globalObject->vm();
    if (!isOnStack()) {
        if (!moveToStorage(globalObject, instance, cursor)) [[unlikely]]
            return nullptr;
        return storage->finish(globalObject, instance, use);
    }
    std::span<const Latin1Character> characters = charactersOnStack(cursor);
    if (characters.size() <= 1)
        return characters.empty() ? jsEmptyString(vm) : jsSingleCharacterString(vm, characters[0]);
    if (use == StringBuilderStorage::StringUse::LikelyKey) {
        if (JSString* key = tryCreateKey(vm, instance, characters))
            return key;
    }
    return jsNontrivialString(vm, String(characters));
}

JSString* StringBuilderStorage::createFlatRun(JSGlobalObject* globalObject, std::span<const Piece> run)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (run.size() == 1 && run[0].string && run[0].length == run[0].string->length())
        return run[0].string;

    size_t total = 0;
    bool is8Bit = true;
    for (const Piece& piece : run) {
        total += piece.length;
        is8Bit &= !piece.string || piece.string->is8Bit();
    }

    std::span<Latin1Character> output8;
    std::span<char16_t> output16;
    RefPtr<StringImpl> impl = is8Bit ? StringImpl::tryCreateUninitialized(total, output8) : StringImpl::tryCreateUninitialized(total, output16);
    if (!impl) {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }

    auto copyPieces = [&](auto output) {
        size_t at = 0;
        for (const Piece& piece : run) {
            auto destination = output.subspan(at, piece.length);
            at += piece.length;
            if (!piece.string) {
                StringImpl::copyCharacters(destination, std::span<const Latin1Character> { m_characters + piece.start, piece.length });
                continue;
            }
            if (piece.length == piece.string->length()) {
                piece.string->resolveToBuffer(destination);
                continue;
            }
            auto view = piece.string->view(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
            view->left(piece.length).getCharacters(destination);
        }
    };
    if (is8Bit)
        copyPieces(output8);
    else
        copyPieces(output16);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return jsString(vm, String { impl.releaseNonNull() });
}

JSString* StringBuilderStorage::createStringFromPieces(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t minFiberLength = m_limits.minLengthAppendedByReference;

    Vector<Piece, 16> pieces;
    forEachPiece([&](const Piece& piece) {
        pieces.append(piece);
    });

    JSRopeString::RopeBuilder<RecordOverflow> fibers(vm);
    size_t runStart = 0;
    for (size_t index = 0; index <= pieces.size(); ++index) {
        bool hasPiece = index < pieces.size();
        if (hasPiece && (!pieces[index].string || pieces[index].length < minFiberLength))
            continue;
        if (index > runStart) {
            JSString* run = createFlatRun(globalObject, pieces.span().subspan(runStart, index - runStart));
            RETURN_IF_EXCEPTION(scope, nullptr);
            fibers.append(run);
        }
        if (hasPiece) {
            const Piece& piece = pieces[index];
            JSString* fiber = piece.length == piece.string->length() ? piece.string : jsSubstring(globalObject, piece.string, 0, piece.length);
            RETURN_IF_EXCEPTION(scope, nullptr);
            fibers.append(fiber);
        }
        runStart = index + 1;
    }
    if (fibers.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    return fibers.release();
}

bool StringBuilderStorage::collapseSegments(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* all = createStringFromPieces(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    uint32_t length = all->length();
    ASSERT(length == this->length());
    {
        Locker locker { cellLock() };
        m_segments.shrink(0);
        m_segments.append(Segment { 0, length, WriteBarrier<JSString>(vm, this, all) });
    }
    m_lengthOutsideBuffer = length;
    m_position = 0;
    m_lastString.set(vm, this, all);
    m_lengthOfLastString = length;
    updateLimit();
    return true;
}

JSString* StringBuilderStorage::createString(JSGlobalObject* globalObject, Instance& instance, StringUse use, bool mayGiveBufferAway)
{
    if (!length())
        return jsEmptyString(globalObject->vm());
    if (m_segments.isEmpty())
        return createStringFromBuffer(globalObject, instance, use, mayGiveBufferAway);
    if (!collapseSegments(globalObject))
        return nullptr;
    return m_segments[0].string.get();
}

JSString* StringBuilderStorage::toString(JSGlobalObject* globalObject, Instance& instance, StringUse use)
{
    if (JSString* string = validLastString())
        return string;
    JSString* string = createString(globalObject, instance, use, false);
    if (!string)
        return nullptr;
    m_lastString.set(globalObject->vm(), this, string);
    m_lengthOfLastString = length();
    return string;
}

ALWAYS_INLINE std::optional<StringKeyCache::Lookup> StringBuilderStorage::tryFinishWithCachedKey(Instance& instance, JSString*& key, unsigned& hash)
{
    if (!holdsOnlyShortTextInOwnBuffer() || m_limit < StringKeyCache::minReadableBytes) [[unlikely]]
        return std::nullopt;
    const StringKeyCache& keys = instance.state->propertyCaches->stringKeys();
    if (m_position > keys.maxCachedKeyLength()) [[unlikely]]
        return std::nullopt;
    StringKeyCache::Lookup lookup = keys.findKeyForText(std::span<const Latin1Character> { m_characters, m_position }, key, hash);
    if (lookup == StringKeyCache::Lookup::Found) {
        m_position = 0;
        returnToPool(instance);
    }
    return lookup;
}

ALWAYS_INLINE JSString* StringBuilderStorage::finishWithNewKey(Instance& instance, unsigned hash)
{
    JSString* key = instance.state->propertyCaches->stringKeys().makeKey(*instance.vm, std::span<const Latin1Character> { m_characters, m_position }, hash);
    m_position = 0;
    returnToPool(instance);
    return key;
}

JSString* StringBuilderStorage::finish(JSGlobalObject* globalObject, Instance& instance, StringUse use)
{
    if (holdsOnlyShortTextInOwnBuffer()) [[likely]] {
        JSString* string = use == StringUse::Unknown ? copyBufferToString(globalObject) : createStringFromBuffer(globalObject, instance, use, true);
        m_position = 0;
        returnToPool(instance);
        return string;
    }
    JSString* string = validLastString();
    if (!string)
        string = createString(globalObject, instance, use, true);
    release(instance);
    return string;
}

static std::optional<int32_t> tryGetInt32(JSValue primitive)
{
    if (primitive.isInt32())
        return primitive.asInt32();
    if (!primitive.isDouble())
        return std::nullopt;
    double number = primitive.asDouble();
    if (!(number >= std::numeric_limits<int32_t>::min() && number <= std::numeric_limits<int32_t>::max()))
        return std::nullopt;
    int32_t integer = static_cast<int32_t>(number);
    return integer == number ? std::optional(integer) : std::nullopt;
}

static ALWAYS_INLINE unsigned lengthAsDecimal(uint32_t value)
{
    static constexpr std::array<uint32_t, 10> powersOfTen { 0, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
    unsigned approximate = ((32 - clz(value | 1)) * 1233) >> 12;
    return approximate - (value < powersOfTen[approximate]) + 1;
}

static ALWAYS_INLINE void writeAsDecimal(uint32_t value, Latin1Character* end)
{
    static constexpr char pairs[] = "0001020304050607080910111213141516171819202122232425262728293031323334353637383940414243444546474849"
        "5051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
    while (value >= 100) {
        unsigned pair = value % 100 * 2;
        value /= 100;
        end -= 2;
        memcpy(end, pairs + pair, 2);
    }
    if (value >= 10)
        memcpy(end - 2, pairs + value * 2, 2);
    else
        end[-1] = '0' + value;
}

static ALWAYS_INLINE void copyShortText(Latin1Character* to, const Latin1Character* from, size_t length)
{
    if (length < 2) {
        if (length)
            *to = *from;
        return;
    }
    if (length > 16) {
        memcpy(to, from, length);
        return;
    }
    if (length >= 8) {
        memcpy(to, from, 8);
        memcpy(to + length - 8, from + length - 8, 8);
        return;
    }
    if (length >= 4) {
        memcpy(to, from, 4);
        memcpy(to + length - 4, from + length - 4, 4);
        return;
    }
    memcpy(to, from, 2);
    memcpy(to + length - 2, from + length - 2, 2);
}

static ALWAYS_INLINE bool tryWriteShortKeyInline(std::span<const EncodedJSValue> primitives, ShortKeyCharacters& key)
{
    constexpr size_t maxLengthOfInt32 = 11;
    static_assert(maxLengthOfInt32 >= sizeof(uint64_t));
    Latin1Character* start = key.characters.data();
    size_t capacity = key.characters.size();
    size_t length = 0;
    for (EncodedJSValue encoded : primitives) {
        JSValue primitive = JSValue::decode(encoded);
        size_t room = capacity - length;
        Latin1Character* cursor = start + length;
        if (primitive.isString()) {
            JSString* string = asString(primitive);
            uint32_t partLength = string->length();
            if (partLength > room || !string->is8Bit())
                return false;
            if (const StringImpl* impl = string->tryGetValueImpl()) [[likely]]
                copyShortText(cursor, impl->span8().data(), partLength);
            else {
                string->resolveToBuffer(std::span<Latin1Character> { cursor, partLength });
                key.hasRopePart = true;
            }
            length += partLength;
            continue;
        }
        std::optional<int32_t> integer = tryGetInt32(primitive);
        if (!integer || room < maxLengthOfInt32)
            return false;
        if (static_cast<uint32_t>(*integer) < 100'000'000) [[likely]] {
            length += writeUpToEightDigits(*integer, cursor);
            continue;
        }
        uint32_t magnitude = *integer < 0 ? 0u - static_cast<uint32_t>(*integer) : *integer;
        size_t partLength = lengthAsDecimal(magnitude) + (*integer < 0);
        if (*integer < 0)
            *cursor = '-';
        writeAsDecimal(magnitude, cursor + partLength);
        length += partLength;
    }
    key.length = length;
    return true;
}

bool tryWriteShortKey(std::span<const EncodedJSValue> primitives, ShortKeyCharacters& key)
{
    return tryWriteShortKeyInline(primitives, key);
}

static ALWAYS_INLINE JSValue onlyPartWithCharacters(VM& vm, std::span<const EncodedJSValue> primitives)
{
    if (primitives.size() > 2)
        return { };
    EncodedJSValue empty = JSValue::encode(vm.smallStrings.emptyString());
    JSValue only;
    for (EncodedJSValue primitive : primitives) {
        if (primitive == empty)
            continue;
        if (only)
            return { };
        only = JSValue::decode(primitive);
    }
    return only;
}

static ALWAYS_INLINE bool isIndexOfIntegerKey(JSValue primitive)
{
    return primitive.isInt32() && primitive.asInt32() >= 0;
}

JSString* StringBuilderStorage::tryFindKeyByParts(Instance& instance, std::span<const EncodedJSValue> primitives)
{
    StringKeyCache& keys = instance.state->propertyCaches->stringKeys();
    if (JSValue only = onlyPartWithCharacters(*instance.vm, primitives)) {
        if (isIndexOfIntegerKey(only))
            return keys.tryFindIntegerKey(only.asInt32());
        if (only.isString())
            return asString(only);
    }
    StringKeyCache::KeyByParts parts;
    return parts.tryTake(keys.cageBase(), primitives) ? keys.tryFindKeyByParts(parts) : nullptr;
}

static JSString* tryConcatenateShortKey(VM& vm, Instance& instance, std::span<const EncodedJSValue> primitives)
{
    StringKeyCache& keys = instance.state->propertyCaches->stringKeys();
    if (JSValue only = onlyPartWithCharacters(vm, primitives)) {
        if (isIndexOfIntegerKey(only)) {
            std::array<Latin1Character, 10> digits;
            uint32_t index = only.asInt32();
            unsigned length = lengthAsDecimal(index);
            writeAsDecimal(index, digits.data() + length);
            return createIndexKey(vm, keys, index, std::span<const Latin1Character> { digits }.first(length));
        }
        if (only.isString())
            return asString(only);
    }
    ShortKeyCharacters key;
    if (!tryWriteShortKey(primitives, key))
        return nullptr;
    if (JSString* index = tryCreateIndexKey(vm, keys, key.span()))
        return index;
    JSString* string = keys.findOrAdd(vm, key.span());
    StringKeyCache::KeyByParts parts;
    if (parts.tryTake(keys.cageBase(), primitives))
        keys.cacheKeyByParts(parts, string);
    return string;
}

JSString* StringBuilderStorage::concatenate(JSGlobalObject* globalObject, Instance& instance, std::span<const EncodedJSValue> primitives, StringUse use)
{
    VM& vm = globalObject->vm();
    if (use == StringUse::LikelyKey) {
        if (JSString* key = tryConcatenateShortKey(vm, instance, primitives))
            return key;
    }
    auto scope = DECLARE_THROW_SCOPE(vm);
    StringBuilderStorage* storage = acquire(instance);
    AppendResult result = storage->append(globalObject, primitives);
    EXCEPTION_ASSERT(!!scope.exception() == (result == AppendResult::Failed));
    if (result == AppendResult::Failed) [[unlikely]] {
        storage->release(instance);
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, storage->finish(globalObject, instance, use));
}

static ALWAYS_INLINE StringBuilderStorage::StringUse stringUseOfResult(uint32_t isLikelyKey)
{
    return isLikelyKey ? StringBuilderStorage::StringUse::LikelyKey : StringBuilderStorage::StringUse::Unknown;
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilder(Instance*, JSCell* builder, uint32_t isLikelyKey);
static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilderWithNewKey(Instance*, JSCell* builder, uint32_t hash);

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderFinish, JSCell*, (Instance* instance, JSCell* builder, uint32_t isLikelyKey))
{
    JSString* key;
    unsigned hash;
    if (std::optional<StringKeyCache::Lookup> lookup = uncheckedDowncast<StringBuilderStorage>(builder)->tryFinishWithCachedKey(*instance, key, hash)) [[likely]] {
        switch (*lookup) {
        case StringKeyCache::Lookup::Found:
            return { key, nullptr };
        case StringKeyCache::Lookup::Absent:
            [[clang::musttail]] return finishStringBuilderWithNewKey(instance, builder, hash);
        case StringKeyCache::Lookup::AbsentIndex:
            [[clang::musttail]] return finishStringBuilder(instance, builder, true);
        }
    }
    [[clang::musttail]] return finishStringBuilder(instance, builder, isLikelyKey);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilderWithNewKey(Instance* instance, JSCell* builder, uint32_t hash)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderFinish);
    OPERATION_RETURN(scope, uncheckedDowncast<StringBuilderStorage>(builder)->finishWithNewKey(*instance, hash));
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilder(Instance* instance, JSCell* builder, uint32_t isLikelyKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderFinish);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderFinish);
    OPERATION_RETURN(scope, storage->finish(globalObject, *instance, stringUseOfResult(isLikelyKey)));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStackStringBuilder(Instance*, uint8_t* builder, uint8_t* cursor, uint32_t isLikelyKey);
static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStackStringBuilderWithNewKey(Instance*, uint8_t* builder, uint8_t* cursor, uint32_t hash);

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderFinish, JSCell*, (Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t isLikelyKey))
{
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    PropertyCaches* caches = instance->state->propertyCaches.getIfCreated();
    if (stackBuilder.isOnStack() && caches) [[likely]] {
        const StringKeyCache& keys = caches->stringKeys();
        std::span<const Latin1Character> characters = stackBuilder.charactersOnStack(byteCast<Latin1Character>(cursor));
        if (characters.size() - 2 <= keys.maxCachedKeyLength() - 2) [[likely]] {
            JSString* key;
            unsigned hash;
            switch (keys.findKeyForText(characters, key, hash)) {
            case StringKeyCache::Lookup::Found:
                return { key, nullptr };
            case StringKeyCache::Lookup::Absent:
                [[clang::musttail]] return finishStackStringBuilderWithNewKey(instance, builder, cursor, hash);
            case StringKeyCache::Lookup::AbsentIndex:
                [[clang::musttail]] return finishStackStringBuilder(instance, builder, cursor, true);
            }
        }
    }
    [[clang::musttail]] return finishStackStringBuilder(instance, builder, cursor, isLikelyKey);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStackStringBuilderWithNewKey(Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t hash)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderFinish);
    std::span<const Latin1Character> characters = reinterpret_cast<StackStringBuilder*>(builder)->charactersOnStack(byteCast<Latin1Character>(cursor));
    OPERATION_RETURN(scope, instance->state->propertyCaches->stringKeys().makeKey(vm, characters, hash, { }));
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStackStringBuilder(Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t isLikelyKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderFinish);
    OPERATION_RETURN(scope, reinterpret_cast<StackStringBuilder*>(builder)->finish(globalObject, *instance, byteCast<Latin1Character>(cursor), stringUseOfResult(isLikelyKey)));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateByContents(Instance*, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey);
static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateShortKey(Instance*, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey);
static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateOtherCount(Instance*, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey);

template<uint32_t count>
static ALWAYS_INLINE JSString* tryFindKeyByParts(StringKeyCache& keys, const EncodedJSValue* parts)
{
    StringKeyCache::KeyByParts taken;
    if (!taken.tryTake<count>(keys.cageBase(), parts)) [[unlikely]]
        taken.countAndKinds = 0;
    else if (JSString* key = keys.tryFindKeyByParts(taken)) [[likely]]
        return key;
    keys.lastLookupByParts() = taken;
    return nullptr;
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcatenate, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey))
{
    PropertyCaches* caches = instance->state->propertyCaches.getIfCreated();
    if (!isLikelyKey || !caches) [[unlikely]]
        [[clang::musttail]] return concatenateByContents(instance, count, parts, isLikelyKey);
    StringKeyCache& keys = caches->stringKeys();
    JSString* key;
    switch (count) {
    case 2: {
        EncodedJSValue empty = JSValue::encode(instance->vm->smallStrings.emptyString());
        if (parts[0] == empty || parts[1] == empty) {
            JSValue other = JSValue::decode(parts[parts[0] == empty]);
            if (other.isString()) [[likely]]
                return { asString(other), nullptr };
            [[clang::musttail]] return concatenateOtherCount(instance, count, parts, isLikelyKey);
        }
        key = tryFindKeyByParts<2>(keys, parts);
        break;
    }
    case 3:
        key = tryFindKeyByParts<3>(keys, parts);
        break;
    case 4:
        key = tryFindKeyByParts<4>(keys, parts);
        break;
    default:
        [[clang::musttail]] return concatenateOtherCount(instance, count, parts, isLikelyKey);
    }
    if (key) [[likely]]
        return { key, nullptr };
    [[clang::musttail]] return concatenateShortKey(instance, count, parts, isLikelyKey);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateOtherCount(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    if (count <= 2) {
        if (JSString* key = StringBuilderStorage::tryFindKeyByParts(*instance, unsafeMakeSpan(parts, count)))
            return { key, nullptr };
        [[clang::musttail]] return concatenateByContents(instance, count, parts, isLikelyKey);
    }
    StringKeyCache& keys = instance->state->propertyCaches->stringKeys();
    StringKeyCache::KeyByParts taken;
    if (!taken.tryTake(keys.cageBase(), unsafeMakeSpan(parts, count)))
        taken.countAndKinds = 0;
    else if (JSString* key = keys.tryFindKeyByParts(taken))
        return { key, nullptr };
    keys.lastLookupByParts() = taken;
    [[clang::musttail]] return concatenateShortKey(instance, count, parts, isLikelyKey);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateShortKey(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    ShortKeyCharacters characters;
    StringKeyCache& keys = instance->state->propertyCaches->stringKeys();
    if (tryWriteShortKeyInline(unsafeMakeSpan(parts, count), characters) && characters.length - 2 <= keys.maxCachedKeyLength() - 2) [[likely]] {
        // A rope is made anew each time, so nobody asks for these parts again.
        if (characters.hasRopePart)
            keys.lastLookupByParts().countAndKinds = 0;
        JSString* key;
        unsigned hash;
        switch (keys.findKeyForText(characters.span(), key, hash)) {
        case StringKeyCache::Lookup::Found:
            if (keys.lastLookupByParts().countAndKinds)
                keys.cacheKeyByParts(keys.lastLookupByParts(), key);
            return { key, nullptr };
        case StringKeyCache::Lookup::Absent: {
            AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatenate);
            return { keys.makeKey(vm, characters.span(), hash, keys.lastLookupByParts()), nullptr };
        }
        case StringKeyCache::Lookup::AbsentIndex:
            break;
        }
    }
    [[clang::musttail]] return concatenateByContents(instance, count, parts, isLikelyKey);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateByContents(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatenate);
    OPERATION_RETURN(scope, StringBuilderStorage::concatenate(globalObject, *instance, unsafeMakeSpan(parts, count), stringUseOfResult(isLikelyKey)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
