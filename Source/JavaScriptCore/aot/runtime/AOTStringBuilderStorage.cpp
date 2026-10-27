/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderStorage.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTJSONHooks.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTThreshold.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "NumberPrototype.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <wtf/HashSet.h>
#include <wtf/dtoa.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringBuilderJSON.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(sizeof(WriteBarrier<StringBuilderStorage>) == sizeof(void*), "compiled code loads and stores the next free cell as one word");
static_assert(defaultLimitValue(LimitID::MaxPooledStringBuilders, hostArchitecture()) <= std::numeric_limits<uint8_t>::max());

const ClassInfo StringBuilderStorage::s_info = { "StringBuilderStorage"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(StringBuilderStorage) };

bool StringBuilderStorage::hasRepeatedHolder(const JSONHolderRecord* innermost)
{
    AOT_THRESHOLD(size_t, maxNumberComparedInPairs, 16);
    size_t numberOfHolders = 0;
    for (const JSONHolderRecord* record = innermost; record; record = record->outer)
        ++numberOfHolders;
    if (numberOfHolders <= maxNumberComparedInPairs) {
        for (const JSONHolderRecord* record = innermost; record; record = record->outer) {
            for (const JSONHolderRecord* other = record->outer; other; other = other->outer) {
                if (other->holder == record->holder)
                    return true;
            }
        }
        return false;
    }
    UncheckedKeyHashSet<EncodedJSValue> seen;
    seen.reserveInitialCapacity(numberOfHolders);
    for (const JSONHolderRecord* record = innermost; record; record = record->outer) {
        if (!seen.add(record->holder).isNewEntry)
            return true;
    }
    return false;
}

// The holders of a value that holds itself come round again and again, so from some length on the list holds the one in its middle twice.
bool StringBuilderStorage::holderInTheMiddleRepeats(const JSONHolderRecord* innermost)
{
    size_t numberOfHolders = 0;
    for (const JSONHolderRecord* record = innermost; record; record = record->outer)
        ++numberOfHolders;
    const JSONHolderRecord* middle = innermost;
    for (size_t index = 0; index < numberOfHolders / 2; ++index)
        middle = middle->outer;
    for (const JSONHolderRecord* record = innermost; record != middle; record = record->outer) {
        if (record->holder == middle->holder)
            return true;
    }
    return false;
}

bool WatchedJSONLevels::isWatchingAfterGrowthTo(size_t numberOfCharacters)
{
    AOT_THRESHOLD(size_t, charactersPerWatchedLevel, 64 * KB);
    uint32_t numberOfLevels = static_cast<uint32_t>(numberOfCharacters / charactersPerWatchedLevel);
    if (numberOfLevels > m_numberOfLevelsLeft) {
        if (!m_numberOfLevelsLeft)
            forgetLevels();
        m_numberOfLevelsLeft = numberOfLevels;
    }
    return m_numberOfLevelsLeft;
}

void WatchedJSONLevels::forgetLevels()
{
    if (!m_levels)
        return;
    m_levels->path.shrink(0);
    m_levels->holders.clear();
}

bool WatchedJSONLevels::enter(EncodedJSValue holder, void* frame)
{
    ASSERT(isWatching());
    if (!m_levels)
        m_levels = makeUnique<Levels>();
    while (!m_levels->path.isEmpty() && std::bit_cast<uintptr_t>(m_levels->path.last().frame) <= std::bit_cast<uintptr_t>(frame))
        m_levels->holders.remove(m_levels->path.takeLast().holder);
    if (!m_levels->holders.add(holder).isNewEntry) {
        stop();
        return false;
    }
    m_levels->path.append(Level { frame, holder });
    --m_numberOfLevelsLeft;
    return true;
}

// The word that compiled code reads. The next change of the limit of the stack overwrites it.
void StringBuilderStorage::setLevelLimit(void* limit)
{
    *std::bit_cast<void**>(std::bit_cast<uintptr_t>(&m_levelLimit) + StackManager::Mirror::offsetOfSoftStackLimit()) = limit;
}

void StringBuilderStorage::watchLevelsAfterGrowth(size_t numberOfCharacters)
{
    bool wasWatching = m_watchedLevels.isWatching();
    if (!m_watchedLevels.isWatchingAfterGrowthTo(numberOfCharacters))
        return;
    if (!wasWatching)
        m_lengthWhenWatchBegan = length();
    setLevelLimit(std::bit_cast<void*>(std::numeric_limits<uintptr_t>::max()));
}

