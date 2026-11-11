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
#include "AOTInliningAcrossFiles.h"
#include "AOTInstance.h"
#include "AOTJSONHooks.h"
#include "AOTJSONRuntime.h"
#include "AOTJSONValueParser.h"
#include "AOTKeyOrderTable.h"
#include "AOTObject.h"
#include "AOTObjectBuilder.h"
#include "AOTObjectBuilderInlines.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeReporter.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferTermination.h"
#include "GCMemoryOperations.h"
#include "JSArrayBufferView.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "ObjectInitializationScope.h"
#include "TopExceptionScope.h"
#include "WeakGCMapInlines.h"
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
{
    instance.json.runtime = this;
    m_hasParsedRecords = instance.state && !instance.state->tables.parsedRecordShapes().empty();
    registry.add(CacheKind::ClearedEachCollection, "JSON source copy"_s, *this);
}

JSONValueParser::LearnedNames& JSONRuntime::learnedNames()
{
    if (!m_learnedNames) [[unlikely]]
        m_learnedNames.make();
    return *m_learnedNames;
}

void JSONRuntime::forgetRecordPlansNotFoundLately(VM& vm)
{
    if (m_numberOfRecordPlans < fewestRecordPlansLookedThrough || m_numberOfTimesAskedForRecordPlan - m_numberOfTimesAskedWhenPlansWereForgotten <= 2 * static_cast<uint64_t>(m_numberOfRecordPlans))
        return;
    m_numberOfTimesAskedWhenPlansWereForgotten = m_numberOfTimesAskedForRecordPlan;
    uint32_t numberOfRecordPlansBefore = m_numberOfRecordPlans;
    m_recordPlans.removeIf([&](auto& plansUnderHash) {
        m_numberOfRecordPlans -= plansUnderHash.value.removeAllMatching([&](const RecordPlanForNames& known) {
            return m_numberOfCollections - known.numberOfCollectionsWhenLastFound > 1 && m_numberOfTimesAskedForRecordPlan - known.numberOfTimesAskedWhenLastFound > 2 * static_cast<uint64_t>(numberOfRecordPlansBefore);
        });
        return plansUnderHash.value.isEmpty();
    });
    if (m_numberOfRecordPlans != numberOfRecordPlansBefore)
        countEvent(vm, RuntimeCounterID::RecordPlansEvicted, numberOfRecordPlansBefore - m_numberOfRecordPlans);
}

