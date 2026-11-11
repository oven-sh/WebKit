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
#include <wtf/MathExtras.h>
#include <wtf/Scope.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/unicode/UTF8Conversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include <wtf/fast_float/fast_float.h>

namespace JSC { namespace AOT {

static constexpr unsigned maxMantissaDigits = 19;
static constexpr uint64_t maxExactInteger = 9007199254740991;

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
    , m_learnedStrings(arena)
{
    instance.json.runtime = this;
    m_hasParsedRecords = instance.state && !instance.state->tables.parsedRecordShapes().empty();
    registry.add(CacheKind::ClearedEachCollection, "JSON source copy"_s, *this);
}

JSONValueParser::LearnedNames& JSONRuntime::learnedNames()
{
    if (!m_learnedNames) [[unlikely]]
        m_learnedNames.make(learnedStrings());
    return *m_learnedNames;
}

JSONValueParser::LearnedStrings& JSONRuntime::learnedStrings()
{
    if (!m_learnedStrings) [[unlikely]]
        m_learnedStrings.make();
    return *m_learnedStrings;
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
    freeSourceStorage(m_sourceCopy, m_sourceCapacity);
    m_sourceCopy = nullptr;
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
    m_sourceCopy = static_cast<uint8_t*>(storage);
    m_sourceCapacity = capacity;
    return poisonUnusedCapacity(m_sourceCopy, size, capacity);
}

ALWAYS_INLINE_INTO_OTHER_FILES uint8_t* JSONRuntime::reserve(size_t size)
{
    size_t capacity = m_sourceCapacity;
    if (size <= capacity && (capacity <= maxRetainedJSONSourceCapacity || size > maxRetainedJSONSourceCapacity)) [[likely]]
        return poisonUnusedCapacity(m_sourceCopy, size, capacity);
    return replace(size);
}

void JSONRuntime::end()
{
    JSONWords& words = m_instance.json;
    ASSERT(words.sourceOwnerStackPointer);
    m_wideText = nullptr;
    words.sourceStart = nullptr;
    words.sourceEnd = nullptr;
    words.sourceOwnerStackPointer = nullptr;
    words.canEndSourceInline = false;
}

ALWAYS_INLINE_INTO_OTHER_FILES void JSONRuntime::beginIn(const uint8_t* start, void* stackPointer, size_t length)
{
    JSONWords& words = m_instance.json;
    words.sourceStart = start;
    words.sourceEnd = start + length;
    words.sourceOwnerStackPointer = stackPointer;
    words.canEndSourceInline = true;
    words.valueParserFailed = false;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint8_t* JSONRuntime::begin(void* stackPointer, size_t length)
{
    ASSERT(stackPointer);
    m_wideText = nullptr;
    uint8_t* start = reserve(length + jsonSourcePadding);
    if (!start) [[unlikely]]
        return nullptr;
    memset(start + length, 0, jsonSourcePadding);
    beginIn(start, stackPointer, length);
    return start;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool JSONRuntime::tryOpenInPlace(std::span<const Latin1Character> characters, void* stackPointer)
{
#if ASAN_ENABLED
    UNUSED_PARAM(characters);
    UNUSED_PARAM(stackPointer);
    return false;
#else
    constexpr uintptr_t minPageSize = 4 * KB;
    size_t length = characters.size();
    while (length && (characters[length - 1] == ' ' || characters[length - 1] == '\n' || characters[length - 1] == '\r' || characters[length - 1] == '\t'))
        --length;
    if (!length || (characters[length - 1] != '}' && characters[length - 1] != ']'))
        return false;
    auto* start = byteCast<uint8_t>(characters.data());
    if ((std::bit_cast<uintptr_t>(start + length - 1) & (minPageSize - 1)) + jsonSourcePadding >= minPageSize) [[unlikely]]
        return false;
    m_wideText = nullptr;
    beginIn(start, stackPointer, length);
    m_instance.json.sourceEncoding = JSONSourceEncoding::Latin1;
    return true;
#endif
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

bool JSONRuntime::open(StringView text, JSONTextMemory memory, void* stackPointer)
{
    if (text.is8Bit()) [[likely]]
        return (memory == JSONTextMemory::Constant && tryOpenInPlace(text.span8(), stackPointer)) || openLatin1(text.span8(), stackPointer);
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
    if (m_learnedStrings)
        m_learnedStrings->clear();
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

bool JSONSourceScope::open(StringView text, JSONTextMemory memory, void* stackPointer)
{
    return m_instance.json.runtime->open(text, memory, stackPointer);
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
        if (cursor >= words.sourceEnd || !appendPlainCharacters(words, builder, { start, cursor }))
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
        if (start >= words.sourceEnd) [[unlikely]]
            return nullptr;
        cursor = findEndOfPlainJSONCharacters(start, words.sourceEnd);
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
    const uint8_t* cursor = findEndOfPlainJSONCharacters(start, words.sourceEnd);
    if (cursor >= words.sourceEnd) [[unlikely]]
        return { jsEmptyString(vm) };
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
    bool isSourceText = words.sourceEncoding == JSONSourceEncoding::Latin1;
    JSString* string = vm.jsonCache().tryMakeJSString(vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(cursor) }, isSourceText);
    if (!string) [[unlikely]]
        return { jsEmptyString(vm) };
    bool textIsASCII = words.sourceEncoding != JSONSourceEncoding::Latin1 && charactersAreAllASCII(std::span<const uint8_t> { start, cursor });
    vm.jsonCache().addString(std::span<const Latin1Character> { byteCast<Latin1Character>(start - 1), jsonSourcePadding }, cursor + 2 - start, string, textIsASCII);
    return { string, cursor + 1 };
}

static NEVER_INLINE ParsedJSONNumber parseJSONNumberSlow(const uint8_t* start, const uint8_t* sourceEnd)
{
    size_t length = 0;
    auto number = WTF::parseJSONDouble(std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(sourceEnd) }, length);
    if (!number || !std::isfinite(*number))
        return { };
    return { *number, start + length };
}

struct EightCharacters {
    static constexpr uint64_t zeros = 0x3030303030303030;

    ALWAYS_INLINE explicit EightCharacters(const uint8_t* cursor)
        : characters(WTF::unalignedLoad<uint64_t>(cursor))
        , bitsOfOthers(((characters + 0x4646464646464646) | (characters - zeros)) & 0x8080808080808080)
    {
    }

    bool areDigits() const { return !bitsOfOthers; }
    unsigned numberOfDigits() const { return std::countr_zero(bitsOfOthers) / 8; }
    uint32_t number() const { return fast_float::parse_eight_digits_unrolled(characters); }
    uint32_t numberOfFirst(unsigned count) const
    {
        ASSERT(count && count < 8);
        return fast_float::parse_eight_digits_unrolled((characters << (64 - 8 * count)) | (zeros >> (8 * count)));
    }

    uint64_t characters;
    uint64_t bitsOfOthers;
};

static ALWAYS_INLINE ParsedJSONNumber parseJSONNumber(const uint8_t* start, const uint8_t* sourceEnd)
{
    const uint8_t* cursor = start;
    bool isNegative = *cursor == '-';
    cursor += isNegative;
    const uint8_t* digits = cursor;
    unsigned digit = *cursor - '0';
    if (digit > 9) [[unlikely]]
        return { };
    uint64_t mantissa = 0;
    if (!digit) {
        if (isASCIIDigit(*++cursor)) [[unlikely]]
            return { };
    } else {
        do {
            mantissa = mantissa * 10 + digit;
            digit = *++cursor - '0';
        } while (digit <= 9);
    }
    size_t numberOfDigits = cursor - digits;
    size_t numberOfFractionDigits = 0;
    if (*cursor == '.') {
        const uint8_t* fraction = ++cursor;
        EightCharacters eight { cursor };
        for (; eight.areDigits(); eight = EightCharacters { cursor }) {
            mantissa = mantissa * 100000000 + eight.number();
            cursor += 8;
        }
        if (unsigned count = eight.numberOfDigits()) {
            static constexpr std::array<uint32_t, 8> powersOfTen { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000 };
            mantissa = mantissa * powersOfTen[count] + eight.numberOfFirst(count);
            cursor += count;
        }
        numberOfFractionDigits = cursor - fraction;
        if (!numberOfFractionDigits) [[unlikely]]
            return { };
    }
    if (numberOfDigits + numberOfFractionDigits > maxMantissaDigits || (*cursor | 0x20) == 'e') [[unlikely]]
        return parseJSONNumberSlow(start, sourceEnd);
    double number;
    if (mantissa <= maxExactInteger) {
        number = static_cast<double>(static_cast<int64_t>(mantissa));
        if (numberOfFractionDigits)
            number /= fast_float::binary_format<double>::exact_power_of_ten(numberOfFractionDigits);
    } else {
        auto adjusted = fast_float::compute_float<fast_float::binary_format<double>>(-static_cast<int64_t>(numberOfFractionDigits), mantissa);
        if (adjusted.power2 < 0) [[unlikely]]
            return parseJSONNumberSlow(start, sourceEnd);
        fast_float::to_float(false, adjusted, number);
    }
    return { isNegative ? -number : number, cursor };
}

static ALWAYS_INLINE const uint8_t* tryParseSmallJSONInteger(const uint8_t* start, JSValue& value)
{
    unsigned digit = *start - '0';
    if (digit > 9)
        return nullptr;
    const uint8_t* cursor = start + 1;
    if (unsigned next = *cursor - '0'; next <= 9) {
        EightCharacters eight { start };
        if (!digit || eight.areDigits()) [[unlikely]]
            return nullptr;
        unsigned count = eight.numberOfDigits();
        digit = eight.numberOfFirst(count);
        cursor = start + count;
    }
    if (*cursor == '.' || (*cursor | 0x20) == 'e') [[unlikely]]
        return nullptr;
    value = jsNumber(static_cast<int32_t>(digit));
    return cursor;
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

ALWAYS_INLINE JSONValueParser::JSONValueParser(Instance& instance, VM& vm, JSGlobalObject* globalObject)
    : m_instance(instance)
    , m_words(instance.json)
    , m_vm(vm)
    , m_globalObject(globalObject)
    , m_cache(vm.jsonCache())
    , m_parsingScope(m_cache)
    , m_learnedNames(instance.json.runtime->learnedNames())
    , m_learnedStrings(instance.json.runtime->learnedStrings())
    , m_sourceEnd(m_words.sourceEnd)
    , m_sourceNumber(m_learnedNames.beginSource(m_words.sourceEncoding))
{
}

ALWAYS_INLINE ParsedJSONValue JSONValueParser::parse(const uint8_t* start)
{
    DeferTerminationForAWhile deferTermination(m_vm);
    return parseValue(start);
}

ALWAYS_INLINE ParsedJSONValue JSONValueParser::parseWholeSource(const uint8_t* start)
{
    ParsedJSONValue parsed = parse(start);
    if (!parsed.end || parsed.end > m_sourceEnd || (parsed.end != m_sourceEnd && skipWhitespace(parsed.end) != m_sourceEnd))
        return { };
    return parsed;
}

ParsedJSONValue JSONValueParser::parseValue(Cursor cursor)
{
    cursor = skipWhitespace(cursor);
    switch (*cursor) {
    case '"':
        if (LearnedString* string = parseString(cursor)) [[likely]]
            return { string->string, cursor + string->textLength };
        return { };
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
    default: {
        JSValue integer;
        if (Cursor end = tryParseSmallJSONInteger(cursor, integer))
            return { integer, end };
        return parseNumber(cursor);
    }
    }
}

ALWAYS_INLINE auto JSONValueParser::unlearnedString(Cursor quote, const ParsedJSONString& parsed) -> LearnedString*
{
    if (!parsed.end) [[unlikely]]
        return nullptr;
    LearnedString& unlearned = m_learnedStrings.unlearned();
    unlearned.string = parsed.string;
    unlearned.textLength = parsed.end - quote;
    return &unlearned;
}

NEVER_INLINE auto JSONValueParser::parseString(Cursor quote) -> LearnedString*
{
    if (m_words.sourceEncoding != JSONSourceEncoding::Latin1) [[unlikely]] {
        if (auto cached = findCachedJSONString(m_words, m_vm, quote))
            return unlearnedString(quote, *cached);
        return unlearnedString(quote, makeUncachedJSONString(m_instance, m_vm, quote + 1));
    }
    Cursor start = quote + 1;
    Cursor end = findEndOfPlainJSONCharacters(start, m_sourceEnd);
    if (end >= m_sourceEnd) [[unlikely]]
        return nullptr;
    if (*end != '"') [[unlikely]]
        return unlearnedString(quote, makeJSONStringWithEscapes(m_instance, m_vm, start, end));
    size_t length = end - start;
    std::span text { start, length };
    constexpr bool isSourceText = true;
    if (length > LearnedStrings::maxLength) [[unlikely]] {
        JSString* string = m_cache.tryMakeJSString(m_vm, byteCast<Latin1Character>(text), isSourceText);
        return unlearnedString(quote, { string, string ? end + 1 : nullptr });
    }
    LearnedString::Key key = LearnedString::Key::of(text);
    LearnedString& learned = m_learnedStrings.at(key);
    bool isKnown = [&] {
        if (learned.key != key || !learned.string)
            return false;
        if (length <= 2 * sizeof(uint64_t))
            return true;
        StringImpl* impl = learned.string->tryGetValueImpl();
        return impl && impl->is8Bit() && equal(impl->span8().data(), byteCast<Latin1Character>(text));
    }();
    if (!isKnown) {
        JSString* string = m_cache.tryMakeJSString(m_vm, byteCast<Latin1Character>(text), isSourceText);
        if (!string) [[unlikely]]
            return nullptr;
        m_learnedStrings.set(learned, key, string);
    }
    learned.quote = quote;
    learned.sourceNumber = m_sourceNumber;
    return &learned;
}

NEVER_INLINE ParsedJSONValue JSONValueParser::parseNumber(Cursor cursor)
{
    ParsedJSONNumber parsed = parseJSONNumber(cursor, m_sourceEnd);
    if (!parsed.end) [[unlikely]]
        return { };
    return { jsNumber(parsed.number), parsed.end };
}

NEVER_INLINE void JSONValueParser::LearnedNames::forgetForNewSource()
{
    clear();
    if (m_sourceNumber)
        return;
    m_strings.clear();
    ++m_sourceNumber;
}

void JSONValueParser::LearnedNames::clear()
{
    ++m_epoch;
    m_count = 1;
    m_hasNamesBeyondASCII = false;
    m_first.fill(nullptr);
}

NEVER_INLINE auto JSONValueParser::LearnedNames::findOtherFirst(Cursor quote) -> LearnedName&
{
    LearnedName*& first = m_first[firstIndex(quote)];
    unsigned numberTried = 0;
    for (LearnedName* previous = first; previous->sibling && numberTried < maxSiblingsTried; previous = previous->sibling, ++numberTried) {
        LearnedName& name = *previous->sibling;
        if (!name.matches(quote))
            continue;
        previous->sibling = name.sibling;
        name.sibling = first;
        first = &name;
        return name;
    }
    return unlearned();
}

NEVER_INLINE auto JSONValueParser::LearnedNames::findOtherNext(LearnedName& name, Cursor quote) -> LearnedName&
{
    unsigned numberTried = 0;
    for (LearnedName* next = name.firstNext; next && numberTried < maxSiblingsTried; next = next->sibling, ++numberTried) {
        if (next->matches(quote)) {
            name.likelyNext = next;
            return *next;
        }
    }
    return unlearned();
}

auto JSONValueParser::LearnedNames::add(const JSONWords& words, Cursor quote, const NextMember& member) -> LearnedName*
{
    size_t length = member.value - quote;
    if (length > LearnedName::maxTextLength || std::ranges::any_of(std::span { quote, length }, [](uint8_t character) { return character == '}' || character == ']'; }))
        return nullptr;
    if (m_count == capacity) [[unlikely]] {
        clear();
        return nullptr;
    }
    if (!charactersAreAllASCII(std::span { quote, length })) [[unlikely]] {
        if (words.sourceEncoding != JSONSourceEncoding::Latin1)
            return nullptr;
        m_hasNamesBeyondASCII = true;
    }
    auto maskFor = [](size_t bytes) -> uint64_t {
        return bytes >= sizeof(uint64_t) ? std::numeric_limits<uint64_t>::max() : (1ull << (8 * bytes)) - 1;
    };
    LearnedName& name = m_names[m_count++];
    name = { };
    name.mask[0] = maskFor(length);
    name.mask[1] = maskFor(length > sizeof(uint64_t) ? length - sizeof(uint64_t) : 0);
    name.text[0] = WTF::unalignedLoad<uint64_t>(quote) & name.mask[0];
    name.text[1] = WTF::unalignedLoad<uint64_t>(quote + sizeof(uint64_t)) & name.mask[1];
    if (length > sizeof(name.text)) {
        name.tail[0] = WTF::unalignedLoad<uint64_t>(quote + length - sizeof(name.tail));
        name.tail[1] = WTF::unalignedLoad<uint64_t>(quote + length - sizeof(uint64_t));
    }
    name.likelyNext = &unlearned();
    name.string = &m_strings.unlearned();
    name.structure = member.structure;
    name.textLength = length;
    return &name;
}

auto JSONValueParser::LearnedNames::addFirst(const JSONWords& words, Cursor quote, const NextMember& member) -> LearnedName&
{
    LearnedName* name = add(words, quote, member);
    if (!name)
        return unlearned();
    LearnedName*& first = m_first[firstIndex(quote)];
    name->sibling = first;
    first = name;
    return *name;
}

auto JSONValueParser::LearnedNames::addNext(const JSONWords& words, LearnedName& previous, Cursor quote, const NextMember& member) -> LearnedName&
{
    LearnedName* name = previous.isLearned() ? add(words, quote, member) : nullptr;
    if (!name)
        return unlearned();
    name->sibling = previous.firstNext;
    previous.firstNext = name;
    previous.likelyNext = name;
    return *name;
}

static ALWAYS_INLINE bool isSameText(const uint8_t* text, const uint8_t* other, unsigned length)
{
    constexpr size_t stride = SIMD::stride<uint8_t>;
    if (length >= stride) {
        auto differs = [](const uint8_t* text, const uint8_t* other) ALWAYS_INLINE_LAMBDA {
            return SIMD::isNonZero(SIMD::bitXor(SIMD::load(text), SIMD::load(other)));
        };
        for (; length > stride; length -= stride, text += stride, other += stride) {
            if (differs(text, other))
                return false;
        }
        return !differs(text + length - stride, other + length - stride);
    }
    uint64_t difference = WTF::unalignedLoad<uint64_t>(text) ^ WTF::unalignedLoad<uint64_t>(other);
    if (length < sizeof(uint64_t))
        return !(difference << (64 - 8 * length));
    size_t last = length - sizeof(uint64_t);
    return !(difference | (WTF::unalignedLoad<uint64_t>(text + last) ^ WTF::unalignedLoad<uint64_t>(other + last)));
}

static ALWAYS_INLINE uint8_t kindOfParsedValue(JSValue value)
{
    if (value.isInt32())
        return ObjectBuilderPlan::AcceptsInt32;
    if (value.isCell())
        return value.asCell()->isString() ? ObjectBuilderPlan::AcceptsString : ObjectBuilderPlan::AcceptsObject;
    if (value.isDouble())
        return ObjectBuilderPlan::AcceptsDouble;
    return value.isBoolean() ? ObjectBuilderPlan::AcceptsBoolean : ObjectBuilderPlan::AcceptsNull;
}

static ALWAYS_INLINE JSObject* createObjectOfStructure(VM& vm, Structure* structure, EncodedJSValue* values, unsigned count)
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

JSObject* JSONValueParser::createOrdinaryObject(Members& members, unsigned count, Structure* structure)
{
    ++m_numberOfOrdinaryObjectsMade;
    return createObjectOfStructure(m_vm, structure, members.values, count);
}

ALWAYS_INLINE JSObject* JSONValueParser::createObject(Members& members, unsigned count, Structure* structure, LearnedName& name)
{
    if (name.plan) [[likely]] {
        if (JSObject* object = ObjectBuilder::tryBuild(m_vm, *name.plan, name.recordStructure, members.values, members.kinds, members.kindsOfAll)) [[likely]] {
            ++m_numberOfRecordsMade;
            return object;
        }
        return createOrdinaryObject(members, count, structure);
    }
    return name.completesOrdinaryObject ? createOrdinaryObject(members, count, structure) : createObjectSlow(members, count, structure, name);
}

NEVER_INLINE ParsedJSONValue JSONValueParser::parseObject(Cursor brace)
{
    if (!m_vm.isSafeToRecurse() || m_vm.traps().needsOutOfMemoryError()) [[unlikely]]
        return { };
    Cursor cursor = skipWhitespace(brace + 1);
    if (*cursor == '}') [[unlikely]]
        return { constructEmptyObject(m_globalObject), cursor + 1 };

    LearnedNames& names = m_learnedNames;
    uint32_t epoch = names.epoch();
    Members members;
    unsigned count = 0;
    LearnedString* previousString = nullptr;
    LearnedName* previous = nullptr;
    LearnedName* name = &names.findFirst(cursor);
    Structure* structure = nullptr;
    auto structureWith = [&](LearnedName& last) ALWAYS_INLINE_LAMBDA {
        return last.isLearned() ? last.structure : structure;
    };
    for (;;) {
        Cursor value = cursor + name->textLength;
        if (!name->isLearned()) [[unlikely]] {
            structure = previous ? structureWith(*previous) : m_globalObject->objectStructureForObjectConstructor();
            NextMember next = findNextMember(structure, count, cursor);
            if (!next.structure)
                return parseRemainingMembers(createOrdinaryObject(members, count, structure), cursor);
            if (names.epoch() != epoch) [[unlikely]]
                previousString = nullptr;
            else if (!next.isNew) {
                name = previous ? &names.addNext(m_words, *previous, cursor, next) : &names.addFirst(m_words, cursor, next);
                epoch = names.epoch();
            }
            value = next.value;
            structure = next.structure;
        }

        JSValue parsedValue;
        uint8_t kind = ObjectBuilderPlan::AcceptsInt32;
        auto parseSlow = [&](auto&& parse) ALWAYS_INLINE_LAMBDA {
            Structure* structureNow = structureWith(*name);
            ParsedJSONValue parsed = parse();
            if (!parsed.end || parsed.end >= m_sourceEnd) [[unlikely]]
                return false;
            parsedValue = parsed.value;
            cursor = parsed.end;
            if (names.epoch() != epoch) [[unlikely]] {
                name = &names.unlearned();
                structure = structureNow;
                previousString = nullptr;
            }
            return true;
        };
        for (;;) {
            if (*value == '"') {
                kind = ObjectBuilderPlan::AcceptsString;
                auto isNext = [&](LearnedString& string) ALWAYS_INLINE_LAMBDA {
                    return string.sourceNumber == m_sourceNumber && value + string.textLength < m_sourceEnd && isSameText(value, string.quote, string.textLength);
                };
                LearnedString* string = previousString ? previousString->next : name->string;
                if (!isNext(*string)) [[unlikely]] {
                    string = name->string;
                    if (!previousString || !isNext(*string)) {
                        Structure* structureNow = structureWith(*name);
                        string = parseString(value);
                        if (!string) [[unlikely]]
                            return { };
                        if (names.epoch() != epoch) [[unlikely]] {
                            name = &names.unlearned();
                            structure = structureNow;
                            previousString = nullptr;
                        }
                    }
                    if (previousString)
                        previousString->next = string;
                }
                parsedValue = string->string;
                cursor = value + string->textLength;
                if (name->isLearned()) [[likely]]
                    name->string = string;
                previousString = string;
                break;
            }
            if (isASCIIDigit(*value) || *value == '-') {
                if (Cursor end = tryParseSmallJSONInteger(value, parsedValue))
                    cursor = end;
                else {
                    ParsedJSONValue parsed = parseNumber(value);
                    if (!parsed.end) [[unlikely]]
                        return { };
                    parsedValue = parsed.value;
                    cursor = parsed.end;
                    kind = parsedValue.isInt32() ? ObjectBuilderPlan::AcceptsInt32 : ObjectBuilderPlan::AcceptsDouble;
                }
                break;
            }
            if (*value <= ' ') [[unlikely]] {
                Cursor next = skipJSONWhitespaceCharacters(value);
                if (next == value)
                    return { };
                value = next;
                continue;
            }
            if (!parseSlow([&] { return parseValue(value); })) [[unlikely]]
                return { };
            kind = kindOfParsedValue(parsedValue);
            break;
        }
        members.values[count] = JSValue::encode(parsedValue);
        members.kinds[count++] = kind;
        members.kindsOfAll |= kind;

        if (*cursor != ',') {
            cursor = skipWhitespace(cursor);
            if (*cursor == '}') [[likely]]
                return { createObject(members, count, structureWith(*name), *name), cursor + 1 };
            if (*cursor != ',') [[unlikely]]
                return { };
        }
        ++cursor;
        previous = name;
        name = previous->likelyNext;
        if (!name->matches(cursor)) [[unlikely]] {
            cursor = skipWhitespace(cursor);
            name = &names.findNext(*previous, cursor);
        }
    }
}

auto JSONValueParser::findNextMember(Structure* from, unsigned index, Cursor quote) -> NextMember
{
    if (index == JSFinalObject::maxInlineCapacity) [[unlikely]]
        return { };
    NextMember next = findKnownMember(from, index, quote);
    if (!next.structure)
        next = findOrAddMember(from, index, quote);
    return next.value < m_sourceEnd ? next : NextMember { };
}

auto JSONValueParser::findKnownMember(Structure* from, unsigned index, Cursor quote) -> NextMember
{
    StructureID fromID = StructureID::encode(from);
    auto* entry = m_cache.findName(fromID, JSONCache::nameIndex(fromID), std::span<const Latin1Character> { byteCast<Latin1Character>(quote), jsonSourcePadding });
    if (!entry || entry->offset != memberPropertyOffset(index))
        return { };
    if (m_words.sourceEncoding != JSONSourceEncoding::Latin1 && !entry->textIsASCII && !isShortTextLatin1AfterDecoding(quote, entry->textLength))
        return { };
    return { entry->to.get(), quote + entry->textLength };
}

auto JSONValueParser::findOrAddMember(Structure* from, unsigned index, Cursor quote) -> NextMember
{
    ParsedName parsed = parseNameAndColon(quote);
    if (!parsed.afterColon) [[unlikely]]
        return { };
    const Identifier& name = parsed.name;
    if (parseIndex(name) || name == m_vm.propertyNames->underscoreProto) [[unlikely]]
        return { };

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
    if (to->isDictionary() || offset != memberPropertyOffset(index))
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
    return { to, parsed.afterColon, isMadeNow };
}

NEVER_INLINE JSObject* JSONValueParser::createObjectSlow(Members& members, unsigned count, Structure* structure, LearnedName& name)
{
    JSONRuntime& runtime = *m_words.runtime;
    uint32_t epoch = m_learnedNames.epoch();
    bool isLearned = name.isLearned();
    if (runtime.hasParsedRecords()) {
        JSONRuntime::RecordPlan record = runtime.recordPlanFor(m_vm, structure);
        isLearned &= m_learnedNames.epoch() == epoch;
        if (record.plan) {
            if (isLearned) {
                name.plan = record.plan;
                name.recordStructure = record.structure;
            }
            if (JSObject* object = ObjectBuilder::tryBuild(m_vm, *record.plan, record.structure, members.values, members.kinds, members.kindsOfAll)) {
                ++m_numberOfRecordsMade;
                return object;
            }
            return createOrdinaryObject(members, count, structure);
        }
        if (JSONRuntime::RecordBesideNames* cached = runtime.recordBesideNamesFor(structure)) {
            isLearned = false;
            JSONRuntime::RecordBesideNames beside = *cached;
            unsigned numberInRecord = beside.numberOfNamesInRecord;
            unsigned numberBeyondRecord = count - numberInRecord;
            auto layNamesBeyondRecord = [&](Object& row) {
                if (beside.structureOfNamesBeyondRecord) {
                    row.setExtraProperties(m_vm, createObjectOfStructure(m_vm, beside.structureOfNamesBeyondRecord, members.values + numberInRecord, numberBeyondRecord), beside.structureWithNamesBeyondRecord);
                    return true;
                }
                Vector<Object::ExtraProperty, 8> properties(numberBeyondRecord, [&](size_t index) {
                    return Object::ExtraProperty { nullptr, JSValue::decode(members.values[numberInRecord + index]) };
                });
                unsigned inlineCapacity = structure->inlineCapacity();
                structure->forEachProperty(m_vm, [&](const auto& entry) -> bool {
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
                if (JSONRuntime::RecordBesideNames* stillCached = runtime.recordBesideNamesFor(structure)) {
                    stillCached->structureOfNamesBeyondRecord = ofNamesBeyondRecord;
                    stillCached->structureWithNamesBeyondRecord = uncheckedDowncast<CompiledStructure>(row.structure());
                }
                return true;
            };
            JSObject* object = ObjectBuilder::tryBuild(m_vm, *beside.plan, beside.structure, members.values, members.kinds, members.kindsOfAll);
            if (object && layNamesBeyondRecord(*uncheckedDowncast<Object>(object))) {
                ++m_numberOfRecordsMade;
                return object;
            }
            if (JSONRuntime::RecordBesideNames* stillCached = object ? runtime.recordBesideNamesFor(structure) : nullptr)
                *stillCached = { };
        }
    }
    if (isLearned)
        name.completesOrdinaryObject = true;
    return createOrdinaryObject(members, count, structure);
}

auto JSONValueParser::parseNameAndColon(const uint8_t* quote) -> ParsedName
{
    if (*quote != '"') [[unlikely]]
        return { };
    ParsedName parsed;
    Cursor start = quote + 1;
    Cursor end = findEndOfPlainJSONCharacters(start, m_sourceEnd);
    if (end >= m_sourceEnd) [[unlikely]]
        return { };
    if (*end == '"' && isLatin1(m_words, { start, end })) [[likely]] {
        parsed.name = Identifier::fromString(m_vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(end) });
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
        if (!parsed.end || parsed.end >= m_sourceEnd) [[unlikely]]
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
        for (;;) {
            ParsedJSONValue parsed = *cursor == '{' ? parseObject(cursor) : parseValue(cursor);
            if (!parsed.end || parsed.end >= m_sourceEnd) [[unlikely]]
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
static ALWAYS_INLINE JSValue tryParse(JSGlobalObject* globalObject, size_t length, Source source, JSONTextMemory memory)
{
    Instance* instance = Instance::of(globalObject);
    if (!instance || !instance->json.runtime || !length || length > String::MaxLength)
        return { };
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSONSourceScope sourceScope(*instance, source, memory);
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

JSValue tryParseJSON(JSGlobalObject* globalObject, StringView text, JSONTextMemory memory)
{
    return tryParse(globalObject, text.length(), text, memory);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
