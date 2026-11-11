/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderStorage.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTInstance.h"
#include "AOTJSONTextWriter.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeReporter.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include "NumberPrototype.h"
#include "NumericStrings.h"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <optional>
#include <ranges>
#include <wtf/ASCIICType.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/dtoa.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(sizeof(WriteBarrier<StringBuilderStorage>) == sizeof(void*), "the next free cell is read and written as one word");

static constexpr uint32_t minLengthOfStringBuilderFiber = std::min(StringBuilderStorage::minLengthAdoptedByFinish, StringBuilderStorage::minLengthAppendedByReference);

WTF_MAKE_TZONE_ALLOCATED_IMPL(StringKeyCache);

const ClassInfo StringBuilderStorage::s_info = { "StringBuilderStorage"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(StringBuilderStorage) };

NEVER_INLINE void StringBuilderStorage::throwOutOfMemory(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwOutOfMemoryError(globalObject, scope);
}

StringBuilderStorage::StringBuilderStorage(VM& vm, Structure* structure, StringBuilderWords& words)
    : Base(vm, structure)
    , m_words(words)
{
}

StringBuilderStorage* StringBuilderStorage::create(Instance& instance)
{
    VM& vm = *instance.vm;
    auto* storage = new (NotNull, allocateCell<StringBuilderStorage>(vm)) StringBuilderStorage(vm, vm.aotState().stringBuilderStorageStructure.get(), instance.stringBuilder);
    storage->finishCreation(vm);
    storage->tryEnsureRoom(1);
    return storage;
}

void StringBuilderStorage::destroy(JSCell* cell)
{
    static_cast<StringBuilderStorage*>(cell)->StringBuilderStorage::~StringBuilderStorage();
}

Structure* StringBuilderStorage::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CellType, StructureFlags), info());
}

template<typename Visitor>
void StringBuilderStorage::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<StringBuilderStorage>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);

    Locker locker { thisObject->cellLock() };
    visitor.append(thisObject->m_lastString);
    for (auto& segment : thisObject->m_segments)
        visitor.append(segment.string);
    visitor.append(thisObject->m_nextFree);
    visitor.reportExtraMemoryVisited(thisObject->capacity());
}

DEFINE_VISIT_CHILDREN(StringBuilderStorage);

StringBuilderStorage* StringBuilderStorage::acquire(Instance& instance)
{
    StringBuilderWords& words = instance.stringBuilder;
    if (StringBuilderStorage* head = words.freeStorage) {
        words.freeStorage = head->m_nextFree.get();
        head->m_nextFree.clear();
        return head;
    }
    if (!words.isPoolRegistered) {
        instance.state->caches->addClearedEachCollection("string builder pool"_s, asMutableByteSpan(words.freeStorage));
        words.isPoolRegistered = true;
    }
    return create(instance);
}

void StringBuilderStorage::release(Instance& instance)
{
    reset();
    returnToPool(instance);
}

void StringBuilderStorage::returnToPool(Instance& instance)
{
    ASSERT(!length() && m_segments.isEmpty() && !m_lastString && !m_sharedLength && !m_positionJoinedIntoFirstSegment);
    VM& vm = *instance.vm;
    StringBuilderWords& words = instance.stringBuilder;
    StringBuilderStorage* head = words.freeStorage;
    ASSERT(head != this);
    unsigned depth = !head ? 1 : head->m_nextFree ? head->m_poolDepth + 1 : 2;
    if (depth > maxPooledBuilders) [[unlikely]] {
        dropBuffer();
        return;
    }
    m_poolDepth = static_cast<uint8_t>(depth);
    m_nextFree.setMayBeNull(vm, this, head);
    words.freeStorage = this;
}

void StringBuilderStorage::reset()
{
    if (!m_segments.isEmpty()) {
        Locker locker { cellLock() };
        m_segments.shrink(0);
    }
    if (m_units.size() > maxRetainedUnits)
        m_units.clear();
    else
        m_units.shrink(0);
    size_t longestTextInBuffer = std::max(m_position, std::exchange(m_positionWhenSegmentsWereJoined, 0));
    m_position = 0;
    m_positionJoinedIntoFirstSegment = 0;
    m_lengthOutsideBuffer = 0;
    m_lengthOfLastString = 0;
    m_lastString.clear();
    if (capacity() > maxRetainedCapacity) {
        dropLargeBuffer(longestTextInBuffer);
        return;
    }
    if (m_buffer && !m_buffer->hasOneRef()) {
        dropBuffer();
        return;
    }
    m_sharedLength = 0;
    updateLimit();
}

void StringBuilderStorage::updateLimit()
{
    m_limit = wouldOverwriteSharedCharacters() ? m_position : std::min<uint32_t>(capacity(), std::max<uint32_t>(String::MaxLength - m_lengthOutsideBuffer, m_position));
}

bool StringBuilderStorage::canGrowBy(size_t added) const
{
    return static_cast<uint64_t>(length()) + added <= String::MaxLength;
}

bool StringBuilderStorage::tryEnsureRoom(size_t length, size_t bytesWritten)
{
    ASSERT(length <= bytesWritten);
    if (bytesWritten <= m_limit - m_position)
        return true;
    return tryMakeRoom(length, bytesWritten);
}

