/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONScanner.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTConvention.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTJSONHooks.h"
#include "AOTJSONKeyOrder.h"
#include "AOTJSONRuntime.h"
#include "AOTJSONValueParser.h"
#include "AOTModuleHost.h"
#include "AOTObject.h"
#include "AOTObjectBuilder.h"
#include "AOTObjectBuilderInlines.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferTermination.h"
#include "GCMemoryOperations.h"
#include "GlobalObjectMethodTable.h"
#include "JSArrayBufferView.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSONObject.h"
#include "ObjectConstructor.h"
#include "ObjectInitializationScope.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <wtf/FastFloat.h>
#include <wtf/Scope.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/unicode/UTF8Conversion.h>

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

JSONRuntime::JSONRuntime(Instance& instance, CacheRegistry& registry, ZeroedTableArena& arena)
    : m_instance(instance)
    , m_recordPlanCache(arena)
    , m_learnedNames(arena)
    , m_learnedExtraMembers(arena)
    , m_learnedObjects(arena)
{
    instance.json.runtime = this;
    m_hasParsedRecords = instance.state && !instance.state->tables.parsedRecordFields().empty();
    registry.add(CacheKind::ClearedEachCollection, "JSON source copy"_s, *this);
}

JSONValueParser::LearnedNames& JSONRuntime::learnedNames()
{
    if (!m_learnedNames) [[unlikely]]
        m_learnedNames.make();
    return *m_learnedNames;
}

std::unique_ptr<ObjectBuilderPlan> JSONRuntime::createRecordPlan(VM& vm, std::span<UniquedStringImpl* const> names)
{
    State& state = *m_instance.state;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::optional<uint32_t> lastName = vm.aotState().identifiers->numberOf(names.back());
    if (!lastName)
        return nullptr;

    std::unique_ptr<ObjectBuilderPlan> best;
    for (const ParsedRecordFieldData& row : std::ranges::equal_range(state.tables.parsedRecordFields(), *lastName, { }, &ParsedRecordFieldData::name)) {
        const LayoutData& layout = layouts.layoutForShape(row.shape);
        if (layout.numberOfFields < names.size() || (best && best->layout().numberOfFields <= layout.numberOfFields) || !ObjectBuilder::canBuildWithoutCompilerKnowledge(layout))
            continue;
        bool hasEveryName = std::ranges::all_of(names, [&](UniquedStringImpl* name) {
            return !!layouts.field(vm, layout, name);
        });
        if (!hasEveryName)
            continue;
        std::unique_ptr<ObjectBuilderPlan> plan = ObjectBuilder::tryCreatePlan(state, layout, names);
        if (plan && plan->hasFastPath())
            best = WTF::move(plan);
    }
    return best;
}

const ObjectBuilderPlan* JSONRuntime::findOrCreateRecordPlan(VM& vm, Structure* structure)
{
    if (structure->isDictionary())
        return nullptr;
    UniquedStringImpl* lastName = structure->transitionPropertyName();
    std::optional<uint32_t> lastNameNumber = lastName ? vm.aotState().identifiers->numberOf(lastName) : std::nullopt;
    if (!lastNameNumber || std::ranges::equal_range(m_instance.state->tables.parsedRecordFields(), *lastNameNumber, { }, &ParsedRecordFieldData::name).empty())
        return nullptr;

    unsigned inlineCapacity = structure->inlineCapacity();
    Vector<UniquedStringImpl*, 16> names;
    names.fill(nullptr, numberOfSlotsForMaxOffset(structure->maxOffset(), inlineCapacity));
    bool hasOnlyPlainProperties = !names.isEmpty();
    structure->forEachProperty(vm, [&](const auto& entry) -> bool {
        size_t index = isInlineOffset(entry.offset()) ? entry.offset() : inlineCapacity + (entry.offset() - firstOutOfLineOffset);
        if (entry.attributes() || entry.key()->isSymbol() || index >= names.size())
            hasOnlyPlainProperties = false;
        else
            names[index] = entry.key();
        return hasOnlyPlainProperties;
    });
    if (!hasOnlyPlainProperties || names.contains(nullptr) || names.last() != lastName)
        return nullptr;

    for (const RecordPlanForNames& known : m_recordPlans) {
        if (std::ranges::equal(known.names, names, { }, &RefPtr<UniquedStringImpl>::get))
            return known.plan.get();
    }
    if (m_recordPlans.size() == maxRecordPlans)
        return nullptr;
    m_recordPlans.append({ { }, createRecordPlan(vm, names.span()) });
    m_recordPlans.last().names.appendContainerWithMapping(names, [](UniquedStringImpl* name) {
        return RefPtr { name };
    });
    return m_recordPlans.last().plan.get();
}

NEVER_INLINE JSONRuntime::RecordPlan JSONRuntime::findRecordPlan(VM& vm, Structure* ordinaryStructure)
{
    if (!m_recordPlanCache)
        m_recordPlanCache.make();
    RecordPlan recordPlan;
    if (const ObjectBuilderPlan* plan = findOrCreateRecordPlan(vm, ordinaryStructure))
        recordPlan = { plan, m_instance.state->objects->structures.objectStructure(vm, plan->layout().shape) };
    uint32_t id = ordinaryStructure->id().bits();
    (*m_recordPlanCache)[recordPlanCacheIndex(id)] = { id, recordPlan };
    return recordPlan;
}

JSONRuntime::~JSONRuntime()
{
    freeCurrent();
    for (JSONSource& source : m_suspendedSources)
        freeSourceStorage(source.start, source.capacity);
    m_instance.json.sourceOwnerStackPointer = nullptr;
    m_instance.json.canEndSourceInline = false;
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
        .furthestAttemptedPosition = m_furthestAttemptedPosition,
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
    m_furthestAttemptedPosition = source.furthestAttemptedPosition;
    m_furthestFailedPosition = source.furthestAttemptedPosition;
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
    m_furthestAttemptedPosition = nullptr;
    m_furthestFailedPosition = nullptr;
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

bool JSONRuntime::open(std::span<const uint8_t> bytes, void* stackPointer)
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

ALWAYS_INLINE bool JSONRuntime::openInline(JSValue textOrBytes, void* stackPointer)
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
        return open(withoutByteOrderMark(uncheckedDowncast<JSArrayBufferView>(cell)->span()), stackPointer);
    return false;
}

bool JSONRuntime::open(JSValue textOrBytes, void* stackPointer)
{
    return openInline(textOrBytes, stackPointer);
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
    m_finishedObjects.fill({ });
    if (m_learnedNames)
        m_learnedNames->clear();
    if (m_learnedExtraMembers) {
        for (LearnedExtraMembers& learned : *m_learnedExtraMembers)
            learned.begin(LearnedExtraMembers::noKey, 0);
    }
    if (m_learnedObjects) {
        for (LearnedObject& learned : *m_learnedObjects)
            learned.forgetCells();
    }
    m_numberOfObjectsLearned = 0;
    m_uncachedFinishesRemaining = 0;
    m_timesNotFoundInARow.fill(0);
    m_timesRefusedInARow.fill(0);
    if (m_recordPlanCache)
        m_recordPlanCache->fill({ });
    for (const RecordPlanForNames& known : m_recordPlans) {
        if (known.plan)
            known.plan->clearAcceptedStructures();
    }
    if (m_instance.json.sourceOwnerStackPointer)
        return;
    m_keyOrderLists.shrink(0);
    if (m_sourceCapacity > maxRetainedJSONSourceCapacity)
        freeCurrent();
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

JSValue decodeJSONBody(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isCell() || value.asCell()->type() != Uint8ArrayType)
        return value;
    std::span<const uint8_t> bytes = uncheckedDowncast<JSArrayBufferView>(value.asCell())->span();
    if (bytes.empty()) {
        const ModuleHost* host = globalObject->globalObjectMethodTable()->moduleHost;
        if (host && host->createEmptyJSONBodyError)
            throwException(globalObject, scope, host->createEmptyJSONBodyError(globalObject));
        else {
            scope.release();
            JSONParseWithException(globalObject, emptyString());
        }
        return { };
    }
    if (!StringImpl::isValidLength<char16_t>(bytes.size()))
        return value;
    String text = String::fromUTF8ReplacingInvalidSequences(withoutByteOrderMark(bytes));
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

static bool appendPlainCharacters(const JSONWords& words, StringBuilder& builder, std::span<const uint8_t> characters)
{
    switch (words.sourceEncoding) {
    case JSONSourceEncoding::Latin1:
        builder.append(byteCast<Latin1Character>(characters));
        return true;
    case JSONSourceEncoding::UTF16:
        builder.append(original16BitCharacters(words, characters));
        return true;
    case JSONSourceEncoding::UTF8:
        break;
    }
    if (!StringImpl::isValidLength<char16_t>(characters.size()))
        return false;
    Vector<char16_t, JSONCache::maxLongStringLength> decoded;
    if (!decoded.tryGrow(characters.size()))
        return false;
    auto result = WTF::Unicode::convert(byteCast<char8_t>(characters), decoded.mutableSpan());
    if (result.code != WTF::Unicode::ConversionResultCode::Success)
        return false;
    builder.append(result.buffer);
    return true;
}

// JSON.parse() keeps a string of over JSONCache's atomized length in the width of its source.
static JSString* tryMakeJSONStringOfWideSource(const JSONWords& words, VM& vm, std::span<const uint8_t> latin1)
{
    if (words.sourceEncoding == JSONSourceEncoding::UTF16)
        return vm.jsonCache().tryMakeJSString(vm, original16BitCharacters(words, latin1));
    Vector<char16_t, JSONCache::maxLongStringLength> wide;
    if (!StringImpl::isValidLength<char16_t>(latin1.size()) || !wide.tryGrow(latin1.size()))
        return nullptr;
    WTF::copyElements(wide.mutableSpan(), byteCast<Latin1Character>(latin1));
    return vm.jsonCache().tryMakeJSString(vm, wide.span());
}

const uint8_t* decodeJSONString(const JSONWords& words, const uint8_t* start, const uint8_t* cursor, StringBuilder& builder)
{
    for (;;) {
        if (!appendPlainCharacters(words, builder, { start, cursor }))
            return nullptr;
        if (*cursor == '"')
            return cursor + 1;
        unsigned length = *cursor == '\\' ? jsonEscapeLength(cursor) : 0;
        if (!length)
            return nullptr;
        switch (cursor[1]) {
        case 'b':
            builder.append('\b');
            break;
        case 'f':
            builder.append('\f');
            break;
        case 'n':
            builder.append('\n');
            break;
        case 'r':
            builder.append('\r');
            break;
        case 't':
            builder.append('\t');
            break;
        case 'u':
            builder.append(static_cast<char16_t>(toASCIIHexValue(cursor[2]) << 12 | toASCIIHexValue(cursor[3]) << 8 | toASCIIHexValue(cursor[4]) << 4 | toASCIIHexValue(cursor[5])));
            break;
        default:
            builder.append(static_cast<Latin1Character>(cursor[1]));
            break;
        }
        start = cursor + length;
        cursor = findEndOfPlainJSONCharacters(start);
    }
}

NEVER_INLINE ParsedJSONString makeJSONStringWithEscapes(Instance& instance, VM& vm, const uint8_t* afterQuote, const uint8_t* firstSpecial)
{
    StringBuilder builder(OverflowPolicy::RecordOverflow);
    const uint8_t* end = decodeJSONString(instance.json, afterQuote, firstSpecial, builder);
    if (!end || builder.hasOverflowed())
        return { jsEmptyString(vm) };
    JSString* string = builder.is8Bit() ? vm.jsonCache().tryMakeJSString(vm, builder.span8()) : vm.jsonCache().tryMakeJSString(vm, builder.span16());
    if (!string)
        return { jsEmptyString(vm) };
    return { string, end };
}

NEVER_INLINE ParsedJSONString makeUncachedJSONString(Instance& instance, VM& vm, const uint8_t* start)
{
    ASSERT(start[-1] == '"');
    const JSONWords& words = instance.json;
    const uint8_t* cursor = findEndOfPlainJSONCharacters(start);
    if (*cursor != '"') [[unlikely]]
        return makeJSONStringWithEscapes(instance, vm, start, cursor);
    if (!isLatin1(words, { start, cursor })) [[unlikely]] {
        if (words.sourceEncoding != JSONSourceEncoding::UTF16)
            return makeJSONStringWithEscapes(instance, vm, start, cursor);
        auto characters = original16BitCharacters(words, { start, cursor });
        std::span<char16_t> copy;
        auto impl = StringImpl::tryCreateUninitialized(characters.size(), copy);
        if (!impl)
            return { jsEmptyString(vm) };
        memcpy(copy.data(), characters.data(), characters.size_bytes());
        return { jsString(vm, String { impl.releaseNonNull() }), cursor + 1 };
    }
    if (words.sourceEncoding != JSONSourceEncoding::Latin1 && static_cast<size_t>(cursor - start) > maxLengthOfJSONStringOfEitherWidth) [[unlikely]] {
        JSString* string = tryMakeJSONStringOfWideSource(words, vm, { start, cursor });
        return string ? ParsedJSONString { string, cursor + 1 } : ParsedJSONString { jsEmptyString(vm) };
    }
    JSString* string = vm.jsonCache().tryMakeJSString(vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(cursor) });
    if (!string) [[unlikely]]
        return { jsEmptyString(vm) };
    // Only a source that is no Latin-1 text asks, so only there is it worth finding out.
    bool textIsASCII = words.sourceEncoding != JSONSourceEncoding::Latin1 && charactersAreAllASCII(std::span<const uint8_t> { start, cursor });
    vm.jsonCache().addString(std::span<const Latin1Character> { byteCast<Latin1Character>(start - 1), jsonSourcePadding }, cursor + 2 - start, string, textIsASCII);
    return { string, cursor + 1 };
}

static NEVER_INLINE NOT_TAIL_CALLED ParsedJSONString makeUncachedJSONStringInFrame(Instance& instance, CallerFrameAndPC* frameRecord, const uint8_t* start)
{
    AOT_OPERATION_BEGIN_IN_FRAME(&instance, frameRecord, Entry::operationAOTJSONParseString);
    return makeUncachedJSONString(instance, vm, start);
}

