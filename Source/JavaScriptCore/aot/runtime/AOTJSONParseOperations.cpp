/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONScanner.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
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
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferTermination.h"
#include "GCMemoryOperations.h"
#include "GlobalObjectMethodTable.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSONObject.h"
#include "ObjectConstructor.h"
#include "ObjectInitializationScope.h"
#include "TopExceptionScope.h"
#include <cmath>
#include <span>
#include <wtf/FastFloat.h>
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
    m_hasParsedRecords = instance.state && !instance.state->tables.parsedRecordFields().empty();
    registry.add(CacheKind::ClearedEachCollection, "JSON source copy"_s, *this);
}

JSONValueParser::LearnedNames& JSONRuntime::learnedNames()
{
    if (!m_learnedNames) [[unlikely]]
        m_learnedNames = makeUnique<JSONValueParser::LearnedNames>();
    return *m_learnedNames;
}

static bool hasEveryMemberForSomeValidator(VM& vm, State& state, std::span<UniquedStringImpl* const> names)
{
    struct MembersFound {
        uint32_t validatedObject;
        uint32_t count;
    };
    Vector<MembersFound, 8> found;
    for (UniquedStringImpl* name : names) {
        std::optional<uint32_t> number = vm.aotState().identifiers->numberOf(name);
        if (!number)
            continue;
        for (const ParsedRecordFieldData& row : std::ranges::equal_range(state.tables.parsedRecordFields(), *number, { }, &ParsedRecordFieldData::name)) {
            if (!row.isForValidator())
                continue;
            size_t index = found.findIf([&](const MembersFound& entry) {
                return entry.validatedObject == row.shape;
            });
            if (index == notFound) {
                index = found.size();
                found.append({ row.shape, 0 });
            }
            if (++found[index].count == row.numberOfRequiredMembers())
                return true;
        }
    }
    return false;
}

