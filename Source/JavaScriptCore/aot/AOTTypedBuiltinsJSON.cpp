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
    size_t length = cursor - instance->jsonBufferStart;
    size_t capacity = instance->jsonBufferEnd - instance->jsonBufferStart;
    if (size > String::MaxLength || length + size > String::MaxLength)
        return nullptr;
    size_t newCapacity = std::max({ capacity * 2, length + size, minJSONBufferCapacity });
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendValue, void*, (Instance* instance, uint64_t cursorBits, EncodedJSValue encodedValue, uint32_t isElement))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* cursor = std::bit_cast<Latin1Character*>(cursorBits);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isCell()) {
        if (!value)
            return nullptr;
        JSType type = value.asCell()->type();
        if (type >= ObjectType)
            return appendObjectGeneric(instance, globalObject, cursor, asObject(value));
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
            return cursor;
        }
        if (type != SymbolType)
            return nullptr;
        value = jsUndefined();
    }
    if (value.isUndefined()) {
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

static NEVER_INLINE RefPtr<StringImpl> makeStringWithWideSegments(std::span<const Latin1Character> characters, std::span<const JSONWideSegment> segments)
{
    static constexpr size_t vectorOverreadCapacity = 32;
    CheckedSize most = characters.size() + vectorOverreadCapacity;
    for (auto& segment : segments)
        most += CheckedSize(segment.text.length()) * static_cast<size_t>(segment.needsEscaping ? 6 : 1);
    if (most.hasOverflowed() || most.value() > String::MaxLength)
        return nullptr;
    Vector<char16_t, 1024> storage;
    if (!storage.tryGrow(most.value()))
        return nullptr;
    std::span<char16_t> output = storage.mutableSpan();
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
    return StringImpl::create(storage.span().first(output.data() - storage.span().data()));
}

static ALWAYS_INLINE void releaseLargeJSONBuffer(Instance* instance)
{
    if (static_cast<size_t>(instance->jsonBufferEnd - instance->jsonBufferStart) > maxRetainedJSONBufferCapacity) [[unlikely]] {
        fastFree(instance->jsonBufferStart);
        instance->jsonBufferStart = nullptr;
        instance->jsonBufferEnd = nullptr;
    }
}

static void resetJSONOutput(Instance* instance)
{
    if (instance->jsonWideSegments)
        instance->jsonWideSegments->clear();
    instance->jsonWideSegmentCount = 0;
    releaseLargeJSONBuffer(instance);
}

static NEVER_INLINE JSCell* finishWithWideSegments(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint64_t cursor)
{
    RefPtr<StringImpl> text;
    if (!mayHaveToJSON(globalObject, vm, globalObject->objectPrototype()) && !mayHaveToJSON(globalObject, vm, globalObject->arrayPrototype()))
        text = makeStringWithWideSegments({ instance->jsonBufferStart, std::bit_cast<Latin1Character*>(cursor) }, instance->jsonWideSegments->span().first(instance->jsonWideSegmentCount));
    resetJSONOutput(instance);
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONFinish, JSCell*, (Instance* instance, uint64_t cursor))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
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
    releaseLargeJSONBuffer(instance);
    return result;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONDiscard, void, (Instance* instance))
{
    countOperationNamed(instance, __func__);
    resetJSONOutput(instance);
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
    resetJSONOutput(instance);
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

void destroyJSONBuffers(Instance& instance)
{
    takePreparedJSON(instance, JSValue());
    fastFree(instance.jsonBufferStart);
    delete instance.jsonWideSegments;
}

namespace Typed {

using namespace B3;

static constexpr unsigned maxInlineJSONDepth = 3;
static constexpr unsigned maxInlineJSONFields = 48;
static constexpr unsigned maxInlineJSONFieldsPerFunction = 24;
static constexpr unsigned maxInlineArrayDepth = 6;
static constexpr int64_t jsonSerializerStackReserve = 32 * KB;
static constexpr int64_t isNotWrittenByDispatcher = 1;
static constexpr unsigned minRequestedJSONCapacity = 128;

std::optional<Writes> writesOfInlineJSON(InlineBuiltin which, std::span<const Type> arguments)
{
    if (which == InlineBuiltin::AppendJSON)
        return Writes::RuntimeState;
    return writesIf(!arguments.empty() && (arguments[0].hasObjects() || arguments[0].withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).isString()), Writes::Heap);
}

std::optional<Operand> Lowering::lowerInlineJSON(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    if (which == InlineBuiltin::AppendJSON)
        return lowerAppendJSON(value);
    if (which == InlineBuiltin::ResponseJSON)
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
        ValueID origin = m_program.originOf(function, id);
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
        auto* engineClass = expected.singleEngineClass();
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
        for (TypeID type : { program.types.object, program.types.integer }) {
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
    if (dispatches && !made.isEmpty()) {
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
            uint32_t born = 0;
            for (unsigned i = 0; i < allocation.order.size(); ++i) {
                if (layout.fields[allocation.order[i]].flags & fieldIsNeverStored)
                    continue;
                expected.append(allocation.order[i]);
                born += i < allocation.initialFieldCount;
            }
            size_t order = layout.orders.findIf([&](auto& candidate) {
                return candidate.size() >= expected.size() && std::equal(expected.begin(), expected.end(), candidate.begin());
            });
            if (order != notFound && born <= maxFieldsInOrder)
                add(member, order, born);
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
    if (auto* engineClass = expected.singleEngineClass(); engineClass && engineClass->type == ArrayType)
        return expected.elements && !m_program.at(expected.elements).isAnything() && hasCodeForObjectsIn(m_program.at(expected.elements));
    return !keyOrdersForJSON(expected).isEmpty() || !serializersFor(expected).isEmpty();
}

bool Lowering::hasCodeForObjectsIn(const Type& type) const
{
    if (!type.hasObjects())
        return true;
    auto* engineClass = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).singleEngineClass();
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
    unsigned wanted = std::max(size, minRequestedJSONCapacity);
    LBasicBlock continuation = m_out.newBlock();
    LBasicBlock grows = newColdBlock();
    LValue cursor = cursorOf(output);
    LValue end = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONBufferEnd()));
    m_out.branch(m_out.aboveOrEqual(m_out.sub(end, cursor), m_out.constInt64(wanted)), usually(continuation), rarely(grows));
    m_out.appendTo(grows);
    Pointer moved = AOT_TYPED_OPERATION(JSONReserve, Integer(cursor), integer(wanted));
    orElse(Word32(m_out.notZero64(moved.value())), output.givesUp);
    setCursor(output, moved.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    output.knownCapacity = wanted;
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
    orElse(Word32(m_out.notZero64(cursor.value())), output.givesUp);
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

void Lowering::appendValue(JSONOutput& output, Operand operand, const Type& type, bool typeIsFact, unsigned depth, JSONPosition position, JSONExit& nothing)
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
        appendCell(output, operand.as<CellPointer>(), type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent), typeIsFact, depth, position, nothing);
        return;
    case Rep::LiteralString:
        appendValue(output, convert(operand, type, Rep::JSValue), type.withLiteralStringWidened(), typeIsFact, depth, position, nothing);
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
        if (typeIsFact)
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
        if (!typeIsFact && expected.isNumber()) {
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
    appendCell(output, asCell(boxed), expected, typeIsFact, depth, position, nothing);
    flushPendingBytes(output);
    m_out.jump(exitThrough(output, continuation));
    m_out.appendTo(notCell);
    output.knownCapacity = capacity;
    if (typeIsFact)
        appendNullishOrOmit(output, haveEqualBits(boxed, boxedConstant(jsNull())), type, position, nothing, continuation);
    else {
        appendGeneric(output, boxed, position, nothing);
        m_out.jump(exitThrough(output, continuation));
    }
    m_out.appendTo(continuation.block);
    output.knownCapacity = continuation.knownCapacity;
}

void Lowering::appendCell(JSONOutput& output, CellPointer cell, const Type& type, bool typeIsFact, unsigned depth, JSONPosition position, JSONExit& nothing)
{
    if (type.isString()) {
        if (typeIsFact) {
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
    if (auto* engineClass = type.singleEngineClass(); engineClass && engineClass->type == ArrayType) {
        appendArray(output, cell, type, depth, position, nothing);
        return;
    }
    if (auto* engineClass = type.singleEngineClass(); engineClass && engineClass->type == JSDateType) {
        appendDate(output, cell, *engineClass, position, nothing);
        return;
    }
    if (type.singleLayout()) {
        appendObject(output, cell, type, depth, position, nothing);
        return;
    }
    appendGeneric(output, box(cell), position, nothing);
}

void Lowering::appendDate(JSONOutput& output, CellPointer date, const EngineClass& dateClass, JSONPosition position, JSONExit& nothing)
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
    Type held = m_program.at(field.stored);
    if (held.primitives & Primitive::Absent)
        held.primitives = (held.primitives & ~Primitive::Absent) | Primitive::Undefined;
    appendValue(output, readField(cell, field, held), held, true, depth, JSONPosition::Property, nothing);
}

void Lowering::appendBySerializer(JSONOutput& output, CellPointer cell, FunctionID serializer)
{
    flushPendingBytes(output);
    LValue cursor = callFunction(serializer, Int64, { cell.value(), cursorOf(output) });
    orElse(Word32(m_out.notZero64(cursor)), output.givesUp);
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
    LValue structure = structureWithID(loadField<Word32>(cell, m_heaps.JSCell_structureID).value());
    orElse(Word32(m_out.equal(m_out.load64(structure, m_heaps.Structure_prototype), fixedPointer(Instance::offsetOfObjectPrototype()))), otherwise);
    LValue words = m_out.load64(at(cell.value(), m_out.constIntPtr(offsetOfOrderWord)));
    uint64_t shapeAndExtraProperties = 0xffffffffull << 32 | orderWordHasExtraProperties;
    orElse(Word32(m_out.equal(m_out.bitAnd(words, integer(static_cast<int64_t>(shapeAndExtraProperties)).value()), integer(static_cast<int64_t>(static_cast<uint64_t>(Typed::shapeWordOf(layout.shape)) << 32)).value())), otherwise);
    LValue places = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfTypedOrderKeyIndices()));
    LValue place = m_out.zeroExtPtr(m_out.load16ZeroExt32(at(places, m_out.constIntPtr(sizeof(uint16_t) * layout.shape))));
    LValue tables = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfTypedOrderKeys()));
    LValue known = m_out.load64(at(tables, m_out.shl(place, m_out.constInt32(3))));
    LBasicBlock isNotCachedYet = newColdBlock();
    LBasicBlock hasOrders = m_out.newBlock();
    ValueFromBlock knownValue = m_out.anchor(known);
    m_out.branch(m_out.notZero64(known), usually(hasOrders), rarely(isNotCachedYet));
    m_out.appendTo(isNotCachedYet);
    Pointer asked = AOT_TYPED_OPERATION(JSONKeyOrdersOf, cell);
    orElse(Word32(m_out.notZero64(asked.value())), otherwise);
    ValueFromBlock computedValue = m_out.anchor(asked.value());
    m_out.jump(hasOrders);
    m_out.appendTo(hasOrders);
    LValue orders = m_out.phi(Int64, knownValue, computedValue);
    LValue order = m_out.bitAnd(words, m_out.constInt64(0xffff));
    orElse(Word32(m_out.below(order, m_out.load64(at(orders, m_out.constIntPtr(0))))), otherwise);
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
    ValueID object = m_program.originOf(m_function, argument);
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
        return m_function.at(m_program.originOf(m_function, member.value)).opcode == Opcode::Undefined;
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
    LValue structure = structureWithID(loadField<Word32>(cell, m_heaps.JSCell_structureID).value());
    orElse(Word32(m_out.equal(m_out.load64(structure, m_heaps.Structure_prototype), fixedPointer(Instance::offsetOfObjectPrototype()))), otherwise);
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
    CellPointer cell = in<CellPointer>(value.arguments[0]);
    Integer start = in<Integer>(value.arguments[1]);
    int64_t written = m_function.at(value.arguments[2]).integer();
    Join join(m_out, Rep::Int64, Int64);
    if (written < 0) {
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
        m_out.switchInstruction(shapeWordOf(cell).value(), cases, matchesNone, FTL::Weight());
        for (auto [block, serializer] : blocks) {
            m_out.appendTo(block);
            join.add(Integer(callFunction(serializer, Int64, { cell.value(), start.value() })));
        }
        m_out.appendTo(matchesNone);
        join.add(integer(isNotWrittenByDispatcher));
        return join.finish();
    }
    JSONOutput output;
    output.givesUp = newColdBlock();
    output.givesUpBeforeWriting = output.givesUp;
    output.cursor = m_proc.addVariable(Int64);
    setCursor(output, start.value());
    JSONExit continuation { m_out.newBlock() };
    LayoutID layout(static_cast<uint32_t>(written));
    bool usesOwnKeys = mayBeBuiltUp(m_program, layout);
    LBasicBlock otherwise = usesOwnKeys ? m_out.newBlock() : newColdBlock();
    orElse(Word32(m_out.aboveOrEqual(m_out.sub(m_out.framePointer(), m_out.constInt64(jsonSerializerStackReserve)), changing64(Instance::offsetOfSoftStackLimit()))), output.givesUp);
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
    orElse(Word32(m_out.notEqual(start.value(), m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONBufferStart())))), output.givesUp);
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
    orElse(isPlainArray(array), otherwise);
    Word32 shape = anyArrayIndexingMode(array, IndexingShapeMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    auto forIndexingType = [&](IndexingType kept, bool isLast, const auto& appendElement) {
        LBasicBlock hasIndexingShape = m_out.newBlock();
        LBasicBlock body = m_out.newBlock();
        LBasicBlock done = m_out.newBlock();
        LBasicBlock isNot = isLast ? otherwise : m_out.newBlock();
        if (isLast)
            m_out.branch(equal(shape, word32(kept)).value(), usually(hasIndexingShape), rarely(isNot));
        else
            m_out.branch(equal(shape, word32(kept)).value(), unsure(hasIndexingShape), unsure(isNot));
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
            orElse(isNotEmpty(element), output.givesUp);
            appendInteger(output, signExtended(low32(element)), maxJSONInt32Length);
        });
        forIndexingType(DoubleShape, true, [&](Integer index) {
            Number element(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
            orElse(equal(element, element), output.givesUp);
            appendNumber(output, element);
        });
    } else {
        forIndexingType(ContiguousShape, true, [&](Integer index) {
            Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
            orElse(isNotEmpty(element), output.givesUp);
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
        uint32_t least = UINT32_MAX;
        uint32_t most = 0;
        for (auto [layout, serializer] : m_program.jsonSerializers) {
            if (uint32_t shape = m_program.at(layout).shape) {
                least = std::min(least, shape);
                most = std::max(most, shape);
            }
        }
        CellPointer cell = objectOrElse(argument, output.givesUpBeforeWriting);
        orElse(below(add(shapeWordOf(cell), word32(static_cast<int32_t>(most))), word32(static_cast<int32_t>(most - least + 1))), output.givesUpBeforeWriting);
        LValue cursor = callFunction(m_program.jsonDispatcher, Int64, { cell.value(), cursorOf(output) });
        orElse(Word32(m_out.above(cursor, m_out.constInt64(isNotWrittenByDispatcher))), output.givesUp);
        setCursor(output, cursor);
    } else
        appendValue(output, get(argument), expected, false, 0, JSONPosition::Property, nothing);
    flushPendingBytes(output);
    return true;
}

Operand Lowering::lowerResponseJSON(ValueID result, const Value& value)
{
    JSONOutput output;
    if (value.arguments.size() <= numberOfArgumentGPRs && appendArgument(output, value.arguments[0])) {
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
