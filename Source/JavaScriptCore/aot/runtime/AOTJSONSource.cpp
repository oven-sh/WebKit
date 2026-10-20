/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONSource.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTJSONRuntime.h"
#include "AOTJSONScanner.h"
#include "AOTRuntimeCounters.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include <wtf/text/ASCIIFastPath.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static ALWAYS_INLINE uint8_t* poisonUnusedCapacity(uint8_t* start, size_t size, size_t capacity)
{
#if ASAN_ENABLED
    __asan_unpoison_memory_region(start, size);
    __asan_poison_memory_region(start + size, capacity - size);
#else
    UNUSED_PARAM(size);
    UNUSED_PARAM(capacity);
#endif
    return start;
}

static void freeSourceStorage(uint8_t* start, size_t capacity)
{
#if ASAN_ENABLED
    __asan_unpoison_memory_region(start, capacity);
#else
    UNUSED_PARAM(capacity);
#endif
    fastFree(start);
}

JSONRuntime::JSONRuntime(Instance& instance, CacheRegistry& registry)
    : m_instance(instance)
{
    instance.json.runtime = this;
    registry.add(CacheKind::ClearedEachCollection, "JSON source copy"_s, *this);
}

JSONRuntime::~JSONRuntime()
{
    freeAll();
}

void JSONRuntime::freeCurrent()
{
    JSONWords& words = m_instance.json;
    freeSourceStorage(const_cast<uint8_t*>(words.sourceStart), m_sourceCapacity);
    words.sourceStart = nullptr;
    words.sourceEnd = nullptr;
    m_sourceCapacity = 0;
}

NEVER_INLINE uint8_t* JSONRuntime::replace(size_t size)
{
    freeCurrent();
    size_t capacity = std::max(WTF::roundUpToPowerOfTwo(size), minJSONSourceCapacity);
    void* storage = nullptr;
    if (!tryFastMalloc(capacity).getValue(storage))
        return nullptr;
    m_instance.json.sourceStart = static_cast<uint8_t*>(storage);
    m_sourceCapacity = capacity;
    return poisonUnusedCapacity(static_cast<uint8_t*>(storage), size, capacity);
}

ALWAYS_INLINE uint8_t* JSONRuntime::reserve(size_t size)
{
    size_t capacity = m_sourceCapacity;
    if (size <= capacity && (capacity <= maxRetainedJSONSourceCapacity || size > maxRetainedJSONSourceCapacity)) [[likely]]
        return poisonUnusedCapacity(const_cast<uint8_t*>(m_instance.json.sourceStart), size, capacity);
    return replace(size);
}

void JSONRuntime::suspend()
{
    JSONWords& words = m_instance.json;
    m_suspendedSources.append(JSONSource {
        .start = const_cast<uint8_t*>(words.sourceStart),
        .end = words.sourceEnd,
        .capacity = m_sourceCapacity,
        .ownerStackPointer = words.sourceOwnerStackPointer,
        .wideText = WTF::move(m_wideText),
        .numberOfKeyOrderLists = m_keyOrderLists.size(),
        .encoding = words.sourceEncoding,
        .canEndInline = words.canEndSourceInline,
        .valueParserFailed = words.valueParserFailed,
    });
    words.sourceStart = nullptr;
    m_sourceCapacity = 0;
}

void JSONRuntime::resume()
{
    JSONWords& words = m_instance.json;
    ASSERT(!words.sourceStart);
    JSONSource source = m_suspendedSources.takeLast();
    words.sourceStart = source.start;
    words.sourceEnd = source.end;
    m_sourceCapacity = source.capacity;
    words.sourceOwnerStackPointer = source.ownerStackPointer;
    m_wideText = WTF::move(source.wideText);
    m_keyOrderLists.shrink(source.numberOfKeyOrderLists);
    words.sourceEncoding = source.encoding;
    words.canEndSourceInline = source.canEndInline;
    words.valueParserFailed = source.valueParserFailed;
}

void JSONRuntime::end()
{
    JSONWords& words = m_instance.json;
    ASSERT(words.sourceOwnerStackPointer);
    m_wideText = nullptr;
    if (m_suspendedSources.isEmpty()) [[likely]] {
        words.sourceOwnerStackPointer = nullptr;
        words.canEndSourceInline = false;
        m_keyOrderLists.shrink(0);
        return;
    }
    freeCurrent();
    resume();
}

