/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTDateTimeWriter.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "AOTTypedThreads.h"
#include "DateInstance.h"
#include "JSCInlines.h"
#include "JSONObject.h"
#include <charconv>
#include <wtf/MathExtras.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/dragonbox/dragonbox_to_chars.h>
#include <wtf/text/EscapedFormsForJSON.h>
#include <wtf/text/StringBuilderJSON.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr size_t minJSONBufferCapacity = 4 * KB;
static constexpr size_t maxRetainedJSONBufferCapacity = 64 * KB;
static constexpr unsigned jsonBufferSpareCapacity = 64;
static constexpr uint32_t jsonValueIsWholeOutput = 2;
static constexpr uintptr_t jsonWroteNothing = 1;
static constexpr unsigned maxJSONInt32Length = 11;
static constexpr unsigned maxJSONIntegerLength = 20;
static constexpr unsigned maxJSONNumberLength = WTF::dragonbox::max_string_length<WTF::dragonbox::ieee754_binary64>();
static constexpr unsigned maxLengthOfDateInJSON = maxISODateTimeLength + 2;
static_assert(maxJSONNumberLength >= maxJSONInt32Length && maxJSONNumberLength + 2 <= jsonBufferSpareCapacity && maxLengthOfDateInJSON <= jsonBufferSpareCapacity);

struct JSONWideSegment {
    size_t offset;
    String text;
    bool needsEscaping;
};

static NEVER_INLINE Latin1Character* appendWideSegment(Instance* instance, Latin1Character* cursor, StringImpl* text, bool needsEscaping)
{
    if (!instance->jsonWideSegments)
        instance->jsonWideSegments = new Vector<JSONWideSegment>();
    auto& segments = *instance->jsonWideSegments;
    segments.shrink(std::min<size_t>(segments.size(), instance->jsonWideSegmentCount));
    segments.append(JSONWideSegment { static_cast<size_t>(cursor - instance->jsonBufferStart), String { text }, needsEscaping });
    instance->jsonWideSegmentCount = segments.size();
    return cursor;
}

static NEVER_INLINE Latin1Character* growJSONBuffer(Instance* instance, Latin1Character* cursor, size_t size)
{
    countOperationNamed(instance, __func__);
    size_t length = cursor - instance->jsonBufferStart;
    size_t capacity = instance->jsonBufferEnd - instance->jsonBufferStart;
    if (size > String::MaxLength || length + size > String::MaxLength)
        return nullptr;
    size_t newCapacity = std::max({ capacity * 2, length + size, minJSONBufferCapacity });
    if (newCapacity > maxRetainedJSONBufferCapacity)
        newCapacity = std::max<size_t>(newCapacity, instance->jsonBufferCapacityHint);
    void* storage = nullptr;
    if (!WTF::tryFastRealloc(instance->jsonBufferStart, newCapacity).getValue(storage))
        return nullptr;
    instance->jsonBufferStart = static_cast<Latin1Character*>(storage);
    instance->jsonBufferEnd = instance->jsonBufferStart + newCapacity;
    return instance->jsonBufferStart + length;
}

static ALWAYS_INLINE Latin1Character* reserveInJSONBuffer(Instance* instance, Latin1Character* cursor, size_t size)
{
    if (static_cast<size_t>(instance->jsonBufferEnd - cursor) >= size) [[likely]]
        return cursor;
    return growJSONBuffer(instance, cursor, size);
}

static ALWAYS_INLINE bool needsEscapeInJSON(uint64_t word)
{
    return hasZeroByte(word & 0xE0E0E0E0E0E0E0E0ULL) || hasZeroByte(word ^ 0x2222222222222222ULL) || hasZeroByte(word ^ 0x5C5C5C5C5C5C5C5CULL);
}

static ALWAYS_INLINE bool copyAndCheckForEscapes(std::span<const Latin1Character> characters, Latin1Character* to)
{
    const Latin1Character* from = characters.data();
    size_t size = characters.size();
    constexpr size_t stride = SIMD::stride<uint8_t>;
    if (size >= stride) {
        constexpr auto quote = SIMD::splat<uint8_t>('"');
        constexpr auto backslash = SIMD::splat<uint8_t>('\\');
        constexpr auto space = SIMD::splat<uint8_t>(' ');
        decltype(SIMD::load(static_cast<const uint8_t*>(nullptr))) found { };
        auto take = [&](size_t offset) ALWAYS_INLINE_LAMBDA {
            auto input = SIMD::load(std::bit_cast<const uint8_t*>(from + offset));
            SIMD::store(input, std::bit_cast<uint8_t*>(to + offset));
            found = SIMD::bitOr(found, SIMD::equal(input, quote), SIMD::equal(input, backslash), SIMD::lessThan(input, space));
        };
        size_t offset = 0;
        for (; offset + stride <= size; offset += stride)
            take(offset);
        if (offset < size)
            take(size - stride);
        return SIMD::isNonZero(found);
    }
    if (size >= 8) {
        uint64_t first = WTF::unalignedLoad<uint64_t>(from);
        uint64_t last = WTF::unalignedLoad<uint64_t>(from + size - 8);
        WTF::unalignedStore<uint64_t>(to, first);
        WTF::unalignedStore<uint64_t>(to + size - 8, last);
        return needsEscapeInJSON(first) || needsEscapeInJSON(last);
    }
    if (size >= 4) {
        uint32_t first = WTF::unalignedLoad<uint32_t>(from);
        uint32_t last = WTF::unalignedLoad<uint32_t>(from + size - 4);
        WTF::unalignedStore<uint32_t>(to, first);
        WTF::unalignedStore<uint32_t>(to + size - 4, last);
        return needsEscapeInJSON(first | static_cast<uint64_t>(last) << 32);
    }
    bool found = false;
    for (size_t i = 0; i < size; ++i) {
        found |= !!WTF::escapedFormsForJSON[from[i]];
        to[i] = from[i];
    }
    return found;
}

static NEVER_INLINE Latin1Character* appendEscapedCharacters(Instance* instance, Latin1Character* cursor, std::span<const Latin1Character> characters)
{
    cursor = reserveInJSONBuffer(instance, cursor, characters.size() * 6 + jsonBufferSpareCapacity);
    if (!cursor)
        return nullptr;
    std::span<Latin1Character> output { cursor, characters.size() * 6 };
    WTF::appendEscapedJSONStringContent(output, characters);
    return output.data();
}

static ALWAYS_INLINE Latin1Character* appendCharacters(Instance* instance, Latin1Character* cursor, const StringImpl& string)
{
    if (!string.is8Bit()) [[unlikely]]
        return appendWideSegment(instance, cursor, const_cast<StringImpl*>(&string), true);
    auto characters = string.span8();
    cursor = reserveInJSONBuffer(instance, cursor, characters.size() + jsonBufferSpareCapacity);
    if (!cursor) [[unlikely]]
        return nullptr;
    if (copyAndCheckForEscapes(characters, cursor)) [[unlikely]]
        return appendEscapedCharacters(instance, cursor, characters);
    return cursor + characters.size();
}

static NEVER_INLINE Latin1Character* appendRopeCharacters(Instance* instance, CallFrame* callFrame, Latin1Character* cursor, JSString* string)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto value = string->tryGetValue();
    if (value.data.isNull())
        return nullptr;
    return appendCharacters(instance, cursor, *value.data.impl());
}