std::unique_ptr<ObjectBuilderPlan> JSONRuntime::createRecordPlan(VM& vm, std::span<UniquedStringImpl* const> names)
{
    State& state = *m_instance.state;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::optional<uint32_t> lastName = vm.aotState().identifiers->numberOf(names.back());
    if (!lastName || hasEveryMemberForSomeValidator(vm, state, names))
        return nullptr;

    std::unique_ptr<ObjectBuilderPlan> best;
    for (const ParsedRecordFieldData& row : std::ranges::equal_range(state.tables.parsedRecordFields(), *lastName, { }, &ParsedRecordFieldData::name)) {
        if (row.isForValidator())
            continue;
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
        m_recordPlanCache = makeUniqueWithoutFastMallocCheck<RecordPlanCache>();
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
        return open(withoutByteOrderMark(uncheckedDowncast<JSArrayBufferView>(cell)->span()), stackPointer);
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
    if (isLatin1(words, characters)) {
        builder.append(byteCast<Latin1Character>(characters));
        return true;
    }
    if (words.sourceEncoding == JSONSourceEncoding::UTF16) {
        builder.append(original16BitCharacters(words, characters));
        return true;
    }
    if (!StringImpl::isValidLength<char16_t>(characters.size()))
        return false;
    String decoded = String::fromUTF8(characters);
    if (decoded.isNull())
        return false;
    builder.append(decoded);
    return true;
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

    Structure* structure = members.structure;
    unsigned inlineCapacity = structure->inlineCapacity();
    for (unsigned i = members.count; i < inlineCapacity; ++i)
        members.values[i] = JSValue::encode(JSValue());
    auto* inlineValues = reinterpret_cast<const WriteBarrierBase<Unknown>*>(members.values);
    unsigned outOfLineCapacity = structure->outOfLineCapacity();
    if (!outOfLineCapacity)
        return JSFinalObject::createWithButterflyCopyingInlineStorage(m_vm, structure, nullptr, inlineValues);

    DeferGC deferGC(m_vm);
    Butterfly* butterfly = Butterfly::create(m_vm, nullptr, 0, outOfLineCapacity, false, IndexingHeader(), 0);
    for (unsigned i = inlineCapacity; i < members.count; ++i)
        butterfly->propertyStorage()[offsetInOutOfLineStorage(offsetForPropertyNumber(i, inlineCapacity))].setWithoutWriteBarrier(JSValue::decode(members.values[i]));
    return JSFinalObject::createWithButterflyCopyingInlineStorage(m_vm, structure, butterfly, inlineValues);
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
    OPERATION_RETURN(scope, instance->json.runtime->open(JSValue::decode(text), stackPointer));
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONKeyRepeats, size_t, (Instance* instance, const uint8_t* keyStart, const uint8_t* valueStart))
{
    return jsonKeyRepeats(instance->json, keyStart, valueStart);
}

struct ExtraMembers {
    Instance& instance;
    const LayoutData& layout;
    JSObject* properties;
    std::optional<uint16_t> unpositionedEntry { };
};

enum class ExtraMemberStatus : uint8_t { Unknown, Failed, Added };

struct ParsedExtraMember {
    ExtraMemberStatus status { ExtraMemberStatus::Unknown };
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

static bool tryFinishObject(VM& vm, Instance& instance, Object& object, JSObject* extraProperties, JSONKeyOrder order)
{
    if (order.kind() == JSONKeyOrder::Kind::Unknown)
        return false;
    if (order.kind() == JSONKeyOrder::Kind::Numbered && !extraProperties) {
        object.setOrderAndCount(order.orderAndCount());
        return true;
    }
    // A list of keys lasts as long as its text; a number lasts.
    bool isFinishedAlikeNextTime = extraProperties && order.kind() == JSONKeyOrder::Kind::Numbered;
    JSONRuntime::FinishedObject* finished = isFinishedAlikeNextTime ? &instance.json.runtime->finishedObjectFor(object.structure(), extraProperties->structure()) : nullptr;
    if (finished && finished->structureAtBirth == object.structure() && finished->extraStructure == extraProperties->structure() && finished->order == order.word() && finished->orderWordAtBirth == object.orderWord()) {
        object.setInitialKeyOrder(vm, { finished->orderAndCount, finished->structure }, extraProperties);
        return true;
    }
    JSONRuntime::FinishedObject born { object.structure(), extraProperties ? extraProperties->structure() : nullptr, order.word(), object.orderWord() };
    Object::KeyOrderStorage keys(entriesOf(instance, object.layout(), order));
    std::optional<Object::InitialKeyOrder> initial = object.initialKeyOrder(vm, keys.span(), extraProperties);
    if (!initial)
        return false;
    object.setInitialKeyOrder(vm, *initial, extraProperties);
    if (finished) {
        born.orderAndCount = initial->orderAndCount;
        born.structure = initial->structure;
        *finished = born;
    }
    return true;
}

static JSObject* ensureExtraProperties(Instance& instance, uint64_t& encodedExtra)
{
    JSValue extra = JSValue::decode(encodedExtra);
    if (extra.isCell())
        return dynamicDowncast<JSFinalObject>(extra.asCell());
    JSObject* created = Object::createExtraProperties(instance.globalObject, { });
    encodedExtra = JSValue::encode(created);
    return created;
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
    members.unpositionedEntry = static_cast<uint16_t>(members.layout.numberOfFields + ordinal);
}

static JSONKeyOrder positionAddedKeys(const ExtraMembers& members, JSONKeyOrder order)
{
    if (!members.unpositionedEntry)
        return order;
    return advance(members.instance, members.layout, order, *members.unpositionedEntry);
}

static RefPtr<UniquedStringImpl> tryAtomize(Instance& instance, JSString* string)
{
    VM& vm = *instance.vm;
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Identifier identifier = string->toIdentifier(instance.globalObject);
    if (catchScope.exception()) [[unlikely]] {
        catchScope.clearException();
        return nullptr;
    }
    return identifier.impl();
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
    JSObject* properties = members.properties;
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
    JSObject* properties = members.properties;
    Structure* before = properties->structure();
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
    if (offset >= firstOutOfLineOffset && before->outOfLineCapacity() != after->outOfLineCapacity()) [[unlikely]]
        properties->putDirect(vm, after->transitionPropertyName(), parsed.value);
    else {
        properties->putDirectOffset(vm, offset, parsed.value);
        properties->setStructure(vm, after);
    }
    didAddExtraKey(members, ordinal);
    return { ExtraMemberStatus::Added, parsed.end };
}

static NEVER_INLINE ParsedExtraMember parseNewExtraMember(VM& vm, ExtraMembers& members, JSONValueParser& parser, const uint8_t* quote)
{
    JSONValueParser::ParsedName key = parser.parseNameAndColon(quote);
    if (!key.afterColon || members.instance.state->objects->layouts.field(vm, members.layout, key.name.impl()))
        return { ExtraMemberStatus::Failed };
    ParsedJSONValue parsed = parser.parse(key.afterColon);
    if (!parsed.end || !addExtraProperty(vm, members, key.name.impl(), parsed.value))
        return { ExtraMemberStatus::Failed };
    return { ExtraMemberStatus::Added, parsed.end };
}

static const uint8_t* parseExtraMembers(VM& vm, ExtraMembers& members, const uint8_t* quote)
{
    JSONValueParser parser(members.instance, vm, members.instance.globalObject);
    ParsedExtraMember member = parseKnownExtraMember(vm, members, parser, quote);
    if (member.status == ExtraMemberStatus::Unknown)
        member = parseNewExtraMember(vm, members, parser, quote);
    const uint8_t* end = nullptr;
    while (member.status == ExtraMemberStatus::Added) {
        end = member.end;
        if (*end != ',')
            return end;
        quote = skipJSONWhitespaceCharacters(end + 1);
        if (*quote != '"')
            return end;
        member = parseKnownExtraMember(vm, members, parser, quote);
    }
    return member.status == ExtraMemberStatus::Failed ? nullptr : end;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONObjectKey, uint64_t, (Instance* instance, uint64_t order, uint32_t shape, uint32_t field))
{
    const LayoutData& layout = layoutOf(*instance, shape);
    if (field >= layout.numberOfFields) [[unlikely]]
        return JSONKeyOrder::unknown().word();
    return advance(*instance, layout, JSONKeyOrder::fromWord(order), static_cast<uint16_t>(field)).word();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONObjectFinish, size_t, (Instance* instance, JSCell* cell, EncodedJSValue encodedExtra, uint64_t order))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONObjectFinish);
    DeferTerminationForAWhile deferTermination(vm);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTJSONObjectFinish);
    JSValue extra = JSValue::decode(encodedExtra);
    bool isFinished = tryFinishObject(vm, *instance, *object, extra.isObject() ? asObject(extra) : nullptr, JSONKeyOrder::fromWord(order));
    if (catchScope.exception()) [[unlikely]] {
        catchScope.clearException();
        isFinished = false;
    }
    if (!isFinished)
        count(*instance, RuntimeCounterID::TypedParsesFellBackPerValue);
    return static_cast<size_t>(isFinished ? Typed::JSONOutcome::Parsed : Typed::JSONOutcome::Unknown);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONAddExtraMember, void, (Instance* instance, uint32_t shape, JSCell* name, EncodedJSValue value, uint64_t* extraAndOrder))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONAddExtraMember);
    DeferTerminationForAWhile deferTermination(vm);
    JSONKeyOrder order = JSONKeyOrder::fromWord(extraAndOrder[1]);
    if (order.kind() == JSONKeyOrder::Kind::Unknown)
        return;
    const LayoutData& layout = layoutOf(*instance, shape);
    ExtraMembers members { *instance, layout, ensureExtraProperties(*instance, extraAndOrder[0]) };
    RefPtr<UniquedStringImpl> uid = members.properties ? tryAtomize(*instance, cellArgument<JSString>(name, Entry::operationAOTJSONAddExtraMember)) : nullptr;
    bool isAdded = uid && !instance->state->objects->layouts.field(vm, layout, uid.get()) && addExtraProperty(vm, members, uid.get(), JSValue::decode(value));
    extraAndOrder[1] = (isAdded ? positionAddedKeys(members, order) : JSONKeyOrder::unknown()).word();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseExtraMembers, const uint8_t*, (Instance* instance, uint32_t shape, const uint8_t* cursor, uint64_t* extraAndOrder))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONParseExtraMembers);
    DeferTerminationForAWhile deferTermination(vm);
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSONKeyOrder order = JSONKeyOrder::fromWord(extraAndOrder[1]);
    const LayoutData& layout = layoutOf(*instance, shape);
    bool canParse = order.kind() != JSONKeyOrder::Kind::Unknown && *cursor == '"';
    ExtraMembers members { *instance, layout, canParse ? ensureExtraProperties(*instance, extraAndOrder[0]) : nullptr };
    const uint8_t* end = members.properties ? parseExtraMembers(vm, members, cursor) : nullptr;
    if (catchScope.exception()) [[unlikely]] {
        catchScope.clearException();
        end = nullptr;
    }
    if (!end) [[unlikely]] {
        count(*instance, RuntimeCounterID::TypedParsesFellBackPerValue);
        return nullptr;
    }
    extraAndOrder[1] = positionAddedKeys(members, order).word();
    return end;
}
} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