ALWAYS_INLINE uint8_t* JSONRuntime::beginIn(uint8_t* start, void* stackPointer, size_t length, bool isNested)
{
    JSONWords& words = m_instance.json;
    words.sourceEnd = start + length;
    memset(start + length, 0, jsonSourcePadding);
    words.sourceOwnerStackPointer = stackPointer;
    words.canEndSourceInline = !isNested;
    words.valueParserFailed = false;
    return start;
}

NEVER_INLINE uint8_t* JSONRuntime::beginBesideOpenSource(void* stackPointer, size_t length)
{
    JSONWords& words = m_instance.json;
    while (words.sourceOwnerStackPointer && !mayHaveOpenSource(words, stackPointer))
        end();

    bool isNested = !!words.sourceOwnerStackPointer;
    if (isNested) {
        VM& vm = *m_instance.vm;
        if (m_suspendedSources.size() + 1 >= runtimeLimitValue(vm, LimitID::MaxOpenJSONSources)) {
            countLimitHit(vm, LimitID::MaxOpenJSONSources);
            return nullptr;
        }
        suspend();
    } else
        m_keyOrderLists.shrink(0);

    uint8_t* start = reserve(length + jsonSourcePadding);
    if (!start) [[unlikely]] {
        if (isNested)
            resume();
        return nullptr;
    }
    return beginIn(start, stackPointer, length, isNested);
}

ALWAYS_INLINE uint8_t* JSONRuntime::begin(void* stackPointer, size_t length)
{
    ASSERT(stackPointer);
    if (m_instance.json.sourceOwnerStackPointer) [[unlikely]]
        return beginBesideOpenSource(stackPointer, length);

    m_keyOrderLists.shrink(0);
    uint8_t* start = reserve(length + jsonSourcePadding);
    if (!start) [[unlikely]]
        return nullptr;
    return beginIn(start, stackPointer, length, false);
}

static NEVER_INLINE StringImpl* flattenJSONSource(JSString* string)
{
    auto value = string->tryGetValue();
    return value.data.impl();
}

ALWAYS_INLINE bool JSONRuntime::openLatin1(std::span<const Latin1Character> characters, void* stackPointer)
{
    uint8_t* start = begin(stackPointer, characters.size());
    if (!start) [[unlikely]]
        return false;
    if (!characters.empty())
        memcpy(start, characters.data(), characters.size());
    m_instance.json.sourceEncoding = JSONSourceEncoding::Latin1;
    return true;
}

NEVER_INLINE bool JSONRuntime::open16Bit(std::span<const char16_t> characters, void* stackPointer)
{
    size_t length = characters.size();
    uint8_t* start = begin(stackPointer, length);
    if (!start)
        return false;
    for (size_t i = 0; i < length; ++i)
        start[i] = std::min<char16_t>(characters[i], saturatedJSONCharacter);
    m_instance.json.sourceEncoding = JSONSourceEncoding::UTF16;
    m_instance.json.canEndSourceInline = false;
    return true;
}

bool JSONRuntime::openUTF8(std::span<const uint8_t> bytes, void* stackPointer)
{
    uint8_t* start = begin(stackPointer, bytes.size());
    if (!start)
        return false;
    if (!bytes.empty())
        memcpy(start, bytes.data(), bytes.size());
    bool isASCII = charactersAreAllASCII(std::span<const uint8_t> { start, bytes.size() });
    m_instance.json.sourceEncoding = isASCII ? JSONSourceEncoding::Latin1 : JSONSourceEncoding::UTF8;
    return true;
}

bool JSONRuntime::open(JSValue textOrBytes, void* stackPointer)
{
    if (!textOrBytes.isCell()) [[unlikely]]
        return false;
    JSCell* cell = textOrBytes.asCell();
    if (cell->type() == StringType) [[likely]] {
        JSString* string = asString(textOrBytes);
        StringImpl* text = string->tryGetValueImpl();
        if (!text) [[unlikely]] {
            text = flattenJSONSource(string);
            if (!text)
                return false;
        }
        if (text->is8Bit()) [[likely]]
            return openLatin1(text->span8(), stackPointer);
        if (!open16Bit(text->span16(), stackPointer))
            return false;
        m_wideText = text;
        return true;
    }
    if (cell->type() == Uint8ArrayType)
        return openUTF8(withoutByteOrderMark(uncheckedDowncast<JSArrayBufferView>(cell)->span()), stackPointer);
    return false;
}