void StringBuilderStorage::stopWatchingLevels()
{
    m_watchedLevels.stop();
    setLevelLimit(vm().softStackLimit());
}

bool StringBuilderStorage::enterWatchedLevel(EncodedJSValue holder, void* frame)
{
    if (std::bit_cast<uintptr_t>(frame) - stackReservedForJSONParsers < std::bit_cast<uintptr_t>(vm().softStackLimit()))
        return false;
    // Without a watch of its own: another builder has grown.
    bool goesOn = true;
    if (m_watchedLevels.isWatching()) {
        // A builder that ends with a short text goes back to the pool as it is.
        if (length() < m_lengthWhenWatchBegan) {
            m_watchedLevels.forgetLevels();
            m_lengthWhenWatchBegan = 0;
        }
        goesOn = m_watchedLevels.enter(holder, frame);
    }
    if (!m_watchedLevels.isWatching())
        stopWatchingLevels();
    return goesOn;
}

NEVER_INLINE void StringBuilderStorage::throwOutOfMemory(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Compiled code that writes a value as JSON does not look for a cycle, and the text of a value that holds itself has no end.
    if (hasRepeatedHolder(m_jsonHolders)) {
        throwTypeError(globalObject, scope, "JSON.stringify cannot serialize cyclic structures."_s);
        return;
    }
    throwOutOfMemoryError(globalObject, scope);
}

StringBuilderStorage::Limits::Limits(VM& vm)
    : minCapacity(safeCast<uint32_t>(runtimeLimitValue(vm, LimitID::MinStringBuilderCapacity)))
    , maxRetainedCapacity(safeCast<uint32_t>(runtimeLimitValue(vm, LimitID::MaxRetainedStringBuilderCapacity)))
    , minLengthAppendedByReference(safeCast<uint32_t>(runtimeLimitValue(vm, LimitID::MinLengthAppendedByReference)))
    , minLengthAdoptedByFinish(safeCast<uint32_t>(runtimeLimitValue(vm, LimitID::MinLengthAdoptedByFinish)))
    , maxSegments(safeCast<uint32_t>(runtimeLimitValue(vm, LimitID::MaxStringBuilderSegments)))
    , maxPooledBuilders(safeCast<uint32_t>(runtimeLimitValue(vm, LimitID::MaxPooledStringBuilders)))
{
}

StringBuilderStorage::StringBuilderStorage(VM& vm, Structure* structure, StackManager::Mirror& levelLimit, StringBuilderWords& words)
    : Base(vm, structure)
    , m_levelLimit(levelLimit)
    , m_words(words)
    , m_limits(vm)
{
}