NEVER_INLINE bool StringBuilderStorage::tryMakeRoom(size_t length, size_t bytesWritten)
{
    ASSERT(bytesWritten);
    if (!canGrowBy(length))
        return false;
    size_t needed = m_position + bytesWritten;
    size_t currentCapacity = capacity();
    if (needed <= currentCapacity && !wouldOverwriteSharedCharacters()) {
        if (m_buffer->hasOneRef())
            m_sharedLength = 0;
        updateLimit();
        return true;
    }
    size_t grownCapacity = needed > currentCapacity ? currentCapacity * 2 : currentCapacity;
    size_t newCapacity = std::max(needed, std::min<size_t>(std::max<size_t>({ grownCapacity, minCapacity, needed }), String::MaxLength));
    size_t expectedCapacity = std::exchange(m_capacityOfNextBuffer, 0);
    if (needed > minCapacity)
        expectedCapacity = std::max<size_t>(expectedCapacity, std::exchange(m_words.capacityOfLastLargeBuffer, 0));
    if (expectedCapacity > newCapacity && tryReplaceBuffer(expectedCapacity))
        return true;
    return tryReplaceBuffer(newCapacity);
}

NEVER_INLINE bool StringBuilderStorage::tryReplaceBuffer(size_t newCapacity)
{
    std::span<Latin1Character> characters;
    RefPtr<StringImpl> buffer = StringImpl::tryCreateUninitialized(newCapacity, characters);
    if (!buffer)
        return false;
    StringImpl::copyCharacters(characters, std::span<const Latin1Character> { m_characters, m_position });
    if (m_buffer)
        countEvent(vm(), RuntimeCounterID::StringBuilderGrowths);
    {
        Locker locker { cellLock() };
        m_buffer = WTF::move(buffer);
    }
    m_characters = characters.data();
    m_sharedLength = 0;
    updateLimit();
    vm().heap.reportExtraMemoryAllocated(this, newCapacity);
    return true;
}

void StringBuilderStorage::dropBuffer()
{
    {
        Locker locker { cellLock() };
        m_buffer = nullptr;
    }
    m_characters = nullptr;
    m_sharedLength = 0;
    updateLimit();
}

void StringBuilderStorage::dropLargeBuffer(size_t longestTextInBuffer)
{
    size_t fittingCapacity = longestTextInBuffer > capacity() / 2 ? capacity() : std::max<size_t>(roundUpToPowerOfTwo(longestTextInBuffer), minCapacity);
    dropBuffer();
    if (fittingCapacity > maxRetainedCapacity)
        m_words.capacityOfLastLargeBuffer = static_cast<uint32_t>(fittingCapacity);
    else
        m_capacityOfNextBuffer = static_cast<uint32_t>(fittingCapacity);
}

bool StringBuilderStorage::ensureRoom(JSGlobalObject* globalObject, size_t size)
{
    if (tryEnsureRoom(size)) [[likely]]
        return true;
    throwOutOfMemory(globalObject);
    return false;
}

auto StringBuilderStorage::append(JSGlobalObject* globalObject, std::span<const Latin1Character> characters) -> AppendResult
{
    if (characters.size() > m_limit - m_position) [[unlikely]] {
        if (!ensureRoom(globalObject, characters.size()))
            return AppendResult::Failed;
    }
    StringImpl::copyCharacters(std::span { m_characters + m_position, characters.size() }, characters);
    m_position += static_cast<uint32_t>(characters.size());
    return AppendResult::Appended;
}

auto StringBuilderStorage::append(JSGlobalObject* globalObject, std::span<const char16_t> characters) -> AppendResult
{
    if (characters.empty())
        return AppendResult::Appended;
    if (hasRoomForUnits(characters.size())) {
        return addUnits(globalObject, static_cast<uint32_t>(characters.size()), [&](std::span<char16_t> units) {
            StringImpl::copyCharacters(units, characters);
        });
    }
    std::span<char16_t> copy;
    RefPtr<StringImpl> impl = canGrowBy(characters.size()) ? StringImpl::tryCreateUninitialized(characters.size(), copy) : nullptr;
    if (!impl) {
        throwOutOfMemory(globalObject);
        return AppendResult::Failed;
    }
    StringImpl::copyCharacters(copy, characters);
    return addSegment(globalObject, jsString(globalObject->vm(), String { impl.releaseNonNull() }));
}

auto StringBuilderStorage::append(JSGlobalObject* globalObject, JSString* string) -> AppendResult
{
    VM& vm = globalObject->vm();
    uint32_t length = string->length();
    if (!length)
        return AppendResult::Appended;
    bool wasEmpty = !this->length();
    uint32_t minLengthByReference = wasEmpty ? minLengthOfStringBuilderFiber : minLengthAppendedByReference;
    bool is8Bit = string->is8Bit();
    AppendResult result;
    if (is8Bit && length < minLengthByReference)
        result = appendCharactersOf(globalObject, string);
    else if (!is8Bit && hasRoomForUnits(length)) {
        result = addUnits(globalObject, length, [&](std::span<char16_t> units) {
            string->resolveToBuffer(units);
        });
    } else
        result = addSegment(globalObject, string);
    if (wasEmpty && result == AppendResult::Appended) {
        m_lastString.set(vm, this, string);
        m_lengthOfLastString = length;
    }
    return result;
}

auto StringBuilderStorage::appendCharactersOf(JSGlobalObject* globalObject, JSString* string) -> AppendResult
{
    uint32_t length = string->length();
    if (!ensureRoom(globalObject, length))
        return AppendResult::Failed;
    std::span<Latin1Character> destination { m_characters + m_position, length };
    if (const StringImpl* impl = string->tryGetValueImpl())
        StringImpl::copyCharacters(destination, impl->span8());
    else
        string->resolveToBuffer(destination);
    ensureStillAliveHere(string);
    m_position += length;
    return AppendResult::Appended;
}