auto JSONRuntime::findOrCreateRecordPlan(VM& vm, Structure* structure) -> RecordPlanFound
{
    if (structure->isDictionary() || (m_structuresThatNoPlanFits && m_structuresThatNoPlanFits->get(structure)))
        return { };
    UniquedStringImpl* lastName = structure->transitionPropertyName();
    UniquedStringImpl* lastNameThatProgramSpells = lastName;
    Structure* structureOfNamesInRecord = structure;
    while (lastNameThatProgramSpells && !identifiersOf(m_instance).numberOf(lastNameThatProgramSpells)) {
        structureOfNamesInRecord = structureOfNamesInRecord->previousID();
        lastNameThatProgramSpells = structureOfNamesInRecord ? structureOfNamesInRecord->transitionPropertyName() : nullptr;
    }
    if (structureOfNamesInRecord && structureOfNamesInRecord != structure && m_structuresThatNoPlanFits && m_structuresThatNoPlanFits->get(structureOfNamesInRecord))
        return { };
    std::optional<uint32_t> lastNameNumber = lastNameThatProgramSpells ? identifiersOf(m_instance).numberOf(lastNameThatProgramSpells) : std::nullopt;
    if (!lastNameNumber || m_instance.state->tables.parsedRecordShapesWithName(*lastNameNumber).empty())
        return { };

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
        return { };
    size_t numberInRecord = names.size();
    while (numberInRecord && names[numberInRecord - 1] != lastNameThatProgramSpells)
        --numberInRecord;
    if (!numberInRecord)
        return { };
    std::span<UniquedStringImpl*> namesInRecord = names.mutableSpan().first(numberInRecord);
    auto found = [&](const ObjectBuilderPlan* plan) {
        return RecordPlanFound { plan, static_cast<unsigned>(numberInRecord), numberInRecord < names.size() };
    };

    uint32_t hash = 0;
    for (UniquedStringImpl* name : namesInRecord)
        hash = pairIntHash(hash, name->existingSymbolAwareHash());
    hash >>= 1;
    if (auto underHash = m_recordPlans.find(hash); underHash != m_recordPlans.end()) {
        for (RecordPlanForNames& known : underHash->value) {
            if (std::ranges::equal(known.names, namesInRecord, { }, &RefPtr<UniquedStringImpl>::get)) {
                known.numberOfCollectionsWhenLastFound = m_numberOfCollections;
                known.numberOfTimesAskedWhenLastFound = m_numberOfTimesAskedForRecordPlan;
                return found(known.plan.get());
            }
        }
    }
    bool imageHasEveryName = std::ranges::all_of(namesInRecord, [&](UniquedStringImpl* name) {
        return !!identifiersOf(m_instance).numberOf(name);
    });
    if (!imageHasEveryName)
        return { };
    std::unique_ptr<ObjectBuilderPlan> plan = ObjectBuilder::tryCreatePlanOfSmallestRecord(*m_instance.state, namesInRecord, [](const ObjectBuilderPlan&) { return true; });
    if (!plan) {
        if (!m_structuresThatNoPlanFits)
            m_structuresThatNoPlanFits = makeUnique<WeakGCMap<Structure*, Structure>>(vm);
        m_structuresThatNoPlanFits->set(structureOfNamesInRecord, structureOfNamesInRecord);
        return { };
    }
    Vector<RecordPlanForNames, 1>& plansUnderHash = m_recordPlans.add(hash, Vector<RecordPlanForNames, 1> { }).iterator->value;
    plansUnderHash.append({ { }, WTF::move(plan), m_numberOfCollections, m_numberOfTimesAskedForRecordPlan });
    if (++m_numberOfRecordPlans > m_mostRecordPlansHeld) {
        m_mostRecordPlansHeld = m_numberOfRecordPlans;
        countEvent(vm, RuntimeCounterID::PeakRecordPlansCached);
    }
    plansUnderHash.last().names.appendContainerWithMapping(namesInRecord, [](UniquedStringImpl* name) {
        return RefPtr { name };
    });
    return found(plansUnderHash.last().plan.get());
}

NEVER_INLINE JSONRuntime::RecordPlan JSONRuntime::findRecordPlan(VM& vm, Structure* ordinaryStructure)
{
    if (!m_recordPlanCache)
        m_recordPlanCache.make();
    ++m_numberOfTimesAskedForRecordPlan;
    forgetRecordPlansNotFoundLately(vm);
    uint32_t numberOfCollectionsAtFirst = m_numberOfCollections;
    uint32_t id = ordinaryStructure->id().bits();
    RecordPlan recordPlan;
    RecordBesideNames besideNames;
    if (RecordPlanFound found = findOrCreateRecordPlan(vm, ordinaryStructure); found.plan) {
        Structure* structure = m_instance.state->objects->structures.objectStructure(vm, found.plan->layout().shape);
        if (found.hasNamesBeyondRecord)
            besideNames = { found.plan, structure, found.numberOfNamesInRecord };
        else
            recordPlan = { found.plan, structure };
    }
    if (m_numberOfCollections == numberOfCollectionsAtFirst)
        (*m_recordPlanCache)[recordPlanCacheIndex(id)] = { id, recordPlan, besideNames };
    return recordPlan;
}