StringBuilderStorage* StringBuilderStorage::create(Instance& instance)
{
    VM& vm = *instance.vm;
    auto* storage = new (NotNull, allocateCell<StringBuilderStorage>(vm)) StringBuilderStorage(vm, vm.aotState().stringBuilderStorageStructure.get(), instance.jsonLevels.limit.get(), instance.stringBuilder);
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
    ASSERT(!length() && m_segments.isEmpty() && !m_lastString && !m_sharedLength);
    VM& vm = *instance.vm;
    StringBuilderWords& words = instance.stringBuilder;
    StringBuilderStorage* head = words.freeStorage;
    ASSERT(head != this);
    // Compiled code lays a builder into the empty pool by one store, which writes no depth: the builder that has none below it is the first, whatever it says.
    unsigned depth = !head ? 1 : head->m_nextFree ? head->m_poolDepth + 1 : 2;
    if (depth > m_limits.maxPooledBuilders) [[unlikely]] {
        countLimitHit(vm, LimitID::MaxPooledStringBuilders);
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
    size_t longestTextInBuffer = std::max(m_position, std::exchange(m_positionWhenSegmentsWereJoined, 0));
    m_position = 0;
    m_lengthOutsideBuffer = 0;
    m_lengthOfLastString = 0;
    m_jsonHolders = nullptr;
    m_watchedLevels.stop();
    m_lastString.clear();
    if (capacity() > m_limits.maxRetainedCapacity) {
        countLimitHit(vm(), LimitID::MaxRetainedStringBuilderCapacity);
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
    m_limit = wouldOverwriteSharedCharacters() ? m_position : std::min<uint32_t>(capacity(), String::MaxLength - m_lengthOutsideBuffer);
}

bool StringBuilderStorage::canGrowBy(size_t added) const
{
    return added <= String::MaxLength - length();
}

// The bytes that are written behind the characters that are appended want room in the buffer. They are no part of the text, so the longest text says nothing about them.
bool StringBuilderStorage::tryEnsureRoom(size_t length, size_t bytesWritten)
{
    ASSERT(length <= bytesWritten);
    if (bytesWritten <= m_limit - m_position)
        return true;
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
    if (m_jsonHolders && holderInTheMiddleRepeats(m_jsonHolders))
        return false;
    size_t grownCapacity = needed > currentCapacity ? currentCapacity * 2 : currentCapacity;
    size_t newCapacity = std::max(needed, std::min<size_t>(std::max<size_t>(grownCapacity, m_limits.minCapacity), String::MaxLength));
    size_t expectedCapacity = std::exchange(m_capacityOfNextBuffer, 0);
    if (needed > m_limits.minCapacity)
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
        count(vm(), RuntimeCounterID::StringBuilderGrowths);
    {
        Locker locker { cellLock() };
        m_buffer = WTF::move(buffer);
    }
    m_characters = characters.data();
    m_sharedLength = 0;
    updateLimit();
    if (length())
        watchLevelsAfterGrowth(newCapacity);
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

// A builder is emptied where compiled code counts on no collection, so the buffer that fits is made by the next write.
void StringBuilderStorage::dropLargeBuffer(size_t longestTextInBuffer)
{
    size_t fittingCapacity = longestTextInBuffer > capacity() / 2 ? capacity() : std::max<size_t>(roundUpToPowerOfTwo(longestTextInBuffer), m_limits.minCapacity);
    dropBuffer();
    if (fittingCapacity > m_limits.maxRetainedCapacity)
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
    // What a builder begins with has been built before. Copying it would cost a loop that begins a builder with its own last result the whole text at every turn.
    uint32_t minLengthAppendedByReference = wasEmpty ? std::min(m_limits.minLengthAdoptedByFinish, m_limits.minLengthAppendedByReference) : m_limits.minLengthAppendedByReference;
    bool isAppendedByReference = length >= minLengthAppendedByReference || !string->is8Bit();
    AppendResult result = isAppendedByReference ? addSegment(globalObject, string) : appendCharactersOf(globalObject, string);
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

auto StringBuilderStorage::addSegment(JSGlobalObject* globalObject, JSString* string) -> AppendResult
{
    VM& vm = globalObject->vm();
    uint32_t length = string->length();
    if (!canGrowBy(length)) {
        throwOutOfMemory(globalObject);
        return AppendResult::Failed;
    }
    if (m_segments.size() >= m_limits.maxSegments && !m_segments.isEmpty()) {
        countLimitHit(vm, LimitID::MaxStringBuilderSegments);
        if (m_jsonHolders && holderInTheMiddleRepeats(m_jsonHolders)) {
            throwOutOfMemory(globalObject);
            return AppendResult::Failed;
        }
        if (!collapseSegments(globalObject))
            return AppendResult::Failed;
        watchLevelsAfterGrowth(this->length());
    }
    {
        Locker locker { cellLock() };
        m_segments.append(Segment { m_position, length, WriteBarrier<JSString>(vm, this, string) });
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

// Compiled code writes whole words.
Latin1Character* StringBuilderStorage::reserve(JSGlobalObject* globalObject, size_t length)
{
    if (tryEnsureRoom(length, roundUpToMultipleOf<sizeof(uint64_t)>(length))) [[likely]]
        return m_characters + m_position;
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

StringBuilderStorage* StackStringBuilder::moveToStorage(JSGlobalObject* globalObject, Instance& instance, const Latin1Character* cursor)
{
    if (isOnStack()) {
        storage = StringBuilderStorage::acquire(instance);
        if (storage->append(globalObject, charactersOnStack(cursor)) == StringBuilderStorage::AppendResult::Failed) [[unlikely]] {
            storage->release(instance);
            return nullptr;
        }
        return storage;
    }
    size_t position = cursor - storage->m_characters;
    ASSERT(position <= storage->capacity());
    if (position > String::MaxLength - storage->m_lengthOutsideBuffer) [[unlikely]] {
        storage->throwOutOfMemory(globalObject);
        return nullptr;
    }
    storage->m_position = static_cast<uint32_t>(position);
    return storage;
}

Latin1Character* StackStringBuilder::didChangeStorage(JSGlobalObject* globalObject)
{
    if (!storage->tryEnsureRoom(0, maxBytesWrittenPastLimit)) [[unlikely]] {
        storage->throwOutOfMemory(globalObject);
        return nullptr;
    }
    ASSERT(!storage->m_sharedLength && storage->capacity() >= storage->m_position + maxBytesWrittenPastLimit);
    uintptr_t characters = std::bit_cast<uintptr_t>(storage->m_characters);
    limit = std::bit_cast<Latin1Character*>(characters + std::min<size_t>(storage->capacity() - maxBytesWrittenPastLimit, String::MaxLength - storage->m_lengthOutsideBuffer));
    lengthOrigin = std::bit_cast<Latin1Character*>(characters - storage->m_lengthOutsideBuffer);
    return storage->m_characters + storage->m_position;
}

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
    if (capacity() <= m_limits.maxRetainedCapacity || m_position <= capacity() / 2)
        return nullptr;
    countLimitHit(vm(), LimitID::MaxRetainedStringBuilderCapacity);
    RefPtr<StringImpl> impl = m_position == capacity() ? m_buffer : RefPtr<StringImpl> { StringImpl::createSubstringSharingImpl(*m_buffer, 0, m_position) };
    dropLargeBuffer(m_position);
    return impl;
}

static JSString* stringOfIndex(VM& vm, Instance& instance, uint32_t index)
{
    ASSERT(index <= INT32_MAX);
    if (index < NumericStrings::cacheSize) {
        JSString* string = int32ToString(vm, static_cast<int32_t>(index), 10);
        string->tryGetValueImpl()->hash();
        return string;
    }
    StringKeyCache& keys = *instance.state->stringKeys;
    if (JSString* string = keys.tryFindIntegerKey(index))
        return string;
    std::array<char, 10> digits;
    JSString* string = keys.makeIntegerKey(vm, index, byteCast<Latin1Character>(std::span<const char> { digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), index).ptr }));
    vm.numericStrings.setJSString(static_cast<int32_t>(index), string->tryGetValue(), string);
    return string;
}

static JSString* tryCreateIndexKey(VM& vm, Instance& instance, std::span<const Latin1Character> characters)
{
    constexpr size_t maxLengthOfIndex = 10;
    if (characters.empty() || characters.size() > maxLengthOfIndex || !isASCIIDigit(characters.back()))
        return nullptr;
    std::optional<uint32_t> index = parseIndex(characters);
    if (!index || *index > INT32_MAX)
        return nullptr;
    return stringOfIndex(vm, instance, *index);
}

ALWAYS_INLINE JSString* StringBuilderStorage::copyBufferToString(JSGlobalObject* globalObject)
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

static JSString* tryCreateKey(VM& vm, Instance& instance, std::span<const Latin1Character> characters)
{
    StringKeyCache& keys = *instance.state->stringKeys;
    if (JSString* key = tryCreateIndexKey(vm, instance, characters))
        return key;
    if (characters.size() <= keys.maxCachedKeyLength())
        return keys.findOrAdd(vm, characters);
    countLimitHit(vm, LimitID::MaxCachedKeyLength);
    return nullptr;
}

JSString* StringBuilderStorage::createStringFromBuffer(JSGlobalObject* globalObject, Instance& instance, StringUse use, bool mayGiveBufferAway)
{
    VM& vm = globalObject->vm();
    std::span<const Latin1Character> characters { m_characters, m_position };
    if (m_position == 1)
        return jsSingleCharacterString(vm, characters[0]);
    if (use == StringUse::LikelyKey) {
        if (JSString* key = tryCreateKey(vm, instance, characters))
            return key;
    }
    if (RefPtr<StringImpl> impl = tryShareOrGiveAwayBuffer(mayGiveBufferAway))
        return jsNontrivialString(vm, String { impl.releaseNonNull() });
    return copyBufferToString(globalObject);
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
    uint32_t minFiberLength = std::min(m_limits.minLengthAdoptedByFinish, m_limits.minLengthAppendedByReference);

    Vector<Piece, 16> pieces;
    forEachPiece([&](const Piece& piece) {
        pieces.append(piece);
    });

    auto is8Bit = [](const Piece& piece) {
        return !piece.string || piece.string->is8Bit();
    };
    // A 16-bit rope may have 8-bit parts, and a piece of the text that lies in an 8-bit part is an 8-bit string.
    auto isFiber = [&](const Piece& piece) {
        return piece.string && (piece.length >= minFiberLength || (!piece.string->is8Bit() && piece.string->isRope() && !piece.string->isSubstring()));
    };
    JSRopeString::RopeBuilder<RecordOverflow> fibers(vm);
    size_t runStart = 0;
    for (size_t index = 0; index <= pieces.size(); ++index) {
        bool hasFiber = index < pieces.size() && isFiber(pieces[index]);
        if (index < pieces.size() && !hasFiber && (index == runStart || is8Bit(pieces[index]) == is8Bit(pieces[runStart])))
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
        }
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
    m_positionWhenSegmentsWereJoined = std::max(m_positionWhenSegmentsWereJoined, m_position);
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
    const StringKeyCache& keys = *instance.state->stringKeys;
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
    JSString* key = instance.state->stringKeys->makeKey(*instance.vm, std::span<const Latin1Character> { m_characters, m_position }, hash);
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
        std::optional<int32_t> integer = primitive.tryGetAsInt32();
        if (!integer || room < maxLengthOfInt32)
            return false;
        if (static_cast<uint32_t>(*integer) < 100'000'000) [[likely]] {
            length += writeUpToEightDigits(*integer, cursor);
            continue;
        }
        length = byteCast<Latin1Character>(std::to_chars(byteCast<char>(cursor), byteCast<char>(cursor) + maxLengthOfInt32, *integer).ptr) - start;
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
    StringKeyCache& keys = *instance.state->stringKeys;
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
    StringKeyCache& keys = *instance.state->stringKeys;
    if (JSValue only = onlyPartWithCharacters(vm, primitives)) {
        if (isIndexOfIntegerKey(only))
            return stringOfIndex(vm, instance, only.asInt32());
        if (only.isString())
            return asString(only);
    }
    ShortKeyCharacters key;
    if (!tryWriteShortKey(primitives, key))
        return nullptr;
    if (JSString* index = tryCreateIndexKey(vm, instance, key.span()))
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
    // Beside a 16-bit string each part stays a part of a rope: a piece of the text that lies in an 8-bit part is an 8-bit string.
    bool hasWidePart = std::ranges::any_of(primitives, [](EncodedJSValue primitive) {
        JSValue value = JSValue::decode(primitive);
        return value.isString() && !asString(value)->is8Bit();
    });
    if (hasWidePart) [[unlikely]] {
        JSRopeString::RopeBuilder<RecordOverflow> parts(vm);
        for (EncodedJSValue primitive : primitives) {
            JSString* part = JSValue::decode(primitive).toString(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (!parts.append(part)) [[unlikely]] {
                throwOutOfMemoryError(globalObject, scope);
                return nullptr;
            }
        }
        return parts.release();
    }
    StringBuilderStorage* storage = acquire(instance);
    AppendResult result = storage->append(globalObject, primitives);
    EXCEPTION_ASSERT(!!scope.exception() == (result == AppendResult::Failed));
    if (result == AppendResult::Failed) [[unlikely]] {
        storage->release(instance);
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, storage->finish(globalObject, instance, use));
}

static ALWAYS_INLINE StringBuilderStorage::StringUse stringUse(uint32_t isLikelyKey)
{
    return isLikelyKey ? StringBuilderStorage::StringUse::LikelyKey : StringBuilderStorage::StringUse::Unknown;
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilder(Instance* instance, JSCell* builder, uint32_t isLikelyKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderFinish);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderFinish);
    OPERATION_RETURN(scope, storage->finish(globalObject, *instance, stringUse(isLikelyKey)));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilderWithNewKey(Instance* instance, JSCell* builder, uint32_t hash)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderFinish);
    OPERATION_RETURN(scope, uncheckedDowncast<StringBuilderStorage>(builder)->finishWithNewKey(*instance, hash));
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
            [[clang::musttail]] return finishStringBuilderWithNewKey(instance, builder, hash);
        case StringKeyCache::Lookup::AbsentIndex:
            [[clang::musttail]] return finishStringBuilder(instance, builder, true);
        }
    }
    [[clang::musttail]] return finishStringBuilder(instance, builder, isLikelyKey);
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStackStringBuilder(Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t isLikelyKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderFinish);
    OPERATION_RETURN(scope, reinterpret_cast<StackStringBuilder*>(builder)->finish(globalObject, *instance, byteCast<Latin1Character>(cursor), stringUse(isLikelyKey)));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStackStringBuilderWithNewKey(Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t hash)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderFinish);
    std::span<const Latin1Character> characters = reinterpret_cast<StackStringBuilder*>(builder)->charactersOnStack(byteCast<Latin1Character>(cursor));
    OPERATION_RETURN(scope, instance->state->stringKeys->makeKey(vm, characters, hash, { }));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderFinish, JSCell*, (Instance* instance, uint8_t* builder, uint8_t* cursor, uint32_t isLikelyKey))
{
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    if (stackBuilder.isOnStack()) [[likely]] {
        std::span<const Latin1Character> characters = stackBuilder.charactersOnStack(byteCast<Latin1Character>(cursor));
        if (characters.size() < 2)
            return { characters.empty() ? jsEmptyString(*instance->vm) : jsSingleCharacterString(*instance->vm, characters[0]), nullptr };
        const StringKeyCache& keys = *instance->state->stringKeys;
        if (characters.size() <= keys.maxCachedKeyLength()) [[likely]] {
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

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateByContents(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatenate);
    OPERATION_RETURN(scope, StringBuilderStorage::concatenate(globalObject, *instance, unsafeMakeSpan(parts, count), stringUse(isLikelyKey)));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateShortKey(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    ShortKeyCharacters characters;
    StringKeyCache& keys = *instance->state->stringKeys;
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

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateOtherCount(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    if (count <= 2) {
        if (JSString* key = StringBuilderStorage::tryFindKeyByParts(*instance, unsafeMakeSpan(parts, count)))
            return { key, nullptr };
        [[clang::musttail]] return concatenateByContents(instance, count, parts, isLikelyKey);
    }
    StringKeyCache& keys = *instance->state->stringKeys;
    StringKeyCache::KeyByParts taken;
    if (!taken.tryTake(keys.cageBase(), unsafeMakeSpan(parts, count)))
        taken.countAndKinds = 0;
    else if (JSString* key = keys.tryFindKeyByParts(taken))
        return { key, nullptr };
    keys.lastLookupByParts() = taken;
    [[clang::musttail]] return concatenateShortKey(instance, count, parts, isLikelyKey);
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcatenate, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey))
{
    if (!isLikelyKey) [[unlikely]]
        [[clang::musttail]] return concatenateByContents(instance, count, parts, isLikelyKey);
    StringKeyCache& keys = *instance->state->stringKeys;
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

static NEVER_INLINE OperationReturnType<void> JIT_OPERATION_ATTRIBUTES appendAnyString(Instance* instance, JSCell* builder, JSCell* string)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderAppendString);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderAppendString);
    storage->append(globalObject, cellArgument<JSString>(string, Entry::operationAOTStringBuilderAppendString));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendString, void, (Instance* instance, JSCell* builder, JSCell* string))
{
    if (uncheckedDowncast<StringBuilderStorage>(builder)->tryAppendShortSubstring(uncheckedDowncast<JSString>(string)))
        return { nullptr };
    [[clang::musttail]] return appendAnyString(instance, builder, string);
}

struct NarrowPart {
    std::span<const Latin1Character> span() const LIFETIME_BOUND { return std::span { characters }.first(length); }

    std::array<Latin1Character, 2> characters;
    size_t length;
};

// A part of a 16-bit string is an 8-bit string if it is one Latin-1 unit or two ASCII units.
static ALWAYS_INLINE std::optional<NarrowPart> narrowPartOf(std::span<const char16_t> units)
{
    if (units.size() == 1 && units[0] <= maxSingleCharacterString)
        return NarrowPart { { static_cast<Latin1Character>(units[0]) }, 1 };
    if (units.size() == 2 && isASCII(units[0] | units[1]))
        return NarrowPart { { static_cast<Latin1Character>(units[0]), static_cast<Latin1Character>(units[1]) }, 2 };
    return std::nullopt;
}

static void appendStringRange(JSGlobalObject* globalObject, StringBuilderStorage& storage, JSString* base, int32_t start, int32_t end, Entry entry)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (start < 0 || end < start || static_cast<unsigned>(end) > base->length()) [[unlikely]]
        crashOnWrongOperand(entry);
    unsigned length = end - start;
    if (!length)
        return;
    // A piece of a 16-bit rope may lie in an 8-bit part of it.
    bool mayHaveNarrowParts = !base->is8Bit() && base->isRope() && !base->isSubstring();
    if (length < storage.minLengthAppendedByReference() && !mayHaveNarrowParts) {
        auto view = base->view(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (view->is8Bit()) {
            scope.release();
            storage.append(globalObject, view->span8().subspan(start, length));
            return;
        }
        if (std::optional<NarrowPart> part = narrowPartOf(view->span16().subspan(start, length))) {
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
    appendStringRange(globalObject, *cellArgument<StringBuilderStorage>(builder, entry), cellArgument<JSString>(string, entry), start, end, entry);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendJSON, size_t, (Instance* instance, JSCell* builder, EncodedJSValue value, EncodedJSValue encodedProperty, EncodedJSValue encodedHolder))
{
    constexpr Entry entry = Entry::operationAOTStringBuilderAppendJSON;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* storage = cellArgument<StringBuilderStorage>(builder, entry);
    JSValue property = JSValue::decode(encodedProperty);
    if (!property.isNumber() && !property.isString()) [[unlikely]]
        crashOnWrongOperand(entry);
    uint32_t checkpoint = storage->length();
    JSValue holder = JSValue::decode(encodedHolder);
    JSONHolderRecord innermost { const_cast<JSONHolderRecord*>(storage->jsonHolders()), encodedHolder };
    bool isAppended = appendJSONTextOfProperty(globalObject, *storage, JSValue::decode(value), property, holder.isCell() ? &innermost : storage->jsonHolders());
    ensureStillAliveHere(holder);
    if (!isAppended)
        storage->rollBack(checkpoint);
    OPERATION_RETURN(scope, isAppended);
}

// Writes the members of an object from one name of a list on, with commas between them. True if it wrote any.
JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendJSONMembers, size_t, (Instance* instance, JSCell* builder, EncodedJSValue encodedObject, EncodedJSValue encodedNames, uint32_t start, EncodedJSValue encodedHolder))
{
    constexpr Entry entry = Entry::operationAOTStringBuilderAppendJSONMembers;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* storage = cellArgument<StringBuilderStorage>(builder, entry);
    JSValue holder = JSValue::decode(encodedHolder);
    JSValue namesValue = JSValue::decode(encodedNames);
    JSArray* names = namesValue.isCell() ? dynamicDowncast<JSArray>(namesValue.asCell()) : nullptr;
    if (!names || !JSValue::decode(encodedObject).isObject()) [[unlikely]]
        crashOnWrongOperand(entry);
    JSObject* object = asObject(JSValue::decode(encodedObject));
    JSONHolderRecord innermost { const_cast<JSONHolderRecord*>(storage->jsonHolders()), encodedHolder };
    const JSONHolderRecord* holders = holder.isCell() ? &innermost : storage->jsonHolders();

    bool hasWrittenMember = false;
    for (unsigned index = start; index < names->length(); ++index) {
        JSValue name = names->getIndex(globalObject, index);
        OPERATION_RETURN_IF_EXCEPTION(scope, false);
        if (!name.isString()) [[unlikely]]
            crashOnWrongOperand(entry);
        Identifier identifier = asString(name)->toIdentifier(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, false);
        JSValue value = object->get(globalObject, identifier);
        OPERATION_RETURN_IF_EXCEPTION(scope, false);

        uint32_t checkpoint = storage->length();
        StringBuilder key;
        if (hasWrittenMember)
            key.append(',');
        key.appendQuotedJSONString(identifier.string());
        key.append(':');
        auto appendedKey = key.is8Bit() ? storage->append(globalObject, key.span8()) : storage->append(globalObject, key.span16());
        OPERATION_RETURN_IF_EXCEPTION(scope, false);
        if (appendedKey != StringBuilderStorage::AppendResult::Appended) [[unlikely]]
            crashOnWrongOperand(entry);
        bool isAppended = appendJSONTextOfProperty(globalObject, *storage, value, name, holders);
        OPERATION_RETURN_IF_EXCEPTION(scope, false);
        if (isAppended)
            hasWrittenMember = true;
        else
            storage->rollBack(checkpoint);
    }
    ensureStillAliveHere(holder);
    OPERATION_RETURN(scope, hasWrittenMember);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderReserve, uint8_t*, (Instance* instance, JSCell* builder, uint64_t size))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderReserve);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderReserve);
    OPERATION_RETURN(scope, byteCast<uint8_t>(storage->reserve(globalObject, size)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONEnterWatchedLevel, size_t, (Instance*, JSCell* builder, EncodedJSValue holder, void* frame))
{
    return cellArgument<StringBuilderStorage>(builder, Entry::operationAOTJSONEnterWatchedLevel)->enterWatchedLevel(holder, frame);
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
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderToString);
    OPERATION_RETURN(scope, storage->toString(globalObject, *instance, stringUse(isLikelyKey)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderGrow, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderGrow);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    if (!stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor))) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, byteCast<uint8_t>(stackBuilder.didChangeStorage(globalObject)));
}