template<typename CopyUnits>
auto StringBuilderStorage::addUnits(JSGlobalObject* globalObject, uint32_t length, const CopyUnits& copyUnits) -> AppendResult
{
    if (!canGrowBy(length)) {
        throwOutOfMemory(globalObject);
        return AppendResult::Failed;
    }
    bool extendsLastSegment = !m_segments.isEmpty() && !m_segments.last().string && m_segments.last().offsetInBuffer == m_position;
    if (!extendsLastSegment && m_segments.size() >= maxSegments) {
        if (!collapseSegments(globalObject))
            return AppendResult::Failed;
    }
    uint32_t start = static_cast<uint32_t>(m_units.size());
    m_units.grow(start + length);
    copyUnits(m_units.mutableSpan().subspan(start));
    if (extendsLastSegment)
        m_segments.last().length += length;
    else {
        Locker locker { cellLock() };
        m_segments.append(Segment { m_position, length, start, { } });
    }
    m_lengthOutsideBuffer += length;
    updateLimit();
    return AppendResult::Appended;
}

auto StringBuilderStorage::addSegment(JSGlobalObject* globalObject, JSString* string) -> AppendResult
{
    VM& vm = globalObject->vm();
    uint32_t length = string->length();
    if (!canGrowBy(length)) {
        throwOutOfMemory(globalObject);
        return AppendResult::Failed;
    }
    if (m_segments.size() >= maxSegments) {
        if (!collapseSegments(globalObject))
            return AppendResult::Failed;
    }
    {
        Locker locker { cellLock() };
        m_segments.append(Segment { m_position, length, 0, WriteBarrier<JSString>(vm, this, string) });
    }
    m_lengthOutsideBuffer += length;
    updateLimit();
    return AppendResult::Appended;
}

auto StringBuilderStorage::append(JSGlobalObject* globalObject, JSValue primitive) -> AppendResult
{
    ASSERT(!primitive.isObject());
    if (primitive.isString()) [[likely]]
        return append(globalObject, asString(primitive));
    if (primitive.isInt32()) {
        std::array<char, 12> digits;
        return append(globalObject, byteCast<Latin1Character>(std::span<const char> { digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), primitive.asInt32()).ptr }));
    }
    if (primitive.isDouble()) {
        NumberToStringBuffer buffer;
        return append(globalObject, byteCast<Latin1Character>(numberToStringAndSize(primitive.asDouble(), buffer)));
    }
    if (primitive.isUndefined())
        return append(globalObject, "undefined"_s.span8());
    if (primitive.isNull())
        return append(globalObject, "null"_s.span8());
    if (primitive.isBoolean())
        return append(globalObject, primitive.isTrue() ? "true"_s.span8() : "false"_s.span8());
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = primitive.toString(globalObject);
    RETURN_IF_EXCEPTION(scope, AppendResult::Failed);
    RELEASE_AND_RETURN(scope, append(globalObject, string));
}

auto StringBuilderStorage::append(JSGlobalObject* globalObject, std::span<const EncodedJSValue> primitives) -> AppendResult
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t start = length();
    for (EncodedJSValue primitive : primitives) {
        AppendResult result = append(globalObject, JSValue::decode(primitive));
        EXCEPTION_ASSERT(!!scope.exception() == (result == AppendResult::Failed));
        if (result == AppendResult::Failed) [[unlikely]] {
            rollBack(start);
            return AppendResult::Failed;
        }
    }
    return AppendResult::Appended;
}

Latin1Character* StringBuilderStorage::tryReserve(size_t length)
{
    return tryEnsureRoom(length, roundUpToMultipleOf<sizeof(uint64_t)>(length)) ? m_characters + m_position : nullptr;
}

Latin1Character* StringBuilderStorage::reserve(JSGlobalObject* globalObject, size_t length)
{
    if (Latin1Character* cursor = tryReserve(length)) [[likely]]
        return cursor;
    throwOutOfMemory(globalObject);
    return nullptr;
}

Latin1Character* StringBuilderStorage::tryReserve(const Reservation& reservation)
{
    size_t bytesWritten = roundUpToMultipleOf<sizeof(uint64_t)>(reservation.length + reservation.maxLengthOfRest);
    if (bytesWritten > m_limit - m_position) {
        if (!tryMakeRoom(reservation.length, bytesWritten))
            return nullptr;
        m_limit = std::max(m_limit, static_cast<uint32_t>(m_position + bytesWritten));
    }
    return m_characters + m_position;
}

Latin1Character* StringBuilderStorage::reserve(JSGlobalObject* globalObject, const Reservation& reservation)
{
    if (Latin1Character* cursor = tryReserve(reservation)) [[likely]]
        return cursor;
    throwOutOfMemory(globalObject);
    return nullptr;
}

void StringBuilderStorage::commit(const Latin1Character* cursor)
{
    ASSERT(cursor >= m_characters && cursor <= m_characters + m_limit);
    m_position = static_cast<uint32_t>(cursor - m_characters);
}

void StringBuilderStorage::rollBack(uint32_t length)
{
    ASSERT(length <= this->length());
    while (!m_segments.isEmpty()) {
        Segment& last = m_segments.last();
        uint32_t start = last.offsetInBuffer + (m_lengthOutsideBuffer - last.length);
        if (length >= start + last.length)
            break;
        Locker locker { cellLock() };
        uint32_t remainingLength = length > start ? length - start : 0;
        m_lengthOutsideBuffer -= last.length - remainingLength;
        if (!last.string)
            m_units.shrink(last.startOfUnits + remainingLength);
        if (remainingLength) {
            last.length = remainingLength;
            break;
        }
        m_segments.removeLast();
    }
    m_position = length - m_lengthOutsideBuffer;
    if (m_lengthOfLastString > length)
        m_lastString.clear();
    updateLimit();
}