NEVER_INLINE ParsedJSONNumber parseJSONNumberSlow(const uint8_t* start, const uint8_t* sourceEnd)
{
    size_t length = 0;
    auto number = WTF::parseJSONDouble(std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(sourceEnd) }, length);
    if (!number || !std::isfinite(*number))
        return { };
    return { *number, start + length };
}

static ALWAYS_INLINE const uint8_t* skipWhitespace(const uint8_t* cursor)
{
    if (*cursor <= ' ') [[unlikely]]
        return skipJSONWhitespaceCharacters(cursor);
    return cursor;
}

static ALWAYS_INLINE PropertyOffset memberPropertyOffset(unsigned index)
{
    return offsetForPropertyNumber(index, JSFinalObject::defaultInlineCapacity);
}

JSONValueParser::JSONValueParser(Instance& instance, VM& vm, JSGlobalObject* globalObject)
    : m_instance(instance)
    , m_words(instance.json)
    , m_vm(vm)
    , m_globalObject(globalObject)
    , m_cache(vm.jsonCache())
    , m_parsingScope(m_cache)
    , m_learnedNames(instance.json.runtime->learnedNames())
{
}

ParsedJSONValue JSONValueParser::parse(const uint8_t* start)
{
    JSONWords& words = m_instance.json;
    if (words.valueParserFailed) [[unlikely]]
        return { };
    DeferTerminationForAWhile deferTermination(m_vm);
    ParsedJSONValue parsed = parseValue(start);
    words.valueParserFailed = !parsed.end;
    return parsed;
}

ParsedJSONValue JSONValueParser::parseWholeSource(const uint8_t* start)
{
    ParsedJSONValue parsed = parse(start);
    if (!parsed.end || skipWhitespace(parsed.end) != m_words.sourceEnd)
        return { };
    return parsed;
}

ParsedJSONValue JSONValueParser::parseValue(Cursor cursor)
{
    cursor = skipWhitespace(cursor);
    switch (*cursor) {
    case '"':
        // "$", " ", ",": one plain ASCII character between the quotes is a string that the engine keeps.
        if (cursor[2] == '"' && cursor[1] - 0x20u < 0x60u && cursor[1] != '"' && cursor[1] != '\\')
            return { m_vm.smallStrings.singleCharacterString(cursor[1]), cursor + 3 };
        return parseString(cursor);
    case '{':
        return parseObject(cursor);
    case '[':
        return parseArray(cursor);
    case 't':
        if (WTF::unalignedLoad<uint32_t>(cursor) != ('t' | 'r' << 8 | 'u' << 16 | 'e' << 24))
            return { };
        return { jsBoolean(true), cursor + 4 };
    case 'f':
        if (WTF::unalignedLoad<uint32_t>(cursor + 1) != ('a' | 'l' << 8 | 's' << 16 | 'e' << 24))
            return { };
        return { jsBoolean(false), cursor + 5 };
    case 'n':
        if (WTF::unalignedLoad<uint32_t>(cursor) != ('n' | 'u' << 8 | 'l' << 16 | 'l' << 24))
            return { };
        return { jsNull(), cursor + 4 };
    default:
        return parseNumber(cursor);
    }
}

NEVER_INLINE ParsedJSONValue JSONValueParser::parseString(Cursor quote)
{
    if (auto cached = findCachedJSONString(m_words, m_vm, quote)) [[likely]]
        return { cached->string, cached->end };
    ParsedJSONString parsed = makeUncachedJSONString(m_instance, m_vm, quote + 1);
    if (!parsed.end) [[unlikely]]
        return { };
    return { parsed.string, parsed.end };
}

NEVER_INLINE ParsedJSONValue JSONValueParser::parseNumber(Cursor cursor)
{
    JSValue integer;
    if (Cursor end = tryParseSmallJSONInteger(cursor, integer)) [[likely]]
        return { integer, end };
    ParsedJSONNumber parsed = parseJSONNumber(cursor, m_words.sourceEnd);
    if (!parsed.end) [[unlikely]]
        return { };
    return { jsNumber(parsed.number), parsed.end };
}

void JSONValueParser::NamesOfElementInFront::note(const JSONWords& words, unsigned index, Cursor quote, const NextMember& next)
{
    if (index > count || index == capacity)
        return;
    // What stood behind this name went on from another Structure.
    count = index;
    size_t length = next.value - quote;
    if (length > maxTextLength)
        return;
    // One byte of the source stands for every character that is no Latin-1: equal bytes are not equal names.
    if (words.sourceEncoding != JSONSourceEncoding::Latin1 && !isShortTextLatin1AfterDecoding(words, quote, length))
        return;
    static_assert(maxTextLength <= jsonSourcePadding);
    auto maskFor = [](size_t bytes) -> uint64_t {
        return bytes >= sizeof(uint64_t) ? std::numeric_limits<uint64_t>::max() : (1ull << (8 * bytes)) - 1;
    };
    Name& name = names[index];
    name.mask[0] = maskFor(length);
    name.mask[1] = maskFor(length > sizeof(uint64_t) ? length - sizeof(uint64_t) : 0);
    name.text[0] = WTF::unalignedLoad<uint64_t>(quote) & name.mask[0];
    name.text[1] = WTF::unalignedLoad<uint64_t>(quote + sizeof(uint64_t)) & name.mask[1];
    if (isSharedBetweenSources && (name.text[0] | name.text[1]) & 0x8080808080808080ULL)
        return;
    name.structure = next.structure;
    name.nameIndex = next.nameIndex;
    name.textLength = length;
    count = index + 1;
}

template<bool isShared>
ALWAYS_INLINE std::optional<ParsedJSONValue> JSONValueParser::parseMembersInFront(Members& members, Cursor& cursor, NamesOfElementInFront& inFront)
{
    static_assert(NamesOfElementInFront::capacity <= std::size(decltype(members.values) { }));
    Structure* structure = nullptr;
    while (members.count < inFront.count) {
        const auto& name = inFront.names[members.count];
        if (((WTF::unalignedLoad<uint64_t>(cursor) & name.mask[0]) ^ name.text[0]) | ((WTF::unalignedLoad<uint64_t>(cursor + sizeof(uint64_t)) & name.mask[1]) ^ name.text[1])) [[unlikely]]
            break;
        structure = name.structure;
        if constexpr (isShared)
            members.structure = structure;
        Cursor start = skipWhitespace(cursor + name.textLength);
        ParsedJSONValue parsed;
        if (Cursor end = tryParseSmallJSONInteger(start, parsed.value))
            parsed.end = end;
        else {
            parsed = parseValue(start);
            if (!parsed.end) [[unlikely]]
                return ParsedJSONValue { };
        }
        members.values[members.count++] = JSValue::encode(parsed.value);
        cursor = skipWhitespace(parsed.end);
        if (*cursor == ',') {
            cursor = skipWhitespace(cursor + 1);
            if constexpr (isShared) {
                if (!inFront.continuesFrom(members.count, structure)) [[unlikely]]
                    break;
            }
            continue;
        }
        if (*cursor != '}') [[unlikely]]
            return ParsedJSONValue { };
        members.structure = structure;
        ++cursor;
        return ParsedJSONValue { createObject(members), cursor };
    }
    if (structure)
        members.structure = structure;
    return std::nullopt;
}

NEVER_INLINE ParsedJSONValue JSONValueParser::parseObject(Cursor brace, NamesOfElementInFront* inFront)
{
    if (!m_vm.isSafeToRecurse() || m_vm.traps().needsOutOfMemoryError()) [[unlikely]]
        return { };
    Cursor cursor = skipWhitespace(brace + 1);
    if (*cursor == '}') [[unlikely]]
        return { constructEmptyObject(m_globalObject), cursor + 1 };

    Members members;
    members.structure = m_globalObject->objectStructureForObjectConstructor();
    ASSERT(members.structure->inlineCapacity() == JSFinalObject::defaultInlineCapacity);
    bool isShared = !inFront;
    if (isShared)
        inFront = &m_learnedNames.at(cursor);
    if (std::optional<ParsedJSONValue> parsed = isShared ? parseMembersInFront<true>(members, cursor, *inFront) : parseMembersInFront<false>(members, cursor, *inFront))
        return *parsed;
    members.nameIndex = JSONCache::nameIndex(StructureID::encode(members.structure));
    for (;;) {
        if (members.count == std::size(members.values)) [[unlikely]]
            return parseRemainingMembers(createObject(members, ObjectKind::Ordinary), cursor);
        NextMember next = findKnownMember(members, cursor);
        if (!next.structure) [[unlikely]] {
            next = findOrAddMember(members, cursor);
            if (!next.structure)
                return parseRemainingMembers(createObject(members, ObjectKind::Ordinary), cursor);
        }
        if (!isShared || inFront->continuesFrom(members.count, members.structure))
            inFront->note(m_words, members.count, cursor, next);
        ParsedJSONValue parsed = parseValue(next.value);
        if (!parsed.end) [[unlikely]]
            return { };
        members.values[members.count++] = JSValue::encode(parsed.value);
        members.structure = next.structure;
        members.nameIndex = next.nameIndex;
        cursor = skipWhitespace(parsed.end);
        if (*cursor == ',') {
            cursor = skipWhitespace(cursor + 1);
            continue;
        }
        if (*cursor != '}') [[unlikely]]
            return { };
        return { createObject(members), cursor + 1 };
    }
}

auto JSONValueParser::findKnownMember(const Members& members, Cursor quote) -> NextMember
{
    auto* entry = m_cache.findName(StructureID::encode(members.structure), members.nameIndex, std::span<const Latin1Character> { byteCast<Latin1Character>(quote), jsonSourcePadding });
    if (!entry || entry->offset != memberPropertyOffset(members.count))
        return { };
    if (m_words.sourceEncoding != JSONSourceEncoding::Latin1 && !entry->textIsASCII && !isShortTextLatin1AfterDecoding(m_words, quote, entry->textLength))
        return { };
    return { entry->to.get(), quote + entry->textLength, entry->toNameIndex };
}

auto JSONValueParser::findOrAddMember(const Members& members, Cursor quote) -> NextMember
{
    ParsedName parsed = parseNameAndColon(quote);
    if (!parsed.afterColon) [[unlikely]]
        return { };
    const Identifier& name = parsed.name;
    if (parseIndex(name) || name == m_vm.propertyNames->underscoreProto) [[unlikely]]
        return { };

    Structure* from = members.structure;
    StringImpl& characters = *name.impl();
    bool is8Bit = characters.is8Bit();
    Structure* to = is8Bit ? m_cache.getTransition(from, characters.span8()) : m_cache.getTransition(from, characters.span16());
    bool isInTransitionCache = !!to;
    PropertyOffset offset = to ? to->transitionOffset() : invalidOffset;
    if (!to)
        to = Structure::addPropertyTransitionToExistingStructure(from, name, 0, offset);
    if (!to) {
        if (isValidOffset(from->get(m_vm, name)))
            return { };
        to = Structure::addNewPropertyTransition(m_vm, from, name, 0, offset);
    }
    if (to->isDictionary() || offset != memberPropertyOffset(members.count))
        return { };

    if (to->isCacheableTransitionFrom(from)) {
        if (from->trySingleTransition() == to)
            m_cache.addName(from, to, offset);
        else {
            if (!isInTransitionCache) {
                if (is8Bit)
                    m_cache.addTransition(from, to, characters.span8());
                else
                    m_cache.addTransition(from, to, characters.span16());
            }
            m_cache.addPrefixedName(from, to, offset);
        }
    }
    return { to, parsed.afterColon, JSONCache::nameIndex(StructureID::encode(to)) };
}

static ALWAYS_INLINE JSObject* createOrdinaryObject(VM& vm, Structure* structure, EncodedJSValue* values, unsigned count)
{
    unsigned inlineCapacity = structure->inlineCapacity();
    for (unsigned i = count; i < inlineCapacity; ++i)
        values[i] = JSValue::encode(JSValue());
    auto* inlineValues = reinterpret_cast<const WriteBarrierBase<Unknown>*>(values);
    unsigned outOfLineCapacity = structure->outOfLineCapacity();
    if (!outOfLineCapacity)
        return JSFinalObject::createWithButterflyCopyingInlineStorage(vm, structure, nullptr, inlineValues);

    DeferGC deferGC(vm);
    Butterfly* butterfly = Butterfly::create(vm, nullptr, 0, outOfLineCapacity, false, IndexingHeader(), 0);
    for (unsigned i = inlineCapacity; i < count; ++i)
        butterfly->propertyStorage()[offsetInOutOfLineStorage(offsetForPropertyNumber(i, inlineCapacity))].setWithoutWriteBarrier(JSValue::decode(values[i]));
    return JSFinalObject::createWithButterflyCopyingInlineStorage(vm, structure, butterfly, inlineValues);
}

JSObject* JSONValueParser::createObject(Members& members, ObjectKind kind)
{
    JSONRuntime& runtime = *m_words.runtime;
    if (kind == ObjectKind::RecordIfPlanned && runtime.hasParsedRecords()) {
        JSONRuntime::RecordPlan record = runtime.recordPlanFor(m_vm, members.structure);
        if (record.plan) {
            if (JSObject* object = ObjectBuilder::tryBuild(m_vm, *record.plan, record.structure, members.values))
                return object;
        }
    }

    return createOrdinaryObject(m_vm, members.structure, members.values, members.count);
}

JSObject* JSONValueParser::createObjectWithStructure(VM& vm, Structure* structure, EncodedJSValue* values, unsigned count)
{
    return createOrdinaryObject(vm, structure, values, count);
}

