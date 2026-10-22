/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONValueParser.h"

#if ENABLE(AOT)

#include "AOTJSONHooks.h"
#include "AOTJSONRuntime.h"
#include "AOTObjectBuilderInlines.h"
#include "DeferTermination.h"
#include "GCMemoryOperations.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "ObjectInitializationScope.h"
#include "TopExceptionScope.h"
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

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

ParsedJSONValue JSONValueParser::parseNumber(Cursor cursor)
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

ParsedJSONValue JSONValueParser::parseObject(Cursor brace, NamesOfElementInFront* inFront)
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

ParsedJSONValue JSONValueParser::parseArray(Cursor bracket)
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