template<typename Functor>
void StringBuilderStorage::forEachPiece(const Functor& functor) const
{
    uint32_t taken = m_positionJoinedIntoFirstSegment;
    for (const Segment& segment : m_segments) {
        if (segment.offsetInBuffer > taken)
            functor(Piece { nullptr, nullptr, taken, segment.offsetInBuffer - taken });
        if (segment.length)
            functor(Piece { segment.string.get(), segment.string ? nullptr : m_units.span().subspan(segment.startOfUnits).data(), 0, segment.length });
        taken = segment.offsetInBuffer;
    }
    if (m_position > taken)
        functor(Piece { nullptr, nullptr, taken, m_position - taken });
}

RefPtr<StringImpl> StringBuilderStorage::tryShareOrGiveAwayBuffer(bool mayGiveBufferAway)
{
    ASSERT(m_segments.isEmpty());
    bool isShared = !mayGiveBufferAway && m_position >= minLengthShared && m_position >= capacity() / 2;
    if (m_position < minLengthAdoptedByFinish && !isShared)
        return nullptr;
    if (!mayGiveBufferAway) {
        m_sharedLength = std::max(m_sharedLength, m_position);
        return StringImpl::createSubstringSharingImpl(*m_buffer, 0, m_position);
    }
    if (capacity() <= maxRetainedCapacity || m_position <= capacity() / 2)
        return nullptr;
    RefPtr<StringImpl> impl = m_position == capacity() ? m_buffer : RefPtr<StringImpl> { StringImpl::createSubstringSharingImpl(*m_buffer, 0, m_position) };
    dropLargeBuffer(m_position);
    return impl;
}

ALWAYS_INLINE_INTO_OTHER_FILES JSString* StringBuilderStorage::copyBufferToString(JSGlobalObject* globalObject)
{
    ASSERT(m_position > 1);
    std::span<Latin1Character> output;
    RefPtr<StringImpl> impl = StringImpl::tryCreateUninitialized(m_position, output);
    if (!impl) [[unlikely]] {
        throwOutOfMemory(globalObject);
        return nullptr;
    }
    StringImpl::copyCharacters(output, std::span<const Latin1Character> { m_characters, m_position });
    return jsNontrivialString(globalObject->vm(), String { impl.releaseNonNull() });
}

static JSString* tryCreateStringKeyOfIndexInStringBuilder(VM& vm, StringKeyCache& keys, std::span<const Latin1Character> characters)
{
    if (characters.empty() || characters.size() > StringKeyCache::maxIndexLength || !isASCIIDigit(characters.back()))
        return nullptr;
    std::optional<uint32_t> index = parseIndex(characters);
    if (!index || *index > INT32_MAX)
        return nullptr;
    return keys.textOfInt32(vm, static_cast<int32_t>(*index));
}

static JSString* tryCreateStringKeyInStringBuilder(VM& vm, Instance& instance, std::span<const Latin1Character> characters)
{
    StringKeyCache& keys = *instance.state->stringKeys;
    if (JSString* key = tryCreateStringKeyOfIndexInStringBuilder(vm, keys, characters))
        return key;
    return characters.size() <= StringKeyCache::maxCachedKeyLength ? keys.findOrAdd(vm, characters) : nullptr;
}