static constexpr char jsonDigitPairs[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

static ALWAYS_INLINE Latin1Character* appendInteger(Latin1Character* cursor, int64_t integer)
{
    if (static_cast<uint64_t>(integer) < 10000) [[likely]] {
        uint32_t value = static_cast<uint32_t>(integer);
        uint32_t high = value / 100;
        uint32_t low = value % 100;
        if (value < 100) {
            if (value < 10) {
                *cursor = '0' + value;
                return cursor + 1;
            }
            memcpy(cursor, jsonDigitPairs + 2 * low, 2);
            return cursor + 2;
        }
        if (value < 1000) {
            *cursor = '0' + high;
            memcpy(cursor + 1, jsonDigitPairs + 2 * low, 2);
            return cursor + 3;
        }
        memcpy(cursor, jsonDigitPairs + 2 * high, 2);
        memcpy(cursor + 2, jsonDigitPairs + 2 * low, 2);
        return cursor + 4;
    }
    char* to = std::bit_cast<char*>(cursor);
    return std::bit_cast<Latin1Character*>(std::to_chars(to, to + maxJSONIntegerLength, integer).ptr);
}

static ALWAYS_INLINE Latin1Character* appendLiteral(Latin1Character* cursor, ASCIILiteral text)
{
    memcpy(cursor, text.characters(), text.length());
    return cursor + text.length();
}

static ALWAYS_INLINE Latin1Character* appendNumber(Latin1Character* cursor, double number)
{
    if (auto int32 = tryConvertToStrictInt32(number))
        return appendInteger(cursor, *int32);
    if (!std::isfinite(number))
        return appendLiteral(cursor, "null"_s);
    if (!number)
        return appendLiteral(cursor, "0"_s);
    return std::bit_cast<Latin1Character*>(WTF::dragonbox::detail::to_chars_n<WTF::dragonbox::Mode::ToShortest>(number, std::span { std::bit_cast<char*>(cursor), maxJSONNumberLength }).data());
}

static NEVER_INLINE bool lookForToJSON(JSGlobalObject* globalObject, VM& vm, JSObject* object)
{
    if (object->noSideEffectMayHaveNonIndexProperty(vm, vm.propertyNames->toJSON))
        return true;
    PropertySlot slot { object, PropertySlot::InternalMethodType::Get };
    object->structure()->cacheSpecialProperty(globalObject, vm, jsUndefined(), CachedSpecialPropertyKey::ToJSON, slot);
    return false;
}

static ALWAYS_INLINE bool mayHaveToJSON(JSGlobalObject* globalObject, VM& vm, JSObject* object)
{
    if (JSValue function = object->structure()->cachedSpecialProperty(CachedSpecialPropertyKey::ToJSON)) [[likely]]
        return !function.isUndefined();
    return lookForToJSON(globalObject, vm, object);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONReserve, void*, (Instance* instance, uint64_t cursor, uint64_t size))
{
    return growJSONBuffer(instance, std::bit_cast<Latin1Character*>(cursor), size);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendString, void*, (Instance* instance, uint64_t cursor, JSCell* cell))
{
    auto* string = uncheckedDowncast<JSString>(cell);
    if (const StringImpl* impl = string->tryGetValueImpl()) [[likely]]
        return appendCharacters(instance, std::bit_cast<Latin1Character*>(cursor), *impl);
    return appendRopeCharacters(instance, DECLARE_CALL_FRAME(*instance->vm), std::bit_cast<Latin1Character*>(cursor), string);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendInteger, void*, (uint64_t cursor, uint64_t integer))
{
    return appendInteger(std::bit_cast<Latin1Character*>(cursor), static_cast<int64_t>(integer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendNumber, void*, (uint64_t cursor, double number))
{
    return appendNumber(std::bit_cast<Latin1Character*>(cursor), number);
}

static ALWAYS_INLINE Latin1Character* appendTimeValue(Latin1Character* cursor, double timeValue)
{
    if (!std::isfinite(timeValue)) [[unlikely]]
        return appendLiteral(cursor, "null"_s);
    *cursor++ = '"';
    cursor = writeISODateTime(cursor, timeValue);
    *cursor++ = '"';
    return cursor;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendDate, void*, (uint64_t cursor, double timeValue))
{
    return appendTimeValue(std::bit_cast<Latin1Character*>(cursor), timeValue);
}

static Latin1Character* appendDate(JSGlobalObject* globalObject, Latin1Character* cursor, DateInstance& date)
{
    if (date.structure() != globalObject->dateStructure() || !globalObject->datePrototype()->structure()->hasImmutableProperties())
        return nullptr;
    return appendTimeValue(cursor, date.internalNumber());
}

static Latin1Character* appendStringified(Instance* instance, Latin1Character* cursor, std::span<const Latin1Character> characters, const WideStringsInJSON& wideStrings)
{
    cursor = reserveInJSONBuffer(instance, cursor, characters.size() + jsonBufferSpareCapacity);
    if (!cursor)
        return nullptr;
    memcpy(cursor, characters.data(), characters.size());
    for (auto& wideString : wideStrings)
        appendWideSegment(instance, cursor + wideString.offset, wideString.string.impl(), true);
    return cursor + characters.size();
}

struct JSONPlanCache {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(JSONPlanCache);
    struct Entry {
        uint64_t orderAndShapeWords { 0 };
        const Typed::State::KeysAsJSON* keys { nullptr };
        uint32_t structureID { 0 };
    };
    static constexpr unsigned size = 64;
    std::array<Entry, size> entries;
    struct WithExtraProperties {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WithExtraProperties);
        StructureID ofExtraProperties;
        uint32_t shapeWord { 0 };
        uint32_t orderAndCount { 0 };
        Typed::State::KeysAsJSON keys;
    };
    static constexpr unsigned maxNumberWithExtraProperties = 1 << 14;
    UncheckedKeyHashMap<uint64_t, std::unique_ptr<WithExtraProperties>> withExtraProperties;
    std::array<WithExtraProperties*, size> recentWithExtraProperties { };
};

static constexpr unsigned maxJSONWalkDepth = 48;
static Latin1Character* const jsonWalkGaveUp = std::bit_cast<Latin1Character*>(static_cast<uintptr_t>(1));

static Latin1Character* appendObject(Instance*, JSGlobalObject*, VM&, Latin1Character* cursor, JSObject*, unsigned depth);

static NEVER_INLINE const Typed::State::KeysAsJSON* findJSONPlan(VM& vm, JSObject* cell, JSONPlanCache::Entry& entry)
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    if (object->hasFlagsInOrderWord() || object->structure()->hasPolyProto())
        return nullptr;
    auto& state = object->state();
    auto properties = state.propertiesInOrder(vm, object->layout(), object->orderWord());
    if (!properties || std::ranges::any_of(*properties, [&](auto& property) { return property.name == vm.propertyNames->toJSON.impl(); }))
        return nullptr;
    auto* keys = state.keysAsJSON(vm, object->layout(), object->orderWord());
    if (keys)
        entry = { object->orderAndShapeWords(), keys, object->structureID().bits() };
    return keys;
}

static ALWAYS_INLINE const Typed::State::KeysAsJSON* jsonPlanFor(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* object)
{
    Structure* structure = object->structure();
    if (structure->classInfoForCells() != Typed::Object::info())
        return nullptr;
    if (!instance->jsonPlanCache) [[unlikely]]
        instance->jsonPlanCache = new JSONPlanCache();
    uint64_t words = WTF::unalignedLoad<uint64_t>(std::bit_cast<const uint8_t*>(object) + Typed::offsetOfOrderWord);
    auto& entry = instance->jsonPlanCache->entries[(static_cast<uint32_t>(words >> 32) + static_cast<uint32_t>(words)) % JSONPlanCache::size];
    const Typed::State::KeysAsJSON* keys = entry.keys;
    if (entry.orderAndShapeWords != words || entry.structureID != object->structureID().bits()) [[unlikely]] {
        keys = findJSONPlan(vm, object, entry);
        if (!keys)
            return nullptr;
    }
    JSValue prototype = structure->storedPrototype();
    if (prototype != globalObject->objectPrototype() && prototype.isObject() && mayHaveToJSON(globalObject, vm, asObject(prototype))) [[unlikely]]
        return nullptr;
    return keys;
}

static ALWAYS_INLINE Latin1Character* appendQuoted(Instance* instance, Latin1Character* cursor, JSString* string)
{
    const StringImpl* impl = string->tryGetValueImpl();
    if (!impl) [[unlikely]] {
        auto value = string->tryGetValue();
        if (value.data.isNull())
            return nullptr;
        impl = value.data.impl();
    }
    *cursor++ = '"';
    cursor = appendCharacters(instance, cursor, *impl);
    if (!cursor) [[unlikely]]
        return nullptr;
    *cursor++ = '"';
    return cursor;
}

static ALWAYS_INLINE Latin1Character* appendPrimitive(Latin1Character* cursor, JSValue value)
{
    if (value.isNumber())
        return appendNumber(cursor, value.asNumber());
    if (value.isBoolean())
        return value.isTrue() ? appendLiteral(cursor, "true"_s) : appendLiteral(cursor, "false"_s);
    return value.isNull() ? appendLiteral(cursor, "null"_s) : nullptr;
}

struct JSONPlan {
    const Typed::State::KeysAsJSON::Key* begin { nullptr };
    const Typed::State::KeysAsJSON::Key* end { nullptr };
    const Latin1Character* text { nullptr };
    size_t fixedSize { 0 };
};

static ALWAYS_INLINE JSONPlan jsonPlanOf(const Typed::State::KeysAsJSON& keys)
{
    auto span = keys.keys.span();
    return { span.data(), span.data() + span.size(), keys.text.span().data(), keys.text.size() + span.size() * maxJSONNumberLength + 2 * jsonBufferSpareCapacity };
}

static constexpr uint32_t readsExtraProperty = (static_cast<uint32_t>(Typed::Object::ReadKind::General) + 1) << Typed::Object::readKindShift;

static ALWAYS_INLINE Latin1Character* writeTypedObject(Instance* instance, JSGlobalObject* globalObject, VM& vm, Latin1Character* cursor, Typed::Object& object, const JSONPlan& plan, unsigned depth, JSObject* extraProperties = nullptr)
{
    using ReadKind = Typed::Object::ReadKind;
    constexpr unsigned copyStride = Typed::State::KeysAsJSON::copyStride;
    constexpr uint32_t offsetMask = (1u << Typed::Object::readKindShift) - 1;
    constexpr auto firstEncodingFor = [](ReadKind kind) { return static_cast<uint32_t>(kind) << Typed::Object::readKindShift; };
    static_assert(!firstEncodingFor(ReadKind::Int32OrNumberBox));
    size_t fixedSize = plan.fixedSize;
    cursor = reserveInJSONBuffer(instance, cursor, fixedSize);
    if (!cursor) [[unlikely]]
        return nullptr;
    size_t start = cursor - instance->jsonBufferStart;
    const uint8_t* body = std::bit_cast<const uint8_t*>(&object);
    const Latin1Character* text = plan.text;
    auto appendKey = [&](const Typed::State::KeysAsJSON::Key& key) ALWAYS_INLINE_LAMBDA {
        const Latin1Character* from = text + key.start;
        memcpy(cursor, from, copyStride);
        for (unsigned i = copyStride; i < key.length; i += copyStride)
            memcpy(cursor + i, from + i, copyStride);
        cursor += key.length;
    };
    for (auto* next = plan.begin; next != plan.end; ++next) {
        auto& key = *next;
        uint32_t offset = key.readKindAndOffset & offsetMask;
        auto field = [&]() ALWAYS_INLINE_LAMBDA -> const Typed::FieldData& { return object.layout().fields()[key.fieldIndex]; };
        JSValue value;
        if (key.readKindAndOffset < firstEncodingFor(ReadKind::Cell32)) {
            if (int32_t word = WTF::unalignedLoad<int32_t>(body + offset); word >= Typed::minInlineInt32) [[likely]] {
                appendKey(key);
                cursor = appendInteger(cursor, word);
                continue;
            }
            value = object.read(field());
        } else if (key.readKindAndOffset < firstEncodingFor(ReadKind::Double)) {
            uint32_t word = WTF::unalignedLoad<uint32_t>(body + offset);
            if (word == Typed::unmaterializedClosureCell32) [[unlikely]]
                value = object.read(field());
            else if (word <= Typed::undefinedCell32) [[unlikely]]
                value = JSValue::decode(word);
            else
                value = MarkedBlockCage::decode(word);
        } else if (key.readKindAndOffset < firstEncodingFor(ReadKind::Boolean)) {
            appendKey(key);
            cursor = appendNumber(cursor, WTF::unalignedLoad<double>(body + offset));
            continue;
        } else if (key.readKindAndOffset < firstEncodingFor(ReadKind::JSValue)) {
            bool isTrue = body[offset];
            appendKey(key);
            memcpy(cursor, isTrue ? "true\0\0\0" : "false\0\0", 8);
            cursor += 5 - isTrue;
            continue;
        } else if (key.readKindAndOffset < firstEncodingFor(ReadKind::Int32))
            value = JSValue::decode(WTF::unalignedLoad<EncodedJSValue>(body + offset));
        else if (key.readKindAndOffset < firstEncodingFor(ReadKind::General)) {
            appendKey(key);
            cursor = appendInteger(cursor, WTF::unalignedLoad<int32_t>(body + offset));
            continue;
        } else if (key.readKindAndOffset < readsExtraProperty)
            value = object.read(field());
        else
            value = extraProperties->getDirect(static_cast<PropertyOffset>(offset));
        if (!value || value.isUndefined())
            continue;
        appendKey(key);
        if (!value.isCell()) {
            cursor = value.isInt32() ? appendInteger(cursor, value.asInt32()) : appendPrimitive(cursor, value);
            if (!cursor) [[unlikely]]
                return nullptr;
            continue;
        }
        JSCell* cell = value.asCell();
        if (cell->type() == StringType) [[likely]]
            cursor = appendQuoted(instance, cursor, uncheckedDowncast<JSString>(cell));
        else if (cell->type() >= ObjectType)
            cursor = appendObject(instance, globalObject, vm, cursor, uncheckedDowncast<JSObject>(cell), depth + 1);
        else
            return nullptr;
        if (!cursor) [[unlikely]]
            return nullptr;
        cursor = reserveInJSONBuffer(instance, cursor, fixedSize);
        if (!cursor) [[unlikely]]
            return nullptr;
    }
    Latin1Character* first = instance->jsonBufferStart + start;
    cursor += first == cursor;
    *first = '{';
    *cursor++ = '}';
    return cursor;
}

static NEVER_INLINE Latin1Character* appendTypedObject(Instance* instance, JSGlobalObject* globalObject, VM& vm, Latin1Character* cursor, Typed::Object& object, const Typed::State::KeysAsJSON& keys, unsigned depth)
{
    return writeTypedObject(instance, globalObject, vm, cursor, object, jsonPlanOf(keys), depth);
}

static NEVER_INLINE JSONPlanCache::WithExtraProperties* makeJSONPlanWithExtraProperties(Instance* instance, VM& vm, Typed::Object& object, JSObject* extra, uint64_t hash, unsigned depth)
{
    auto& plans = instance->jsonPlanCache->withExtraProperties;
    if (plans.size() >= JSONPlanCache::maxNumberWithExtraProperties) {
        if (depth)
            return nullptr;
        plans.clear();
        instance->jsonPlanCache->recentWithExtraProperties.fill(nullptr);
    }
    if (depth && plans.contains(hash))
        return nullptr;
    auto* properties = hasIndexedProperties(extra->indexingType()) ? nullptr : object.fieldsAndExtraPropertiesInKeyOrder(vm);
    if (!properties)
        return nullptr;
    auto newPlan = makeUnique<JSONPlanCache::WithExtraProperties>();
    auto& keys = newPlan->keys;
    const Typed::FieldData* fields = object.layout().fields();
    for (auto& property : *properties) {
        if (property.name->isSymbol())
            continue;
        if (!property.name->is8Bit() || property.name == vm.propertyNames->toJSON.impl())
            return nullptr;
        auto name = property.name->span8();
        for (Latin1Character character : name) {
            if (character < 0x20 || character == '"' || character == '\\')
                return nullptr;
        }
        uint32_t start = keys.text.size();
        keys.text.append(',');
        keys.text.append('"');
        keys.text.append(name);
        keys.text.append('"');
        keys.text.append(':');
        uint32_t length = keys.text.size() - start;
        if (property.field)
            keys.keys.append({ Typed::Object::readKindAndOffset(*property.field), static_cast<uint32_t>(property.field - fields), start, length });
        else
            keys.keys.append({ readsExtraProperty | static_cast<uint32_t>(property.offsetInExtraProperties), 0, start, length });
    }
    for (unsigned i = 0; i < Typed::State::KeysAsJSON::copyStride; ++i)
        keys.text.append(0);
    newPlan->ofExtraProperties = extra->structureID();
    newPlan->shapeWord = Typed::shapeWordOf(object.shape());
    newPlan->orderAndCount = object.orderWord() & Typed::orderAndCountMask;
    return plans.set(hash, WTF::move(newPlan)).iterator->value.get();
}

static NEVER_INLINE Latin1Character* appendTypedObjectWithExtraProperties(Instance* instance, JSGlobalObject* globalObject, VM& vm, Latin1Character* cursor, JSObject* cell, unsigned depth)
{
    Structure* structure = cell->structure();
    if (structure->classInfoForCells() != Typed::Object::info() || structure->hasPolyProto())
        return nullptr;
    auto& object = *uncheckedDowncast<Typed::Object>(cell);
    JSObject* extra = object.extraProperties();
    if (!extra)
        return nullptr;
    JSValue prototype = structure->storedPrototype();
    if (prototype != globalObject->objectPrototype() && prototype.isObject() && mayHaveToJSON(globalObject, vm, asObject(prototype))) [[unlikely]]
        return nullptr;
    uint32_t orderWord = object.orderWord();
    if (orderWord & ~(Typed::orderAndCountMask | Typed::orderWordHasExtraProperties)) [[unlikely]]
        return nullptr;
    if (!instance->jsonPlanCache) [[unlikely]]
        instance->jsonPlanCache = new JSONPlanCache();
    uint32_t orderAndCount = orderWord & Typed::orderAndCountMask;
    uint32_t shapeWord = Typed::shapeWordOf(object.shape());
    StructureID ofExtraProperties = extra->structureID();
    uint64_t hash = (static_cast<uint64_t>(ofExtraProperties.bits()) << 32 | orderAndCount) * 0x9E3779B97F4A7C15ULL ^ shapeWord;
    hash = std::max<uint64_t>(hash, 1) - (hash == std::numeric_limits<uint64_t>::max());
    auto describes = [&](JSONPlanCache::WithExtraProperties* plan) ALWAYS_INLINE_LAMBDA {
        return plan && plan->ofExtraProperties == ofExtraProperties && plan->shapeWord == shapeWord && plan->orderAndCount == orderAndCount;
    };
    auto& recent = instance->jsonPlanCache->recentWithExtraProperties[(hash >> 32) % JSONPlanCache::size];
    if (!describes(recent)) [[unlikely]] {
        auto found = instance->jsonPlanCache->withExtraProperties.find(hash);
        auto* plan = found == instance->jsonPlanCache->withExtraProperties.end() ? nullptr : found->value.get();
        if (!describes(plan))
            plan = makeJSONPlanWithExtraProperties(instance, vm, object, extra, hash, depth);
        if (!plan)
            return nullptr;
        recent = plan;
    }
    return writeTypedObject(instance, globalObject, vm, cursor, object, jsonPlanOf(recent->keys), depth, extra);
}

static ALWAYS_INLINE bool isArrayOrInstanceOfSubclass(JSCell* cell, JSType type)
{
    return type == ArrayType || (type == DerivedArrayType && cell->classInfo() == JSArray::info());
}

static ALWAYS_INLINE bool isInstanceOfSubclassWithoutToJSON(JSGlobalObject* globalObject, VM& vm, JSArray& array)
{
    Structure* structure = array.structure();
    unsigned attributes;
    if (array.type() != DerivedArrayType || structure->hasPolyProto() || isValidOffset(structure->get(vm, vm.propertyNames->toJSON, attributes)))
        return false;
    JSValue prototype = structure->storedPrototype();
    return !prototype.isObject() || !mayHaveToJSON(globalObject, vm, asObject(prototype));
}

static NEVER_INLINE Latin1Character* appendElementsByEngine(Instance* instance, JSGlobalObject* globalObject, Latin1Character* cursor, JSArray& array)
{
    cursor = reserveInJSONBuffer(instance, cursor, 2 * jsonBufferSpareCapacity);
    if (!cursor)
        return nullptr;
    *cursor++ = '[';
    WideStringsInJSON wideStrings;
    bool isWritten = tryStringifyElementsWithoutSideEffects(*globalObject, array, 0, wideStrings, [&](std::span<const Latin1Character> characters) {
        cursor = appendStringified(instance, cursor, characters, wideStrings);
    });
    if (!isWritten || !cursor)
        return nullptr;
    *cursor++ = ']';
    return cursor;
}

static NEVER_INLINE Latin1Character* appendArray(Instance* instance, JSGlobalObject* globalObject, VM& vm, Latin1Character* cursor, JSArray& array, unsigned depth)
{
    if (!globalObject->isOriginalArrayStructure(array.structure()) && !isInstanceOfSubclassWithoutToJSON(globalObject, vm, array)) [[unlikely]]
        return nullptr;
    unsigned length = array.length();
    IndexingType shape = array.indexingType() & IndexingShapeMask;
    cursor = reserveInJSONBuffer(instance, cursor, 2 * jsonBufferSpareCapacity);
    if (!cursor) [[unlikely]]
        return nullptr;
    if (!length) {
        if (shape == ArrayStorageShape || shape == SlowPutArrayStorageShape)
            return nullptr;
        return appendLiteral(cursor, "[]"_s);
    }
    *cursor++ = '[';
    switch (shape) {
    case Int32Shape:
    case ContiguousShape: {
        bool hasPlan = false;
        JSONPlan plan;
        uint64_t lastWords = 0;
        uint32_t lastStructureID = 0;
        for (unsigned i = 0; i < length; ++i) {
            JSValue value = array.butterfly()->contiguous().at(&array, i).get();
            if (!value) [[unlikely]]
                return nullptr;
            if (value.isCell()) {
                JSCell* cell = value.asCell();
                JSType type = cell->type();
                if (type == FinalObjectType)
                    return nullptr;
                if (type == ObjectType) [[likely]] {
                    auto* object = uncheckedDowncast<JSObject>(cell);
                    uint64_t words = WTF::unalignedLoad<uint64_t>(std::bit_cast<const uint8_t*>(object) + Typed::offsetOfOrderWord);
                    uint32_t structureID = object->structureID().bits();
                    if (!hasPlan || words != lastWords || structureID != lastStructureID) [[unlikely]] {
                        auto* keys = jsonPlanFor(instance, globalObject, vm, object);
                        hasPlan = keys;
                        if (keys)
                            plan = jsonPlanOf(*keys);
                        lastWords = words;
                        lastStructureID = structureID;
                    }
                    cursor = hasPlan ? writeTypedObject(instance, globalObject, vm, cursor, *uncheckedDowncast<Typed::Object>(object), plan, depth + 1) : appendObject(instance, globalObject, vm, cursor, object, depth + 1);
                } else if (isArrayOrInstanceOfSubclass(cell, type)) {
                    if (depth + 1 >= maxJSONWalkDepth) [[unlikely]]
                        return jsonWalkGaveUp;
                    cursor = appendArray(instance, globalObject, vm, cursor, *uncheckedDowncast<JSArray>(cell), depth + 1);
                    if (!cursor || cursor == jsonWalkGaveUp) [[unlikely]]
                        return cursor;
                } else if (type >= ObjectType)
                    cursor = appendObject(instance, globalObject, vm, cursor, uncheckedDowncast<JSObject>(cell), depth + 1);
                else if (cell->type() == StringType)
                    cursor = appendQuoted(instance, cursor, uncheckedDowncast<JSString>(cell));
                else
                    return nullptr;
                if (!cursor) [[unlikely]]
                    return jsonWalkGaveUp;
            } else if (value.isInt32())
                cursor = appendInteger(cursor, value.asInt32());
            else
                cursor = value.isUndefined() ? appendLiteral(cursor, "null"_s) : appendPrimitive(cursor, value);
            if (!cursor) [[unlikely]]
                return nullptr;
            *cursor++ = ',';
            cursor = reserveInJSONBuffer(instance, cursor, 2 * jsonBufferSpareCapacity);
            if (!cursor) [[unlikely]]
                return nullptr;
        }
        break;
    }
    case DoubleShape: {
        for (unsigned i = 0; i < length; ++i) {
            double value = array.butterfly()->contiguousDouble().at(&array, i);
            if (value != value) [[unlikely]]
                return nullptr;
            cursor = appendNumber(cursor, value);
            *cursor++ = ',';
            cursor = reserveInJSONBuffer(instance, cursor, 2 * jsonBufferSpareCapacity);
            if (!cursor) [[unlikely]]
                return nullptr;
        }
        break;
    }
    default:
        return nullptr;
    }
    cursor[-1] = ']';
    return cursor;
}

static Latin1Character* appendObjectGeneric(Instance* instance, JSGlobalObject* globalObject, Latin1Character* cursor, JSObject* object)
{
    if (object->type() == JSDateType) {
        cursor = reserveInJSONBuffer(instance, cursor, 2 * jsonBufferSpareCapacity);
        return cursor ? appendDate(globalObject, cursor, *uncheckedDowncast<DateInstance>(object)) : nullptr;
    }
    WideStringsInJSON wideStrings;
    bool isWritten = tryStringifyWithoutSideEffects(*globalObject, object, wideStrings, [&](std::span<const Latin1Character> characters) {
        cursor = appendStringified(instance, cursor, characters, wideStrings);
    });
    return isWritten ? cursor : nullptr;
}

static ALWAYS_INLINE bool isListWithPlainObjectNearStart(JSValue value)
{
    if (!value || !value.isCell() || value.asCell()->type() != ArrayType)
        return false;
    auto* array = uncheckedDowncast<JSArray>(value.asCell());
    if ((array->indexingType() & IndexingShapeMask) != ContiguousShape)
        return false;
    unsigned examined = std::min(array->length(), 8u);
    for (unsigned i = 0; i < examined; ++i) {
        JSValue element = array->butterfly()->contiguous().at(array, i).get();
        if (!element || !element.isCell())
            continue;
        JSType type = element.asCell()->type();
        if (type == FinalObjectType)
            return true;
        if (type != ArrayType)
            continue;
        auto* inner = uncheckedDowncast<JSArray>(element.asCell());
        if ((inner->indexingType() & IndexingShapeMask) != ContiguousShape || !inner->length())
            continue;
        JSValue first = inner->butterfly()->contiguous().at(inner, 0).get();
        if (first && first.isCell() && first.asCell()->type() == FinalObjectType)
            return true;
    }
    return false;
}

static Latin1Character* appendObject(Instance* instance, JSGlobalObject* globalObject, VM& vm, Latin1Character* cursor, JSObject* object, unsigned depth)
{
    if (depth >= maxJSONWalkDepth) [[unlikely]]
        return nullptr;
    JSType type = object->type();
    if (type == ObjectType) {
        if (auto* keys = jsonPlanFor(instance, globalObject, vm, object)) [[likely]]
            return appendTypedObject(instance, globalObject, vm, cursor, *uncheckedDowncast<Typed::Object>(object), *keys, depth);
        size_t offset = cursor - instance->jsonBufferStart;
        uint32_t wideSegmentCount = instance->jsonWideSegmentCount;
        if (Latin1Character* end = appendTypedObjectWithExtraProperties(instance, globalObject, vm, cursor, object, depth))
            return end;
        cursor = instance->jsonBufferStart + offset;
        instance->jsonWideSegmentCount = wideSegmentCount;
    } else if (isArrayOrInstanceOfSubclass(object, type)) {
        auto& array = *uncheckedDowncast<JSArray>(object);
        size_t offset = cursor - instance->jsonBufferStart;
        uint32_t wideSegmentCount = instance->jsonWideSegmentCount;
        Latin1Character* end = appendArray(instance, globalObject, vm, cursor, array, depth);
        if (end == jsonWalkGaveUp) [[unlikely]]
            return nullptr;
        if (end) [[likely]]
            return end;
        cursor = instance->jsonBufferStart + offset;
        instance->jsonWideSegmentCount = wideSegmentCount;
        if (isInstanceOfSubclassWithoutToJSON(globalObject, vm, array))
            return appendElementsByEngine(instance, globalObject, cursor, array);
    }
    return appendObjectGeneric(instance, globalObject, cursor, object);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendValue, void*, (Instance* instance, uint64_t cursorBits, EncodedJSValue encodedValue, uint32_t isElement))
{
    if (isElement == jsonValueIsWholeOutput && isListWithPlainObjectNearStart(JSValue::decode(encodedValue)))
        return std::bit_cast<void*>(jsonWroteNothing);
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    ASSERT(isElement != jsonValueIsWholeOutput || !instance->jsonWideSegmentCount);
    auto* cursor = std::bit_cast<Latin1Character*>(cursorBits);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isCell()) {
        if (!value)
            return nullptr;
        JSType type = value.asCell()->type();
        if (type >= ObjectType && isElement == jsonValueIsWholeOutput) {
            if (type == ObjectType) {
                if (auto* keys = jsonPlanFor(instance, globalObject, vm, asObject(value)))
                    return appendTypedObject(instance, globalObject, vm, cursor, *uncheckedDowncast<Typed::Object>(value.asCell()), *keys, 0);
                Latin1Character* start = instance->jsonBufferStart;
                Latin1Character* limit = instance->jsonBufferEnd;
                if (Latin1Character* end = appendTypedObjectWithExtraProperties(instance, globalObject, vm, cursor, asObject(value), 0))
                    return end;
                return instance->jsonBufferStart == start && instance->jsonBufferEnd == limit && !instance->jsonWideSegmentCount && !(instance->jsonWideSegments && instance->jsonWideSegments->size()) ? std::bit_cast<void*>(jsonWroteNothing) : nullptr;
            }
            if (!isArrayOrInstanceOfSubclass(value.asCell(), type))
                return std::bit_cast<void*>(jsonWroteNothing);
            auto& array = *uncheckedDowncast<JSArray>(value.asCell());
            Latin1Character* start = instance->jsonBufferStart;
            Latin1Character* limit = instance->jsonBufferEnd;
            size_t offset = cursor - start;
            Latin1Character* end = appendArray(instance, globalObject, vm, cursor, array, 0);
            if (end == jsonWalkGaveUp) [[unlikely]]
                return nullptr;
            if (end) [[likely]]
                return end;
            if (isInstanceOfSubclassWithoutToJSON(globalObject, vm, array)) {
                instance->jsonWideSegmentCount = 0;
                return appendElementsByEngine(instance, globalObject, instance->jsonBufferStart + offset, array);
            }
            return instance->jsonBufferStart == start && instance->jsonBufferEnd == limit && !instance->jsonWideSegmentCount && !(instance->jsonWideSegments && instance->jsonWideSegments->size()) ? std::bit_cast<void*>(jsonWroteNothing) : nullptr;
        }
        if (type >= ObjectType)
            return appendObject(instance, globalObject, vm, cursor, asObject(value), 0);
        if (type == StringType) {
            auto string = asString(value)->tryGetValue();
            if (string.data.isNull())
                return nullptr;
            cursor = reserveInJSONBuffer(instance, cursor, 2 * jsonBufferSpareCapacity);
            if (!cursor)
                return nullptr;
            *cursor++ = '"';
            cursor = appendCharacters(instance, cursor, *string.data.impl());
            if (!cursor)
                return nullptr;
            *cursor++ = '"';
            return reserveInJSONBuffer(instance, cursor, jsonBufferSpareCapacity);
        }
        if (type != SymbolType)
            return nullptr;
        value = jsUndefined();
    }
    if (value.isUndefined()) {
        if (isElement == jsonValueIsWholeOutput)
            return std::bit_cast<void*>(jsonWroteNothing);
        if (!isElement)
            return cursor;
        value = jsNull();
    }
    cursor = reserveInJSONBuffer(instance, cursor, 2 * jsonBufferSpareCapacity);
    if (!cursor)
        return nullptr;
    if (value.isNull())
        return appendLiteral(cursor, "null"_s);
    if (value.isBoolean())
        return value.isTrue() ? appendLiteral(cursor, "true"_s) : appendLiteral(cursor, "false"_s);
    if (value.isNumber())
        return appendNumber(cursor, value.asNumber());
    return nullptr;
}