static NEVER_INLINE OperationReturnType<uint8_t*> JIT_OPERATION_ATTRIBUTES appendToStorageOfStackStringBuilder(Instance* instance, uint8_t* builder, uint8_t* cursor, EncodedJSValue primitive)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderAppend);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    StringBuilderStorage* storage = stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor));
    if (!storage || storage->append(globalObject, JSValue::decode(primitive)) == StringBuilderStorage::AppendResult::Failed) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, byteCast<uint8_t>(stackBuilder.didChangeStorage(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderAppend, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor, EncodedJSValue primitive))
{
    JSValue value = JSValue::decode(primitive);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    size_t room = byteCast<uint8_t>(stackBuilder.limit) - cursor;
    if (value.isString()) {
        JSString* string = asString(value);
        size_t length = string->length();
        if (length <= room && length < StackStringBuilder::minLengthAppendedByReference && string->is8Bit()) {
            string->resolveToBuffer(std::span<Latin1Character> { byteCast<Latin1Character>(cursor), length });
            return { cursor + length, nullptr };
        }
    } else if (value.isInt32() && static_cast<uint32_t>(value.asInt32()) < 100'000'000 && room >= sizeof(uint64_t))
        return { cursor + writeUpToEightDigits(value.asInt32(), byteCast<Latin1Character>(cursor)), nullptr };
    [[clang::musttail]] return appendToStorageOfStackStringBuilder(instance, builder, cursor, primitive);
}