JSString* StringBuilderStorage::createStringFromBuffer(JSGlobalObject* globalObject, Instance& instance, StringUse use, bool mayGiveBufferAway)
{
    VM& vm = globalObject->vm();
    std::span<const Latin1Character> characters { m_characters, m_position };
    if (m_position == 1)
        return jsSingleCharacterString(vm, characters[0]);
    if (use != StringUse::Unknown) {
        if (JSString* key = tryCreateStringKeyInStringBuilder(vm, instance, characters))
            return key;
    }
    if (RefPtr<StringImpl> impl = tryShareOrGiveAwayBuffer(mayGiveBufferAway))
        return jsNontrivialString(vm, String { impl.releaseNonNull() });
    return copyBufferToString(globalObject);
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
        is8Bit &= piece.is8Bit();
    }

    std::span<Latin1Character> output8;
    std::span<char16_t> output16;
    RefPtr<StringImpl> impl = is8Bit ? StringImpl::tryCreateUninitialized(total, output8) : StringImpl::tryCreateUninitialized(total, output16);
    if (!impl) {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }

    auto copyPieces = [&]<typename CharacterType>(std::span<CharacterType> output) {
        size_t at = 0;
        for (const Piece& piece : run) {
            auto destination = output.subspan(at, piece.length);
            at += piece.length;
            if constexpr (std::is_same_v<CharacterType, char16_t>) {
                if (piece.units) {
                    StringImpl::copyCharacters(destination, unsafeMakeSpan(piece.units, piece.length));
                    continue;
                }
            }
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

    Vector<Piece, 16> pieces;
    forEachPiece([&](const Piece& piece) {
        pieces.append(piece);
    });
    if (length() < minLengthOfStringBuilderFiber)
        RELEASE_AND_RETURN(scope, createFlatRun(globalObject, pieces.span()));

    constexpr size_t minLengthOfRunOfOtherWidth = 256;
    auto isFiber = [&](const Piece& piece) {
        return piece.string && (piece.length >= minLengthOfStringBuilderFiber || (!piece.string->is8Bit() && piece.string->isRope() && !piece.string->isSubstring()));
    };
    JSRopeString::RopeBuilder<RecordOverflow> fibers(vm);
    size_t runStart = 0;
    size_t lengthOfRun = 0;
    bool runIs8Bit = true;
    auto extendsRun = [&](size_t index) {
        const Piece& piece = pieces[index];
        if (index == runStart) {
            lengthOfRun = piece.length;
            runIs8Bit = piece.is8Bit();
            return true;
        }
        if (piece.is8Bit() != runIs8Bit && (lengthOfRun >= minLengthOfRunOfOtherWidth || piece.length >= minLengthOfRunOfOtherWidth))
            return false;
        lengthOfRun += piece.length;
        runIs8Bit &= piece.is8Bit();
        return true;
    };
    for (size_t index = 0; index <= pieces.size(); ++index) {
        bool hasFiber = index < pieces.size() && isFiber(pieces[index]);
        if (index < pieces.size() && !hasFiber && extendsRun(index))
            continue;
        if (index > runStart) {
            JSString* run = createFlatRun(globalObject, pieces.span().subspan(runStart, index - runStart));
            RETURN_IF_EXCEPTION(scope, nullptr);
            fibers.append(run);
        }
        runStart = index;
        if (hasFiber) {
            const Piece& piece = pieces[index];
            JSString* fiber = piece.length == piece.string->length() ? piece.string : jsSubstring(globalObject, piece.string, 0, piece.length);
            RETURN_IF_EXCEPTION(scope, nullptr);
            fibers.append(fiber);
            runStart = index + 1;
        } else if (index < pieces.size())
            extendsRun(index);
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
        m_segments.append(Segment { 0, length, 0, WriteBarrier<JSString>(vm, this, all) });
    }
    m_units.shrink(0);
    m_lengthOutsideBuffer = length;
    m_positionWhenSegmentsWereJoined = std::max(m_positionWhenSegmentsWereJoined, m_position);
    m_position = 0;
    m_positionJoinedIntoFirstSegment = 0;
    m_lastString.set(vm, this, all);
    m_lengthOfLastString = length;
    updateLimit();
    return true;
}

void StringBuilderStorage::joinPiecesInto(VM& vm, JSString* all)
{
    ASSERT(all->length() == length());
    {
        Locker locker { cellLock() };
        m_segments.shrink(0);
        m_segments.append(Segment { m_position, all->length(), 0, WriteBarrier<JSString>(vm, this, all) });
    }
    m_units.shrink(0);
    m_positionJoinedIntoFirstSegment = m_position;
    m_lastString.set(vm, this, all);
    m_lengthOfLastString = all->length();
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

ALWAYS_INLINE_INTO_OTHER_FILES std::optional<StringKeyCache::Lookup> StringBuilderStorage::tryFinishWithCachedKey(Instance& instance, JSString*& key, unsigned& hash)
{
    if (!holdsOnlyShortTextInOwnBuffer() || m_limit < StringKeyCache::minReadableBytes || m_position > StringKeyCache::maxCachedKeyLength) [[unlikely]]
        return std::nullopt;
    StringKeyCache::Lookup lookup = instance.state->stringKeys->findKeyForText(std::span<const Latin1Character> { m_characters, m_position }, key, hash);
    if (lookup == StringKeyCache::Lookup::Found) {
        m_position = 0;
        returnToPool(instance);
    }
    return lookup;
}

ALWAYS_INLINE_INTO_OTHER_FILES JSString* StringBuilderStorage::finishWithNewKey(JSGlobalObject* globalObject, Instance& instance, unsigned hash)
{
    JSString* key = instance.state->stringKeys->makeKey(*instance.vm, std::span<const Latin1Character> { m_characters, m_position }, hash);
    if (!key) [[unlikely]]
        throwOutOfMemory(globalObject);
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

StringBuilderStorage* StackStringBuilder::moveToStorage(JSGlobalObject* globalObject, Instance& instance, const Latin1Character* cursor)
{
    if (isOnStack() || holdsOneString()) {
        JSString* first = holdsOneString() ? oneString : nullptr;
        StringBuilderStorage* acquired = StringBuilderStorage::acquire(instance);
        bool failed = first && acquired->append(globalObject, first) == StringBuilderStorage::AppendResult::Failed;
        if (failed || acquired->append(globalObject, charactersOnStack(cursor)) == StringBuilderStorage::AppendResult::Failed) [[unlikely]] {
            acquired->release(instance);
            return nullptr;
        }
        return acquired;
    }
    size_t position = cursor - storage->m_characters;
    ASSERT(position <= storage->capacity());
    if (position > JSString::MaxLength - storage->m_lengthOutsideBuffer) [[unlikely]] {
        storage->throwOutOfMemory(globalObject);
        return nullptr;
    }
    storage->m_position = static_cast<uint32_t>(position);
    return storage;
}

Latin1Character* StackStringBuilder::holdOneString(JSString* string)
{
    ASSERT(isOnStack());
    oneString = string;
    limit = buffer;
    lengthOrigin = std::bit_cast<Latin1Character*>(std::bit_cast<uintptr_t>(&buffer[0]) - string->length());
    savedCursor = buffer;
    return buffer;
}

Latin1Character* StackStringBuilder::didChangeStorage(JSGlobalObject* globalObject, StringBuilderStorage& changed)
{
    if (!changed.tryEnsureRoom(0, maxBytesWrittenPastLimit)) [[unlikely]] {
        changed.throwOutOfMemory(globalObject);
        return nullptr;
    }
    ASSERT(!changed.wouldOverwriteSharedCharacters() && changed.capacity() >= changed.m_position + maxBytesWrittenPastLimit);
    uintptr_t characters = std::bit_cast<uintptr_t>(changed.m_characters);
    storage = &changed;
    limit = std::bit_cast<Latin1Character*>(characters + std::min<size_t>(changed.capacity() - maxBytesWrittenPastLimit, JSString::MaxLength - changed.m_lengthOutsideBuffer));
    lengthOrigin = std::bit_cast<Latin1Character*>(characters - changed.m_lengthOutsideBuffer);
    savedCursor = changed.m_characters + changed.m_position;
    return savedCursor;
}

void StackStringBuilder::abandonChangeOfStorage(Instance& instance, StringBuilderStorage& changed)
{
    if (isOnStack() || holdsOneString())
        changed.release(instance);
}

JSString* StackStringBuilder::toString(JSGlobalObject* globalObject, Instance& instance, const Latin1Character* cursor)
{
    VM& vm = globalObject->vm();
    if (holdsOneString() && cursor == buffer)
        return oneString;
    if (isOnStack()) {
        std::span<const Latin1Character> characters = charactersOnStack(cursor);
        if (characters.size() <= 1)
            return characters.empty() ? jsEmptyString(vm) : jsSingleCharacterString(vm, characters[0]);
        return jsNontrivialString(vm, String(characters));
    }
    if (!moveToStorage(globalObject, instance, cursor)) [[unlikely]]
        return nullptr;
    if (JSString* string = storage->validLastString())
        return string;
    if (!storage->length())
        return jsEmptyString(vm);
    if (storage->m_segments.isEmpty())
        return storage->createStringFromBuffer(globalObject, instance, StringBuilderStorage::StringUse::Unknown, false);
    JSString* all = storage->createStringFromPieces(globalObject);
    if (all) [[likely]]
        storage->joinPiecesInto(vm, all);
    return all;
}

JSString* StackStringBuilder::finish(JSGlobalObject* globalObject, Instance& instance, const Latin1Character* cursor, StringBuilderStorage::StringUse use)
{
    VM& vm = globalObject->vm();
    if (holdsOneString() && cursor == buffer)
        return oneString;
    if (!isOnStack()) {
        if (!moveToStorage(globalObject, instance, cursor)) [[unlikely]]
            return nullptr;
        return storage->finish(globalObject, instance, use);
    }
    std::span<const Latin1Character> characters = charactersOnStack(cursor);
    if (characters.size() <= 1)
        return characters.empty() ? jsEmptyString(vm) : jsSingleCharacterString(vm, characters[0]);
    if (use != StringBuilderStorage::StringUse::Unknown) {
        if (JSString* key = tryCreateStringKeyInStringBuilder(vm, instance, characters))
            return key;
    }
    return jsNontrivialString(vm, String(characters));
}

static ALWAYS_INLINE StringBuilderStorage::StringUse stringUseOf(uint32_t isLikelyKey)
{
    ASSERT(isLikelyKey <= static_cast<uint32_t>(StringBuilderStorage::StringUse::LikelyName));
    return static_cast<StringBuilderStorage::StringUse>(isLikelyKey);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringBuilderBegin, JSCell*, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderBegin);
    OPERATION_RETURN(scope, StringBuilderStorage::acquire(*instance));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppend, void, (Instance* instance, JSCell* builder, uint32_t count, const EncodedJSValue* parts))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderAppend);
    cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderAppend)->append(globalObject, unsafeMakeSpan(parts, count));
    OPERATION_RETURN(scope);
}