static ALWAYS_INLINE size_t writeWithWideSegments(std::span<char16_t> storage, std::span<const Latin1Character> characters, std::span<const JSONWideSegment> segments)
{
    std::span<char16_t> output = storage;
    size_t taken = 0;
    auto copyUpTo = [&](size_t end) {
        StringImpl::copyCharacters(output, characters.subspan(taken, end - taken));
        skip(output, end - taken);
        taken = end;
    };
    for (auto& segment : segments) {
        copyUpTo(segment.offset);
        if (!segment.needsEscaping) {
            if (segment.text.is8Bit())
                StringImpl::copyCharacters(output, segment.text.span8());
            else
                StringImpl::copyCharacters(output, segment.text.span16());
            skip(output, segment.text.length());
        } else if (segment.text.is8Bit())
            WTF::appendEscapedJSONStringContent(output, segment.text.span8());
        else
            WTF::appendEscapedJSONStringContent(output, segment.text.span16());
    }
    copyUpTo(characters.size());
    return storage.size() - output.size();
}

static NEVER_INLINE RefPtr<StringImpl> makeLargeStringWithWideSegments(std::span<const Latin1Character> characters, std::span<const JSONWideSegment> segments, size_t capacity)
{
    std::span<char16_t> storage;
    RefPtr<StringImpl> text = StringImpl::tryCreateUninitialized(capacity, storage);
    if (!text)
        return nullptr;
    size_t length = writeWithWideSegments(storage, characters, segments);
    if (capacity - length > length / 8)
        return StringImpl::create(storage.first(length));
    char16_t* data = nullptr;
    auto shrunk = StringImpl::tryReallocate(text.releaseNonNull(), length, data);
    return shrunk ? RefPtr<StringImpl> { WTF::move(*shrunk) } : nullptr;
}

static NEVER_INLINE RefPtr<StringImpl> makeStringWithWideSegments(std::span<const Latin1Character> characters, std::span<const JSONWideSegment> segments)
{
    static constexpr size_t vectorOverreadCapacity = 32;
    static constexpr size_t inlineCapacity = 1024;
    CheckedSize most = characters.size() + vectorOverreadCapacity;
    for (auto& segment : segments)
        most += CheckedSize(segment.text.length()) * static_cast<size_t>(segment.needsEscaping ? 6 : 1);
    if (most.hasOverflowed() || most.value() > String::MaxLength)
        return nullptr;
    if (most.value() > inlineCapacity)
        return makeLargeStringWithWideSegments(characters, segments, most.value());
    std::array<char16_t, inlineCapacity> storage;
    return StringImpl::create(std::span { storage }.first(writeWithWideSegments(storage, characters, segments)));
}

static NEVER_INLINE void replaceLargeJSONBuffer(Instance* instance, size_t capacityHint)
{
    instance->jsonBufferCapacityHint = std::min<size_t>(capacityHint, std::numeric_limits<uint32_t>::max());
    fastFree(instance->jsonBufferStart);
    void* storage = nullptr;
    size_t capacity = WTF::tryFastMalloc(maxRetainedJSONBufferCapacity).getValue(storage) ? maxRetainedJSONBufferCapacity : 0;
    instance->jsonBufferStart = static_cast<Latin1Character*>(storage);
    instance->jsonBufferEnd = instance->jsonBufferStart + capacity;
}

static ALWAYS_INLINE void releaseLargeJSONBuffer(Instance* instance, const Invocable<size_t()> auto& outputSize)
{
    if (static_cast<size_t>(instance->jsonBufferEnd - instance->jsonBufferStart) > maxRetainedJSONBufferCapacity) [[unlikely]] {
        size_t size = outputSize();
        replaceLargeJSONBuffer(instance, size + size / 2);
    }
}

static void clearJSONWideSegments(Instance* instance)
{
    if (instance->jsonWideSegments)
        instance->jsonWideSegments->clear();
    instance->jsonWideSegmentCount = 0;
}

static NEVER_INLINE JSCell* finishWithWideSegments(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint64_t cursor)
{
    RefPtr<StringImpl> text;
    if (!mayHaveToJSON(globalObject, vm, globalObject->objectPrototype()) && !mayHaveToJSON(globalObject, vm, globalObject->arrayPrototype()))
        text = makeStringWithWideSegments({ instance->jsonBufferStart, std::bit_cast<Latin1Character*>(cursor) }, instance->jsonWideSegments->span().first(instance->jsonWideSegmentCount));
    clearJSONWideSegments(instance);
    releaseLargeJSONBuffer(instance, [&] -> size_t { return std::bit_cast<Latin1Character*>(cursor) - instance->jsonBufferStart; });
    return text ? jsString(vm, String { text.releaseNonNull() }) : nullptr;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendElementsFrom, void*, (Instance* instance, uint64_t cursorBits, JSCell* array, uint64_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* cursor = std::bit_cast<Latin1Character*>(cursorBits);
    WideStringsInJSON wideStrings;
    bool isWritten = tryStringifyElementsWithoutSideEffects(*globalObject, *asArray(array), index, wideStrings, [&](std::span<const Latin1Character> characters) {
        cursor = appendStringified(instance, cursor, characters, wideStrings);
    });
    return isWritten ? cursor : nullptr;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONKeyOrdersOf, void*, (Instance* instance, JSCell* cell))
{
    auto& object = *uncheckedDowncast<Typed::Object>(cell);
    auto& layout = object.layout();
    for (auto& field : std::span { layout.fields(), layout.numberOfFields }) {
        if (uint32_t key = Typed::slotKeyFor(field.offset, field.flags & Typed::fieldIsOutOfLine)) {
            object.state().fieldAt(layout, key);
            return instance->typedOrderKeys[instance->typedOrderKeyIndices[layout.shape]];
        }
    }
    return nullptr;
}

static ALWAYS_INLINE JSCell* finishJSON(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint64_t cursor)
{
    if (instance->jsonWideSegmentCount) [[unlikely]]
        return finishWithWideSegments(instance, globalObject, vm, cursor);
    std::span<const Latin1Character> characters { instance->jsonBufferStart, std::bit_cast<Latin1Character*>(cursor) };
    JSString* result = nullptr;
    if (!mayHaveToJSON(globalObject, vm, globalObject->objectPrototype()) && !mayHaveToJSON(globalObject, vm, globalObject->arrayPrototype())) [[likely]] {
        std::span<Latin1Character> copy;
        if (auto impl = StringImpl::tryCreateUninitialized(characters.size(), copy)) [[likely]] {
            memcpy(copy.data(), characters.data(), characters.size());
            result = jsString(vm, String { impl.releaseNonNull() });
        }
    }
    releaseLargeJSONBuffer(instance, [&] -> size_t { return result ? result->length() : 0; });
    return result;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONFinish, JSCell*, (Instance* instance, uint64_t cursor))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    return finishJSON(instance, globalObject, vm, cursor);
}