bool JSONRuntime::open(StringView text, void* stackPointer)
{
    if (text.is8Bit()) [[likely]]
        return openLatin1(text.span8(), stackPointer);
    if (!open16Bit(text.span16(), stackPointer))
        return false;
    m_wideText = StringImpl::createWithoutCopying(text.span16());
    return true;
}

bool JSONRuntime::open(std::span<const uint8_t> utf8, void* stackPointer)
{
    return openUTF8(utf8, stackPointer);
}

bool JSONRuntime::close(const uint8_t* cursor)
{
    JSONWords& words = m_instance.json;
    if (!words.sourceOwnerStackPointer)
        return false;
    bool isComplete = skipJSONWhitespaceCharacters(cursor) == words.sourceEnd;
    end();
    return isComplete;
}

void JSONRuntime::abandon()
{
    if (m_instance.json.sourceOwnerStackPointer)
        end();
}

void JSONRuntime::dropSourcesBelow(void* stackPointer)
{
    JSONWords& words = m_instance.json;
    while (words.sourceOwnerStackPointer && std::bit_cast<uintptr_t>(words.sourceOwnerStackPointer) < std::bit_cast<uintptr_t>(stackPointer))
        end();
}

void JSONRuntime::clear()
{
    if (m_instance.json.sourceOwnerStackPointer)
        return;
    m_keyOrderLists.shrink(0);
    if (m_sourceCapacity > maxRetainedJSONSourceCapacity)
        freeCurrent();
}

void JSONRuntime::freeAll()
{
    freeCurrent();
    for (JSONSource& source : m_suspendedSources)
        freeSourceStorage(source.start, source.capacity);
    m_suspendedSources.clear();
    m_wideText = nullptr;
    m_keyOrderLists.shrink(0);
    m_instance.json.sourceOwnerStackPointer = nullptr;
    m_instance.json.canEndSourceInline = false;
}

void JSONWords::dropSourcesBelow(void* stackPointer)
{
    runtime->dropSourcesBelow(stackPointer);
}

bool JSONSourceScope::open(StringView text, void* stackPointer)
{
    return m_instance.json.runtime->open(text, stackPointer);
}

bool JSONSourceScope::open(std::span<const uint8_t> utf8, void* stackPointer)
{
    return m_instance.json.runtime->open(utf8, stackPointer);
}

JSONSourceScope::~JSONSourceScope()
{
    if (m_isOpen)
        m_instance.json.runtime->abandon();
}

const uint8_t* JSONSourceScope::start() const
{
    ASSERT(m_isOpen);
    return m_instance.json.sourceStart;
}

static String decodeUTF16LittleEndian(std::span<const uint8_t> bytes)
{
    size_t length = bytes.size() / sizeof(char16_t);
    std::span<char16_t> characters;
    RefPtr<StringImpl> text = StringImpl::tryCreateUninitialized(length, characters);
    if (!text)
        return { };
    for (size_t index = 0; index < length; ++index)
        characters[index] = static_cast<char16_t>(bytes[2 * index] | bytes[2 * index + 1] << 8);
    return String(WTF::move(text));
}

JSValue decodeJSONBody(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isCell() || value.asCell()->type() != Uint8ArrayType)
        return value;
    std::span<const uint8_t> bytes = uncheckedDowncast<JSArrayBufferView>(value.asCell())->span();
    if (bytes.empty()) {
        throwSyntaxError(globalObject, scope, "Unexpected end of JSON input"_s);
        return { };
    }
    if (!StringImpl::isValidLength<char16_t>(bytes.size()))
        return value;
    String text = hasUTF16LittleEndianMark(bytes) ? decodeUTF16LittleEndian(bytes.subspan(2)) : String::fromUTF8ReplacingInvalidSequences(withoutByteOrderMark(bytes));
    return text.isNull() ? value : jsString(vm, WTF::move(text));
}

bool isLatin1AfterDecoding(const JSONWords& words, std::span<const uint8_t> characters)
{
    if (words.sourceEncoding == JSONSourceEncoding::UTF8)
        return charactersAreAllASCII(characters);
    return !memchr(characters.data(), saturatedJSONCharacter, characters.size());
}

std::span<const char16_t> original16BitCharacters(const JSONWords& words, std::span<const uint8_t> characters)
{
    ASSERT(words.sourceEncoding == JSONSourceEncoding::UTF16);
    return { words.runtime->wideText()->span16().data() + (characters.data() - words.sourceStart), characters.size() };
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