static NEVER_INLINE OperationReturnType<void> JIT_OPERATION_ATTRIBUTES appendAnyStringToBuilder(Instance* instance, JSCell* builder, JSCell* string)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderAppendString);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderAppendString);
    storage->append(globalObject, cellArgument<JSString>(string, Entry::operationAOTStringBuilderAppendString));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendString, void, (Instance* instance, JSCell* builder, JSCell* string))
{
    if (uncheckedDowncast<StringBuilderStorage>(builder)->tryAppendShortString(uncheckedDowncast<JSString>(string)))
        return { nullptr };
    AOT_MUST_TAIL_CALL return appendAnyStringToBuilder(instance, builder, string);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderAppendCodeUnit, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t codeUnit))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderAppendCodeUnit);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    StringBuilderStorage* storage = stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor));
    if (!storage) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    std::array<char16_t, 1> units { static_cast<char16_t>(codeUnit) };
    bool appended = storage->append(globalObject, std::span<const char16_t> { units }) == StringBuilderStorage::AppendResult::Appended;
    Latin1Character* next = appended ? stackBuilder.didChangeStorage(globalObject, *storage) : nullptr;
    if (!next) [[unlikely]]
        stackBuilder.abandonChangeOfStorage(*instance, *storage);
    OPERATION_RETURN(scope, byteCast<uint8_t>(next));
}

struct NarrowPartOfWideString {
    std::span<const Latin1Character> span() const LIFETIME_BOUND { return std::span { characters }.first(length); }

    std::array<Latin1Character, 2> characters;
    size_t length;
};

static ALWAYS_INLINE std::optional<NarrowPartOfWideString> narrowPartOfWideString(std::span<const char16_t> units)
{
    if (units.size() == 1 && units[0] <= maxSingleCharacterString)
        return NarrowPartOfWideString { { static_cast<Latin1Character>(units[0]) }, 1 };
    if (units.size() == 2 && isASCII(units[0] | units[1]))
        return NarrowPartOfWideString { { static_cast<Latin1Character>(units[0]), static_cast<Latin1Character>(units[1]) }, 2 };
    return std::nullopt;
}