static void discardJSON(Instance* instance)
{
    clearJSONWideSegments(instance);
    size_t capacity = instance->jsonBufferEnd - instance->jsonBufferStart;
    if (capacity > maxRetainedJSONBufferCapacity)
        replaceLargeJSONBuffer(instance, capacity);
}

JSString* tryStringifyObjectWithExtraProperties(JSGlobalObject* globalObject, JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != ObjectType)
        return nullptr;
    JSObject* object = asObject(value);
    Structure* structure = object->structure();
    if (structure->classInfoForCells() != Typed::Object::info() || !uncheckedDowncast<Typed::Object>(object)->hasExtraProperties())
        return nullptr;
    Instance* instance = structure->aotInstance();
    VM& vm = globalObject->vm();
    Latin1Character* end = appendTypedObjectWithExtraProperties(instance, globalObject, vm, instance->jsonBufferStart, object, 0);
    JSCell* written = end ? finishJSON(instance, globalObject, vm, std::bit_cast<uint64_t>(end)) : nullptr;
    if (!written) {
        discardJSON(instance);
        return nullptr;
    }
    countOperationNamed(instance, "objectWithExtraPropertiesWrittenByTypedWriter");
    return uncheckedDowncast<JSString>(written);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewJSONResponse, EncodedJSValue, (Instance* instance, EncodedJSValue value, EncodedJSValue status, EncodedJSValue statusText, EncodedJSValue headers, uint32_t headerCount, const EncodedJSValue* headerNamesAndValues))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    EncodedJSValue response = Typed::hostClassEntryPoints().newJSONResponse(globalObject, value, status, statusText, headers, headerCount, headerNamesAndValues);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, response);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONDiscard, void, (Instance* instance))
{
    countOperationNamed(instance, __func__);
    discardJSON(instance);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONPrepare, void, (Instance* instance, uint64_t cursor, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Instance* holder = globalObject->aotInstance();
    takePreparedJSON(*holder, JSValue());
    std::span<const Latin1Character> characters { instance->jsonBufferStart, std::bit_cast<Latin1Character*>(cursor) };
    RefPtr<StringImpl> text;
    if (!mayHaveToJSON(globalObject, vm, globalObject->objectPrototype()) && !mayHaveToJSON(globalObject, vm, globalObject->arrayPrototype())) [[likely]] {
        if (instance->jsonWideSegmentCount) [[unlikely]]
            text = makeStringWithWideSegments(characters, instance->jsonWideSegments->span().first(instance->jsonWideSegmentCount));
        else {
            std::span<Latin1Character> copy;
            text = StringImpl::tryCreateUninitialized(characters.size(), copy);
            if (text) [[likely]]
                memcpy(copy.data(), characters.data(), characters.size());
        }
    }
    clearJSONWideSegments(instance);
    releaseLargeJSONBuffer(instance, [&] -> size_t { return characters.size(); });
    if (!text)
        return;
    holder->preparedJSONString = text.leakRef();
    holder->preparedJSONValue = value;
}

String takePreparedJSON(Instance& instance, JSValue value)
{
    RefPtr<StringImpl> text = adoptRef(std::exchange(instance.preparedJSONString, nullptr));
    EncodedJSValue prepared = std::exchange(instance.preparedJSONValue, 0);
    return value && prepared == JSValue::encode(value) ? String { WTF::move(text) } : String();
}

void forgetDeadJSONPlans(Instance& instance)
{
    auto* cache = instance.jsonPlanCache;
    if (!cache || cache->withExtraProperties.isEmpty())
        return;
    cache->recentWithExtraProperties.fill(nullptr);
    cache->withExtraProperties.removeIf([&](auto& entry) {
        return !instance.vm->heap.isMarked(entry.value->ofExtraProperties.decode());
    });
}

void destroyJSONBuffers(Instance& instance)
{
    takePreparedJSON(instance, JSValue());
    fastFree(instance.jsonBufferStart);
    delete instance.jsonWideSegments;
    delete instance.jsonPlanCache;
}

namespace Typed {

using namespace B3;

static constexpr unsigned maxInlineJSONDepth = 3;
static constexpr unsigned maxInlineJSONFields = 48;
static constexpr unsigned maxInlineJSONFieldsPerFunction = 24;
static constexpr unsigned maxInlineArrayDepth = 6;
static constexpr int64_t jsonSerializerStackReserve = 32 * KB;
static constexpr int64_t isNotWrittenByDispatcher = 1;
static_assert(isNotWrittenByDispatcher == jsonWroteNothing);
static constexpr unsigned minRequestedJSONCapacity = 128;

std::optional<Writes> writesOfInlineJSON(InlineBuiltin which, std::span<const Type> arguments)
{
    if (which == InlineBuiltin::AppendJSON)
        return Writes::RuntimeState;
    if (which == InlineBuiltin::NewJSONResponse)
        return Writes::Heap;
    return writesIf(!arguments.empty() && (arguments[0].hasObjects() || arguments[0].withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).isString()), Writes::Heap);
}

std::optional<Operand> Lowering::lowerInlineJSON(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    if (which == InlineBuiltin::AppendJSON)
        return lowerAppendJSON(value);
    if (which == InlineBuiltin::ResponseJSON || which == InlineBuiltin::NewJSONResponse)
        return lowerResponseJSON(result, value);
    return lowerJSONStringify(result, value);
}

static bool needsNoJSONEscaping(std::span<const uint8_t> name)
{
    return std::ranges::all_of(name, [](uint8_t character) {
        return character >= ' ' && character < 0x7f && character != '"' && character != '\\';
    });
}

static bool mayBeBuiltUp(const Program& program, LayoutID id)
{
    return std::ranges::any_of(program.at(id).fields, [&](const Field& field) {
        return field.flags & (fieldIsOptional | fieldIsAbsentAtCreation) && !(field.flags & fieldIsNeverStored);
    });
}

static bool isKeyEmittedInline(std::span<const uint8_t> name)
{
    return !name.empty() && !isASCIIDigit(name.front()) && !equalSpans(name, asBytes("__proto__"_s.span8())) && !equalSpans(name, asBytes("toJSON"_s.span8())) && needsNoJSONEscaping(name);
}

namespace {

class JSONSites {
public:
    JSONSites(const Program& program, BuiltinID stringify, BuiltinID responseJSON)
        : m_program(program)
        , m_stringify(stringify)
        , m_responseJSON(responseJSON)
    {
    }

    bool isEmpty() const { return !m_first; }
    unsigned first() const { return *m_first; }
    struct Expected {
        TypeID type;
        unsigned depth;
    };
    const Vector<Expected>& expected() const { return m_expected; }

    void find()
    {
        unsigned count = m_program.functions.size();
        static constexpr OpcodeSet builtinCalls { Opcode::CallBuiltin };
        Vector<Vector<ValueID, 0>> sites(count);
        forEachIndexInParallel(count, [&](unsigned i) {
            m_program.functions.forEachIndexedValue(i, builtinCalls, [&](ValueID id) {
                auto& value = m_program.functions[i].at(id);
                if ((value.builtin() == m_stringify || value.builtin() == m_responseJSON) && !value.arguments.isEmpty())
                    sites[i].append(id);
            });
        });
        for (unsigned i = 0; i < count; ++i) {
            for (ValueID id : sites[i]) {
                if (!m_first)
                    m_first = i;
                add(m_program.functions[i], m_program.functions[i].at(id).arguments[0], 0);
            }
        }
    }

private:
    void add(const Function& function, ValueID id, unsigned depth)
    {
        ValueID origin = m_program.underlyingValue(function, id);
        if (function.at(origin).opcode != Opcode::NewObject) {
            auto& value = function.at(id);
            if (m_program.at(value.type).hasObjects())
                m_expected.append({ value.prediction() ? value.prediction() : value.type, depth });
            return;
        }
        if (depth + 1 >= maxInlineJSONDepth)
            return;
        for (auto& block : function.blocks) {
            size_t allocation = block.values.find(origin);
            if (allocation == notFound)
                continue;
            for (size_t index : fieldsInitializedWithAllocation(m_program, function, block, allocation, false, false)) {
                auto& field = function.at(block.values[index]);
                add(function, field.opcode == Opcode::PutById ? field.child(1) : field.arguments[2], depth + 1);
            }
            return;
        }
    }

    const Program& m_program;
    BuiltinID m_stringify;
    BuiltinID m_responseJSON;
    Vector<Expected> m_expected;
    std::optional<unsigned> m_first;
};

} // namespace

void createJSONSerializers(Program& program)
{
    static constexpr unsigned maxFields = 32;
    auto& builtins = Builtins::singleton();
    BuiltinID stringify = builtins.findStatic(BuiltinKind::StaticFunction, builtins.nameOf(asBytes("JSON"_s.span8())), builtins.nameOf(asBytes("stringify"_s.span8())));
    BuiltinID append = Operations::AppendJSON;
    BuiltinID responseJSON = builtins.find(BuiltinKind::StaticFunction, builtins.nameOf(asBytes("Response"_s.span8())), builtins.nameOf(asBytes("json"_s.span8())));
    RELEASE_ASSERT(stringify && append && responseJSON);
    unsigned count = program.functions.size();
    JSONSites sites(program, stringify, responseJSON);
    sites.find();
    if (sites.isEmpty())
        return;
    static constexpr unsigned maxMembers = 4;
    auto isTaken = [&](LayoutID member) {
        auto& layout = program.at(member);
        return layout.kind == LayoutKind::Record && layout.isAllocated && layout.fields.size() <= maxFields && std::ranges::all_of(layout.fields, [&](const Field& field) {
            auto name = program.string(field.name);
            return !isStringKey(name) || isKeyEmittedInline(name);
        });
    };
    auto forEachSupertype = [&](TypeID id, const auto& functor) {
        for (unsigned levels = 0; id && levels < 4; ++levels) {
            auto& type = program.at(id);
            for (LayoutID view : type.objectLayouts)
                functor(view, levels);
            id = type.elements;
        }
    };
    BitVector isWritten(program.layouts.size());
    Vector<LayoutID> worklist;
    auto assignSerializer = [&](LayoutID member) {
        if (!isWritten.set(member.index()))
            worklist.append(member);
    };
    std::array<BitVector, maxInlineJSONDepth> isVisitedAt;
    auto visit = [&](auto& self, LayoutID layout, unsigned depth) -> void {
        if (isVisitedAt[depth].set(layout.index()))
            return;
        for (auto& field : program.at(layout).fields) {
            forEachSupertype(field.type, [&](LayoutID view, unsigned levels) {
                if (program.at(view).subtypes.size() > maxMembers)
                    return;
                for (LayoutID member : program.at(view).subtypes) {
                    if (!isTaken(member))
                        continue;
                    if (depth + 1 + levels >= maxInlineJSONDepth || mayBeBuiltUp(program, member))
                        assignSerializer(member);
                    if (depth + 1 + levels < maxInlineJSONDepth)
                        self(self, member, depth + 1 + levels);
                }
            });
        }
    };
    bool dispatches = false;
    for (auto [id, depth] : sites.expected()) {
        Type expected = program.at(id).withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent);
        auto* engineClass = expected.singleBuiltinClass();
        bool isList = engineClass && engineClass->type == ArrayType && expected.elements && !program.at(expected.elements).isAnything();
        LayoutID only = expected.singleLayout();
        bool isEmittedInline = isList || (only && !program.at(only).subtypes.isEmpty() && program.at(only).subtypes.size() <= maxMembers && std::ranges::all_of(program.at(only).subtypes, isTaken));
        if (depth && !isEmittedInline)
            continue;
        dispatches |= !isEmittedInline;
        forEachSupertype(id, [&, depth = depth](LayoutID view, unsigned levels) {
            for (LayoutID member : program.at(view).subtypes) {
                if (!isTaken(member))
                    continue;
                if (isEmittedInline && depth + levels < maxInlineJSONDepth)
                    visit(visit, member, depth + levels);
                if (!isEmittedInline || depth + levels >= maxInlineJSONDepth || mayBeBuiltUp(program, member))
                    assignSerializer(member);
            }
        });
    }
    while (!worklist.isEmpty())
        visit(visit, worklist.takeLast(), 0);
    auto& site = std::as_const(program).functions[sites.first()];
    auto make = [&](int64_t written) {
        Function made;
        made.name = program.emptyString;
        made.module = site.module;
        made.position = site.position;
        made.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces | FunctionHasUnknownCallers;
        made.isSharedOutlinedBody = true;
        made.length = 2;
        made.resultType = program.types.integer;
        made.blocks.append(Block { });
        Value call;
        call.opcode = Opcode::CallBuiltin;
        call.position = site.position;
        call.type = program.types.integer;
        call.rep = repOf(program.at(call.type));
        call.immediates = { append.index(), Format::none };
        for (TypeID type : { written < 0 ? program.types.anything : program.types.object, program.types.integer }) {
            Value parameter;
            parameter.opcode = Opcode::Parameter;
            parameter.position = site.position;
            parameter.type = type;
            parameter.rep = repOf(program.at(type));
            parameter.immediates = { 0, static_cast<uint32_t>(made.parameterTypes.size()) };
            made.parameterTypes.append(type);
            made.blocks[0].parameters.append(ValueID(made.values.size()));
            call.arguments.append(ValueID(made.values.size()));
            made.values.append(WTF::move(parameter));
        }
        Value constant;
        constant.opcode = Opcode::Integer;
        constant.position = site.position;
        constant.type = program.types.integer;
        constant.rep = repOf(program.at(constant.type));
        constant.setBits(static_cast<uint64_t>(written));
        made.blocks[0].values.append(ValueID(made.values.size()));
        call.arguments.append(ValueID(made.values.size()));
        made.values.append(WTF::move(constant));
        made.blocks[0].values.append(ValueID(made.values.size()));
        made.blocks[0].terminator.kind = TerminatorKind::Return;
        made.blocks[0].terminator.value = ValueID(made.values.size());
        made.values.append(WTF::move(call));
        return made;
    };
    Vector<Function> made;
    for (size_t layout : isWritten) {
        program.jsonSerializers.append({ LayoutID(layout), FunctionID(count + made.size()) });
        made.append(make(layout));
    }
    if (dispatches && std::ranges::any_of(program.layouts, [](auto& layout) { return isObjectLayout(layout.kind) && layout.isAllocated; })) {
        program.jsonDispatcher = FunctionID(count + made.size());
        made.append(make(-1));
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", program.jsonSerializers.size(), " layouts have a function that writes their objects as JSON", dispatches ? ", found by shape where the type of what is written is not known" : "");
    program.functions.appendVector(WTF::move(made));
}

Vector<Lowering::JSONKeyOrder, 4> Lowering::keyOrdersForJSON(const Type& type) const
{
    static constexpr unsigned maxMembers = 4;
    LayoutID view = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).singleLayout();
    if (!view || m_program.at(view).subtypes.size() > maxMembers)
        return { };
    return keyOrdersForJSON(m_program.at(view).subtypes.span());
}

