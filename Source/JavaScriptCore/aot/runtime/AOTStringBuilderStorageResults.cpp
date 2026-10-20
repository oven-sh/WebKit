/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderStorage.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "NumberPrototype.h"
#include <algorithm>
#include <array>
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

static JSString* tryCreateIndexKey(VM& vm, std::span<const Latin1Character> characters)
{
    std::optional<uint32_t> index = parseIndex(characters);
    if (!index || *index > INT32_MAX)
        return nullptr;
    JSString* string = int32ToString(vm, static_cast<int32_t>(*index), 10);
    string->tryGetValueImpl()->hash();
    return string;
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

JSString* StringBuilderStorage::createStringFromBuffer(JSGlobalObject* globalObject, Instance& instance, StringUse use, bool mayGiveBufferAway)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const Latin1Character> characters { m_characters, m_position };
    if (m_position == 1)
        return jsSingleCharacterString(vm, characters[0]);
    if (use == StringUse::LikelyKey) {
        if (JSString* key = tryCreateIndexKey(vm, characters))
            return key;
        StringKeyCache& keys = instance.state->propertyCaches->stringKeys();
        if (m_position <= keys.maxCachedKeyLength())
            return keys.findOrAdd(vm, characters);
        countLimitHit(vm, LimitID::MaxCachedKeyLength);
    }
    if (RefPtr<StringImpl> impl = tryShareOrGiveAwayBuffer(mayGiveBufferAway))
        return jsNontrivialString(vm, String { impl.releaseNonNull() });
    RELEASE_AND_RETURN(scope, copyBufferToString(globalObject));
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

JSString* StringBuilderStorage::finish(JSGlobalObject* globalObject, Instance& instance, StringUse use)
{
    if (use == StringUse::Unknown && holdsOnlyShortTextInOwnBuffer()) [[likely]] {
        JSString* string = copyBufferToString(globalObject);
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

static JSString* tryConcatenateShortKey(VM& vm, Instance& instance, std::span<const EncodedJSValue> primitives)
{
    constexpr size_t maxLength = defaultLimitValue(LimitID::MaxCachedKeyLength, hostArchitecture());
    constexpr size_t maxLengthOfInt32 = 11;
    std::array<Latin1Character, maxLength> characters;
    size_t length = 0;
    for (EncodedJSValue encoded : primitives) {
        JSValue primitive = JSValue::decode(encoded);
        std::span<Latin1Character> room = std::span { characters }.subspan(length);
        if (primitive.isString()) {
            JSString* string = asString(primitive);
            uint32_t partLength = string->length();
            if (partLength > room.size() || !string->is8Bit())
                return nullptr;
            if (const StringImpl* impl = string->tryGetValueImpl())
                StringImpl::copyCharacters(room.first(partLength), impl->span8());
            else
                string->resolveToBuffer(room.first(partLength));
            length += partLength;
            continue;
        }
        std::optional<int32_t> integer = tryGetInt32(primitive);
        if (!integer || room.size() < maxLengthOfInt32)
            return nullptr;
        char* digits = byteCast<char>(room.data());
        length += std::to_chars(digits, digits + room.size(), *integer).ptr - digits;
    }
    std::span<const Latin1Character> key = std::span { characters }.first(length);
    if (JSString* index = tryCreateIndexKey(vm, key))
        return index;
    return instance.state->propertyCaches->stringKeys().findOrAdd(vm, key);
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