static void appendStringRangeToBuilder(JSGlobalObject* globalObject, StringBuilderStorage& storage, JSString* base, unsigned start, unsigned length)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!length)
        return;
    bool mayHaveNarrowParts = !base->is8Bit() && base->isRope() && !base->isSubstring();
    if (length < StringBuilderStorage::minLengthAppendedByReference && !mayHaveNarrowParts) {
        auto view = base->view(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (view->is8Bit()) {
            scope.release();
            storage.append(globalObject, view->span8().subspan(start, length));
            return;
        }
        if (std::optional<NarrowPartOfWideString> part = narrowPartOfWideString(view->span16().subspan(start, length))) {
            scope.release();
            storage.append(globalObject, part->span());
            return;
        }
    }
    JSString* range = jsSubstring(globalObject, base, start, length);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    storage.append(globalObject, range);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendStringRange, void, (Instance* instance, JSCell* builder, JSCell* string, int32_t start, int32_t end))
{
    constexpr Entry entry = Entry::operationAOTStringBuilderAppendStringRange;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* storage = cellArgument<StringBuilderStorage>(builder, entry);
    JSString* base = cellArgument<JSString>(string, entry);
    if (start < 0 || end < start || static_cast<unsigned>(end) > base->length()) [[unlikely]]
        crashOnWrongOperand(entry);
    appendStringRangeToBuilder(globalObject, *storage, base, start, end - start);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderReserve, uint8_t*, (Instance* instance, JSCell* builder, uint64_t size))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderReserve);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderReserve);
    OPERATION_RETURN(scope, byteCast<uint8_t>(storage->reserve(globalObject, size)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringBuilderRollBack, void, (Instance*, JSCell* builder, uint32_t checkpoint))
{
    constexpr Entry entry = Entry::operationAOTStringBuilderRollBack;
    auto* storage = cellArgument<StringBuilderStorage>(builder, entry);
    if (checkpoint > storage->length()) [[unlikely]]
        crashOnWrongOperand(entry);
    storage->rollBack(checkpoint);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderToString, JSCell*, (Instance* instance, JSCell* builder, uint32_t isLikelyKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderToString);
    OPERATION_RETURN(scope, cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderToString)->toString(globalObject, *instance, stringUseOf(isLikelyKey)));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilderByContents(Instance* instance, JSCell* builder, uint32_t isLikelyKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderFinish);
    OPERATION_RETURN(scope, cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderFinish)->finish(globalObject, *instance, stringUseOf(isLikelyKey)));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilderWithNewKey(Instance* instance, JSCell* builder, uint32_t hash)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderFinish);
    OPERATION_RETURN(scope, uncheckedDowncast<StringBuilderStorage>(builder)->finishWithNewKey(globalObject, *instance, hash));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderFinish, JSCell*, (Instance* instance, JSCell* builder, uint32_t isLikelyKey))
{
    JSString* key;
    unsigned hash;
    if (std::optional<StringKeyCache::Lookup> lookup = uncheckedDowncast<StringBuilderStorage>(builder)->tryFinishWithCachedKey(*instance, key, hash)) [[likely]] {
        switch (*lookup) {
        case StringKeyCache::Lookup::Found:
            return { key, nullptr };
        case StringKeyCache::Lookup::Absent:
            AOT_MUST_TAIL_CALL return finishStringBuilderWithNewKey(instance, builder, hash);
        case StringKeyCache::Lookup::AbsentIndex:
            AOT_MUST_TAIL_CALL return finishStringBuilderByContents(instance, builder, static_cast<uint32_t>(StringBuilderStorage::StringUse::LikelyKey));
        }
    }
    AOT_MUST_TAIL_CALL return finishStringBuilderByContents(instance, builder, isLikelyKey);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringBuilderDiscard, void, (Instance* instance, JSCell* builder))
{
    cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderDiscard)->release(*instance);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderGrow, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderGrow);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    StringBuilderStorage* storage = stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor));
    if (!storage) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    Latin1Character* next = stackBuilder.didChangeStorage(globalObject, *storage);
    if (!next) [[unlikely]]
        stackBuilder.abandonChangeOfStorage(*instance, *storage);
    OPERATION_RETURN(scope, byteCast<uint8_t>(next));
}

static NEVER_INLINE OperationReturnType<uint8_t*> JIT_OPERATION_ATTRIBUTES appendToStorageOfStackStringBuilder(Instance* instance, uint8_t* builder, uint8_t* cursor, EncodedJSValue primitive)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderAppend);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    StringBuilderStorage* storage = stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor));
    if (!storage) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    bool appended = storage->append(globalObject, JSValue::decode(primitive)) == StringBuilderStorage::AppendResult::Appended;
    Latin1Character* next = appended ? stackBuilder.didChangeStorage(globalObject, *storage) : nullptr;
    if (!next) [[unlikely]]
        stackBuilder.abandonChangeOfStorage(*instance, *storage);
    OPERATION_RETURN(scope, byteCast<uint8_t>(next));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderAppend, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor, EncodedJSValue primitive))
{
    constexpr size_t maxLengthOfInt32 = 11;
    JSValue value = JSValue::decode(primitive);
    size_t room = byteCast<uint8_t>(reinterpret_cast<StackStringBuilder*>(builder)->limit) - cursor;
    if (value.isString()) {
        JSString* string = asString(value);
        size_t length = string->length();
        if (length <= room && length < StackStringBuilder::minLengthAppendedByReference && string->is8Bit()) {
            string->resolveToBuffer(std::span<Latin1Character> { byteCast<Latin1Character>(cursor), length });
            return { cursor + length, nullptr };
        }
    } else if (value.isInt32() && room >= maxLengthOfInt32)
        return { byteCast<uint8_t>(appendJSONInteger(byteCast<Latin1Character>(cursor), value.asInt32())), nullptr };
    AOT_MUST_TAIL_CALL return appendToStorageOfStackStringBuilder(instance, builder, cursor, primitive);
}