auto JSONValueParser::parseNameAndColon(const uint8_t* quote) -> ParsedName
{
    if (*quote != '"') [[unlikely]]
        return { };
    ParsedName parsed;
    Cursor start = quote + 1;
    Cursor end = findEndOfPlainJSONCharacters(start);
    if (*end == '"' && isLatin1(m_words, { start, end })) [[likely]] {
        parsed.name = Identifier::fromString(m_vm, m_cache.makeIdentifier(m_vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(end) }));
        ++end;
    } else {
        ParsedJSONString decoded = makeJSONStringWithEscapes(m_instance, m_vm, start, end);
        if (!decoded.end)
            return { };
        StringImpl* impl = decoded.string->tryGetValueImpl();
        if (!impl)
            return { };
        parsed.name = Identifier::fromString(m_vm, String { impl });
        end = decoded.end;
    }
    Cursor colon = skipWhitespace(end);
    if (*colon != ':') [[unlikely]]
        return { };
    parsed.afterColon = colon + 1;
    return parsed;
}

ParsedJSONValue JSONValueParser::parseRemainingMembers(JSObject* object, Cursor quote)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
    for (;;) {
        ParsedName member = parseNameAndColon(quote);
        if (!member.afterColon) [[unlikely]]
            return { };
        ParsedJSONValue parsed = parseValue(member.afterColon);
        if (!parsed.end) [[unlikely]]
            return { };
        if (std::optional<uint32_t> index = parseIndex(member.name)) {
            object->putDirectIndex(m_globalObject, *index, parsed.value);
            CLEAR_AND_RETURN_IF_EXCEPTION(scope, { });
        } else
            object->putDirect(m_vm, member.name, parsed.value);
        Cursor cursor = skipWhitespace(parsed.end);
        if (*cursor == ',') {
            quote = skipWhitespace(cursor + 1);
            continue;
        }
        if (*cursor != '}') [[unlikely]]
            return { };
        return { object, cursor + 1 };
    }
}

NEVER_INLINE ParsedJSONValue JSONValueParser::parseArray(Cursor bracket)
{
    if (!m_vm.isSafeToRecurse() || m_vm.traps().needsOutOfMemoryError()) [[unlikely]]
        return { };
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
    Cursor cursor = skipWhitespace(bracket + 1);
    JSArray* array = nullptr;
    if (*cursor == ']') {
        ++cursor;
        array = constructEmptyArray(m_globalObject, nullptr);
    } else {
        size_t base = m_elements.size();
        NamesOfElementInFront inFront;
        // The first object has no element in front. `["$", "td", null, { .. }]` has one object and no more: its names are looked for among those of the objects parsed last.
        bool hasObjectInFront = false;
        for (;;) {
            ParsedJSONValue parsed;
            if (*cursor != '{')
                parsed = parseValue(cursor);
            else if (hasObjectInFront) [[likely]]
                parsed = parseObject(cursor, &inFront);
            else {
                parsed = parseValue(cursor);
                hasObjectInFront = true;
            }
            if (!parsed.end) [[unlikely]]
                return { };
            m_elements.append(parsed.value);
            if (m_elements.hasOverflowed()) [[unlikely]]
                return { };
            cursor = skipWhitespace(parsed.end);
            if (*cursor == ',') {
                cursor = skipWhitespace(cursor + 1);
                continue;
            }
            if (*cursor != ']') [[unlikely]]
                return { };
            ++cursor;
            break;
        }
        array = createArray(m_elements.begin() + base, static_cast<unsigned>(m_elements.size() - base));
        m_elements.shrink(base);
    }
    CLEAR_AND_RETURN_IF_EXCEPTION(scope, { });
    if (!array) [[unlikely]]
        return { };
    return { array, cursor };
}

JSArray* JSONValueParser::createArray(const JSValue* values, unsigned length)
{
    IndexingType indexingType = ArrayWithInt32;
    for (unsigned i = 0; i < length; ++i) {
        if (values[i].isInt32())
            continue;
        if (!values[i].isDouble()) {
            indexingType = ArrayWithContiguous;
            break;
        }
        indexingType = ArrayWithDouble;
    }
    ObjectInitializationScope initializationScope(m_vm);
    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, m_globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), length);
    if (!array) [[unlikely]]
        return nullptr;
    IndexingType shape = array->indexingType() & IndexingShapeMask;
    if (shape == DoubleShape) {
        double* data = array->butterfly()->contiguousDouble().data();
        for (unsigned i = 0; i < length; ++i)
            data[i] = values[i].asNumber();
    } else if (shape == Int32Shape)
        memcpy(std::bit_cast<JSValue*>(array->butterfly()->contiguous().data()), values, length * sizeof(JSValue));
    else if (shape == ContiguousShape) {
        gcSafeMemcpy(std::bit_cast<JSValue*>(array->butterfly()->contiguous().data()), values, length * sizeof(JSValue));
        m_vm.writeBarrier(array);
    } else {
        for (unsigned i = 0; i < length; ++i)
            array->initializeIndex(initializationScope, i, values[i]);
    }
    return array;
}

ParsedJSONValue parseJSONValueAt(Instance& instance, VM& vm, JSGlobalObject* globalObject, const uint8_t* start)
{
    if (auto scalar = tryParseJSONScalarAt(instance, vm, start); scalar && scalar->end)
        return *scalar;
    return JSONValueParser(instance, vm, globalObject).parse(start);
}

template<typename Source>
static ALWAYS_INLINE JSValue tryParse(JSGlobalObject* globalObject, size_t length, Source source)
{
    Instance* instance = Instance::of(globalObject);
    if (!instance || !instance->json.runtime || !length || length > String::MaxLength)
        return { };
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSONSourceScope sourceScope(*instance, source);
    if (!sourceScope)
        return { };
    ParsedJSONValue parsed = JSONValueParser(*instance, vm, globalObject).parseWholeSource(sourceScope.start());
    CLEAR_AND_RETURN_IF_EXCEPTION(scope, { });
    return parsed.end ? parsed.value : JSValue();
}

JSValue tryParseJSON(JSGlobalObject* globalObject, StringView text)
{
    return tryParse(globalObject, text.length(), text);
}

JSValue tryParseUTF8JSON(JSGlobalObject* globalObject, std::span<const uint8_t> bytes)
{
    return tryParse(globalObject, bytes.size(), bytes);
}