Vector<Lowering::JSONKeyOrder, 4> Lowering::keyOrdersForJSON(std::span<const LayoutID> members) const
{
    static constexpr unsigned maxOrders = 4;
    static constexpr unsigned maxFields = 32;
    Vector<JSONKeyOrder, 4> result;
    bool isRefused = false;
    auto add = [&](LayoutID member, uint32_t order, uint32_t count) {
        if (isRefused || result.size() == maxOrders || result.containsIf([&](const JSONKeyOrder& known) { return known.layout == member && known.order == order && known.count == count; }))
            return;
        auto& layout = m_program.at(member);
        bool isWhole = count == layout.orders[order].size();
        JSONKeyOrder keys { member, order, count, isWhole ? 0 : count, { } };
        uint32_t position = 0;
        for (uint16_t index : layout.orders[order].span().first(count)) {
            ++position;
            if (isWhole && !(m_program.at(layout.fields[index].stored).primitives & Primitive::Absent))
                keys.minKeyCount = position;
            auto name = m_program.string(layout.fields[index].name);
            if (!isStringKey(name))
                continue;
            isRefused |= name.empty() || isASCIIDigit(name.front()) || equalSpans(name, asBytes("__proto__"_s.span8())) || equalSpans(name, asBytes("toJSON"_s.span8())) || !needsNoJSONEscaping(name);
            keys.fields.append(index);
        }
        isRefused |= keys.fields.size() > maxFields || keys.count > maxFieldsInOrder;
        if (!isRefused)
            result.append(WTF::move(keys));
    };
    for (LayoutID member : members) {
        auto& layout = m_program.at(member);
        if (layout.kind != LayoutKind::Record)
            return { };
        for (auto& allocation : m_program.allocations) {
            if (allocation.layout != member)
                continue;
            Vector<uint16_t, 16> expected;
            uint32_t numberOfInitialFields = 0;
            for (unsigned i = 0; i < allocation.order.size(); ++i) {
                if (layout.fields[allocation.order[i]].flags & fieldIsNeverStored)
                    continue;
                expected.append(allocation.order[i]);
                numberOfInitialFields += i < allocation.initialFieldCount;
            }
            size_t order = layout.orders.findIf([&](auto& candidate) {
                return candidate.size() >= expected.size() && std::equal(expected.begin(), expected.end(), candidate.begin());
            });
            if (order != notFound && numberOfInitialFields <= maxFieldsInOrder)
                add(member, order, numberOfInitialFields);
        }
    }
    for (LayoutID member : members) {
        for (unsigned order = 0; order < m_program.at(member).orders.size(); ++order)
            add(member, order, m_program.at(member).orders[order].size());
    }
    return isRefused ? Vector<JSONKeyOrder, 4> { } : result;
}

bool Lowering::hasCodeForJSON(const Type& type) const
{
    Type expected = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent);
    if (auto* engineClass = expected.singleBuiltinClass(); engineClass && engineClass->type == ArrayType)
        return expected.elements && !m_program.at(expected.elements).isAnything() && hasCodeForObjectsIn(m_program.at(expected.elements));
    return !keyOrdersForJSON(expected).isEmpty() || !serializersFor(expected).isEmpty();
}

bool Lowering::hasCodeForObjectsIn(const Type& type) const
{
    if (!type.hasObjects())
        return true;
    auto* engineClass = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).singleBuiltinClass();
    return (engineClass && engineClass->type == JSDateType) || hasCodeForJSON(type);
}

Vector<std::pair<LayoutID, FunctionID>, 4> Lowering::serializersFor(const Type& type) const
{
    static constexpr unsigned maxMembers = 4;
    LayoutID view = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).singleLayout();
    if (!view || m_program.at(view).subtypes.size() > maxMembers)
        return { };
    Vector<std::pair<LayoutID, FunctionID>, 4> result;
    for (LayoutID member : m_program.at(view).subtypes) {
        FunctionID serializer = m_program.jsonSerializerFor(member);
        if (!serializer || !m_program.hasCode(serializer))
            return { };
        result.append({ member, serializer });
    }
    return result;
}

LValue Lowering::cursorOf(JSONOutput& output)
{
    return m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), output.cursor);
}

void Lowering::setCursor(JSONOutput& output, LValue cursor)
{
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), output.cursor, cursor);
}

LBasicBlock Lowering::exitThrough(JSONOutput& output, JSONExit& exit)
{
    RELEASE_ASSERT(output.pendingBytes.isEmpty());
    exit.knownCapacity = std::min(exit.knownCapacity, output.knownCapacity);
    return exit.block;
}

void Lowering::reserveCapacity(JSONOutput& output, unsigned size)
{
    if (output.knownCapacity >= size)
        return;
    unsigned requestedCapacity = std::max(size, minRequestedJSONCapacity);
    LBasicBlock continuation = m_out.newBlock();
    LBasicBlock grows = newColdBlock();
    LValue cursor = cursorOf(output);
    LValue end = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONBufferEnd()));
    m_out.branch(m_out.aboveOrEqual(m_out.sub(end, cursor), m_out.constInt64(requestedCapacity)), usually(continuation), rarely(grows));
    m_out.appendTo(grows);
    Pointer moved = AOT_TYPED_OPERATION(JSONReserve, Integer(cursor), integer(requestedCapacity));
    branchUnless(Word32(m_out.notZero64(moved.value())), output.givesUp);
    setCursor(output, moved.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    output.knownCapacity = requestedCapacity;
}

void Lowering::appendBytes(JSONOutput& output, std::span<const uint8_t> text)
{
    output.pendingBytes.append(text);
}

void Lowering::flushPendingBytes(JSONOutput& output)
{
    if (output.pendingBytes.isEmpty())
        return;
    Vector<uint8_t, 64> text = std::exchange(output.pendingBytes, { });
    reserveCapacity(output, WTF::roundUpToMultipleOf<8>(text.size()));
    LValue cursor = cursorOf(output);
    for (size_t offset = 0; offset < text.size();) {
        size_t remaining = text.size() - offset;
        unsigned width = remaining > 4 ? 8 : remaining > 2 ? 4 : remaining;
        uint64_t bits = 0;
        for (unsigned i = 0; i < width && i < remaining; ++i)
            bits |= static_cast<uint64_t>(text[offset + i]) << (8 * i);
        TypedPointer pointer = m_out.address(m_heaps.AOTTyped_argumentBuffer, cursor, offset);
        switch (width) {
        case 8:
            m_out.store64(integer(static_cast<int64_t>(bits)).value(), pointer);
            break;
        case 4:
            m_out.store32(m_out.constInt32(static_cast<int32_t>(bits)), pointer);
            break;
        case 2:
            m_out.store32As16(m_out.constInt32(static_cast<int32_t>(bits)), pointer);
            break;
        default:
            m_out.store32As8(m_out.constInt32(static_cast<int32_t>(bits)), pointer);
            break;
        }
        offset += width;
    }
    setCursor(output, m_out.add(cursor, m_out.constInt64(text.size())));
    output.knownCapacity -= text.size();
}

void Lowering::didAppendWithinCapacity(JSONOutput& output, Pointer cursor)
{
    branchUnless(Word32(m_out.notZero64(cursor.value())), output.givesUp);
    setCursor(output, cursor.value());
    output.knownCapacity = jsonBufferSpareCapacity;
}

void Lowering::appendInteger(JSONOutput& output, Integer value, unsigned maxLength)
{
    flushPendingBytes(output);
    reserveCapacity(output, maxLength);
    setCursor(output, AOT_TYPED_OPERATION(JSONAppendInteger, Integer(cursorOf(output)), value).value());
    output.knownCapacity -= maxLength;
}

void Lowering::appendNumber(JSONOutput& output, Number value)
{
    flushPendingBytes(output);
    reserveCapacity(output, maxJSONNumberLength);
    setCursor(output, AOT_TYPED_OPERATION(JSONAppendNumber, Integer(cursorOf(output)), value).value());
    output.knownCapacity -= maxJSONNumberLength;
}

void Lowering::appendBoolean(JSONOutput& output, Word32 value)
{
    flushPendingBytes(output);
    reserveCapacity(output, 5);
    LValue cursor = cursorOf(output);
    m_out.store32(m_out.select(value.value(), m_out.constInt32('t' | 'r' << 8 | 'u' << 16 | 'e' << 24), m_out.constInt32('f' | 'a' << 8 | 'l' << 16 | 's' << 24)), m_out.address(m_heaps.AOTTyped_argumentBuffer, cursor, 0));
    m_out.store32As8(m_out.constInt32('e'), m_out.address(m_heaps.AOTTyped_argumentBuffer, cursor, 4));
    setCursor(output, m_out.sub(m_out.add(cursor, m_out.constInt64(5)), m_out.zeroExtPtr(value.value())));
    output.knownCapacity -= 5;
}

void Lowering::appendString(JSONOutput& output, CellPointer string)
{
    appendBytes(output, "\""_s);
    flushPendingBytes(output);
    didAppendWithinCapacity(output, AOT_TYPED_OPERATION(JSONAppendString, Integer(cursorOf(output)), string));
    appendBytes(output, "\""_s);
}

void Lowering::appendGeneric(JSONOutput& output, Boxed value, JSONPosition position, JSONExit& nothing)
{
    flushPendingBytes(output);
    LValue before = cursorOf(output);
    Pointer cursor = AOT_TYPED_OPERATION(JSONAppendValue, Integer(before), value, word32(position == JSONPosition::Element));
    if (position == JSONPosition::Property) {
        LBasicBlock hasText = m_out.newBlock();
        m_out.branch(m_out.equal(cursor.value(), before), unsure(exitThrough(output, nothing)), unsure(hasText));
        m_out.appendTo(hasText);
    }
    didAppendWithinCapacity(output, cursor);
}

void Lowering::appendNullishOrOmit(JSONOutput& output, Word32 isNull, const Type& type, JSONPosition position, JSONExit& nothing, JSONExit& continuation)
{
    bool mayBeNull = type.primitives & Primitive::Null;
    bool mayBeUndefined = type.primitives & (Primitive::Undefined | Primitive::Absent);
    if (position == JSONPosition::Property && mayBeUndefined) {
        if (!mayBeNull) {
            m_out.jump(exitThrough(output, nothing));
            return;
        }
        LBasicBlock isNullCase = m_out.newBlock();
        m_out.branch(isNull.value(), unsure(isNullCase), unsure(exitThrough(output, nothing)));
        m_out.appendTo(isNullCase);
    }
    appendBytes(output, "null"_s);
    flushPendingBytes(output);
    m_out.jump(exitThrough(output, continuation));
}

void Lowering::appendValue(JSONOutput& output, Operand operand, const Type& type, bool typeIsProven, unsigned depth, JSONPosition position, JSONExit& nothing)
{
    switch (operand.rep) {
    case Rep::Int32:
        appendBoolean(output, operand.as<Word32>());
        return;
    case Rep::Int64:
        appendInteger(output, operand.as<Integer>(), type.integers.isSubsetOf(Range::int32()) ? maxJSONInt32Length : maxJSONIntegerLength);
        return;
    case Rep::Double:
        appendNumber(output, operand.as<Number>());
        return;
    case Rep::OptionalInt64:
    case Rep::OptionalDouble: {
        flushPendingBytes(output);
        unsigned capacity = output.knownCapacity;
        JSONExit continuation { m_out.newBlock() };
        LBasicBlock isNumber = m_out.newBlock();
        LBasicBlock notNumber = m_out.newBlock();
        m_out.branch(isMarker(operand).value(), unsure(notNumber), unsure(isNumber));
        m_out.appendTo(isNumber);
        if (operand.rep == Rep::OptionalInt64)
            appendInteger(output, assumedInteger(operand.as<OptionalInteger>()), maxJSONIntegerLength);
        else
            appendNumber(output, assumedNumber(operand.as<OptionalNumber>()));
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(notNumber);
        output.knownCapacity = capacity;
        appendNullishOrOmit(output, equal(bitsOf(operand), integer(markerBits(Primitive::Null, operand.rep))), type, position, nothing, continuation);
        m_out.appendTo(continuation.block);
        output.knownCapacity = continuation.knownCapacity;
        return;
    }
    case Rep::Cell:
        appendCell(output, operand.as<CellPointer>(), type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent), typeIsProven, depth, position, nothing);
        return;
    case Rep::LiteralString:
        appendValue(output, convert(operand, type, Rep::JSValue), type.withLiteralStringWidened(), typeIsProven, depth, position, nothing);
        return;
    case Rep::JSValue:
        break;
    case Rep::Void:
        RELEASE_ASSERT_NOT_REACHED();
    }
    Boxed boxed = operand.as<Boxed>();
    Type expected = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent);
    if (expected.isBoolean()) {
        flushPendingBytes(output);
        reserveCapacity(output, 5);
        unsigned capacity = output.knownCapacity;
        JSONExit continuation { m_out.newBlock() };
        LBasicBlock isBooleanCase = m_out.newBlock();
        LBasicBlock notBoolean = m_out.newBlock();
        m_out.branch(isBoolean(boxed).value(), unsure(isBooleanCase), unsure(notBoolean));
        m_out.appendTo(isBooleanCase);
        appendBoolean(output, Word32(m_out.bitAnd(low32(boxed).value(), m_out.constInt32(1))));
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(notBoolean);
        output.knownCapacity = capacity;
        if (typeIsProven)
            appendNullishOrOmit(output, haveEqualBits(boxed, boxedConstant(jsNull())), type, position, nothing, continuation);
        else {
            appendGeneric(output, boxed, position, nothing);
            m_out.jump(exitThrough(output, continuation));
        }
        m_out.appendTo(continuation.block);
        output.knownCapacity = continuation.knownCapacity;
        return;
    }
    if (expected.isNever() || repOf(expected) != Rep::Cell || expected.primitives & (Primitive::Symbol | Primitive::BigInt)) {
        if (!typeIsProven && expected.isNumber()) {
            flushPendingBytes(output);
            reserveCapacity(output, maxJSONInt32Length);
            unsigned capacity = output.knownCapacity;
            JSONExit continuation { m_out.newBlock() };
            LBasicBlock isInt32Case = m_out.newBlock();
            LBasicBlock isOther = newUnlikelyBlock();
            m_out.branch(isInt32(boxed).value(), usually(isInt32Case), rarely(isOther));
            m_out.appendTo(isInt32Case);
            appendInteger(output, signExtended(low32(boxed)), maxJSONInt32Length);
            m_out.jump(exitThrough(output, continuation));
            m_out.appendTo(isOther);
            output.knownCapacity = capacity;
            appendGeneric(output, boxed, position, nothing);
            m_out.jump(exitThrough(output, continuation));
            m_out.appendTo(continuation.block);
            output.knownCapacity = continuation.knownCapacity;
            return;
        }
        appendGeneric(output, boxed, position, nothing);
        return;
    }
    flushPendingBytes(output);
    unsigned capacity = output.knownCapacity;
    JSONExit continuation { m_out.newBlock() };
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock notCell = m_out.newBlock();
    m_out.branch(isCell(boxed).value(), unsure(isCellCase), unsure(notCell));
    m_out.appendTo(isCellCase);
    appendCell(output, asCell(boxed), expected, typeIsProven, depth, position, nothing);
    flushPendingBytes(output);
    m_out.jump(exitThrough(output, continuation));
    m_out.appendTo(notCell);
    output.knownCapacity = capacity;
    if (typeIsProven)
        appendNullishOrOmit(output, haveEqualBits(boxed, boxedConstant(jsNull())), type, position, nothing, continuation);
    else {
        appendGeneric(output, boxed, position, nothing);
        m_out.jump(exitThrough(output, continuation));
    }
    m_out.appendTo(continuation.block);
    output.knownCapacity = continuation.knownCapacity;
}