JSONRuntime::~JSONRuntime()
{
    freeCurrent();
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

ALWAYS_INLINE_INTO_OTHER_FILES uint8_t* JSONRuntime::reserve(size_t size)
{
    size_t capacity = m_sourceCapacity;
    if (size <= capacity && (capacity <= maxRetainedJSONSourceCapacity || size > maxRetainedJSONSourceCapacity)) [[likely]]
        return poisonUnusedCapacity(const_cast<uint8_t*>(m_instance.json.sourceStart), size, capacity);
    return replace(size);
}

void JSONRuntime::end()
{
    JSONWords& words = m_instance.json;
    ASSERT(words.sourceOwnerStackPointer);
    m_wideText = nullptr;
    words.sourceOwnerStackPointer = nullptr;
    words.canEndSourceInline = false;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint8_t* JSONRuntime::beginIn(uint8_t* start, void* stackPointer, size_t length)
{
    JSONWords& words = m_instance.json;
    words.sourceEnd = start + length;
    memset(start + length, 0, jsonSourcePadding);
    words.sourceOwnerStackPointer = stackPointer;
    words.canEndSourceInline = true;
    words.valueParserFailed = false;
    return start;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint8_t* JSONRuntime::begin(void* stackPointer, size_t length)
{
    ASSERT(stackPointer);
    m_wideText = nullptr;
    uint8_t* start = reserve(length + jsonSourcePadding);
    if (!start) [[unlikely]]
        return nullptr;
    return beginIn(start, stackPointer, length);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool JSONRuntime::openLatin1(std::span<const Latin1Character> characters, void* stackPointer)
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

bool JSONRuntime::open(StringView text, void* stackPointer)
{
    if (text.is8Bit()) [[likely]]
        return openLatin1(text.span8(), stackPointer);
    if (!open16Bit(text.span16(), stackPointer))
        return false;
    m_wideText = StringImpl::createWithoutCopying(text.span16());
    return true;
}

void JSONRuntime::abandon()
{
    if (m_instance.json.sourceOwnerStackPointer)
        end();
}

void JSONRuntime::clear()
{
    ++m_numberOfCollections;
    if (m_learnedNames)
        m_learnedNames->clear();
    if (m_recordPlanCache)
        m_recordPlanCache->fill({ });
    for (const Vector<RecordPlanForNames, 1>& plansUnderHash : m_recordPlans.values()) {
        for (const RecordPlanForNames& known : plansUnderHash)
            known.plan->clearAcceptedStructures();
    }
    if (m_instance.json.sourceOwnerStackPointer)
        return;
    if (m_sourceCapacity > maxRetainedJSONSourceCapacity)
        freeCurrent();
}

bool JSONSourceScope::open(StringView text, void* stackPointer)
{
    return m_instance.json.runtime->open(text, stackPointer);
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

bool isLatin1AfterDecoding(const JSONWords& words, std::span<const uint8_t> characters)
{
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
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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
    bool textIsASCII = words.sourceEncoding != JSONSourceEncoding::Latin1 && charactersAreAllASCII(std::span<const uint8_t> { start, cursor });
    vm.jsonCache().addString(std::span<const Latin1Character> { byteCast<Latin1Character>(start - 1), jsonSourcePadding }, cursor + 2 - start, string, textIsASCII);
    return { string, cursor + 1 };
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
    DeferTerminationForAWhile deferTermination(m_vm);
    return parseValue(start);
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
    count = index;
    size_t length = next.value - quote;
    if (length > maxTextLength)
        return;
    if (words.sourceEncoding != JSONSourceEncoding::Latin1 && !isShortTextLatin1AfterDecoding(quote, length))
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
    if (m_words.sourceEncoding != JSONSourceEncoding::Latin1 && !entry->textIsASCII && !isShortTextLatin1AfterDecoding(quote, entry->textLength))
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
    bool isMadeNow = !to;
    if (!to) {
        if (isValidOffset(from->get(m_vm, name)))
            return { };
        to = Structure::addNewPropertyTransition(m_vm, from, name, 0, offset);
    }
    if (to->isDictionary() || offset != memberPropertyOffset(members.count))
        return { };

    if (!isMadeNow && to->isCacheableTransitionFrom(from)) {
        ++m_numberOfTransitionsCached;
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
            if (JSObject* object = ObjectBuilder::tryBuild(m_vm, *record.plan, record.structure, members.values)) {
                ++m_numberOfRecordsMade;
                return object;
            }
        } else if (JSONRuntime::RecordBesideNames* cached = runtime.recordBesideNamesFor(members.structure)) {
            JSONRuntime::RecordBesideNames beside = *cached;
            unsigned numberInRecord = beside.numberOfNamesInRecord;
            unsigned numberBeyondRecord = members.count - numberInRecord;
            auto layNamesBeyondRecord = [&](Object& row) {
                if (beside.structureOfNamesBeyondRecord) {
                    row.setExtraProperties(m_vm, createOrdinaryObject(m_vm, beside.structureOfNamesBeyondRecord, members.values + numberInRecord, numberBeyondRecord), beside.structureWithNamesBeyondRecord);
                    return true;
                }
                Vector<Object::ExtraProperty, 8> properties(numberBeyondRecord, [&](size_t index) {
                    return Object::ExtraProperty { nullptr, JSValue::decode(members.values[numberInRecord + index]) };
                });
                unsigned inlineCapacity = members.structure->inlineCapacity();
                members.structure->forEachProperty(m_vm, [&](const auto& entry) -> bool {
                    size_t index = isInlineOffset(entry.offset()) ? entry.offset() : inlineCapacity + (entry.offset() - firstOutOfLineOffset);
                    if (index >= numberInRecord)
                        properties[index - numberInRecord].name = entry.key();
                    return true;
                });
                if (!row.tryCreateExtraPropertiesOfNewObject(m_vm, properties.span()))
                    return false;
                Structure* ofNamesBeyondRecord = row.extraProperties()->structure();
                if (ofNamesBeyondRecord->isDictionary() || numberInRecord + ofNamesBeyondRecord->inlineCapacity() > JSFinalObject::maxInlineCapacity)
                    return true;
                if (JSONRuntime::RecordBesideNames* stillCached = runtime.recordBesideNamesFor(members.structure)) {
                    stillCached->structureOfNamesBeyondRecord = ofNamesBeyondRecord;
                    stillCached->structureWithNamesBeyondRecord = uncheckedDowncast<CompiledStructure>(row.structure());
                }
                return true;
            };
            JSObject* object = ObjectBuilder::tryBuild(m_vm, *beside.plan, beside.structure, members.values);
            if (object && layNamesBeyondRecord(*uncheckedDowncast<Object>(object))) {
                ++m_numberOfRecordsMade;
                return object;
            }
            if (JSONRuntime::RecordBesideNames* stillCached = object ? runtime.recordBesideNamesFor(members.structure) : nullptr)
                *stillCached = { };
        }
    }
    ++m_numberOfOrdinaryObjectsMade;
    return createOrdinaryObject(m_vm, members.structure, members.values, members.count);
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
        parsed.name = impl->is8Bit() ? Identifier::fromString(m_vm, String { impl }) : Identifier::fromString(m_vm, impl->span16());
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
    JSONValueParser parser(*instance, vm, globalObject);
    ParsedJSONValue parsed = parser.parseWholeSource(sourceScope.start());
    CLEAR_AND_RETURN_IF_EXCEPTION(scope, { });
    countEvent(vm, RuntimeCounterID::JSONParseFastPaths);
    countEvent(vm, RuntimeCounterID::ObjectsParsedIntoRecords, parser.numberOfRecordsMade());
    countEvent(vm, RuntimeCounterID::ObjectsParsedIntoOrdinaryObjects, parser.numberOfOrdinaryObjectsMade());
    countEvent(vm, RuntimeCounterID::TransitionsCachedForParsedNames, parser.numberOfTransitionsCached());
    return parsed.end ? parsed.value : JSValue();
}

JSValue tryParseJSON(JSGlobalObject* globalObject, StringView text)
{
    return tryParse(globalObject, text.length(), text);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