static ALWAYS_INLINE UGPRPair toRegisterPair(ParsedJSONString parsed)
{
    return encodeResult(parsed.string, parsed.end);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONSourceOpen, size_t, (Instance* instance, EncodedJSValue text, void* stackPointer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONSourceOpen);
    OPERATION_RETURN(scope, instance->json.runtime->openInline(JSValue::decode(text), stackPointer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONSourceClose, size_t, (Instance* instance, const uint8_t* cursor))
{
    return instance->json.runtime->close(cursor);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONSourceAbandon, void, (Instance* instance))
{
    instance->json.runtime->abandon();
}

JSC_DEFINE_JIT_OPERATION(operationAOTJSONDecodeBytes, EncodedJSValue, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONDecodeBytes);
    OPERATION_RETURN(scope, JSValue::encode(decodeJSONBody(globalObject, JSValue::decode(value))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseString, UGPRPair, (Instance* instance, const uint8_t* cursor))
{
    VM& vm = *instance->vm;
    if (*cursor != '"') [[unlikely]]
        return toRegisterPair({ jsEmptyString(vm) });
    if (auto cached = findCachedJSONString(instance->json, vm, cursor)) [[likely]]
        return toRegisterPair(*cached);
    return toRegisterPair(makeUncachedJSONStringInFrame(*instance, AOT_OPERATION_FRAME_RECORD(), cursor + 1));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseKey, UGPRPair, (Instance* instance, const uint8_t* cursor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONParseKey);
    if (*cursor != '"') [[unlikely]]
        OPERATION_RETURN(scope, toRegisterPair({ jsEmptyString(vm) }));
    const uint8_t* start = cursor + 1;
    const uint8_t* end = findEndOfPlainJSONCharacters(start);
    if (*end != '"') [[unlikely]]
        OPERATION_RETURN(scope, toRegisterPair({ jsEmptyString(vm) }));
    if (!isLatin1(instance->json, { start, end })) [[unlikely]]
        OPERATION_RETURN(scope, toRegisterPair(makeJSONStringWithEscapes(*instance, vm, start, end)));
    JSString* string = vm.jsonCache().tryMakeJSString(vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(end) });
    if (!string) [[unlikely]]
        OPERATION_RETURN(scope, toRegisterPair({ jsEmptyString(vm) }));
    OPERATION_RETURN(scope, toRegisterPair({ string, end + 1 }));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseValue, UGPRPair, (Instance* instance, const uint8_t* cursor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONParseValue);
    DeferTerminationForAWhile deferTermination(vm);
    ParsedJSONValue parsed = parseJSONValueAt(*instance, vm, globalObject, cursor);
    OPERATION_RETURN(scope, makeUGPRPair(static_cast<UCPURegister>(JSValue::encode(parsed.value)), std::bit_cast<UCPURegister>(parsed.end)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseNumber, UGPRPair, (Instance* instance, const uint8_t* cursor))
{
    ParsedJSONNumber parsed = parseJSONNumber(cursor, instance->json.sourceEnd);
    return makeUGPRPair(std::bit_cast<uint64_t>(parsed.number), std::bit_cast<UCPURegister>(parsed.end));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseInteger, UGPRPair, (Instance*, const uint8_t* cursor))
{
    ParsedJSONInteger parsed = parseJSONInteger(cursor);
    return makeUGPRPair(static_cast<uint64_t>(parsed.integer), std::bit_cast<UCPURegister>(parsed.end));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONFindMember, UGPRPair, (Instance* instance, const uint8_t* cursor, uint32_t name))
{
    FoundJSONMember found = findJSONMember(instance->json, cursor, [&](StringView text) {
        return ProgramStrings::hasText(*instance->vm->aotState().image, StringID(name), text);
    });
    return makeUGPRPair(static_cast<UCPURegister>(found.search), std::bit_cast<UCPURegister>(found.value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONMemberRepeats, size_t, (Instance* instance, const uint8_t* valueStart, uint32_t name))
{
    return jsonMemberRepeats(instance->json, skipJSONValue(valueStart), [&](StringView text) {
        return ProgramStrings::hasText(*instance->vm->aotState().image, StringID(name), text);
    });
}

struct ExtraMembers {
    Structure* structure() const { return properties ? properties->structure() : instance.realm.emptyExtraPropertiesStructure; }
    JSObject* ensureProperties(VM& vm)
    {
        if (!properties)
            setProperties(constructEmptyObject(vm, instance.realm.emptyExtraPropertiesStructure));
        return properties;
    }
    void setProperties(JSObject* created)
    {
        properties = created;
        encodedProperties = JSValue::encode(created);
    }

    Instance& instance;
    const LayoutData& layout;
    uint64_t& encodedProperties;
    JSObject* properties;
    std::optional<unsigned> unpositionedOrdinal { };
};

enum class ExtraMemberStatus : uint8_t { Unknown, Declared, Failed, Added };

struct ParsedExtraMember {
    ExtraMemberStatus status { ExtraMemberStatus::Unknown };
    const uint8_t* valueStart { nullptr };
    const uint8_t* end { nullptr };
};

uint32_t JSONKeyOrderLists::add(std::span<const uint16_t> entries)
{
    uint32_t list = m_lists.size();
    m_lists.append(List(entries));
    return list;
}

static const LayoutData& layoutOf(Instance& instance, uint32_t shape)
{
    return instance.state->objects->layouts.layoutForShape(shape);
}

static std::span<const uint16_t> entriesOf(Instance& instance, const LayoutData& layout, JSONKeyOrder order)
{
    switch (order.kind()) {
    case JSONKeyOrder::Kind::Numbered:
        return instance.state->objects->keyOrders.entriesInOrder(layout, orderOf(order.orderAndCount())).first(countOf(order.orderAndCount()));
    case JSONKeyOrder::Kind::Listed:
        return instance.json.runtime->keyOrderLists().entries(order.list()).first(order.countInList());
    case JSONKeyOrder::Kind::Unknown:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSONKeyOrder advanceInList(JSONKeyOrderLists& lists, JSONKeyOrder order, uint16_t entry)
{
    uint32_t list = order.list();
    uint32_t count = order.countInList();
    std::span<const uint16_t> entries = lists.entries(list);
    if (count > entries.size() || count == JSONKeyOrder::maxCountInList) [[unlikely]]
        return JSONKeyOrder::unknown();
    if (count < entries.size() && entries[count] == entry)
        return JSONKeyOrder::listed(list, count + 1);
    if (count < entries.size())
        list = lists.add(entries.first(count));
    lists.append(list, entry);
    return JSONKeyOrder::listed(list, count + 1);
}

static JSONKeyOrder advance(Instance& instance, const LayoutData& layout, JSONKeyOrder order, uint16_t entry)
{
    if (layout.keysAreNeverEnumerated)
        return order;
    JSONKeyOrderLists& lists = instance.json.runtime->keyOrderLists();
    switch (order.kind()) {
    case JSONKeyOrder::Kind::Unknown:
        return order;
    case JSONKeyOrder::Kind::Listed:
        return advanceInList(lists, order, entry);
    case JSONKeyOrder::Kind::Numbered:
        break;
    }
    if (std::optional<uint32_t> next = instance.state->objects->keyOrders.orderAfterCreating(layout, order.orderAndCount(), entry)) [[likely]]
        return JSONKeyOrder::numbered(*next);
    uint32_t list = lists.add(entriesOf(instance, layout, order));
    lists.append(list, entry);
    return JSONKeyOrder::listed(list, countOf(order.orderAndCount()) + 1);
}

static ALWAYS_INLINE bool tryFinishObjectAsBefore(VM& vm, Instance& instance, Object& object, JSObject* extraProperties, uint64_t order)
{
    const JSONRuntime::FinishedObject& finished = instance.json.runtime->finishedObjectFor(object.structure(), extraProperties->structure());
    if (finished.structureAtBirth != object.structure() || finished.extraStructure != extraProperties->structure() || finished.order != order || finished.orderWordAtBirth != object.orderWord())
        return false;
    object.setInitialKeyOrder(vm, { finished.orderAndCount, finished.structure }, extraProperties);
    return true;
}

void JSONRuntime::ObservedMember::recordListStorage(unsigned level, ListStorage storage)
{
    ListStorage before = listStorageAt(listStorageKinds, level);
    if (before != Unknown && before <= storage)
        return;
    listStorageKinds &= ~(((1 << bitsPerListStorage) - 1) << (level * bitsPerListStorage));
    listStorageKinds |= storage << (level * bitsPerListStorage);
}

static void recordObservedMemberValue(JSONRuntime& runtime, uint32_t shape, uint16_t field, JSCell* cell)
{
    using Observed = JSONRuntime::ObservedMember;
    AOT_THRESHOLD(unsigned, maxElementsInspected, 16);
    Observed observed = runtime.observedMember(shape, field);
    for (unsigned level = 0; cell->type() == ArrayType; ++level) {
        auto* list = uncheckedDowncast<JSArray>(cell);
        IndexingType storage = list->indexingType() & IndexingShapeMask;
        if (level == Observed::maxListDepth || (storage != Int32Shape && storage != DoubleShape && storage != ContiguousShape))
            return;
        observed.recordListStorage(level, storage == Int32Shape ? Observed::Int32 : storage == DoubleShape ? Observed::Double : Observed::Contiguous);
        cell = nullptr;
        for (unsigned index = 0; storage == ContiguousShape && !cell && index < std::min(list->length(), maxElementsInspected); ++index) {
            JSValue element = list->butterfly()->contiguous().at(list, index).get();
            cell = element && element.isObject() ? element.asCell() : nullptr;
        }
        if (!cell) {
            runtime.didSeeMember(shape, field, observed);
            return;
        }
    }
    if (isCompiledObject(cell))
        observed.shape = uncheckedDowncast<Object>(cell)->shape();
    else if (cell->isObject() && !observed.shape)
        observed.shape = JSONRuntime::LearnedObject::plainObjectShape;
    runtime.didSeeMember(shape, field, observed);
}

static bool tryFinishObject(VM& vm, Instance& instance, Object& object, JSObject* extraProperties, JSONKeyOrder order)
{
    if (order.kind() == JSONKeyOrder::Kind::Unknown)
        return false;
    if (order.kind() == JSONKeyOrder::Kind::Numbered && !extraProperties) {
        object.setOrderAndCount(order.orderAndCount());
        return true;
    }
    JSONRuntime::FinishedObject born { object.structure(), extraProperties ? extraProperties->structure() : nullptr, order.word(), object.orderWord() };
    Object::KeyOrderStorage keys(entriesOf(instance, object.layout(), order));
    std::optional<Object::InitialKeyOrder> initial = object.initialKeyOrder(vm, keys.span(), extraProperties);
    if (!initial)
        return false;
    object.setInitialKeyOrder(vm, *initial, extraProperties);
    if (extraProperties) {
        const LayoutData& layout = object.layout();
        for (const FieldData& field : layout.fields()) {
            JSValue value = field.isPrivate() || field.isAccessor() ? JSValue() : object.readInline(field);
            if (value && value.isObject())
                recordObservedMemberValue(*instance.json.runtime, layout.shape, layout.indexOf(field), value.asCell());
        }
    }
    // A list of keys lasts as long as its text; a number lasts.
    bool isCacheable = extraProperties && !instance.json.runtime->consumeUncachedFinish();
    if (isCacheable && order.kind() == JSONKeyOrder::Kind::Numbered) {
        born.orderAndCount = initial->orderAndCount;
        born.structure = initial->structure;
        instance.json.runtime->finishedObjectFor(born.structureAtBirth, born.extraStructure) = born;
    }
    return true;
}

static ALWAYS_INLINE unsigned propertyCountOf(Structure* structure)
{
    return numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity());
}

static ALWAYS_INLINE bool canPositionExtraKey(const LayoutData& layout, unsigned ordinal)
{
    return layout.numberOfFields + ordinal < maxKeyOrderEntry;
}

static ALWAYS_INLINE void didAddExtraKey(ExtraMembers& members, unsigned ordinal)
{
    members.unpositionedOrdinal = ordinal;
}

bool JSONRuntime::LearnedName::trySetText(const uint8_t* quote, size_t length)
{
    if (length > maxTextLength)
        return false;
    uint64_t everyByte = 0;
    for (unsigned word = 0; word < wordsOfText; ++word) {
        size_t start = word * sizeof(uint64_t);
        size_t bytes = std::min(length - std::min(length, start), sizeof(uint64_t));
        mask[word] = bytes == sizeof(uint64_t) ? std::numeric_limits<uint64_t>::max() : (1ull << (8 * bytes)) - 1;
        text[word] = WTF::unalignedLoad<uint64_t>(quote + start) & mask[word];
        everyByte |= text[word];
    }
    textLength = length;
    return !(everyByte & 0x8080808080808080ULL);
}

void JSONRuntime::LearnedName::setTextThatStandsNowhere()
{
    text[0] = 0;
    mask[0] = std::numeric_limits<uint64_t>::max();
    text[1] = 0;
    mask[1] = 0;
    textLength = 0;
}

void JSONRuntime::LearnedExtraMembers::begin(uint64_t keyOfMembers, unsigned outOfLineCapacityInFront)
{
    key = keyOfMembers;
    orderBefore = JSONKeyOrder::unknown().word();
    outOfLineCapacityBefore = outOfLineCapacityInFront;
    count = 0;
    wasFound = false;
    isOnlySeen = false;
    names[0].setTextThatStandsNowhere();
}

void JSONRuntime::LearnedExtraMembers::note(unsigned index, const uint8_t* quote, size_t length, Structure* structure)
{
    if (index != count || index == capacity)
        return;
    Name name;
    if (!name.trySetText(quote, length))
        return;
    name.structure = structure;
    name.offset = structure->transitionOffset();
    name.outOfLineCapacity = structure->outOfLineCapacity();
    names[index] = name;
    count = index + 1;
}

void JSONRuntime::LearnedObject::forgetCells()
{
    shape = 0;
    structureAtBirth = nullptr;
    extraStructure = nullptr;
    structure = nullptr;
    if (plan)
        plan->clearAcceptedStructures();
}

bool JSONRuntime::mayLearnAnotherObject()
{
    if (m_numberOfObjectsLearned == maxObjectsLearnedBetweenCollections)
        return false;
    ++m_numberOfObjectsLearned;
    return true;
}

JSONRuntime::LearnedObject* JSONRuntime::learnedObjectToReplace(uint32_t shape, uint64_t firstWord)
{
    if (!m_learnedObjects)
        m_learnedObjects.make();
    unsigned index = indexOfLearnedObject(shape, firstWord);
    LearnedObject& first = (*m_learnedObjects)[index];
    LearnedObject& second = (*m_learnedObjects)[index ^ 1];
    if (first.isBeingLearned)
        return second.isBeingLearned ? nullptr : &second;
    if (second.isBeingLearned)
        return &first;
    bool takesSecond = first.shape && (!second.shape || (std::exchange(first.wasFound, false) && !std::exchange(second.wasFound, false)));
    return takesSecond ? &second : &first;
}

void JSONRuntime::didSeeExtraMembers(uint32_t shape)
{
    unsigned bit = shape % numberOfJSONShapeBits;
    if (m_timesRefusedInARow[bit] != maxTimesRefusedInARow)
        m_instance.json.shapesSeenWithExtraMembers[bit / 32] |= 1u << (bit % 32);
}

void JSONRuntime::didRefuseTextOf(uint32_t shape)
{
    unsigned bit = shape % numberOfJSONShapeBits;
    if (++m_timesRefusedInARow[bit] == maxTimesRefusedInARow)
        m_instance.json.shapesSeenWithExtraMembers[bit / 32] &= ~(1u << (bit % 32));
}

void JSONRuntime::didNotFindTextOf(uint32_t shape)
{
    unsigned bit = shape % numberOfJSONShapeBits;
    if (++m_timesNotFoundInARow[bit] == maxTimesNotFoundInARow)
        m_instance.json.shapesSeenWithExtraMembers[bit / 32] &= ~(1u << (bit % 32));
}

JSONRuntime::LearnedExtraMembers* JSONRuntime::learnedExtraMembersWithoutNames(uint64_t key, const uint8_t* firstName)
{
    if (!m_learnedExtraMembers)
        return nullptr;
    unsigned index = indexOfLearnedExtraMembers(key, firstName);
    uint64_t firstWord = JSONRuntime::startOfName(firstName);
    for (unsigned way = 0; way < 2; ++way) {
        LearnedExtraMembers& learned = (*m_learnedExtraMembers)[index ^ way];
        if (learned.holdsNoNamesOf(key, firstWord))
            return &learned;
    }
    return nullptr;
}

JSONRuntime::LearnedExtraMembers& JSONRuntime::learnedExtraMembersToReplace(uint64_t key, const uint8_t* firstName)
{
    if (!m_learnedExtraMembers)
        m_learnedExtraMembers.make();
    unsigned index = indexOfLearnedExtraMembers(key, firstName);
    LearnedExtraMembers& first = (*m_learnedExtraMembers)[index];
    LearnedExtraMembers& second = (*m_learnedExtraMembers)[index ^ 1];
    bool takesSecond = std::exchange(first.wasFound, false) && !std::exchange(second.wasFound, false);
    return takesSecond ? second : first;
}

static NEVER_INLINE uint64_t positionExtraKeys(Instance& instance, uint32_t shape, JSONRuntime::LearnedExtraMembers* learned, uint64_t orderBefore, unsigned ordinal)
{
    const LayoutData& layout = layoutOf(instance, shape);
    JSONKeyOrder before = JSONKeyOrder::fromWord(orderBefore);
    JSONKeyOrder after = advance(instance, layout, before, static_cast<uint16_t>(layout.numberOfFields + ordinal));
    if (learned && before.kind() == JSONKeyOrder::Kind::Numbered && after.kind() == JSONKeyOrder::Kind::Numbered) {
        learned->orderBefore = orderBefore;
        learned->orderAfter = after.word();
        learned->positionedOrdinal = ordinal;
    }
    return after.word();
}

static void cacheTransitionName(JSONCache& cache, Structure* before, Structure* after)
{
    if (before->isDictionary() || after->isDictionary() || !after->isCacheableTransitionFrom(before))
        return;
    if (before->trySingleTransition() == after)
        cache.addName(before, after, after->transitionOffset());
    else
        cache.addPrefixedName(before, after, after->transitionOffset());
}

static bool addExtraProperty(VM& vm, ExtraMembers& members, UniquedStringImpl* name, JSValue value)
{
    JSObject* properties = members.ensureProperties(vm);
    if (auto index = parseIndex(*name)) [[unlikely]] {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        properties->putDirectIndex(members.instance.globalObject, *index, value);
        if (catchScope.exception()) [[unlikely]] {
            catchScope.clearException();
            return false;
        }
        return true;
    }
    Structure* before = WTF::opaque(properties->structure());
    unsigned attributes;
    if (PropertyOffset offset = before->get(vm, name, attributes); isValidOffset(offset)) {
        properties->putDirectOffset(vm, offset, value);
        return true;
    }
    unsigned ordinal = propertyCountOf(before);
    if (!canPositionExtraKey(members.layout, ordinal)) [[unlikely]]
        return false;
    properties->putDirect(vm, name, value);
    cacheTransitionName(vm.jsonCache(), before, properties->structure());
    didAddExtraKey(members, ordinal);
    return true;
}

static ParsedExtraMember parseKnownExtraMember(VM& vm, ExtraMembers& members, JSONValueParser& parser, const uint8_t* quote)
{
    const JSONWords& words = members.instance.json;
    Structure* before = members.structure();
    StructureID beforeID = before->id();
    auto* entry = vm.jsonCache().findName(beforeID, JSONCache::nameIndex(beforeID), std::span<const Latin1Character> { byteCast<Latin1Character>(quote), jsonSourcePadding });
    if (!entry || (words.sourceEncoding != JSONSourceEncoding::Latin1 && !entry->textIsASCII && !isShortTextLatin1AfterDecoding(words, quote, entry->textLength)))
        return { };
    // The cache is weak and no object need have this Structure: this pointer alone keeps it alive while the value is parsed.
    Structure* after = WTF::opaque(entry->to.unvalidatedGet());
    PropertyOffset offset = entry->offset;
    const uint8_t* valueStart = quote + entry->textLength;
    if (members.instance.state->objects->layouts.field(vm, members.layout, after->transitionPropertyName()))
        return { };
    unsigned ordinal = propertyCountOf(before);
    if (!canPositionExtraKey(members.layout, ordinal)) [[unlikely]]
        return { ExtraMemberStatus::Failed };
    ParsedJSONValue parsed = parser.parse(valueStart);
    if (!parsed.end) [[unlikely]]
        return { ExtraMemberStatus::Failed };
    JSObject* properties = members.ensureProperties(vm);
    if (offset >= firstOutOfLineOffset && before->outOfLineCapacity() != after->outOfLineCapacity()) [[unlikely]]
        properties->putDirect(vm, after->transitionPropertyName(), parsed.value);
    else {
        properties->putDirectOffset(vm, offset, parsed.value);
        properties->setStructure(vm, after);
    }
    didAddExtraKey(members, ordinal);
    return { ExtraMemberStatus::Added, valueStart, parsed.end };
}

static NEVER_INLINE ParsedExtraMember parseNewExtraMember(VM& vm, ExtraMembers& members, JSONValueParser& parser, const uint8_t* quote)
{
    JSONValueParser::ParsedName key = parser.parseNameAndColon(quote);
    if (!key.afterColon)
        return { ExtraMemberStatus::Failed };
    if (members.instance.state->objects->layouts.field(vm, members.layout, key.name.impl()))
        return { ExtraMemberStatus::Declared };
    ParsedJSONValue parsed = parser.parse(key.afterColon);
    if (!parsed.end || !addExtraProperty(vm, members, key.name.impl(), parsed.value))
        return { ExtraMemberStatus::Failed };
    return { ExtraMemberStatus::Added, key.afterColon, parsed.end };
}

static bool addsOneSlot(Structure* before, Structure* after)
{
    return after->previousID() == before && !after->isDictionary() && !hasIndexedProperties(after->indexingType()) && propertyCountOf(after) == propertyCountOf(before) + 1;
}

static uint64_t keyOfLearnedExtraMembers(const ExtraMembers& members, uint32_t shape)
{
    return JSONRuntime::LearnedExtraMembers::keyFor(shape, members.properties ? members.properties->structureID() : StructureID());
}

static JSONRuntime::LearnedExtraMembers& beginLearning(const ExtraMembers& members, JSONRuntime::LearnedExtraMembers& learned, uint32_t shape)
{
    learned.begin(keyOfLearnedExtraMembers(members, shape), members.properties ? members.properties->structure()->outOfLineCapacity() : 0);
    return learned;
}

static const uint8_t* parseExtraMembers(VM& vm, ExtraMembers& members, JSONRuntime::LearnedExtraMembers* learned, uint32_t shape, const uint8_t* quote)
{
    JSONValueParser parser(members.instance, vm, members.instance.globalObject);
    Structure* before = members.structure();
    for (unsigned index = 0;; ++index) {
        ParsedExtraMember member = parseKnownExtraMember(vm, members, parser, quote);
        if (member.status == ExtraMemberStatus::Unknown) {
            if (index && !learned)
                return quote;
            member = parseNewExtraMember(vm, members, parser, quote);
        }
        if (member.status == ExtraMemberStatus::Declared)
            return quote;
        if (member.status == ExtraMemberStatus::Failed)
            return nullptr;
        Structure* after = members.structure();
        if (learned && addsOneSlot(before, after))
            learned->note(index, quote, member.valueStart - quote, after);
        before = after;
        const uint8_t* separator = skipWhitespace(member.end);
        if (*separator == '}')
            return separator;
        if (*separator != ',')
            return nullptr;
        quote = skipWhitespace(separator + 1);
        if (*quote != '"')
            return nullptr;
        if (learned && learned->count == JSONRuntime::LearnedExtraMembers::capacity && index + 1 == learned->count) {
            learned = &beginLearning(members, members.instance.json.runtime->learnedExtraMembersToReplace(keyOfLearnedExtraMembers(members, shape), quote), shape);
            index = std::numeric_limits<unsigned>::max();
        }
    }
}

static NEVER_INLINE const uint8_t* parseAndLearnExtraMembers(Instance* instance, VM& vm, uint32_t shape, const uint8_t* cursor, uint64_t* extraAndOrder)
{
    DeferTerminationForAWhile deferTermination(vm);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue extra = JSValue::decode(extraAndOrder[0]);
    JSObject* properties = extra.isCell() ? dynamicDowncast<JSFinalObject>(extra.asCell()) : nullptr;
    const uint8_t* next = nullptr;
    if (JSONKeyOrder::fromWord(extraAndOrder[1]).kind() != JSONKeyOrder::Kind::Unknown && extra.isCell() == !!properties) {
        JSONRuntime& runtime = *instance->json.runtime;
        ExtraMembers members { *instance, layoutOf(*instance, shape), extraAndOrder[0], properties };
        JSONRuntime::LearnedExtraMembers* learned = nullptr;
        bool learnsNames = false;
        if (runtime.remembersTextsOf(shape)) {
            uint64_t key = keyOfLearnedExtraMembers(members, shape);
            JSONRuntime::LearnedExtraMembers* seenBefore = runtime.learnedExtraMembersWithoutNames(key, cursor);
            learnsNames = seenBefore && seenBefore->isOnlySeen;
            learned = seenBefore ? seenBefore : &runtime.learnedExtraMembersToReplace(key, cursor);
            if (!seenBefore || learnsNames) {
                beginLearning(members, *learned, shape);
                learned->firstWord = JSONRuntime::startOfName(cursor);
                learned->isOnlySeen = !seenBefore;
            }
        }
        next = parseExtraMembers(vm, members, learnsNames ? learned : nullptr, shape, cursor);
        if (learned && members.properties) {
            runtime.didSeeExtraMembers(shape);
            runtime.didNotFindTextOf(shape);
        }
        if (next && members.unpositionedOrdinal)
            extraAndOrder[1] = positionExtraKeys(*instance, shape, learned, extraAndOrder[1], *members.unpositionedOrdinal);
    }
    if (catchScope.exception()) [[unlikely]] {
        catchScope.clearException();
        next = nullptr;
    }
    if (!next)
        count(*instance, RuntimeCounterID::TypedParsesFellBackPerValue);
    return next;
}

static ParsedJSONValue parseNestedLearnedValue(Instance&, VM&, const JSONRuntime::LearnedObject&, JSONRuntime::LearnedObject::Member&, const uint8_t* start);

static ALWAYS_INLINE ParsedJSONValue parseExtraValue(Instance& instance, VM& vm, const uint8_t* start, const JSONRuntime::LearnedObject* learned = nullptr, JSONRuntime::LearnedObject::Member* member = nullptr)
{
    JSValue integer;
    if (const uint8_t* end = tryParseSmallJSONInteger(start, integer))
        return { integer, end };
    if (*start == '"') {
        if (auto cached = findCachedJSONString(instance.json, vm, start)) [[likely]]
            return { cached->string, cached->end };
    } else if (WTF::unalignedLoad<uint32_t>(start) == ('t' | 'r' << 8 | 'u' << 16 | 'e' << 24))
        return { jsBoolean(true), start + 4 };
    else if (WTF::unalignedLoad<uint32_t>(start + 1) == ('a' | 'l' << 8 | 's' << 16 | 'e' << 24) && *start == 'f')
        return { jsBoolean(false), start + 5 };
    // What the general parser makes of an object or a list is none of the field's type.
    if (member && member->shapeOfValue && (*start == '{' || *start == '['))
        return parseNestedLearnedValue(instance, vm, *learned, *member, start);
    return parseJSONValueAt(instance, vm, instance.globalObject, start);
}

static ALWAYS_INLINE const uint8_t* tryParseLearnedExtraMembers(Instance& instance, VM& vm, uint32_t shape, const uint8_t* quote, uint64_t* extraAndOrder)
{
    using Learned = JSONRuntime::LearnedExtraMembers;
    static_assert(extraPropertiesInlineCapacity == 2 && Learned::capacity >= extraPropertiesInlineCapacity);
    JSONRuntime& runtime = *instance.json.runtime;
    JSValue extra = JSValue::decode(extraAndOrder[0]);
    JSObject* properties = extra.isUndefined() ? nullptr : asObject(extra);
    Learned* first = runtime.findLearnedExtraMembers(Learned::keyFor(shape, properties ? properties->structureID() : StructureID()), quote);
    if (!first) [[unlikely]]
        return nullptr;
    first->wasFound = true;
    runtime.didFindTextOf(shape);
    const uint8_t* reached = nullptr;
    PropertyOffset lastOffset = invalidOffset;
    unsigned outOfLineCapacity = first->outOfLineCapacityBefore;
    for (Learned* learned = first; learned;) {
        EncodedJSValue values[Learned::capacity];
        PropertyOffset offsets[Learned::capacity];
        unsigned count = 0;
        unsigned outOfLineCapacityBefore = outOfLineCapacity;
        Structure* after;
        bool hasMore;
        do {
            const Learned::Name& name = learned->names[count];
            after = name.structure;
            outOfLineCapacity = name.outOfLineCapacity;
            offsets[count] = name.offset;
            ParsedJSONValue parsed = parseExtraValue(instance, vm, skipWhitespace(quote + name.textLength));
            if (!parsed.end) [[unlikely]]
                goto done;
            values[count++] = JSValue::encode(parsed.value);
            quote = skipWhitespace(parsed.end);
            hasMore = *quote == ',';
            if (!hasMore) {
                if (*quote != '}') [[unlikely]]
                    goto done;
                break;
            }
            quote = skipWhitespace(quote + 1);
            if (*quote != '"') [[unlikely]]
                goto done;
        } while (count < learned->count && learned->names[count].standsAt(quote));

        if (!properties) {
            if (count == 1)
                values[1] = JSValue::encode(JSValue());
            properties = outOfLineCapacity ? JSONValueParser::createObjectWithStructure(vm, after, values, count) : JSFinalObject::createWithButterflyCopyingInlineStorage(vm, after, nullptr, reinterpret_cast<const WriteBarrierBase<Unknown>*>(values));
            extraAndOrder[0] = JSValue::encode(properties);
        } else {
            if (outOfLineCapacityBefore != outOfLineCapacity)
                properties->nukeStructureAndSetButterfly(vm, properties->structureID(), properties->allocateMoreOutOfLineStorage(vm, outOfLineCapacityBefore, outOfLineCapacity));
            for (unsigned i = 0; i < count; ++i)
                properties->putDirectWithoutBarrier(offsets[i], JSValue::decode(values[i]));
            properties->setStructure(vm, after);
            vm.writeBarrier(properties);
        }
        reached = quote;
        lastOffset = offsets[count - 1];
        learned = hasMore && count == Learned::capacity ? runtime.findLearnedExtraMembers(Learned::keyFor(shape, after->id()), quote) : nullptr;
    }
done:
    if (!reached) [[unlikely]]
        return nullptr;
    unsigned ordinal = isInlineOffset(lastOffset) ? lastOffset : lastOffset - firstOutOfLineOffset + extraPropertiesInlineCapacity;
    uint64_t order = extraAndOrder[1];
    extraAndOrder[1] = first->orderBefore == order && first->positionedOrdinal == ordinal ? first->orderAfter : positionExtraKeys(instance, shape, first, order, ordinal);
    return reached;
}

static IndexingType indexingTypeForListStorage(JSONRuntime::ObservedMember::ListStorage storage)
{
    using Observed = JSONRuntime::ObservedMember;
    return storage == Observed::Int32 ? ArrayWithInt32 : storage == Observed::Double ? ArrayWithDouble : ArrayWithContiguous;
}

enum class LearnedContext : uint8_t { Member, ListElement };
static bool learnObject(Instance&, VM&, JSONRuntime::LearnedObject&, uint32_t shape, const uint8_t* firstName, unsigned depth, LearnedContext = LearnedContext::Member);

static std::optional<JSONRuntime::LearnedObject::Store> storeOf(ObjectBuilderPlan::FastField field, uint8_t kind)
{
    using Learned = JSONRuntime::LearnedObject;
    using Store = Learned::Store;
    SlotRepresentation representation = field.representation;
    switch (kind) {
    case Learned::String:
        if (!(field.acceptedValues & ObjectBuilderPlan::AcceptsString))
            return std::nullopt;
        if (representation == SlotRepresentation::JSValue || representation == SlotRepresentation::Cell)
            return Store::Word;
        if (representation == SlotRepresentation::Cell32 || representation == SlotRepresentation::OptionalCell32)
            return Store::Cell32;
        return std::nullopt;
    case Learned::SmallInteger:
        if (!(field.acceptedValues & ObjectBuilderPlan::AcceptsInt32))
            return std::nullopt;
        if (representation == SlotRepresentation::JSValue)
            return Store::Word;
        if (representation == SlotRepresentation::Double || representation == SlotRepresentation::OptionalDouble)
            return Store::Int32AsDouble;
        if (representation == SlotRepresentation::Int32)
            return Store::Int32;
        return std::nullopt;
    case Learned::Boolean:
        if (!(field.acceptedValues & ObjectBuilderPlan::AcceptsBoolean))
            return std::nullopt;
        if (representation == SlotRepresentation::JSValue)
            return Store::Word;
        if (representation == SlotRepresentation::Boolean)
            return Store::Boolean;
        if (representation == SlotRepresentation::OptionalBoolean)
            return Store::LowByte;
        return std::nullopt;
    case Learned::List:
        if (representation == SlotRepresentation::JSValue || representation == SlotRepresentation::Cell)
            return Store::Word;
        if (representation == SlotRepresentation::Cell32 || representation == SlotRepresentation::OptionalCell32)
            return Store::Cell32;
        return std::nullopt;
    case Learned::OtherKind:
        break;
    }
    return std::nullopt;
}

static bool learnObjectInside(Instance& instance, VM& vm, uint32_t shape, const uint8_t* brace, unsigned depth, LearnedContext context)
{
    AOT_THRESHOLD(unsigned, maxDepthOfLearnedObjects, 8);
    const uint8_t* firstName = skipWhitespace(brace + 1);
    if (!shape || depth == maxDepthOfLearnedObjects || *firstName != '"')
        return false;
    JSONRuntime& runtime = *instance.json.runtime;
    uint64_t firstWord = JSONRuntime::startOfName(firstName);
    if (context == LearnedContext::ListElement) {
        unsigned index = JSONRuntime::indexOfLearnedObject(shape, firstWord);
        for (unsigned way = 0; way < 2; ++way) {
            const JSONRuntime::LearnedObject& known = (*runtime.learnedObjects())[index ^ way];
            if (known.isFor(shape, firstWord) && (known.isBeingLearned || (!known.members.isEmpty() && known.numberOfExtraMembers)))
                return true;
        }
    }
    JSONRuntime::LearnedObject* inner = runtime.learnedObjectToReplace(shape, firstWord);
    return inner && learnObject(instance, vm, *inner, shape, firstName, depth + 1, context);
}

static bool learnNestedValue(Instance& instance, VM& vm, JSONRuntime::LearnedObject::Member& member, uint32_t shape, const uint8_t* value, unsigned depth)
{
    using Observed = JSONRuntime::ObservedMember;
    JSONRuntime& runtime = *instance.json.runtime;
    Observed observed = runtime.observedMember(shape, member.field);
    unsigned level = 0;
    bool isObserved = true;
    for (; *value == '['; ++level) {
        if (level == Observed::maxListDepth)
            return false;
        isObserved &= Observed::listStorageAt(observed.listStorageKinds, level) != Observed::Unknown;
        value = skipWhitespace(value + 1);
    }
    if (!isObserved || (*value == '{' && !observed.shape)) {
        runtime.disableFinishCache();
        runtime.setWaitsForShapeOfMember(true);
        return false;
    }
    if (observed.shape)
        member.shapeOfValue = observed.shape;
    if (level)
        member.listStorageKinds = observed.listStorageKinds;
    return *value != '{' || !member.hasRecordShape() || learnObjectInside(instance, vm, observed.shape, value, depth, level ? LearnedContext::ListElement : LearnedContext::Member);
}

static NEVER_INLINE bool learnObject(Instance& instance, VM& vm, JSONRuntime::LearnedObject& learned, uint32_t shape, const uint8_t* firstName, unsigned depth, LearnedContext context)
{
    using Learned = JSONRuntime::LearnedObject;
    learned.shape = shape;
    learned.firstWord = JSONRuntime::startOfName(firstName);
    learned.structure = nullptr;
    learned.wasFound = false;
    learned.isOnlySeen = false;
    learned.consecutiveFailures = 0;
    learned.members.shrink(0);
    learned.keys.shrink(0);
    learned.stores.shrink(0);
    learned.wordsBehindHeader.shrink(0);
    learned.isBeingLearned = true;
    bool isLearned = false;
    if (!depth)
        instance.json.runtime->setWaitsForShapeOfMember(false);
    auto endLearning = makeScopeExit([&] {
        learned.isBeingLearned = false;
        if (isLearned)
            return;
        learned.members.shrink(0);
        learned.keys.shrink(0);
        learned.stores.shrink(0);
        learned.plan = nullptr;
        learned.isOnlySeen = !depth && instance.json.runtime->waitsForShapeOfMember();
        // The object around this one is refused. Another text that begins with the same name may still be learned.
        if (depth)
            learned.shape = 0;
    });

    const LayoutData& layout = layoutOf(instance, shape);
    RuntimeLayoutTable& layouts = instance.state->objects->layouts;
    JSONValueParser parser(instance, vm, instance.globalObject);
    Vector<Identifier, 16> names;
    Vector<UniquedStringImpl*, 16> fieldNames;
    Vector<Learned::Kind, 16> kindsOfFields;
    Vector<uint32_t, 16> listStructureIDs;
    Vector<UniquedStringImpl*, 16> extraNames;
    Vector<Learned::Member, 16> members;
    size_t numberOfPositionedExtraNames = 0;
    size_t numberOfRuns = 0;
    size_t numberOfFieldsInFront = 0;
    auto positionExtraNames = [&] {
        if (extraNames.size() == numberOfPositionedExtraNames)
            return;
        numberOfPositionedExtraNames = extraNames.size();
        ++numberOfRuns;
        learned.keys.append(static_cast<uint16_t>(layout.numberOfFields + extraNames.size() - 1));
    };
    for (const uint8_t* quote = firstName;;) {
        JSONValueParser::ParsedName key = parser.parseNameAndColon(quote);
        Learned::Member member;
        if (!key.afterColon || !member.trySetText(quote, key.afterColon - quote))
            return false;
        UniquedStringImpl* name = key.name.impl();
        const uint8_t* value = skipWhitespace(key.afterColon);
        member.shapeOfValue = 0;
        member.listStorageKinds = 0;
        member.field = 0;
        if (const FieldData* field = layouts.field(vm, layout, name)) {
            JSValue integer;
            kindsOfFields.append(*value == '"' ? Learned::String : *value == 't' || *value == 'f' ? Learned::Boolean : tryParseSmallJSONInteger(value, integer) ? Learned::SmallInteger : Learned::OtherKind);
            listStructureIDs.append(0);
            if (fieldNames.contains(name) || fieldNames.size() == Learned::maxNumberOfFields)
                return false;
            if (!layouts.typeOf(*field).acceptsEveryObject()) {
                member.shapeOfValue = Learned::shapeOfNoObject;
                member.field = layout.indexOf(*field);
                if (*value == '[' && !depth && extraNames.isEmpty())
                    return false;
                if ((*value == '{' || *value == '[') && !learnNestedValue(instance, vm, member, shape, value, depth))
                    return false;
                if (member.listStorageKinds && !instance.globalObject->isHavingABadTime()) {
                    Structure* listStructure = instance.globalObject->originalArrayStructureForIndexingType(indexingTypeForListStorage(JSONRuntime::ObservedMember::listStorageAt(member.listStorageKinds, 0)));
                    JSArray* list = tryCreateArrayWithStorage(vm, listStructure, 0, arrayLiteralVectorLength(0));
                    if (list && layouts.slotAdmits(*field, list)) {
                        kindsOfFields.last() = Learned::List;
                        listStructureIDs.last() = listStructure->id().bits();
                    }
                }
            }
            positionExtraNames();
            learned.keys.append(layout.indexOf(*field));
            member.indexOfValue = fieldNames.size();
            fieldNames.append(name);
        } else {
            if (parseIndex(*name) || name == vm.propertyNames->underscoreProto.impl() || extraNames.contains(name) || extraNames.size() == Learned::maxNumberOfExtraMembers || !canPositionExtraKey(layout, extraNames.size()))
                return false;
            if (extraNames.isEmpty())
                numberOfFieldsInFront = fieldNames.size();
            member.indexOfValue = Learned::maxNumberOfFields + extraNames.size();
            extraNames.append(name);
        }
        names.append(WTF::move(key.name));
        members.append(member);
        const uint8_t* end = skipJSONValue(value);
        if (!end)
            return false;
        end = skipWhitespace(end);
        if (*end == '}')
            break;
        if (*end != ',')
            return false;
        quote = skipWhitespace(end + 1);
    }
    positionExtraNames();
    if (!depth && (!numberOfRuns || numberOfFieldsInFront > Learned::maxMembersParsedTwiceForEachReturnSaved * (numberOfRuns + 1)))
        return false;
    bool hasListMember = members.containsIf([](const Learned::Member& member) { return member.listStorageKinds; });
    if ((context == LearnedContext::ListElement && !numberOfRuns) || (hasListMember && numberOfRuns < Learned::minExtraRunsWithList))
        return false;

    learned.plan = ObjectBuilder::tryCreatePlan(*instance.state, layout, fieldNames.span());
    if (!learned.plan || !learned.plan->hasFastPath())
        return false;
    learned.extraStructure = nullptr;
    if (!extraNames.isEmpty()) {
        learned.extraStructure = Structure::createWithProperties(vm, instance.realm.emptyExtraPropertiesStructure, extraNames.span(), 0);
        if (!learned.extraStructure || learned.extraStructure->isDictionary())
            return false;
    }
    learned.structureAtBirth = instance.state->objects->structures.objectStructure(vm, shape);
    learned.allocatorOfExtraProperties = subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(extraPropertiesInlineCapacity), AllocatorForMode::EnsureAllocator);
    learned.allocator = vm.cellSpace().allocatorFor(layout.size, AllocatorForMode::EnsureAllocator);
    learned.allocationSize = learned.allocator.cellSize();
    learned.numberOfExtraMembers = extraNames.size();
    for (Learned::Member& member : members) {
        if (member.indexOfValue >= Learned::maxNumberOfFields)
            continue;
        Learned::Kind kind = kindsOfFields[member.indexOfValue];
        std::optional<Learned::Store> store = storeOf(learned.plan->fastFields()[member.indexOfValue], kind);
        if (!store) {
            learned.stores.shrink(0);
            break;
        }
        learned.stores.append({ learned.plan->fastFields()[member.indexOfValue].offset, *store, member.indexOfValue, kind, kind == Learned::String ? vm.stringStructure->id().bits() : listStructureIDs[member.indexOfValue] });
    }
    learned.members.appendVector(members);
    isLearned = true;
    return true;
}

static void rememberWordsOf(JSONRuntime::LearnedObject& learned, Object& object)
{
    if (!learned.shape || learned.stores.isEmpty())
        return;
    learned.hasPerCellBit = object.perCellBit();
    learned.wordsBehindHeader.resize(WTF::roundUpToMultipleOf<sizeof(uint64_t)>(object.layout().size - sizeof(JSCell)) / sizeof(uint64_t));
    memcpySpan(asMutableByteSpan(learned.wordsBehindHeader.mutableSpan()), std::span { reinterpret_cast<const uint8_t*>(&object) + sizeof(JSCell), learned.wordsBehindHeader.size() * sizeof(uint64_t) });
}

static NEVER_INLINE bool finishFirstLearnedObject(VM& vm, JSONRuntime::LearnedObject& learned, Object& object, JSObject* extraProperties)
{
    if (!extraProperties) {
        rememberWordsOf(learned, object);
        return true;
    }
    DeferTerminationForAWhile deferTermination(vm);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    std::optional<Object::InitialKeyOrder> initial = object.initialKeyOrder(vm, learned.keys.span(), extraProperties);
    if (catchScope.exception()) [[unlikely]] {
        catchScope.clearException();
        return false;
    }
    if (!initial)
        return false;
    object.setInitialKeyOrder(vm, *initial, extraProperties);
    std::optional<SlotData> slot = initial->structure->extraPropertiesSlot();
    if (learned.shape && slot) {
        learned.orderWord = object.orderWord();
        learned.extraPropertiesSlot = *slot;
        learned.structure = initial->structure;
        rememberWordsOf(learned, object);
    }
    return true;
}

static ALWAYS_INLINE JSObject* createLearnedExtraProperties(VM& vm, const JSONRuntime::LearnedObject& learned, Structure* structure, const EncodedJSValue* values)
{
    static_assert(extraPropertiesInlineCapacity == 2);
    Butterfly* butterfly = nullptr;
    unsigned count = learned.numberOfExtraMembers;
    std::optional<DeferGC> deferGC;
    if (count > extraPropertiesInlineCapacity) {
        deferGC.emplace(vm);
        butterfly = Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0);
        for (unsigned i = extraPropertiesInlineCapacity; i < count; ++i)
            butterfly->propertyStorage()[offsetInOutOfLineStorage(offsetForPropertyNumber(i, extraPropertiesInlineCapacity))].setWithoutWriteBarrier(JSValue::decode(values[i]));
    }
    auto* cell = static_cast<uint64_t*>(learned.allocatorOfExtraProperties.allocate(vm.heap, JSFinalObject::allocationSize(extraPropertiesInlineCapacity), nullptr, AllocationFailureMode::Assert));
    static_assert(JSObject::butterflyOffset() == sizeof(uint64_t) && JSObject::offsetOfInlineStorage() == 2 * sizeof(uint64_t));
    cell[0] = structure->id().bits() | static_cast<uint64_t>(structure->typeInfoBlob()) << 32;
    cell[1] = std::bit_cast<uint64_t>(butterfly);
    cell[2] = values[0];
    cell[3] = count > 1 ? values[1] : JSValue::encode(JSValue());
    return reinterpret_cast<JSObject*>(cell);
}

static ALWAYS_INLINE void storeExtraProperties(const JSONRuntime::LearnedObject& learned, uint8_t* cell, JSObject* extraProperties)
{
    SlotData slot = learned.extraPropertiesSlot;
    if (slot.representation() == SlotRepresentation::Cell)
        *reinterpret_cast<JSObject**>(cell + slot.offset) = extraProperties;
    else
        *reinterpret_cast<uint32_t*>(cell + slot.offset) = encodeCell32(reinterpret_cast<HeapCell*>(cell), extraProperties);
}

static ALWAYS_INLINE bool valuesAreOfLearnedKinds(const JSONRuntime::LearnedObject& learned, const EncodedJSValue* values)
{
    using Learned = JSONRuntime::LearnedObject;
    bool areOfLearnedKinds = true;
    for (const Learned::FieldStore& field : learned.stores) {
        JSValue value = JSValue::decode(values[field.indexOfValue]);
        if (field.structureID)
            areOfLearnedKinds &= value.isCell() && value.asCell()->structureID().bits() == field.structureID;
        else if (field.kind == Learned::SmallInteger)
            areOfLearnedKinds &= value.isInt32();
        else
            areOfLearnedKinds &= value.isBoolean();
    }
    return areOfLearnedKinds;
}

static ALWAYS_INLINE JSObject* createLearnedObject(VM& vm, const JSONRuntime::LearnedObject& learned, Structure* structure, const EncodedJSValue* values, JSObject* extraProperties)
{
    using Store = JSONRuntime::LearnedObject::Store;
    auto* cell = static_cast<uint8_t*>(learned.allocator.allocate(vm.heap, learned.allocationSize, nullptr, AllocationFailureMode::Assert));
    auto* words = reinterpret_cast<uint64_t*>(cell);
    words[0] = structure->id().bits() | static_cast<uint64_t>(structure->typeInfoBlob()) << 32;
    const uint64_t* word = learned.wordsBehindHeader.begin();
    const uint64_t* end = learned.wordsBehindHeader.end();
#pragma clang loop unroll(disable) vectorize(disable)
    for (uint64_t* target = words + 1; word != end; ++word, ++target)
        *target = *word;
    for (const JSONRuntime::LearnedObject::FieldStore& field : learned.stores) {
        EncodedJSValue value = values[field.indexOfValue];
        uint8_t* slot = cell + field.offset;
        switch (field.store) {
        case Store::Word:
            *reinterpret_cast<EncodedJSValue*>(slot) = value;
            break;
        case Store::Cell32:
            *reinterpret_cast<uint32_t*>(slot) = encodeCell32(reinterpret_cast<HeapCell*>(cell), JSValue::decode(value).asCell());
            break;
        case Store::Int32:
            *reinterpret_cast<int32_t*>(slot) = static_cast<int32_t>(value);
            break;
        case Store::Int32AsDouble:
            *reinterpret_cast<double*>(slot) = static_cast<int32_t>(value);
            break;
        case Store::Boolean:
            *slot = JSValue::decode(value).asBoolean();
            break;
        case Store::LowByte:
            *slot = static_cast<uint8_t>(value);
            break;
        }
    }
    if (extraProperties)
        storeExtraProperties(learned, cell, extraProperties);
    auto* object = reinterpret_cast<JSObject*>(cell);
    if (learned.hasPerCellBit) [[unlikely]]
        object->setPerCellBit(true);
    return object;
}

static bool membersParseIdentically(const JSONRuntime::LearnedObject::Member& first, const JSONRuntime::LearnedObject::Member& second, JSValue parsed)
{
    if (first.textLength != second.textLength || first.indexOfValue != second.indexOfValue || memcmp(first.text, second.text, sizeof(first.text)))
        return false;
    return !parsed.isObject() || (first.shapeOfValue == second.shapeOfValue && first.listStorageKinds == second.listStorageKinds);
}

enum class ExpectedToken : uint8_t { Name, ClosingBrace };

static NEVER_INLINE JSONRuntime::LearnedObject* findAlternateForm(JSONRuntime& runtime, JSONRuntime::LearnedObject& learned, size_t numberOfMembersParsed, const EncodedJSValue* values, ExpectedToken expected, const uint8_t* cursor)
{
    JSONRuntime::LearnedObjects& table = *runtime.learnedObjects();
    JSONRuntime::LearnedObject& other = table[(&learned - table.data()) ^ 1];
    if (!learned.shape || !other.isFor(learned.shape, learned.firstWord) || other.members.size() < numberOfMembersParsed || other.members.isEmpty())
        return nullptr;
    for (size_t index = 0; index < numberOfMembersParsed; ++index) {
        if (!membersParseIdentically(learned.members[index], other.members[index], JSValue::decode(values[learned.members[index].indexOfValue])))
            return nullptr;
    }
    bool continues = expected == ExpectedToken::ClosingBrace ? other.members.size() == numberOfMembersParsed : other.members.size() > numberOfMembersParsed && other.members[numberOfMembersParsed].standsAt(cursor);
    return continues ? &other : nullptr;
}

static NEVER_INLINE const uint8_t* failAttempt(JSONRuntime& runtime, JSONRuntime::LearnedObject& learned, size_t numberOfMembersParsed, const EncodedJSValue* values, const uint8_t* firstName, const uint8_t* reached)
{
    if (!runtime.isAttemptAbandoned())
        runtime.setUnmatchedObject(learned.shape, firstName);
    bool shouldAbandon = false;
    for (size_t index = 0; index < numberOfMembersParsed; ++index)
        shouldAbandon |= learned.members[index].mayRecurse() && JSValue::decode(values[learned.members[index].indexOfValue]).isObject();
    runtime.noteAttemptFailure(reached, shouldAbandon);
    runtime.setOutermostFailedForm(&learned);
    return nullptr;
}

static ALWAYS_INLINE const uint8_t* tryParseObjectInlineAs(Instance& instance, VM& vm, JSONRuntime::LearnedObject*& matchedForm, const uint8_t* firstName, uint64_t& result)
{
    using Learned = JSONRuntime::LearnedObject;
    EncodedJSValue values[Learned::maxNumberOfFields + Learned::maxNumberOfExtraMembers];
    Learned* learned = matchedForm;
    const uint8_t* quote = firstName;
    Structure* structureAtBirth = learned->structureAtBirth;
    Structure* extraStructure = learned->extraStructure;
    CompiledStructure* structure = learned->structure;
    Learned::Member* member = learned->members.begin();
    Learned::Member* end = learned->members.end();
    auto switchToAlternateForm = [&](ExpectedToken expected, const uint8_t* cursor) ALWAYS_INLINE_LAMBDA {
        size_t numberOfMembersParsed = member - learned->members.begin();
        Learned* other = findAlternateForm(*instance.json.runtime, *learned, numberOfMembersParsed, values, expected, cursor);
        if (!other)
            return false;
        member = other->members.begin() + numberOfMembersParsed;
        end = other->members.end();
        matchedForm = learned = other;
        structureAtBirth = other->structureAtBirth;
        extraStructure = other->extraStructure;
        structure = other->structure;
        return true;
    };
    auto fail = [&](const uint8_t* reached) ALWAYS_INLINE_LAMBDA {
        return failAttempt(*instance.json.runtime, *learned, member - learned->members.begin(), values, firstName, reached);
    };
    for (;;) {
        if (!member->standsAt(quote)) [[unlikely]] {
            if (!switchToAlternateForm(ExpectedToken::Name, quote))
                return fail(quote);
        }
        ASSERT(member < end);
        const uint8_t* start = skipWhitespace(quote + member->textLength);
        ParsedJSONValue parsed = parseExtraValue(instance, vm, start, learned, member);
        if (!parsed.end) [[unlikely]]
            return fail(nullptr);
        values[member->indexOfValue] = JSValue::encode(parsed.value);
        ++member;
        quote = skipWhitespace(parsed.end);
        if (*quote != ',')
            break;
        if (member == end) [[unlikely]] {
            if (!switchToAlternateForm(ExpectedToken::Name, skipWhitespace(quote + 1)))
                return fail(quote);
        }
        quote = skipWhitespace(quote + 1);
    }
    if (*quote != '}' || member != end) [[unlikely]] {
        if (*quote != '}' || !switchToAlternateForm(ExpectedToken::ClosingBrace, quote))
            return fail(quote);
    }

    JSObject* extraProperties = extraStructure ? createLearnedExtraProperties(vm, *learned, extraStructure, values + Learned::maxNumberOfFields) : nullptr;
    bool isFinishedAsBefore = structure || !extraProperties;
    if (isFinishedAsBefore && !learned->wordsBehindHeader.isEmpty() && valuesAreOfLearnedKinds(*learned, values)) [[likely]] {
        result = JSValue::encode(createLearnedObject(vm, *learned, structure ? structure : structureAtBirth, values, extraProperties));
        return quote;
    }
    const ObjectBuilderPlan* plan = learned->plan.get();
    JSObject* object = plan ? ObjectBuilder::tryBuild(vm, *plan, structure ? structure : structureAtBirth, values) : nullptr;
    if (!object) [[unlikely]]
        return fail(quote);
    if (isFinishedAsBefore && (extraProperties || !learned->stores.isEmpty() == !learned->wordsBehindHeader.isEmpty())) {
        if (extraProperties) {
            uncheckedDowncast<Object>(object)->at<uint32_t>(offsetOfOrderWord) = learned->orderWord;
            storeExtraProperties(*learned, reinterpret_cast<uint8_t*>(object), extraProperties);
        }
    } else if (!finishFirstLearnedObject(vm, *learned, *uncheckedDowncast<Object>(object), extraProperties))
        return fail(quote);
    result = JSValue::encode(object);
    return quote;
}

static NEVER_INLINE const uint8_t* tryParseObjectAs(Instance& instance, VM& vm, JSONRuntime::LearnedObject*& learned, const uint8_t* quote, uint64_t& result)
{
    return tryParseObjectInlineAs(instance, vm, learned, quote, result);
}

static NEVER_INLINE const uint8_t* learnAndParseObject(Instance& instance, VM& vm, JSONRuntime::LearnedObject* seenOnce, uint32_t shape, const uint8_t* firstName, uint64_t& result)
{
    JSONRuntime& runtime = *instance.json.runtime;
    if (!runtime.remembersTextsOf(shape))
        return nullptr;
    runtime.didNotFindTextOf(shape);
    if (!seenOnce) {
        JSONRuntime::LearnedObject* seen = runtime.learnedObjectToReplace(shape, JSONRuntime::startOfName(firstName));
        if (!seen)
            return nullptr;
        seen->shape = shape;
        seen->firstWord = JSONRuntime::startOfName(firstName);
        seen->wasFound = false;
        seen->isOnlySeen = true;
        seen->members.shrink(0);
        return nullptr;
    }
    if (runtime.isFinishCacheDisabled() || !runtime.mayLearnAnotherObject() || !learnObject(instance, vm, *seenOnce, shape, firstName, 0) || !seenOnce->shape)
        return nullptr;
    runtime.resetAttemptState();
    return tryParseObjectAs(instance, vm, seenOnce, firstName, result);
}

static NEVER_INLINE ParsedJSONValue parseLearnedObjectInside(Instance& instance, VM& vm, uint32_t shape, const uint8_t* brace)
{
    JSONRuntime& runtime = *instance.json.runtime;
    if (vm.isSafeToRecurse()) [[likely]] {
        const uint8_t* firstName = skipWhitespace(brace + 1);
        uint64_t firstWord = JSONRuntime::startOfName(firstName);
        unsigned index = JSONRuntime::indexOfLearnedObject(shape, firstWord);
        for (unsigned way = 0; way < 2 && !runtime.isAttemptAbandoned(); ++way) {
            JSONRuntime::LearnedObject* learned = &(*runtime.learnedObjects())[index ^ way];
            if (!learned->isFor(shape, firstWord) || learned->members.isEmpty())
                continue;
            uint64_t result;
            if (const uint8_t* end = tryParseObjectAs(instance, vm, learned, firstName, result)) [[likely]] {
                learned->wasFound = true;
                runtime.setLearnedObjectFoundLast(learned);
                return { JSValue::decode(result), end + 1 };
            }
        }
    }
    if (!runtime.isAttemptAbandoned()) {
        runtime.setOutermostFailedForm(nullptr);
        runtime.setUnmatchedObject(shape, skipWhitespace(brace + 1));
    }
    runtime.noteAttemptFailure(brace, true);
    return { };
}

static ALWAYS_INLINE JSArray* createList(Instance& instance, VM& vm, JSONRuntime::ObservedMember::ListStorage storage, const EncodedJSValue* values, unsigned length)
{
    using Observed = JSONRuntime::ObservedMember;
    static_assert(sizeof(JSArray) == 2 * sizeof(uint64_t) && sizeof(IndexingHeader) == sizeof(uint64_t) && JSObject::butterflyOffset() == sizeof(uint64_t));
    for (unsigned index = 0; index < length && storage != Observed::Contiguous; ++index) {
        JSValue value = JSValue::decode(values[index]);
        if (!value.isInt32())
            storage = value.isDouble() ? Observed::Double : Observed::Contiguous;
    }
    const InstancePrefix& prefix = instance.prefix();
    uint64_t header = storage == Observed::Contiguous ? prefix.newArrayWithContiguousHeader : storage == Observed::Double ? prefix.newArrayWithDoubleHeader : prefix.newArrayWithInt32Header;
    // The header is zero while the realm is having a bad time.
    if (!static_cast<uint32_t>(header)) [[unlikely]]
        return nullptr;
    unsigned vectorLength = arrayLiteralVectorLength(length);
    auto* words = static_cast<uint64_t*>(vm.cellSpace().allocate(vm, sizeOfArrayWithStorage(vectorLength), nullptr, AllocationFailureMode::Assert));
    uint64_t* elements = words + 3;
    words[2] = length | static_cast<uint64_t>(vectorLength) << 32;
    for (unsigned index = 0; index < length; ++index)
        elements[index] = storage == Observed::Double ? std::bit_cast<uint64_t>(JSValue::decode(values[index]).asNumber()) : static_cast<uint64_t>(values[index]);
    uint64_t hole = storage == Observed::Double ? std::bit_cast<uint64_t>(PNaN) : static_cast<uint64_t>(JSValue::encode(JSValue()));
    for (unsigned index = length; index < vectorLength; ++index)
        elements[index] = hole;
    words[1] = std::bit_cast<uint64_t>(elements);
    words[0] = header;
    vm.mutatorFence();
    return std::bit_cast<JSArray*>(words);
}

static NEVER_INLINE JSArray* flushListElements(Instance& instance, VM& vm, JSArray* array, JSONRuntime::ObservedMember::ListStorage storage, const EncodedJSValue* values, unsigned count)
{
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]]
        return nullptr;
    if (!array)
        return createList(instance, vm, storage, values, count);
    IndexingType shape = array->indexingType() & IndexingShapeMask;
    bool fits = shape == ContiguousShape;
    if (shape == Int32Shape || shape == DoubleShape) {
        fits = true;
        for (unsigned index = 0; index < count; ++index)
            fits &= shape == Int32Shape ? JSValue::decode(values[index]).isInt32() : JSValue::decode(values[index]).isNumber();
    }
    unsigned length = array->length();
    if (fits && length <= MAX_STORAGE_VECTOR_LENGTH - count && array->ensureLength(vm, length + count)) [[likely]] {
        auto* elements = array->butterfly()->indexingPayload<uint64_t>() + length;
        for (unsigned index = 0; index < count; ++index)
            elements[index] = shape == DoubleShape ? std::bit_cast<uint64_t>(JSValue::decode(values[index]).asNumber()) : static_cast<uint64_t>(values[index]);
        if (shape == ContiguousShape)
            vm.writeBarrier(array);
        return array;
    }
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    for (unsigned index = 0; index < count; ++index) {
        array->push(instance.globalObject, JSValue::decode(values[index]));
        if (catchScope.exception()) [[unlikely]] {
            catchScope.clearException();
            return nullptr;
        }
    }
    return array;
}

static NEVER_INLINE bool refreshObservedMember(JSONRuntime& runtime, const JSONRuntime::LearnedObject& learned, JSONRuntime::LearnedObject::Member& member)
{
    JSONRuntime::ObservedMember observed = runtime.observedMember(learned.shape, member.field);
    uint32_t shapeOfValue = observed.shape ? observed.shape : JSONRuntime::LearnedObject::shapeOfNoObject;
    if (!learned.shape || (shapeOfValue == member.shapeOfValue && observed.listStorageKinds == member.listStorageKinds)) {
        runtime.disableFinishCache();
        return false;
    }
    member.shapeOfValue = shapeOfValue;
    member.listStorageKinds = observed.listStorageKinds;
    return true;
}

static NEVER_INLINE JSArray* flushNumbers(Instance& instance, VM& vm, JSArray* array, const double* numbers, unsigned count)
{
    unsigned length = 0;
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]]
        return nullptr;
    if (!array) {
        if (!static_cast<uint32_t>(instance.prefix().newArrayWithDoubleHeader)) [[unlikely]]
            return nullptr;
        array = tryCreateArrayWithStorage(vm, instance.globalObject->originalArrayStructureForIndexingType(ArrayWithDouble), count, arrayLiteralVectorLength(count));
    } else {
        length = array->length();
        if (length > MAX_STORAGE_VECTOR_LENGTH - count || !array->ensureLength(vm, length + count)) [[unlikely]]
            return nullptr;
    }
    if (array) [[likely]]
        memcpy(array->butterfly()->contiguousDouble().data() + length, numbers, count * sizeof(double));
    return array;
}

static NEVER_INLINE ParsedJSONValue tryParseNumberList(Instance& instance, VM& vm, const uint8_t* cursor)
{
    static constexpr unsigned capacity = 32;
    double numbers[capacity];
    unsigned count = 0;
    JSArray* array = nullptr;
    for (;;) {
        ParsedJSONNumber number;
        JSValue integer;
        if (const uint8_t* end = tryParseSmallJSONInteger(cursor, integer))
            number = { static_cast<double>(integer.asInt32()), end };
        else
            number = parseJSONNumber(cursor, instance.json.sourceEnd);
        if (!number.end) [[unlikely]]
            return { };
        if (count == capacity) {
            array = flushNumbers(instance, vm, array, numbers, std::exchange(count, 0));
            if (!array) [[unlikely]]
                return { };
        }
        numbers[count++] = number.number;
        cursor = skipWhitespace(number.end);
        if (*cursor != ',')
            break;
        cursor = skipWhitespace(cursor + 1);
    }
    if (*cursor != ']') [[unlikely]]
        return { };
    array = flushNumbers(instance, vm, array, numbers, count);
    return array ? ParsedJSONValue { array, cursor + 1 } : ParsedJSONValue { };
}

static const uint8_t* appendLearnedObjects(Instance&, VM&, uint32_t shape, JSArray*, const uint8_t* element, JSONRuntime::LearnedObject* likelyForm);

static NEVER_INLINE ParsedJSONValue parseLearnedList(Instance& instance, VM& vm, const JSONRuntime::LearnedObject& learned, JSONRuntime::LearnedObject::Member& member, unsigned level, const uint8_t* bracket)
{
    using Learned = JSONRuntime::LearnedObject;
    using Observed = JSONRuntime::ObservedMember;
    static constexpr unsigned capacity = 16;
    JSONRuntime& runtime = *instance.json.runtime;
    EncodedJSValue elements[capacity];
    unsigned count = 0;
    JSArray* array = nullptr;
    const uint8_t* cursor = skipWhitespace(bracket + 1);
    auto storage = [&]() ALWAYS_INLINE_LAMBDA {
        return Observed::listStorageAt(member.listStorageKinds, level);
    };
    bool isComplete = false;
    if (vm.isSafeToRecurse() && (storage() != Observed::Unknown || refreshObservedMember(runtime, learned, member)) && storage() != Observed::Unknown) {
        if (storage() == Observed::Double && *cursor != ']') {
            if (ParsedJSONValue numbers = tryParseNumberList(instance, vm, cursor); numbers.end) [[likely]]
                return numbers;
        }
        isComplete = *cursor == ']';
        while (!isComplete) {
            ParsedJSONValue parsed;
            if (*cursor == '{' && (member.hasRecordShape() || (member.shapeOfValue == Learned::shapeOfNoObject && refreshObservedMember(runtime, learned, member) && member.hasRecordShape()))) {
                array = flushListElements(instance, vm, array, storage(), elements, std::exchange(count, 0));
                if (!array) [[unlikely]]
                    break;
                cursor = appendLearnedObjects(instance, vm, member.shapeOfValue, array, cursor, member.shapeOfValue == learned.shape ? const_cast<Learned*>(&learned) : runtime.learnedObjectFoundLast());
                if (*cursor == '{' || runtime.isAttemptAbandoned()) [[unlikely]]
                    break;
                isComplete = *cursor == ']';
                continue;
            }
            if (*cursor == '[' && (member.shapeOfValue != Learned::plainObjectShape || Observed::listStorageAt(member.listStorageKinds, level + 1) != Observed::Unknown))
                parsed = parseLearnedList(instance, vm, learned, member, level + 1, cursor);
            else if (*cursor != '{' || member.shapeOfValue == Learned::plainObjectShape)
                parsed = parseExtraValue(instance, vm, cursor);
            if (!parsed.end) [[unlikely]]
                break;
            if (count == capacity) {
                array = flushListElements(instance, vm, array, storage(), elements, std::exchange(count, 0));
                if (!array) [[unlikely]]
                    break;
            }
            elements[count++] = JSValue::encode(parsed.value);
            cursor = skipWhitespace(parsed.end);
            if (*cursor == ',') {
                cursor = skipWhitespace(cursor + 1);
                continue;
            }
            isComplete = *cursor == ']';
            if (!isComplete) [[unlikely]]
                break;
        }
    }
    if (isComplete) [[likely]] {
        if (count || !array)
            array = flushListElements(instance, vm, array, storage(), elements, count);
        if (array) [[likely]]
            return { array, cursor + 1 };
    }
    if (!runtime.isAttemptAbandoned())
        runtime.setUnmatchedObject(0, nullptr);
    runtime.noteAttemptFailure(cursor, true);
    return { };
}

static NEVER_INLINE ParsedJSONValue parseNestedLearnedValue(Instance& instance, VM& vm, const JSONRuntime::LearnedObject& learned, JSONRuntime::LearnedObject::Member& member, const uint8_t* start)
{
    if (*start == '[' && member.listStorageKinds) {
        if (const uint8_t* first = skipWhitespace(start + 1); *first == ']') {
            if (JSArray* list = createList(instance, vm, JSONRuntime::ObservedMember::listStorageAt(member.listStorageKinds, 0), nullptr, 0)) [[likely]]
                return { list, first + 1 };
        }
        return parseLearnedList(instance, vm, learned, member, 0, start);
    }
    if (*start == '{' && member.hasRecordShape())
        return parseLearnedObjectInside(instance, vm, member.shapeOfValue, start);
    if (*start == '{' && member.shapeOfValue == JSONRuntime::LearnedObject::plainObjectShape)
        return parseJSONValueAt(instance, vm, instance.globalObject, start);
    instance.json.runtime->noteAttemptReached(start);
    return { };
}

static NEVER_INLINE const uint8_t* appendLearnedObjects(Instance& instance, VM& vm, uint32_t shape, JSArray* array, const uint8_t* element, JSONRuntime::LearnedObject* likelyForm)
{
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSONRuntime& runtime = *instance.json.runtime;
    JSONRuntime::LearnedObject* last = likelyForm;
    if (!runtime.learnedObjects())
        return element;
    while (*element == '{') {
        const uint8_t* firstName = skipWhitespace(element + 1);
        ParsedJSONValue parsed;
        uint64_t result;
        const uint8_t* end = last && last->holdsMembersOf(shape) ? tryParseObjectInlineAs(instance, vm, last, firstName, result) : nullptr;
        if (end) [[likely]]
            parsed = { JSValue::decode(result), end + 1 };
        else {
            if (runtime.isAttemptAbandoned())
                break;
            parsed = parseLearnedObjectInside(instance, vm, shape, element);
            if (!parsed.end)
                break;
            last = runtime.learnedObjectFoundLast();
        }
        const uint8_t* next = skipWhitespace(parsed.end);
        if (*next == ',') {
            next = skipWhitespace(next + 1);
            if (*next == ']')
                break;
        } else if (*next != ']')
            break;
        array->pushInline(instance.globalObject, parsed.value);
        if (catchScope.exception()) [[unlikely]] {
            catchScope.clearException();
            break;
        }
        element = next;
    }
    if (last && !runtime.isAttemptAbandoned())
        last->consecutiveFailures = 0;
    return element;
}

static NEVER_INLINE void handleAbandonedAttempt(Instance& instance, VM& vm, JSONRuntime& runtime, bool isListRequest)
{
    AOT_THRESHOLD(uint8_t, maxConsecutiveFailures, 4);
    JSONRuntime::LearnedObject* learned = runtime.outermostFailedForm();
    auto [shape, firstName] = runtime.unmatchedObject();
    runtime.resetAttemptState();
    if (!learned)
        return;
    if (!isListRequest) {
        if (learned->shape)
            runtime.didRefuseTextOf(learned->shape);
    } else if (++learned->consecutiveFailures == maxConsecutiveFailures) {
        learned->members.shrink(0);
        learned->isOnlySeen = false;
        return;
    }
    if (!shape || !runtime.remembersTextsOf(shape) || runtime.isFinishCacheDisabled() || !runtime.mayLearnAnotherObject())
        return;
    if (JSONRuntime::LearnedObject* missing = runtime.learnedObjectToReplace(shape, JSONRuntime::startOfName(firstName)); missing && missing != learned)
        learnObject(instance, vm, *missing, shape, firstName, 1, LearnedContext::ListElement);
}

static ALWAYS_INLINE const uint8_t* tryParseObjectAsLearned(Instance& instance, VM& vm, uint32_t shape, const uint8_t* firstName, uint64_t& result)
{
    JSONRuntime& runtime = *instance.json.runtime;
    JSONRuntime::LearnedObject* last = runtime.learnedObjectFoundLast();
    if (last && last->holdsMembersOf(shape)) {
        JSONRuntime::LearnedObject* matched = last;
        if (const uint8_t* end = tryParseObjectAs(instance, vm, matched, firstName, result)) [[likely]] {
            runtime.didFindWholeTextOf(shape);
            if (matched != last) [[unlikely]] {
                matched->wasFound = true;
                runtime.setLearnedObjectFoundLast(matched);
            }
            return end;
        }
    } else
        last = nullptr;
    uint64_t firstWord = JSONRuntime::startOfName(firstName);
    JSONRuntime::LearnedObject* seenOnce = nullptr;
    if (JSONRuntime::LearnedObjects* table = runtime.learnedObjects()) [[likely]] {
        unsigned index = JSONRuntime::indexOfLearnedObject(shape, firstWord);
        for (unsigned way = 0; way < 2; ++way) {
            JSONRuntime::LearnedObject* learned = &(*table)[index ^ way];
            if (!learned->isFor(shape, firstWord))
                continue;
            if (learned->isOnlySeen) {
                seenOnce = learned;
                continue;
            }
            if (learned->isRefused()) {
                runtime.didRefuseTextOf(shape);
                return nullptr;
            }
            if (learned == last || runtime.isAttemptAbandoned())
                continue;
            if (const uint8_t* end = tryParseObjectAs(instance, vm, learned, firstName, result)) [[likely]] {
                learned->wasFound = true;
                runtime.didFindWholeTextOf(shape);
                runtime.setLearnedObjectFoundLast(learned);
                return end;
            }
        }
    }
    return learnAndParseObject(instance, vm, seenOnce, shape, firstName, result);
}

static ALWAYS_INLINE const uint8_t* appendLearnedObjectsForRequest(Instance& instance, VM& vm, JSONRuntime& runtime, uint32_t shape, JSArray* array, const uint8_t* cursor)
{
    const uint8_t* end = appendLearnedObjects(instance, vm, shape, array, cursor, nullptr);
    if (runtime.isAttemptAbandoned()) [[unlikely]] {
        handleAbandonedAttempt(instance, vm, runtime, true);
        runtime.noteAttemptReached(end);
    } else
        runtime.noteParsedThrough(end);
    return end;
}

static ALWAYS_INLINE const uint8_t* parseWithLearnedForms(Instance& instance, VM& vm, uint32_t shape, const uint8_t* cursor, uint64_t& listOrResult, bool isListRequest)
{
    JSONRuntime& runtime = *instance.json.runtime;
    if (!runtime.canStartAttemptAt(cursor)) [[unlikely]] {
        if (!isListRequest && !runtime.isBeforeFailedPosition(cursor))
            runtime.didRefuseTextOf(shape);
        return cursor;
    }
    if (isListRequest)
        return appendLearnedObjectsForRequest(instance, vm, runtime, shape, uncheckedDowncast<JSArray>(JSValue::decode(listOrResult).asCell()), cursor);
    if (const uint8_t* end = tryParseObjectAsLearned(instance, vm, shape, skipWhitespace(cursor + 1), listOrResult)) [[likely]] {
        runtime.noteParsedThrough(end);
        return end;
    }
    if (runtime.isAttemptAbandoned())
        handleAbandonedAttempt(instance, vm, runtime, false);
    return cursor;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONObjectKey, uint64_t, (Instance* instance, uint64_t order, uint32_t shape, uint32_t field))
{
    const LayoutData& layout = layoutOf(*instance, shape);
    if (field >= layout.numberOfFields) [[unlikely]]
        return JSONKeyOrder::unknown().word();
    return advance(*instance, layout, JSONKeyOrder::fromWord(order), static_cast<uint16_t>(field)).word();
}

static NEVER_INLINE bool finishObject(VM& vm, Instance& instance, Object& object, JSValue extra, uint64_t order)
{
    DeferTerminationForAWhile deferTermination(vm);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    bool isFinished = tryFinishObject(vm, instance, object, extra.isObject() ? asObject(extra) : nullptr, JSONKeyOrder::fromWord(order));
    if (catchScope.exception()) [[unlikely]] {
        catchScope.clearException();
        isFinished = false;
    }
    if (!isFinished)
        count(instance, RuntimeCounterID::TypedParsesFellBackPerValue);
    return isFinished;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONObjectFinish, size_t, (Instance* instance, JSCell* cell, EncodedJSValue encodedExtra, uint64_t order))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONObjectFinish);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTJSONObjectFinish);
    JSValue extra = JSValue::decode(encodedExtra);
    bool isFinished = (extra.isObject() && tryFinishObjectAsBefore(vm, *instance, *object, asObject(extra), order)) || finishObject(vm, *instance, *object, extra, order);
    return static_cast<size_t>(isFinished ? Typed::JSONOutcome::Parsed : Typed::JSONOutcome::Unknown);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseExtraMembers, const uint8_t*, (Instance* instance, uint32_t shape, const uint8_t* cursor, uint64_t* extraAndOrder))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONParseExtraMembers);
    JSValue extra = JSValue::decode(extraAndOrder[0]);
    bool isListRequest = extra.isCell() && extra.asCell()->type() == ArrayType;
    if (extra.isNull() || isListRequest)
        return parseWithLearnedForms(*instance, vm, shape, cursor, extraAndOrder[0], isListRequest);
    if (*cursor != '"') [[unlikely]]
        return cursor;
    if (instance->json.runtime->remembersTextsOf(shape)) [[likely]] {
        if (const uint8_t* next = tryParseLearnedExtraMembers(*instance, vm, shape, cursor, extraAndOrder)) [[likely]]
            return next;
    }
    return parseAndLearnExtraMembers(instance, vm, shape, cursor, extraAndOrder);
}
} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