void Lowering::appendCell(JSONOutput& output, CellPointer cell, const Type& type, bool typeIsProven, unsigned depth, JSONPosition position, JSONExit& nothing)
{
    if (type.isString()) {
        if (typeIsProven) {
            appendString(output, cell);
            return;
        }
        flushPendingBytes(output);
        unsigned capacity = output.knownCapacity;
        JSONExit continuation { m_out.newBlock() };
        LBasicBlock isString = m_out.newBlock();
        LBasicBlock isOther = newColdBlock();
        m_out.branch(equal(loadCellType(cell), word32(StringType)).value(), usually(isString), rarely(isOther));
        m_out.appendTo(isString);
        appendString(output, cell);
        flushPendingBytes(output);
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(isOther);
        output.knownCapacity = capacity;
        appendGeneric(output, box(cell), position, nothing);
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(continuation.block);
        output.knownCapacity = continuation.knownCapacity;
        return;
    }
    if (auto* engineClass = type.singleBuiltinClass(); engineClass && engineClass->type == ArrayType) {
        appendArray(output, cell, type, depth, position, nothing);
        return;
    }
    if (auto* engineClass = type.singleBuiltinClass(); engineClass && engineClass->type == JSDateType) {
        appendDate(output, cell, *engineClass, position, nothing);
        return;
    }
    if (type.singleLayout()) {
        appendObject(output, cell, type, depth, position, nothing);
        return;
    }
    appendGeneric(output, box(cell), position, nothing);
}

void Lowering::appendDate(JSONOutput& output, CellPointer date, const BuiltinClass& dateClass, JSONPosition position, JSONExit& nothing)
{
    flushPendingBytes(output);
    reserveCapacity(output, maxLengthOfDateInJSON);
    unsigned capacity = output.knownCapacity;
    JSONExit continuation { m_out.newBlock() };
    LBasicBlock isOriginal = m_out.newBlock();
    LBasicBlock hasChanged = newColdBlock();
    m_out.branch(hasOriginalStructure(date, dateClass).value(), usually(isOriginal), rarely(hasChanged));
    m_out.appendTo(isOriginal);
    Number timeValue(m_out.loadDouble(m_out.address(m_heaps.AOTTyped_runtimeState, date.value(), DateInstance::offsetOfInternalNumber())));
    setCursor(output, AOT_TYPED_OPERATION(JSONAppendDate, Integer(cursorOf(output)), timeValue).value());
    output.knownCapacity -= maxLengthOfDateInJSON;
    m_out.jump(exitThrough(output, continuation));
    m_out.appendTo(hasChanged);
    output.knownCapacity = capacity;
    appendGeneric(output, box(date), position, nothing);
    m_out.jump(exitThrough(output, continuation));
    m_out.appendTo(continuation.block);
    output.knownCapacity = continuation.knownCapacity;
}

void Lowering::appendField(JSONOutput& output, CellPointer cell, const Field& field, unsigned depth, JSONExit& nothing)
{
    if (field.store == Store::Int32OrNumberBox) {
        flushPendingBytes(output);
        reserveCapacity(output, maxJSONInt32Length);
        unsigned capacity = output.knownCapacity;
        JSONExit continuation { m_out.newBlock() };
        LBasicBlock inlinePath = m_out.newBlock();
        LBasicBlock elsewhere = newUnlikelyBlock();
        Integer word = signExtended(load(field.store, fieldAt(cell, field), safeIfAllSubtypesHave(cell, field)).as<Word32>());
        branchOnInt32Inline(word, usually(inlinePath), rarely(elsewhere));
        m_out.appendTo(inlinePath);
        appendInteger(output, word, maxJSONInt32Length);
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(elsewhere);
        output.knownCapacity = capacity;
        appendGeneric(output, readField(cell, field, m_program.at(m_program.types.anything)).as<Boxed>(), JSONPosition::Property, nothing);
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(continuation.block);
        output.knownCapacity = continuation.knownCapacity;
        return;
    }
    Type storedType = m_program.at(field.stored);
    if (storedType.primitives & Primitive::Absent)
        storedType.primitives = (storedType.primitives & ~Primitive::Absent) | Primitive::Undefined;
    appendValue(output, readField(cell, field, storedType), storedType, true, depth, JSONPosition::Property, nothing);
}

void Lowering::appendBySerializer(JSONOutput& output, CellPointer cell, FunctionID serializer)
{
    flushPendingBytes(output);
    LValue cursor = callFunction(serializer, Int64, { cell.value(), cursorOf(output) });
    branchUnless(Word32(m_out.notZero64(cursor)), output.givesUp);
    setCursor(output, cursor);
    output.knownCapacity = 0;
}

void Lowering::appendObjectOutOfLine(JSONOutput& output, CellPointer cell, const Type& type, JSONPosition position, JSONExit& nothing)
{
    auto serializers = serializersFor(type);
    if (serializers.isEmpty()) {
        appendGeneric(output, box(cell), position, nothing);
        return;
    }
    if (serializers.size() == 1) {
        appendBySerializer(output, cell, serializers[0].second);
        return;
    }
    flushPendingBytes(output);
    unsigned capacity = output.knownCapacity;
    JSONExit continuation { m_out.newBlock() };
    LBasicBlock otherwise = newColdBlock();
    Word32 shapeWord = shapeWordOf(cell);
    for (auto& [layout, serializer] : serializers) {
        LBasicBlock matches = m_out.newBlock();
        bool isLast = layout == serializers.last().first;
        LBasicBlock next = isLast ? otherwise : m_out.newBlock();
        Word32 hasLayout = equal(shapeWord, word32(static_cast<int32_t>(Typed::shapeWordOf(m_program.at(layout).shape))));
        if (isLast)
            m_out.branch(hasLayout.value(), usually(matches), rarely(next));
        else
            m_out.branch(hasLayout.value(), unsure(matches), unsure(next));
        m_out.appendTo(matches);
        output.knownCapacity = capacity;
        appendBySerializer(output, cell, serializer);
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(next);
    }
    output.knownCapacity = capacity;
    appendGeneric(output, box(cell), position, nothing);
    m_out.jump(exitThrough(output, continuation));
    m_out.appendTo(continuation.block);
    output.knownCapacity = continuation.knownCapacity;
}

void Lowering::appendObject(JSONOutput& output, CellPointer cell, const Type& type, unsigned depth, JSONPosition position, JSONExit& nothing)
{
    Vector<JSONKeyOrder, 4> orders;
    if (depth < maxInlineJSONDepth)
        orders = keyOrdersForJSON(type);
    unsigned numberOfFields = 0;
    for (auto& order : orders)
        numberOfFields += order.fields.size() + 1;
    if (orders.isEmpty() || numberOfFields > output.remainingFields) {
        appendObjectOutOfLine(output, cell, type, position, nothing);
        return;
    }
    output.remainingFields -= numberOfFields;
    flushPendingBytes(output);
    unsigned capacity = output.knownCapacity;
    JSONExit continuation { m_out.newBlock() };
    bool usesSerializerFunction = serializersFor(type).containsIf([&](auto& serializer) {
        return mayBeBuiltUp(m_program, serializer.first);
    });
    LBasicBlock delegatesRestOfList = std::exchange(output.delegatesRestOfList, nullptr);
    LBasicBlock otherwise = usesSerializerFunction ? newColdBlock() : delegatesRestOfList ? delegatesRestOfList : depth ? newColdBlock() : output.givesUpBeforeWriting;
    appendObjectInline(output, cell, orders, depth, otherwise, continuation);
    if (usesSerializerFunction) {
        output.delegatesRestOfList = delegatesRestOfList;
        m_out.appendTo(otherwise);
        output.knownCapacity = capacity;
        appendObjectOutOfLine(output, cell, type, position, nothing);
        if (continuation.knownCapacity != UINT_MAX)
            reserveCapacity(output, continuation.knownCapacity);
        m_out.jump(exitThrough(output, continuation));
    } else if (depth && !delegatesRestOfList) {
        m_out.appendTo(otherwise);
        output.knownCapacity = capacity;
        appendGeneric(output, box(cell), position, nothing);
        m_out.jump(exitThrough(output, continuation));
    }
    m_out.appendTo(continuation.block);
    output.knownCapacity = continuation.knownCapacity;
}

template<typename Append>
void Lowering::appendMember(JSONOutput& output, JSONSeparator& separator, JSONMember member, const Append& append)
{
    auto [name, mayBeLeftOut] = member;
    LValue separatorLength = nullptr;
    unsigned lengthOfKey = 0;
    if (mayBeLeftOut || separator == JSONSeparator::Unknown)
        flushPendingBytes(output);
    if (separator == JSONSeparator::Unknown) {
        reserveCapacity(output, 1);
        LValue cursor = cursorOf(output);
        LValue previous = m_out.load8ZeroExt32(TypedPointer(m_heaps.root, m_out.add(cursor, m_out.constIntPtr(-1))));
        m_out.store32As8(m_out.constInt32(','), m_out.address(m_heaps.AOTTyped_argumentBuffer, cursor, 0));
        separatorLength = m_out.zeroExtPtr(m_out.notEqual(previous, m_out.constInt32('{')));
        setCursor(output, m_out.add(cursor, separatorLength));
        output.knownCapacity -= 1;
    } else if (separator == JSONSeparator::Always) {
        appendBytes(output, ","_s);
        ++lengthOfKey;
    }
    appendBytes(output, "\""_s);
    appendBytes(output, name);
    appendBytes(output, "\":"_s);
    lengthOfKey += name.size() + 3;
    if (!mayBeLeftOut) {
        JSONExit never { output.givesUp };
        append(never);
        separator = JSONSeparator::Always;
        return;
    }
    flushPendingBytes(output);
    JSONExit isLeftOut { m_out.newBlock() };
    JSONExit isDone { m_out.newBlock() };
    append(isLeftOut);
    flushPendingBytes(output);
    m_out.jump(exitThrough(output, isDone));
    m_out.appendTo(isLeftOut.block);
    output.knownCapacity = isLeftOut.knownCapacity == UINT_MAX ? 0 : isLeftOut.knownCapacity;
    LValue rewound = m_out.sub(cursorOf(output), m_out.constInt64(lengthOfKey));
    setCursor(output, separatorLength ? m_out.sub(rewound, separatorLength) : rewound);
    m_out.jump(exitThrough(output, isDone));
    m_out.appendTo(isDone.block);
    output.knownCapacity = isDone.knownCapacity;
    if (separator == JSONSeparator::None)
        separator = JSONSeparator::Unknown;
}

template<typename Describe, typename Append>
void Lowering::appendMembers(JSONOutput& output, unsigned count, const Describe& describe, const Append& append)
{
    appendBytes(output, "{"_s);
    JSONSeparator separator = JSONSeparator::None;
    for (unsigned i = 0; i < count; ++i) {
        appendMember(output, separator, describe(i), [&](JSONExit& nothing) {
            append(i, nothing);
        });
    }
    appendBytes(output, "}"_s);
}

void Lowering::appendObjectByOwnKeys(JSONOutput& output, CellPointer cell, LayoutID id, LBasicBlock otherwise, JSONExit& continuation)
{
    auto& layout = m_program.at(id);
    flushPendingBytes(output);
    auto at = [&](LValue base, LValue offset) {
        return TypedPointer(m_heaps.root, m_out.add(base, offset));
    };
    LValue structure = decodeStructureID(loadField<Word32>(cell, m_heaps.JSCell_structureID).value());
    branchUnless(Word32(m_out.equal(m_out.load64(structure, m_heaps.Structure_prototype), loadImmutablePointer(Instance::offsetOfObjectPrototype()))), otherwise);
    LValue words = m_out.load64(at(cell.value(), m_out.constIntPtr(offsetOfOrderWord)));
    uint64_t shapeAndExtraProperties = 0xffffffffull << 32 | orderWordHasExtraProperties;
    branchUnless(Word32(m_out.equal(m_out.bitAnd(words, integer(static_cast<int64_t>(shapeAndExtraProperties)).value()), integer(static_cast<int64_t>(static_cast<uint64_t>(Typed::shapeWordOf(layout.shape)) << 32)).value())), otherwise);
    LValue orderKeyIndices = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfTypedOrderKeyIndices()));
    LValue orderKeyIndex = m_out.zeroExtPtr(m_out.load16ZeroExt32(at(orderKeyIndices, m_out.constIntPtr(sizeof(uint16_t) * layout.shape))));
    LValue tables = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfTypedOrderKeys()));
    LValue known = m_out.load64(at(tables, m_out.shl(orderKeyIndex, m_out.constInt32(3))));
    LBasicBlock isNotCachedYet = newColdBlock();
    LBasicBlock hasOrders = m_out.newBlock();
    ValueFromBlock knownValue = m_out.anchor(known);
    m_out.branch(m_out.notZero64(known), usually(hasOrders), rarely(isNotCachedYet));
    m_out.appendTo(isNotCachedYet);
    Pointer computedKeyOrders = AOT_TYPED_OPERATION(JSONKeyOrdersOf, cell);
    branchUnless(Word32(m_out.notZero64(computedKeyOrders.value())), otherwise);
    ValueFromBlock computedValue = m_out.anchor(computedKeyOrders.value());
    m_out.jump(hasOrders);
    m_out.appendTo(hasOrders);
    LValue orders = m_out.phi(Int64, knownValue, computedValue);
    LValue order = m_out.bitAnd(words, m_out.constInt64(0xffff));
    branchUnless(Word32(m_out.below(order, m_out.load64(at(orders, m_out.constIntPtr(0))))), otherwise);
    LValue keys = m_out.load64(at(orders, m_out.add(m_out.shl(order, m_out.constInt32(3)), m_out.constIntPtr(sizeof(uintptr_t)))));
    LValue count = m_out.bitAnd(m_out.lShr(words, m_out.constInt32(16)), m_out.constInt64(maxFieldsInOrder));
    appendBytes(output, "{"_s);
    flushPendingBytes(output);
    LBasicBlock header = m_out.newBlock();
    LBasicBlock body = m_out.newBlock();
    LBasicBlock next = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    ValueFromBlock first = m_out.anchor(m_out.int64Zero);
    m_out.jump(header);
    m_out.appendTo(header);
    LValue index = m_out.phi(Int64, first);
    m_out.branch(m_out.below(index, count), usually(body), rarely(done));
    m_out.appendTo(body);
    LValue key = m_out.load16ZeroExt32(at(keys, m_out.add(m_out.shl(index, m_out.constInt32(1)), m_out.constIntPtr(sizeof(uint16_t)))));
    Vector<FTL::SwitchCase> cases;
    Vector<std::pair<LBasicBlock, const Field*>> blocks;
    for (auto& field : layout.fields) {
        uint32_t slotKey = slotKeyFor(field.offset, field.isOutOfLine);
        if (!slotKey || field.flags & fieldIsNeverStored)
            continue;
        blocks.append({ m_out.newBlock(), &field });
        cases.append(FTL::SwitchCase(m_out.constInt32(static_cast<int32_t>(slotKey)), blocks.last().first, FTL::Weight()));
    }
    m_out.switchInstruction(key, cases, output.givesUp, FTL::Weight());
    for (auto [block, field] : blocks) {
        m_out.appendTo(block);
        output.knownCapacity = 0;
        auto name = m_program.string(field->name);
        if (isStringKey(name)) {
            JSONSeparator separator = JSONSeparator::Unknown;
            appendMember(output, separator, { name, !!(m_program.at(field->stored).primitives & (Primitive::Absent | Primitive::Undefined | Primitive::Symbol)) }, [&, field = field](JSONExit& nothing) {
                appendField(output, cell, *field, 1, nothing);
            });
            flushPendingBytes(output);
        }
        m_out.jump(next);
    }
    m_out.appendTo(next);
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.constInt64(1))));
    m_out.jump(header);
    m_out.appendTo(done);
    output.knownCapacity = 0;
    appendBytes(output, "}"_s);
    flushPendingBytes(output);
    m_out.jump(exitThrough(output, continuation));
}