static NEVER_INLINE OperationReturnType<uint8_t*> JIT_OPERATION_ATTRIBUTES appendStringRangeToStorageOfStackStringBuilder(Instance* instance, uint8_t* builder, uint8_t* cursor, JSCell* string, int32_t start, int32_t end)
{
    constexpr Entry entry = Entry::operationAOTStackStringBuilderAppendStringRange;
    AOT_OPERATION_BEGIN(instance, entry);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    JSString* base = cellArgument<JSString>(string, entry);
    if (start < 0 || end < start || static_cast<unsigned>(end) > base->length()) [[unlikely]]
        crashOnWrongOperand(entry);
    if (stackBuilder.isOnStack() && byteCast<Latin1Character>(cursor) == stackBuilder.buffer && start < end) {
        JSString* range = jsSubstring(globalObject, base, start, end - start);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        OPERATION_RETURN(scope, byteCast<uint8_t>(stackBuilder.holdOneString(range)));
    }
    StringBuilderStorage* storage = stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor));
    if (!storage) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    appendStringRangeToBuilder(globalObject, *storage, base, start, end - start);
    Latin1Character* next = scope.exception() ? nullptr : stackBuilder.didChangeStorage(globalObject, *storage);
    if (!next) [[unlikely]]
        stackBuilder.abandonChangeOfStorage(*instance, *storage);
    OPERATION_RETURN(scope, byteCast<uint8_t>(next));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderAppendStringRange, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor, JSCell* string, int32_t start, int32_t end))
{
    if (start == end)
        return { cursor, nullptr };
    const StringImpl* impl = uncheckedDowncast<JSString>(string)->tryGetValueImpl();
    size_t room = byteCast<uint8_t>(reinterpret_cast<StackStringBuilder*>(builder)->limit) - cursor;
    if (impl && !impl->is8Bit() && start >= 0 && start <= end && static_cast<unsigned>(end) <= impl->length() && static_cast<size_t>(end - start) <= room) {
        size_t length = end - start;
        if (std::optional<NarrowPartOfWideString> part = narrowPartOfWideString(impl->span16().subspan(start, length))) {
            StringImpl::copyCharacters(std::span<Latin1Character> { byteCast<Latin1Character>(cursor), length }, part->span());
            return { cursor + length, nullptr };
        }
    }
    AOT_MUST_TAIL_CALL return appendStringRangeToStorageOfStackStringBuilder(instance, builder, cursor, string, start, end);
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStackStringBuilderByContents(Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t isLikelyKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderFinish);
    OPERATION_RETURN(scope, reinterpret_cast<StackStringBuilder*>(builder)->finish(globalObject, *instance, byteCast<Latin1Character>(cursor), stringUseOf(isLikelyKey)));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStackStringBuilderWithNewKey(Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t hash)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderFinish);
    JSString* key = instance->state->stringKeys->makeKey(vm, reinterpret_cast<StackStringBuilder*>(builder)->charactersOnStack(byteCast<Latin1Character>(cursor)), hash);
    if (!key) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope, key);
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES makeStringOfStackStringBuilderAsNewKey(Instance* instance, uint8_t* builder, uint8_t* cursor)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderToString);
    std::span<const Latin1Character> characters = reinterpret_cast<StackStringBuilder*>(builder)->charactersOnStack(byteCast<Latin1Character>(cursor));
    JSString* key = instance->state->stringKeys->makeKey(vm, characters, RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters));
    if (!key) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope, key);
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES makeStringOfStackStringBuilderByContents(Instance* instance, uint8_t* builder, uint8_t* cursor)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderToString);
    OPERATION_RETURN(scope, reinterpret_cast<StackStringBuilder*>(builder)->toString(globalObject, *instance, byteCast<Latin1Character>(cursor)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderToString, JSCell*, (Instance* instance, uint8_t* builder, uint8_t* cursor))
{
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    if (stackBuilder.isOnStack()) [[likely]] {
        std::span<const Latin1Character> characters = stackBuilder.charactersOnStack(byteCast<Latin1Character>(cursor));
        if (characters.size() < 2)
            return { characters.empty() ? jsEmptyString(*instance->vm) : jsSingleCharacterString(*instance->vm, characters[0]), nullptr };
        if (characters.size() <= StringKeyCache::maxCachedKeyLength) {
            JSString* key;
            unsigned hash;
            switch (instance->state->stringKeys->findKeyForText(characters, key, hash)) {
            case StringKeyCache::Lookup::Found:
                return { key, nullptr };
            case StringKeyCache::Lookup::Absent:
                AOT_MUST_TAIL_CALL return makeStringOfStackStringBuilderAsNewKey(instance, builder, cursor);
            case StringKeyCache::Lookup::AbsentIndex:
                break;
            }
        }
    }
    AOT_MUST_TAIL_CALL return makeStringOfStackStringBuilderByContents(instance, builder, cursor);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderFinish, JSCell*, (Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t isLikelyKey))
{
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    if (!stackBuilder.isOnStack()) [[unlikely]]
        AOT_MUST_TAIL_CALL return finishStackStringBuilderByContents(instance, builder, cursor, isLikelyKey);
    std::span<const Latin1Character> characters = stackBuilder.charactersOnStack(byteCast<Latin1Character>(cursor));
    if (characters.size() < 2)
        return { characters.empty() ? jsEmptyString(*instance->vm) : jsSingleCharacterString(*instance->vm, characters[0]), nullptr };
    if (characters.size() <= StringKeyCache::maxCachedKeyLength) [[likely]] {
        JSString* key;
        unsigned hash;
        switch (instance->state->stringKeys->findKeyForText(characters, key, hash)) {
        case StringKeyCache::Lookup::Found:
            return { key, nullptr };
        case StringKeyCache::Lookup::Absent:
            AOT_MUST_TAIL_CALL return finishStackStringBuilderWithNewKey(instance, builder, cursor, hash);
        case StringKeyCache::Lookup::AbsentIndex:
            AOT_MUST_TAIL_CALL return finishStackStringBuilderByContents(instance, builder, cursor, static_cast<uint32_t>(StringBuilderStorage::StringUse::LikelyKey));
        }
    }
    AOT_MUST_TAIL_CALL return finishStackStringBuilderByContents(instance, builder, cursor, isLikelyKey);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStackStringBuilderDiscard, void, (Instance* instance, uint8_t* builder))
{
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    if (!stackBuilder.isOnStack() && !stackBuilder.holdsOneString())
        stackBuilder.storage->release(*instance);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
