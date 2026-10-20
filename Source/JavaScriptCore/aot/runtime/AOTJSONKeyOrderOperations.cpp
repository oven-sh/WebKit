/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONKeyOrder.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTJSONRuntime.h"
#include "AOTJSONScanner.h"
#include "AOTJSONValueParser.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "JSONCacheInlines.h"
#include "TopExceptionScope.h"
#include <optional>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

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
    Object::KeyOrderStorage keys(entriesOf(instance, object.layout(), order));
    std::optional<Object::InitialKeyOrder> initial = object.initialKeyOrder(vm, keys.span(), extraProperties);
    if (!initial)
        return false;
    object.setInitialKeyOrder(vm, *initial, extraProperties);
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
    if (before->isDictionary() || after->isDictionary() || after->previousID() != before)
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
    Structure* before = properties->structure();
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
    if (!entry || (words.sourceEncoding != JSONSourceEncoding::Latin1 && !isShortTextLatin1AfterDecoding(words, quote, entry->textLength)))
        return { };
    Structure* after = entry->to.unvalidatedGet();
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