std::optional<Vector<Lowering::JSONLiteralMember, 8>> Lowering::freshLiteralMembers(ValueID argument, ValueID holder) const
{
    static constexpr unsigned maxMembers = 32;
    ValueID object = m_program.underlyingValue(m_function, argument);
    bool isRecord = m_function.at(object).opcode == Opcode::NewObjectWithShape;
    if ((m_function.at(object).opcode != Opcode::NewObject && !isRecord) || !m_currentBlock)
        return std::nullopt;
    const Block* home = nullptr;
    size_t allocation = notFound;
    for (auto& block : m_function.blocks) {
        allocation = block.values.find(object);
        if (allocation != notFound) {
            home = &block;
            break;
        }
    }
    if (!home || home->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    Vector<size_t, 6> fields;
    if (isRecord) {
        auto& allocationValue = m_function.at(object);
        auto& layout = m_program.at(allocationValue.layout());
        auto& allocationInfo = m_program.allocations[allocationValue.allocation()];
        if (layout.kind != LayoutKind::Record || allocationInfo.layout != allocationValue.layout() || allocationInfo.constructedBy || allocationInfo.discriminant != UINT16_MAX || layout.discriminant != UINT16_MAX)
            return std::nullopt;
        Vector<uint16_t, 8> initialKeys;
        for (uint16_t index : allocationInfo.order.span().first(allocationInfo.initialFieldCount)) {
            if (!(layout.fields[index].flags & fieldIsNeverStored))
                initialKeys.append(index);
        }
        for (size_t k = allocation + 1; k < home->values.size() && fields.size() < initialKeys.size(); ++k) {
            auto& value = m_function.at(home->values[k]);
            bool usesObject = false;
            value.forEachChild([&](ValueID child) {
                usesObject |= child == object;
            });
            if (!usesObject)
                continue;
            if (value.opcode != Opcode::PutField || value.child(0) != object || value.child(1) == object || value.layout() != allocationValue.layout() || value.field() != initialKeys[fields.size()])
                return std::nullopt;
            fields.append(k);
        }
        if (fields.size() != initialKeys.size())
            return std::nullopt;
    } else
        fields = fieldsInitializedWithAllocation(m_program, m_function, *home, allocation, false, false);
    if (fields.size() > maxMembers)
        return std::nullopt;
    Vector<JSONLiteralMember, 8> members;
    for (size_t index : fields) {
        auto& field = m_function.at(home->values[index]);
        if (isRecord) {
            StringID name = m_program.at(field.layout()).fields[field.field()].name;
            if (!isKeyEmittedInline(m_program.string(name)))
                return std::nullopt;
            members.append({ name, field.child(1), home->values[index] });
            continue;
        }
        StringID name = field.opcode == Opcode::PutById ? field.string() : m_function.at(field.arguments[1]).string();
        if (!isKeyEmittedInline(m_program.string(name)))
            return std::nullopt;
        members.append({ name, field.opcode == Opcode::PutById ? field.child(1) : field.arguments[2], home->values[index] });
    }
    members.removeAllMatching([&](const JSONLiteralMember& member) {
        return m_function.at(m_program.underlyingValue(m_function, member.value)).opcode == Opcode::Undefined;
    });
    auto hasProperty = [&](ValueID id) {
        if (id == argument || id == holder)
            return false;
        bool uses = false;
        m_function.at(id).forEachChild([&](ValueID child) {
            uses |= child == object || child == argument;
        });
        return uses;
    };
    auto isForwardedBy = [&](const Terminator& terminator) {
        bool uses = terminator.value == object || terminator.value == argument;
        for (auto& target : terminator.targets) {
            for (ValueID passed : target.arguments)
                uses |= passed == object || passed == argument;
        }
        return uses;
    };
    size_t end = home == m_currentBlock ? m_currentIndexInBlock : home->values.size();
    for (size_t k = (fields.isEmpty() ? allocation : fields.last()) + 1; k < end; ++k) {
        if (hasProperty(home->values[k]))
            return std::nullopt;
    }
    if (home == m_currentBlock)
        return members;
    if (isForwardedBy(home->terminator))
        return std::nullopt;
    unsigned ways = 0;
    for (auto& block : m_function.blocks) {
        for (auto& target : block.terminator.targets) {
            if (&m_function.blocks[target.block.index()] != m_currentBlock)
                continue;
            if (&block != home)
                return std::nullopt;
            ++ways;
        }
    }
    if (ways != 1)
        return std::nullopt;
    for (size_t k = 0; k < m_currentIndexInBlock; ++k) {
        if (hasProperty(m_currentBlock->values[k]))
            return std::nullopt;
    }
    return members;
}

Lowering::DeferredLiteral* Lowering::deferredLiteralFor(ValueID object)
{
    for (auto& literal : m_deferredLiterals) {
        if (literal.object == object)
            return &literal;
    }
    return nullptr;
}

std::optional<Lowering::DeferredLiteral> Lowering::findDeferrableLiteral(ValueID object, size_t index, std::span<const size_t> plainObjectFields, unsigned depth)
{
    auto uses = [&](const Value& value) {
        bool result = false;
        value.forEachChild([&](ValueID child) {
            result |= child == object;
        });
        return result;
    };
    auto& values = m_currentBlock->values;
    DeferredLiteral literal { m_currentBlock, object, 0, { index }, nullptr, { } };
    if (!plainObjectFields.empty()) {
        literal.isPlainObject = true;
        literal.memberIndices = Vector<size_t, 8>(plainObjectFields);
    }
    size_t useIndex = index + 1;
    for (; useIndex < values.size(); ++useIndex) {
        auto& value = m_function.at(values[useIndex]);
        if (!uses(value))
            continue;
        if (literal.isPlainObject || value.opcode != Opcode::PutField || value.child(0) != object || value.child(1) == object)
            break;
        literal.memberIndices.append(useIndex);
    }
    if (useIndex == values.size())
        return std::nullopt;
    auto& user = m_function.at(values[useIndex]);
    ValueID holder;
    auto isJSONStringify = [&] {
        if (user.opcode != Opcode::CallBuiltin || user.arguments.size() != 1 || user.arguments[0] != object)
            return false;
        auto& implementation = m_builtins.at(user.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::JSONStringify;
    };
    auto isFormattedByPattern = [&] {
        if (user.opcode != Opcode::CallBuiltin || !user.child(2) || user.arguments.size() != 2 || user.arguments[1] != object)
            return false;
        auto& implementation = m_builtins.at(user.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && familyOf(implementation.inlined) == BuiltinFamily::IntlTemporal;
    };
    bool isOnlyReadOnSlowPath = isFormattedByPattern();
    if (isOnlyReadOnSlowPath || isJSONStringify())
        literal.callIndex = useIndex;
    else {
        if (depth + 1 >= maxInlineJSONDepth)
            return std::nullopt;
        ValueID enclosingObject;
        if ((user.opcode == Opcode::PutField || user.opcode == Opcode::PutById) && user.child(1) == object && user.child(0) != object)
            enclosingObject = user.child(0);
        else if (isDefineField(user) && user.arguments.size() == 3 && user.arguments[2] == object && user.arguments[0] != object && user.arguments[1] != object)
            enclosingObject = user.arguments[0];
        else
            return std::nullopt;
        size_t enclosingIndex = values.find(enclosingObject);
        if (enclosingIndex == notFound || enclosingIndex > useIndex)
            return std::nullopt;
        std::optional<DeferredLiteral> enclosing;
        if (m_function.at(enclosingObject).opcode == Opcode::NewObjectWithShape)
            enclosing = findDeferrableLiteral(enclosingObject, enclosingIndex, { }, depth + 1);
        else if (auto* plainLiteral = objectLiteralAt(enclosingIndex); plainLiteral && plainLiteral->allocation == enclosingIndex && mayDeferObjectLiteral(*plainLiteral))
            enclosing = findDeferrableLiteral(enclosingObject, plainLiteral->fields.last(), plainLiteral->fields.span(), depth + 1);
        if (!enclosing || !enclosing->memberIndices.contains(useIndex))
            return std::nullopt;
        holder = values[useIndex];
        literal.callIndex = enclosing->callIndex;
    }
    for (auto& block : m_function.blocks) {
        for (size_t k = &block == m_currentBlock ? useIndex + 1 : 0; k < block.values.size(); ++k) {
            if (uses(m_function.at(block.values[k])))
                return std::nullopt;
        }
        if (block.terminator.value == object)
            return std::nullopt;
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments) {
                if (argument == object)
                    return std::nullopt;
            }
        }
    }
    SetForScope indexInBlock(m_currentIndexInBlock, literal.callIndex);
    if (!isOnlyReadOnSlowPath && !freshLiteralMembers(object, holder))
        return std::nullopt;
    return literal;
}

bool Lowering::tryDeferLiteralAllocation(ValueID object, std::span<const size_t> plainObjectFields)
{
    static constexpr unsigned maxDeferredLiterals = 16;
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return false;
    if (!m_deferredLiterals.isEmpty() && m_deferredLiterals[0].block != m_currentBlock)
        m_deferredLiterals.clear();
    if (deferredLiteralFor(object) || m_deferredLiterals.size() == maxDeferredLiterals)
        return false;
    auto literal = findDeferrableLiteral(object, m_currentIndexInBlock, plainObjectFields, 0);
    if (!literal)
        return false;
    m_deferredLiterals.append(WTF::move(*literal));
    return true;
}

bool Lowering::isDeferredLiteralStore(size_t index) const
{
    for (auto& literal : m_deferredLiterals) {
        if (literal.block == m_currentBlock && !literal.isPlainObject && !literal.isBeingMaterialized && literal.memberIndices.contains(index))
            return true;
    }
    return false;
}

Operand Lowering::materializeDeferredLiteral(DeferredLiteral& literal)
{
    if (literal.materializedIn == m_out.m_block)
        return literal.materialized;
    SetForScope isBeingMaterialized(literal.isBeingMaterialized, true);
    SetForScope indexInBlock(m_currentIndexInBlock, m_currentIndexInBlock);
    auto& values = m_currentBlock->values;
    if (literal.isPlainObject) {
        m_currentIndexInBlock = literal.memberIndices.last();
        lowerObjectLiteral(*objectLiteralAt(m_currentIndexInBlock));
    } else {
        Vector<DeferredLiteral*, 4> nestedLiterals;
        for (size_t index : literal.memberIndices) {
            auto& store = m_function.at(values[index]);
            if (store.opcode != Opcode::PutField)
                continue;
            if (auto* nested = deferredLiteralFor(store.child(1)); nested && !nested->isBeingMaterialized) {
                materializeDeferredLiteral(*nested);
                define(nested->object, nested->materialized);
                nested->isBeingMaterialized = true;
                nestedLiterals.append(nested);
            }
        }
        for (size_t index : literal.memberIndices) {
            m_currentIndexInBlock = index;
            define(values[index], lower(values[index], m_function.at(values[index])));
        }
        for (auto* nested : nestedLiterals)
            nested->isBeingMaterialized = false;
    }
    literal.materialized = m_values[literal.object.index()];
    literal.materializedIn = m_out.m_block;
    return literal.materialized;
}

void Lowering::appendFreshLiteral(JSONOutput& output, const Vector<JSONLiteralMember, 8>& literal, unsigned depth)
{
    Vector<std::optional<Vector<JSONLiteralMember, 8>>, 8> literalsWithin;
    for (auto& member : literal) {
        literalsWithin.append(depth + 1 < maxInlineJSONDepth ? freshLiteralMembers(member.value, member.definition) : std::nullopt);
        if (literalsWithin.last() && literalsWithin.last()->size() + 1 > output.remainingFields)
            literalsWithin.last() = std::nullopt;
        if (literalsWithin.last())
            output.remainingFields -= literalsWithin.last()->size() + 1;
    }
    appendMembers(output, literal.size(), [&](unsigned i) {
        auto& type = typeOf(literal[i].value);
        return JSONMember { m_program.string(literal[i].name), !literalsWithin[i] && (type.hasObjects() || type.primitives & (Primitive::Absent | Primitive::Undefined | Primitive::Symbol)) };
    }, [&](unsigned i, JSONExit& isLeftOut) {
        ValueID member = literal[i].value;
        if (literalsWithin[i]) {
            appendFreshLiteral(output, *literalsWithin[i], depth + 1);
            return;
        }
        bool isPredicted = !!m_function.at(member).prediction();
        appendValue(output, get(member), isPredicted ? predictedTypeOf(member) : Type(typeOf(member)), !isPredicted, depth + 1, JSONPosition::Property, isLeftOut);
    });
}

void Lowering::appendObjectInline(JSONOutput& output, CellPointer cell, const Vector<JSONKeyOrder, 4>& orders, unsigned depth, LBasicBlock otherwise, JSONExit& continuation)
{
    unsigned capacity = output.knownCapacity;
    LValue structure = decodeStructureID(loadField<Word32>(cell, m_heaps.JSCell_structureID).value());
    branchUnless(Word32(m_out.equal(m_out.load64(structure, m_heaps.Structure_prototype), loadImmutablePointer(Instance::offsetOfObjectPrototype()))), otherwise);
    static_assert(offsetOfOrderWord + sizeof(uint32_t) == offsetOfShapeWord && !(offsetOfOrderWord % sizeof(uint64_t)));
    LValue words = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
    uint64_t count = static_cast<uint64_t>(Typed::orderWordOf(0, maxFieldsInOrder));
    auto shapeAndOrderOf = [&](const JSONKeyOrder& order) {
        return static_cast<uint64_t>(Typed::shapeWordOf(m_program.at(order.layout).shape)) << 32 | Typed::orderWordOf(order.order, 0);
    };
    Vector<LBasicBlock, 4> bodies;
    const JSONKeyOrder* lastForEachCount = nullptr;
    for (auto& order : orders) {
        bodies.append(m_out.newBlock());
        if (order.minKeyCount < order.count)
            lastForEachCount = &order;
    }
    for (auto& order : orders) {
        LValue isComplete = m_out.equal(words, integer(static_cast<int64_t>(shapeAndOrderOf(order) | Typed::orderWordOf(0, order.count))).value());
        if (&order == &orders.last() && !lastForEachCount) {
            m_out.branch(isComplete, usually(bodies.last()), rarely(otherwise));
            break;
        }
        LBasicBlock next = m_out.newBlock();
        m_out.branch(isComplete, unsure(bodies[&order - orders.begin()]), unsure(next));
        m_out.appendTo(next);
    }
    for (auto& order : orders) {
        if (order.minKeyCount == order.count)
            continue;
        LBasicBlock isInOrder = m_out.newBlock();
        LBasicBlock next = &order == lastForEachCount ? otherwise : m_out.newBlock();
        m_out.branch(m_out.equal(m_out.bitAnd(words, integer(static_cast<int64_t>(~count)).value()), integer(static_cast<int64_t>(shapeAndOrderOf(order))).value()), unsure(isInOrder), unsure(next));
        m_out.appendTo(isInOrder);
        m_out.branch(m_out.aboveOrEqual(m_out.bitAnd(words, integer(static_cast<int64_t>(count)).value()), integer(static_cast<int64_t>(Typed::orderWordOf(0, order.minKeyCount))).value()), unsure(bodies[&order - orders.begin()]), unsure(next));
        if (next != otherwise)
            m_out.appendTo(next);
    }
    for (auto& order : orders) {
        auto& fields = m_program.at(order.layout).fields;
        m_out.appendTo(bodies[&order - orders.begin()]);
        output.knownCapacity = capacity;
        appendMembers(output, order.fields.size(), [&](unsigned i) {
            auto& field = fields[order.fields[i]];
            return JSONMember { m_program.string(field.name), !!(m_program.at(field.stored).primitives & (Primitive::Absent | Primitive::Undefined | Primitive::Symbol)) };
        }, [&](unsigned i, JSONExit& nothing) {
            appendField(output, cell, fields[order.fields[i]], depth + 1, nothing);
        });
        flushPendingBytes(output);
        m_out.jump(exitThrough(output, continuation));
    }
}

Operand Lowering::lowerAppendJSON(const Value& value)
{
    Integer start = in<Integer>(value.arguments[1]);
    int64_t written = m_function.at(value.arguments[2]).integer();
    Join join(m_out, Rep::Int64, Int64);
    if (written < 0) {
        LBasicBlock walks = m_out.newBlock();
        LBasicBlock writesNothing = m_out.newBlock();
        CellPointer cell = objectOrElse(value.arguments[0], walks);
        Vector<FTL::SwitchCase> cases;
        Vector<std::pair<LBasicBlock, FunctionID>> blocks;
        for (auto [layout, serializer] : m_program.jsonSerializers) {
            uint32_t shape = m_program.at(layout).shape;
            if (!shape || !m_program.hasCode(serializer))
                continue;
            blocks.append({ m_out.newBlock(), serializer });
            cases.append(FTL::SwitchCase(m_out.constInt32(static_cast<int32_t>(Typed::shapeWordOf(shape))), blocks.last().first, FTL::Weight()));
        }
        LBasicBlock matchesNone = m_out.newBlock();
        if (cases.isEmpty())
            m_out.jump(matchesNone);
        else
            m_out.switchInstruction(shapeWordOf(cell).value(), cases, matchesNone, FTL::Weight());
        for (auto [block, serializer] : blocks) {
            m_out.appendTo(block);
            join.add(Integer(callFunction(serializer, Int64, { cell.value(), start.value() })));
        }
        m_out.appendTo(matchesNone);
        Word32 cellType = loadByte(cell, m_heaps.JSCell_typeInfoType, safeIfReaches(cell, sizeof(JSCell)));
        branchUnless(notEqual(cellType, word32(FinalObjectType)), writesNothing);
        LBasicBlock hasNoLayout = m_out.newBlock();
        m_out.branch(equal(cellType, word32(ObjectType)).value(), unsure(walks), unsure(hasNoLayout));
        m_out.appendTo(hasNoLayout);
        static_assert(DerivedArrayType == ArrayType + 1);
        branchUnless(below(binary<Word32>(&Output::sub, cellType, word32(ArrayType)), word32(2)), writesNothing);
        branchUnless(equal(anyArrayIndexingMode(cell, IndexingShapeMask), word32(ContiguousShape)), walks);
        Pointer butterfly = butterflyOf(cell);
        branchUnless(Word32(m_out.notZero64(publicLength(butterfly).value())), walks);
        Boxed first(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), m_out.constInt64(0))));
        branchUnless(isNotEmpty(first), walks);
        branchUnless(isCell(first), walks);
        CellPointer firstCell = asCell(first);
        branchUnless(notEqual(loadByte(firstCell, m_heaps.JSCell_typeInfoType, safeIfReaches(firstCell, sizeof(JSCell))), word32(FinalObjectType)), writesNothing);
        m_out.jump(walks);
        m_out.appendTo(walks);
        join.add(Integer(AOT_TYPED_OPERATION(JSONAppendValue, start, in<Boxed>(value.arguments[0]), word32(jsonValueIsWholeOutput)).value()));
        m_out.appendTo(writesNothing);
        join.add(integer(isNotWrittenByDispatcher));
        return join.finish();
    }
    CellPointer cell = in<CellPointer>(value.arguments[0]);
    JSONOutput output;
    output.givesUp = newColdBlock();
    output.givesUpBeforeWriting = output.givesUp;
    output.cursor = m_proc.addVariable(Int64);
    setCursor(output, start.value());
    JSONExit continuation { m_out.newBlock() };
    LayoutID layout(static_cast<uint32_t>(written));
    bool usesOwnKeys = mayBeBuiltUp(m_program, layout);
    LBasicBlock otherwise = usesOwnKeys ? m_out.newBlock() : newColdBlock();
    branchUnless(Word32(m_out.aboveOrEqual(m_out.sub(m_out.framePointer(), m_out.constInt64(jsonSerializerStackReserve)), loadMutable64(Instance::offsetOfSoftStackLimit()))), output.givesUp);
    output.remainingFields = maxInlineJSONFieldsPerFunction;
    auto orders = keyOrdersForJSON(std::span<const LayoutID> { &layout, 1 });
    if (orders.isEmpty())
        m_out.jump(otherwise);
    else
        appendObjectInline(output, cell, orders, 0, otherwise, continuation);
    m_out.appendTo(otherwise);
    if (usesOwnKeys) {
        LBasicBlock isNotWritten = newColdBlock();
        output.knownCapacity = 0;
        output.remainingFields = maxInlineJSONFieldsPerFunction;
        appendObjectByOwnKeys(output, cell, layout, isNotWritten, continuation);
        m_out.appendTo(isNotWritten);
    }
    branchUnless(Word32(m_out.notEqual(start.value(), m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONBufferStart())))), output.givesUp);
    output.knownCapacity = 0;
    JSONExit never { output.givesUp };
    appendGeneric(output, box(cell), JSONPosition::Element, never);
    m_out.jump(exitThrough(output, continuation));
    m_out.appendTo(continuation.block);
    join.add(Integer(cursorOf(output)));
    m_out.appendTo(output.givesUp);
    join.add(integer(0));
    return join.finish();
}

