/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderStorage.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include <array>
#include <charconv>
#include <wtf/dtoa.h>
#include <wtf/text/ASCIIFastPath.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(sizeof(WriteBarrier<StringBuilderStorage>) == sizeof(void*), "compiled code loads and stores the next free cell as one word");
static_assert(defaultLimitValue(LimitID::MaxPooledStringBuilders, hostArchitecture()) <= std::numeric_limits<uint8_t>::max());

const ClassInfo StringBuilderStorage::s_info = { "StringBuilderStorage"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(StringBuilderStorage) };

static NEVER_INLINE void throwOutOfMemory(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
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

StringBuilderStorage::StringBuilderStorage(VM& vm, Structure* structure)
    : Base(vm, structure)
    , m_limits(vm)
{
}

StringBuilderStorage* StringBuilderStorage::create(VM& vm)
{
    auto* storage = new (NotNull, allocateCell<StringBuilderStorage>(vm)) StringBuilderStorage(vm, vm.aotState().stringBuilderStorageStructure.get());
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
    return create(*instance.vm);
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
    unsigned depth = head ? head->m_poolDepth + 1 : 1;
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
    m_position = 0;
    m_lengthOutsideBuffer = 0;
    m_lengthOfLastString = 0;
    m_lastString.clear();
    bool isTooLargeToRetain = capacity() > m_limits.maxRetainedCapacity;
    if (isTooLargeToRetain)
        countLimitHit(vm(), LimitID::MaxRetainedStringBuilderCapacity);
    if (isTooLargeToRetain || (m_buffer && !m_buffer->hasOneRef())) {
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
    size_t grownCapacity = needed > currentCapacity ? currentCapacity * 2 : currentCapacity;
    return tryReplaceBuffer(std::max(needed, std::min<size_t>(std::max<size_t>(grownCapacity, m_limits.minCapacity), String::MaxLength)));
}

NEVER_INLINE bool StringBuilderStorage::tryReplaceBuffer(size_t newCapacity)
{
    std::span<Latin1Character> characters;
    RefPtr<StringImpl> buffer = StringImpl::tryCreateUninitialized(newCapacity, characters);
    if (!buffer)
        return false;
    StringImpl::copyCharacters(characters, std::span<const Latin1Character> { m_characters, m_position });
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
    if (WTF::charactersAreAllLatin1(characters)) {
        if (!ensureRoom(globalObject, characters.size()))
            return AppendResult::Failed;
        StringImpl::copyCharacters(std::span { m_characters + m_position, characters.size() }, characters);
        m_position += static_cast<uint32_t>(characters.size());
        return AppendResult::Appended;
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

// The units of a 16-bit string that lie in one piece of memory: of a flat string, and of a part of one.
static std::optional<std::span<const char16_t>> flatUnitsOf(JSString* string)
{
    if (const StringImpl* impl = string->tryGetValueImpl())
        return impl->span16();
    if (!string->isSubstring())
        return std::nullopt;
    auto* rope = uncheckedDowncast<JSRopeString>(string);
    return rope->substringBase()->tryGetValueImpl()->span16().subspan(rope->substringOffset(), rope->length());
}

auto StringBuilderStorage::append(JSGlobalObject* globalObject, JSString* string) -> AppendResult
{
    VM& vm = globalObject->vm();
    uint32_t length = string->length();
    if (!length)
        return AppendResult::Appended;
    bool wasEmpty = !this->length();
    bool isAppendedByReference = length >= m_limits.minLengthAppendedByReference || !string->is8Bit();
    if (isAppendedByReference && length < m_limits.minLengthAppendedByReference) {
        if (std::optional<std::span<const char16_t>> units = flatUnitsOf(string); units && WTF::charactersAreAllLatin1(*units))
            return append(globalObject, *units);
    }
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
        if (!collapseSegments(globalObject))
            return AppendResult::Failed;
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

Latin1Character* StringBuilderStorage::tryReserve(size_t size)
{
    return tryEnsureRoom(size) ? m_characters + m_position : nullptr;
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
        throwOutOfMemory(globalObject);
        return nullptr;
    }
    storage->m_position = static_cast<uint32_t>(position);
    return storage;
}

Latin1Character* StackStringBuilder::didChangeStorage(JSGlobalObject* globalObject)
{
    if (!storage->tryEnsureRoom(0, maxBytesWrittenPastLimit)) [[unlikely]] {
        throwOutOfMemory(globalObject);
        return nullptr;
    }
    ASSERT(!storage->m_sharedLength && storage->capacity() >= storage->m_position + maxBytesWrittenPastLimit);
    uintptr_t characters = std::bit_cast<uintptr_t>(storage->m_characters);
    limit = std::bit_cast<Latin1Character*>(characters + std::min<size_t>(storage->capacity() - maxBytesWrittenPastLimit, String::MaxLength - storage->m_lengthOutsideBuffer));
    lengthOrigin = std::bit_cast<Latin1Character*>(characters - storage->m_lengthOutsideBuffer);
    return storage->m_characters + storage->m_position;
}

extern "C" uint8_t* JSC__AOT__StringBuilderStorage__tryReserve(StringBuilderStorage* storage, size_t maxLength)
{
    return byteCast<uint8_t>(storage->tryReserve(maxLength));
}

extern "C" void JSC__AOT__StringBuilderStorage__commit(StringBuilderStorage* storage, size_t written)
{
    if (written)
        storage->commit(storage->tryReserve(0) + written);
}

extern "C" bool JSC__AOT__StringBuilderStorage__appendUTF16(StringBuilderStorage* storage, JSGlobalObject* globalObject, const char16_t* characters, size_t length)
{
    return storage->append(globalObject, std::span { characters, length }) == StringBuilderStorage::AppendResult::Appended;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