static NEVER_INLINE OperationReturnType<uint8_t*> JIT_OPERATION_ATTRIBUTES appendStringRangeToStorageOfStackStringBuilder(Instance* instance, uint8_t* builder, uint8_t* cursor, JSCell* string, int32_t start, int32_t end)
{
    constexpr Entry entry = Entry::operationAOTStackStringBuilderAppendStringRange;
    AOT_OPERATION_BEGIN(instance, entry);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    StringBuilderStorage* storage = stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor));
    if (!storage) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    appendStringRange(globalObject, *storage, cellArgument<JSString>(string, entry), start, end, entry);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, byteCast<uint8_t>(stackBuilder.didChangeStorage(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderAppendStringRange, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor, JSCell* string, int32_t start, int32_t end))
{
    const StringImpl* impl = uncheckedDowncast<JSString>(string)->tryGetValueImpl();
    size_t room = byteCast<uint8_t>(reinterpret_cast<StackStringBuilder*>(builder)->limit) - cursor;
    if (impl && !impl->is8Bit() && start >= 0 && start <= end && static_cast<unsigned>(end) <= impl->length() && static_cast<size_t>(end - start) <= std::min<size_t>(room, StackStringBuilder::minLengthAppendedByReference - 1)) {
        size_t length = end - start;
        if (std::optional<NarrowPart> part = narrowPartOf(impl->span16().subspan(start, length))) {
            StringImpl::copyCharacters(std::span<Latin1Character> { byteCast<Latin1Character>(cursor), length }, part->span());
            return { cursor + length, nullptr };
        }
    }
    [[clang::musttail]] return appendStringRangeToStorageOfStackStringBuilder(instance, builder, cursor, string, start, end);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStackStringBuilderDiscard, void, (Instance* instance, uint8_t* builder))
{
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    if (!stackBuilder.isOnStack())
        stackBuilder.storage->release(*instance);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringBuilderDiscard, void, (Instance* instance, JSCell* builder))
{
    cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderDiscard)->release(*instance);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