void Lowering::appendArray(JSONOutput& output, CellPointer array, const Type& type, unsigned depth, JSONPosition position, JSONExit& nothing)
{
    bool exceedsInlineDepth = depth >= maxInlineJSONDepth && (depth >= maxInlineArrayDepth || !type.elements || serializersFor(m_program.at(type.elements)).isEmpty());
    if (exceedsInlineDepth || !type.elements || output.remainingFields < 2 || !hasCodeForObjectsIn(m_program.at(type.elements))) {
        appendGeneric(output, box(array), position, nothing);
        return;
    }
    output.remainingFields -= 2;
    auto& elements = m_program.at(type.elements);
    bool expectsNumbers = elements.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).isNumber();
    flushPendingBytes(output);
    reserveCapacity(output, 2);
    unsigned capacity = output.knownCapacity;
    JSONExit continuation { m_out.newBlock() };
    LBasicBlock otherwise = depth ? newColdBlock() : output.givesUpBeforeWriting;
    branchUnless(isPlainArray(array), otherwise);
    Word32 shape = anyArrayIndexingMode(array, IndexingShapeMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    auto forIndexingType = [&](IndexingType expectedShape, bool isLast, const auto& appendElement) {
        LBasicBlock hasIndexingShape = m_out.newBlock();
        LBasicBlock body = m_out.newBlock();
        LBasicBlock done = m_out.newBlock();
        LBasicBlock isNot = isLast ? otherwise : m_out.newBlock();
        if (isLast)
            m_out.branch(equal(shape, word32(expectedShape)).value(), usually(hasIndexingShape), rarely(isNot));
        else
            m_out.branch(equal(shape, word32(expectedShape)).value(), unsure(hasIndexingShape), unsure(isNot));
        m_out.appendTo(hasIndexingShape);
        output.knownCapacity = capacity;
        appendBytes(output, "["_s);
        flushPendingBytes(output);
        ValueFromBlock first = m_out.anchor(m_out.int64Zero);
        m_out.branch(isZero(length).value(), unsure(done), unsure(body));
        m_out.appendTo(body);
        LValue index = m_out.phi(Int64, first);
        output.knownCapacity = 0;
        appendElement(Integer(index));
        appendBytes(output, ","_s);
        flushPendingBytes(output);
        LValue following = m_out.add(index, m_out.constInt64(1));
        m_out.addIncomingToPhi(index, m_out.anchor(following));
        m_out.branch(m_out.below(following, length.value()), usually(body), rarely(done));
        m_out.appendTo(done);
        LValue last = m_out.sub(cursorOf(output), m_out.zeroExtPtr(m_out.notZero64(length.value())));
        m_out.store32As8(m_out.constInt32(']'), m_out.address(m_heaps.AOTTyped_argumentBuffer, last, 0));
        setCursor(output, m_out.add(last, m_out.constInt64(1)));
        ensureStillAliveHere(array);
        output.knownCapacity = 0;
        m_out.jump(exitThrough(output, continuation));
        if (!isLast)
            m_out.appendTo(isNot);
    };
    if (expectsNumbers) {
        forIndexingType(Int32Shape, false, [&](Integer index) {
            Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedInt32Properties, butterfly.value(), index.value())));
            branchUnless(isNotEmpty(element), output.givesUp);
            appendInteger(output, signExtended(low32(element)), maxJSONInt32Length);
        });
        forIndexingType(DoubleShape, true, [&](Integer index) {
            Number element(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
            branchUnless(equal(element, element), output.givesUp);
            appendNumber(output, element);
        });
    } else {
        forIndexingType(ContiguousShape, true, [&](Integer index) {
            Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
            branchUnless(isNotEmpty(element), output.givesUp);
            JSONExit never { output.givesUp };
            LBasicBlock delegatesRest = newColdBlock();
            LBasicBlock enclosingListDelegate = std::exchange(output.delegatesRestOfList, delegatesRest);
            appendValue(output, element, elements, false, depth + 1, JSONPosition::Element, never);
            bool isReached = !std::exchange(output.delegatesRestOfList, enclosingListDelegate);
            flushPendingBytes(output);
            LBasicBlock isWritten = m_out.newBlock();
            unsigned capacityAfter = output.knownCapacity;
            m_out.jump(isWritten);
            m_out.appendTo(delegatesRest);
            if (!isReached) {
                m_out.unreachable();
                m_out.appendTo(isWritten);
                return;
            }
            Pointer afterLast = AOT_TYPED_OPERATION(JSONAppendElementsFrom, Integer(cursorOf(output)), array, index);
            didAppendWithinCapacity(output, afterLast);
            appendBytes(output, "]"_s);
            flushPendingBytes(output);
            ensureStillAliveHere(array);
            m_out.jump(exitThrough(output, continuation));
            m_out.appendTo(isWritten);
            output.knownCapacity = capacityAfter;
        });
    }
    if (depth) {
        m_out.appendTo(otherwise);
        output.knownCapacity = capacity;
        appendGeneric(output, box(array), position, nothing);
        m_out.jump(exitThrough(output, continuation));
    }
    m_out.appendTo(continuation.block);
    output.knownCapacity = continuation.knownCapacity;
}

bool Lowering::appendArgument(JSONOutput& output, ValueID argument)
{
    Type expected = predictedTypeOf(argument);
    auto literal = freshLiteralMembers(argument, { });
    bool dispatchesOnShape = !literal && !expected.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).isString() && !hasCodeForJSON(expected);
    if (Options::auditAOTTypedAssertions() || (dispatchesOnShape && !(m_program.jsonDispatcher && m_program.hasCode(m_program.jsonDispatcher) && expected.hasObjects())))
        return false;
    output.givesUp = newColdBlock();
    output.givesUpBeforeWriting = newColdBlock();
    output.cursor = m_proc.addVariable(Int64);
    output.remainingFields = maxInlineJSONFields;
    setCursor(output, m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONBufferStart())));
    JSONExit nothing { output.givesUp };
    if (literal)
        appendFreshLiteral(output, *literal, 0);
    else if (dispatchesOnShape) {
        LValue cursor = callFunction(m_program.jsonDispatcher, Int64, { in<Boxed>(argument).value(), cursorOf(output) });
        LBasicBlock isWritten = m_out.newBlock();
        LBasicBlock isNotWritten = m_out.newBlock();
        m_out.branch(m_out.above(cursor, m_out.constInt64(isNotWrittenByDispatcher)), unsure(isWritten), unsure(isNotWritten));
        m_out.appendTo(isNotWritten);
        m_out.branch(m_out.notZero64(cursor), unsure(output.givesUpBeforeWriting), unsure(output.givesUp));
        m_out.appendTo(isWritten);
        setCursor(output, cursor);
    } else
        appendValue(output, get(argument), expected, false, 0, JSONPosition::Property, nothing);
    flushPendingBytes(output);
    return true;
}

Operand Lowering::lowerResponseJSON(ValueID result, const Value& value)
{
    JSONOutput output;
    bool takesInitOperands = m_builtins.at(value.builtin()).implementation.inlined == InlineBuiltin::NewJSONResponse;
    if ((takesInitOperands || value.arguments.size() <= numberOfArgumentGPRs) && appendArgument(output, value.arguments[0])) {
        LBasicBlock calls = m_out.newBlock();
        AOT_TYPED_OPERATION(JSONPrepare, Integer(cursorOf(output)), in<Boxed>(value.arguments[0]));
        m_out.jump(calls);
        m_out.appendTo(output.givesUp);
        AOT_TYPED_OPERATION(JSONDiscard);
        m_out.jump(calls);
        m_out.appendTo(output.givesUpBeforeWriting);
        m_out.jump(calls);
        m_out.appendTo(calls);
    }
    auto& type = typeOf(result);
    if (takesInitOperands) {
        constexpr unsigned firstHeaderOperand = 4;
        unsigned headerOperandCount = value.arguments.size() - firstHeaderOperand;
        LValue buffer = transientBuffer(std::max(headerOperandCount, 1u) * sizeof(EncodedJSValue));
        for (unsigned i = 0; i < headerOperandCount; ++i)
            m_out.store64(in<Boxed>(value.arguments[firstHeaderOperand + i]).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
        return ensure(AOT_TYPED_OPERATION(NewJSONResponse, in<Boxed>(value.arguments[0]), in<Boxed>(value.arguments[1]), in<Boxed>(value.arguments[2]), in<Boxed>(value.arguments[3]), word32(headerOperandCount / 2), Pointer(buffer)), type);
    }
    Boxed yielded = callBuiltinFunction(value, builtinCallEffects(value));
    return guaranteesResultType(m_program, m_function, value) ? convert(yielded, type, repOf(type)) : ensure(yielded, type);
}

Operand Lowering::lowerJSONStringify(ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    auto genericPath = [&] {
        Boxed yielded = callBuiltinFunction(value, builtinCallEffects(value));
        return guaranteesResultType(m_program, m_function, value) ? convert(yielded, type, rep) : ensure(yielded, type);
    };
    JSONOutput output;
    if (!appendArgument(output, value.arguments[0]))
        return genericPath();
    Join join(m_out, rep, b3TypeOf(rep));
    LBasicBlock isFinished = m_out.newBlock();
    CellPointer string = AOT_TYPED_OPERATION(JSONFinish, Integer(cursorOf(output)));
    m_out.branch(m_out.notNull(string.value()), usually(isFinished), rarely(output.givesUp));
    m_out.appendTo(isFinished);
    join.add(ensure(string, m_program.at(m_program.types.string), type));
    m_out.appendTo(output.givesUp);
    AOT_TYPED_OPERATION(JSONDiscard);
    m_out.jump(output.givesUpBeforeWriting);
    m_out.appendTo(output.givesUpBeforeWriting);
    join.add(genericPath());
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
