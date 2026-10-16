/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

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

static constexpr size_t leastCapacityOfJSONBuffer = 4 * KB;
static constexpr size_t mostCapacityOfJSONBufferKept = 64 * KB;
static constexpr unsigned spareCapacityOfJSONBuffer = 64;
static constexpr unsigned maxLengthOfInt32InJSON = 11;
static constexpr unsigned maxLengthOfIntegerInJSON = 20;
static constexpr unsigned maxLengthOfNumberInJSON = WTF::dragonbox::max_string_length<WTF::dragonbox::ieee754_binary64>();
static_assert(maxLengthOfNumberInJSON >= maxLengthOfInt32InJSON && maxLengthOfNumberInJSON + 2 <= spareCapacityOfJSONBuffer);

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
    size_t newCapacity = std::max({ capacity * 2, length + size, leastCapacityOfJSONBuffer });
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

static ALWAYS_INLINE bool copyAndAskWhetherToEscape(std::span<const Latin1Character> characters, Latin1Character* to)
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
    cursor = reserveInJSONBuffer(instance, cursor, characters.size() * 6 + spareCapacityOfJSONBuffer);
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
    cursor = reserveInJSONBuffer(instance, cursor, characters.size() + spareCapacityOfJSONBuffer);
    if (!cursor) [[unlikely]]
        return nullptr;
    if (copyAndAskWhetherToEscape(characters, cursor)) [[unlikely]]
        return appendEscapedCharacters(instance, cursor, characters);
    return cursor + characters.size();
}

static NEVER_INLINE Latin1Character* appendCharactersOfRope(Instance* instance, CallFrame* callFrame, Latin1Character* cursor, JSString* string)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto value = string->tryGetValue();
    if (value.data.isNull())
        return nullptr;
    return appendCharacters(instance, cursor, *value.data.impl());
}

static ALWAYS_INLINE Latin1Character* appendInteger(Latin1Character* cursor, int64_t integer)
{
    char* to = std::bit_cast<char*>(cursor);
    return std::bit_cast<Latin1Character*>(std::to_chars(to, to + maxLengthOfIntegerInJSON, integer).ptr);
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
    return std::bit_cast<Latin1Character*>(WTF::dragonbox::detail::to_chars_n<WTF::dragonbox::Mode::ToShortest>(number, std::span { std::bit_cast<char*>(cursor), maxLengthOfNumberInJSON }).data());
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
    return appendCharactersOfRope(instance, DECLARE_CALL_FRAME(*instance->vm), std::bit_cast<Latin1Character*>(cursor), string);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendInteger, void*, (uint64_t cursor, uint64_t integer))
{
    return appendInteger(std::bit_cast<Latin1Character*>(cursor), static_cast<int64_t>(integer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendNumber, void*, (uint64_t cursor, double number))
{
    return appendNumber(std::bit_cast<Latin1Character*>(cursor), number);
}

static Latin1Character* appendDate(JSGlobalObject* globalObject, VM& vm, Latin1Character* cursor, DateInstance& date)
{
    if (date.structure() != globalObject->dateStructure() || !globalObject->datePrototype()->structure()->hasImmutableProperties())
        return nullptr;
    double time = date.internalNumber();
    if (!std::isfinite(time))
        return appendLiteral(cursor, "null"_s);
    auto fields = date.gregorianDateTimeUTC(vm.dateCache);
    if (!fields)
        return nullptr;
    int milliseconds = static_cast<int>(fmod(time, msPerSecond));
    if (milliseconds < 0)
        milliseconds += msPerSecond;
    auto append = [&](char before, unsigned number, unsigned digits) {
        *cursor++ = before;
        for (unsigned i = digits; i--; number /= 10)
            cursor[i] = '0' + number % 10;
        cursor += digits;
    };
    int year = fields.year();
    if (year < 0 || year > 9999) {
        *cursor++ = '"';
        append(year < 0 ? '-' : '+', std::abs(year), 6);
    } else
        append('"', year, 4);
    append('-', fields.month() + 1, 2);
    append('-', fields.monthDay(), 2);
    append('T', fields.hour(), 2);
    append(':', fields.minute(), 2);
    append(':', fields.second(), 2);
    append('.', milliseconds, 3);
    *cursor++ = 'Z';
    *cursor++ = '"';
    return cursor;
}

static Latin1Character* appendObjectGenerally(Instance* instance, JSGlobalObject* globalObject, VM& vm, Latin1Character* cursor, JSObject* object)
{
    if (object->type() != JSDateType) [[likely]] {
        bool isWritten = tryStringifyWithoutSideEffects(*globalObject, object, [&](std::span<const Latin1Character> characters) {
            cursor = reserveInJSONBuffer(instance, cursor, characters.size() + spareCapacityOfJSONBuffer);
            if (!cursor)
                return;
            memcpy(cursor, characters.data(), characters.size());
            cursor += characters.size();
        });
        if (isWritten)
            return cursor;
    }
    cursor = reserveInJSONBuffer(instance, cursor, 2 * spareCapacityOfJSONBuffer);
    if (!cursor)
        return nullptr;
    if (object->type() == JSDateType)
        return appendDate(globalObject, vm, cursor, *uncheckedDowncast<DateInstance>(object));
    String text = tryStringifyWithoutSideEffects(*globalObject, object);
    if (text.isNull())
        return nullptr;
    if (!text.is8Bit()) {
        unsigned last = text.length() - 1;
        String allButLast = text.left(last);
        cursor = appendWideSegment(instance, cursor, allButLast.impl(), false);
        *cursor++ = static_cast<Latin1Character>(text[last]);
        return cursor;
    }
    auto characters = text.span8();
    cursor = reserveInJSONBuffer(instance, cursor, characters.size() + spareCapacityOfJSONBuffer);
    if (!cursor)
        return nullptr;
    memcpy(cursor, characters.data(), characters.size());
    return cursor + characters.size();
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
            return appendObjectGenerally(instance, globalObject, vm, cursor, asObject(value));
        if (type == StringType) {
            auto string = asString(value)->tryGetValue();
            if (string.data.isNull())
                return nullptr;
            cursor = reserveInJSONBuffer(instance, cursor, 2 * spareCapacityOfJSONBuffer);
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
    cursor = reserveInJSONBuffer(instance, cursor, 2 * spareCapacityOfJSONBuffer);
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

static NEVER_INLINE RefPtr<StringImpl> makeTextWithWideSegments(std::span<const Latin1Character> characters, std::span<const JSONWideSegment> segments)
{
    static constexpr size_t roomForWholeVectors = 32;
    CheckedSize most = characters.size() + roomForWholeVectors;
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

static NEVER_INLINE JSCell* finishWithWideSegments(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint64_t cursor)
{
    RefPtr<StringImpl> text;
    if (!mayHaveToJSON(globalObject, vm, globalObject->objectPrototype()) && !mayHaveToJSON(globalObject, vm, globalObject->arrayPrototype()))
        text = makeTextWithWideSegments({ instance->jsonBufferStart, std::bit_cast<Latin1Character*>(cursor) }, instance->jsonWideSegments->span().first(instance->jsonWideSegmentCount));
    instance->jsonWideSegments->clear();
    instance->jsonWideSegmentCount = 0;
    return text ? jsNontrivialString(vm, String { text.releaseNonNull() }) : nullptr;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAppendElementsFrom, void*, (Instance* instance, uint64_t cursorBits, JSCell* array, uint64_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* cursor = std::bit_cast<Latin1Character*>(cursorBits);
    bool isWritten = tryStringifyElementsWithoutSideEffects(*globalObject, *asArray(array), index, [&](std::span<const Latin1Character> characters) {
        cursor = reserveInJSONBuffer(instance, cursor, characters.size() + spareCapacityOfJSONBuffer);
        if (!cursor)
            return;
        memcpy(cursor, characters.data(), characters.size());
        cursor += characters.size();
    });
    return isWritten ? cursor : nullptr;
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
            result = jsNontrivialString(vm, String { impl.releaseNonNull() });
        }
    }
    if (static_cast<size_t>(instance->jsonBufferEnd - instance->jsonBufferStart) > mostCapacityOfJSONBufferKept) [[unlikely]] {
        fastFree(instance->jsonBufferStart);
        instance->jsonBufferStart = nullptr;
        instance->jsonBufferEnd = nullptr;
    }
    return result;
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
            text = makeTextWithWideSegments(characters, instance->jsonWideSegments->span().first(instance->jsonWideSegmentCount));
        else {
            std::span<Latin1Character> copy;
            text = StringImpl::tryCreateUninitialized(characters.size(), copy);
            if (text) [[likely]]
                memcpy(copy.data(), characters.data(), characters.size());
        }
    }
    if (instance->jsonWideSegmentCount) [[unlikely]] {
        instance->jsonWideSegments->clear();
        instance->jsonWideSegmentCount = 0;
    }
    if (static_cast<size_t>(instance->jsonBufferEnd - instance->jsonBufferStart) > mostCapacityOfJSONBufferKept) [[unlikely]] {
        fastFree(instance->jsonBufferStart);
        instance->jsonBufferStart = nullptr;
        instance->jsonBufferEnd = nullptr;
    }
    if (!text)
        return;
    holder->preparedJSONText = text.leakRef();
    holder->preparedJSONValue = value;
}

String takePreparedJSON(Instance& instance, JSValue value)
{
    RefPtr<StringImpl> text = adoptRef(std::exchange(instance.preparedJSONText, nullptr));
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

static constexpr unsigned maxDepthOfJSONInPlace = 3;
static constexpr unsigned maxFieldsOfJSONInPlace = 48;
static constexpr unsigned maxFieldsOfJSONInPlaceInFunction = 24;
static constexpr unsigned maxDepthOfArraysInPlace = 6;
static constexpr int64_t stackKeptFreeByJSONSerializers = 32 * KB;
static constexpr int64_t isNotWrittenByDispatcher = 1;
static constexpr unsigned leastCapacityAskedForJSON = 128;

std::optional<Writes> writesOfInlineJSON(InlineBuiltin which, std::span<const Type> arguments)
{
    if (which == InlineBuiltin::AppendJSON)
        return Writes::RuntimeState;
    return writesIf(!arguments.empty() && arguments[0].hasObjects(), Writes::Heap);
}

std::optional<Operand> Lowering::lowerInlineJSON(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    if (which == InlineBuiltin::AppendJSON)
        return lowerAppendJSON(value);
    if (which == InlineBuiltin::ResponseJSON)
        return lowerResponseJSON(result, value);
    return lowerJSONStringify(result, value);
}

static bool isWrittenAsItIsInJSON(std::span<const uint8_t> name)
{
    return std::ranges::all_of(name, [](uint8_t character) {
        return character >= ' ' && character < 0x7f && character != '"' && character != '\\';
    });
}

static bool isKeyWrittenInPlace(std::span<const uint8_t> name)
{
    return !name.empty() && !isASCIIDigit(name.front()) && !equalSpans(name, asBytes("__proto__"_s.span8())) && !equalSpans(name, asBytes("toJSON"_s.span8())) && isWrittenAsItIsInJSON(name);
}

namespace {

class SitesOfJSON {
public:
    SitesOfJSON(const Program& program, BuiltinID stringify, BuiltinID responseJSON)
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
            if (!m_program.functions.mayHold(i, builtinCalls))
                return;
            auto& function = m_program.functions[i];
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode == Opcode::CallBuiltin && (value.builtin() == m_stringify || value.builtin() == m_responseJSON) && !value.arguments.isEmpty())
                        sites[i].append(id);
                }
            }
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
        if (depth + 1 >= maxDepthOfJSONInPlace)
            return;
        for (auto& block : function.blocks) {
            size_t allocation = block.values.find(origin);
            if (allocation == notFound)
                continue;
            for (size_t index : fieldsMadeWithAllocation(m_program, function, block, allocation, false, false)) {
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
    BuiltinID append = builtins.find(BuiltinKind::Operation, { }, builtins.nameOf(asBytes("AppendJSON"_s.span8())));
    BuiltinID responseJSON = builtins.find(BuiltinKind::StaticFunction, builtins.nameOf(asBytes("Response"_s.span8())), builtins.nameOf(asBytes("json"_s.span8())));
    RELEASE_ASSERT(stringify && append && responseJSON);
    unsigned count = program.functions.size();
    SitesOfJSON sites(program, stringify, responseJSON);
    sites.find();
    if (sites.isEmpty())
        return;
    static constexpr unsigned maxMembers = 4;
    auto isTaken = [&](LayoutID member) {
        auto& layout = program.at(member);
        return layout.kind == LayoutKind::Record && layout.isAllocated && layout.fields.size() <= maxFields && std::ranges::all_of(layout.fields, [&](const Field& field) {
            auto name = program.string(field.name);
            return isPrivateName(name) || isKeyWrittenInPlace(name);
        });
    };
    auto forEachView = [&](TypeID id, const auto& functor) {
        for (unsigned levels = 0; id && levels < 4; ++levels) {
            auto& type = program.at(id);
            for (LayoutID view : type.views)
                functor(view, levels);
            id = type.elements;
        }
    };
    BitVector isWritten(program.layouts.size());
    Vector<LayoutID> worklist;
    auto giveFunction = [&](LayoutID member) {
        if (!isWritten.set(member.index()))
            worklist.append(member);
    };
    std::array<BitVector, maxDepthOfJSONInPlace> isVisitedAt;
    auto visit = [&](auto& self, LayoutID layout, unsigned depth) -> void {
        if (isVisitedAt[depth].set(layout.index()))
            return;
        for (auto& field : program.at(layout).fields) {
            forEachView(field.type, [&](LayoutID view, unsigned levels) {
                if (program.at(view).members.size() > maxMembers)
                    return;
                for (LayoutID member : program.at(view).members) {
                    if (!isTaken(member))
                        continue;
                    if (depth + 1 + levels >= maxDepthOfJSONInPlace)
                        giveFunction(member);
                    else
                        self(self, member, depth + 1 + levels);
                }
            });
        }
    };
    bool dispatches = false;
    for (auto [id, depth] : sites.expected()) {
        Type expected = program.at(id).withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent);
        auto* engineClass = expected.onlyEngineClass();
        bool isList = engineClass && engineClass->type == ArrayType && expected.elements && !program.at(expected.elements).isAnything();
        LayoutID only = expected.onlyView();
        bool isWrittenInPlace = isList || (only && !program.at(only).members.isEmpty() && program.at(only).members.size() <= maxMembers && std::ranges::all_of(program.at(only).members, isTaken));
        if (depth && !isWrittenInPlace)
            continue;
        dispatches |= !isWrittenInPlace;
        forEachView(id, [&, depth = depth](LayoutID view, unsigned levels) {
            for (LayoutID member : program.at(view).members) {
                if (!isTaken(member))
                    continue;
                if (isWrittenInPlace && depth + levels < maxDepthOfJSONInPlace)
                    visit(visit, member, depth + levels);
                else
                    giveFunction(member);
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
        made.isBodyOfSeveralSites = true;
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
    LayoutID view = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).onlyView();
    if (!view || m_program.at(view).members.size() > maxMembers)
        return { };
    return keyOrdersForJSON(m_program.at(view).members.span());
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
                keys.leastCount = position;
            auto name = m_program.string(layout.fields[index].name);
            if (isPrivateName(name))
                continue;
            isRefused |= name.empty() || isASCIIDigit(name.front()) || equalSpans(name, asBytes("__proto__"_s.span8())) || equalSpans(name, asBytes("toJSON"_s.span8())) || !isWrittenAsItIsInJSON(name);
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
                born += i < allocation.born;
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
    if (auto* engineClass = expected.onlyEngineClass(); engineClass && engineClass->type == ArrayType)
        return expected.elements && !m_program.at(expected.elements).isAnything() && hasCodeForObjectsIn(m_program.at(expected.elements));
    return !keyOrdersForJSON(expected).isEmpty() || !serializersFor(expected).isEmpty();
}

bool Lowering::hasCodeForObjectsIn(const Type& type) const
{
    if (!type.hasObjects())
        return true;
    auto* engineClass = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).onlyEngineClass();
    return (engineClass && engineClass->type == JSDateType) || hasCodeForJSON(type);
}

Vector<std::pair<LayoutID, FunctionID>, 4> Lowering::serializersFor(const Type& type) const
{
    static constexpr unsigned maxMembers = 4;
    LayoutID view = type.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).onlyView();
    if (!view || m_program.at(view).members.size() > maxMembers)
        return { };
    Vector<std::pair<LayoutID, FunctionID>, 4> result;
    for (LayoutID member : m_program.at(view).members) {
        FunctionID serializer = m_program.jsonSerializerOf(member);
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
    RELEASE_ASSERT(output.pendingText.isEmpty());
    exit.knownCapacity = std::min(exit.knownCapacity, output.knownCapacity);
    return exit.block;
}

void Lowering::reserveCapacity(JSONOutput& output, unsigned size)
{
    if (output.knownCapacity >= size)
        return;
    unsigned wanted = std::max(size, leastCapacityAskedForJSON);
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

void Lowering::appendText(JSONOutput& output, std::span<const uint8_t> text)
{
    output.pendingText.append(text);
}

void Lowering::flushText(JSONOutput& output)
{
    if (output.pendingText.isEmpty())
        return;
    Vector<uint8_t, 64> text = std::exchange(output.pendingText, { });
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

void Lowering::didAppendWithRoomToSpare(JSONOutput& output, Pointer cursor)
{
    orElse(Word32(m_out.notZero64(cursor.value())), output.givesUp);
    setCursor(output, cursor.value());
    output.knownCapacity = spareCapacityOfJSONBuffer;
}

void Lowering::appendInteger(JSONOutput& output, Integer value, unsigned maxLength)
{
    flushText(output);
    reserveCapacity(output, maxLength);
    setCursor(output, AOT_TYPED_OPERATION(JSONAppendInteger, Integer(cursorOf(output)), value).value());
    output.knownCapacity -= maxLength;
}

void Lowering::appendNumber(JSONOutput& output, Number value)
{
    flushText(output);
    reserveCapacity(output, maxLengthOfNumberInJSON);
    setCursor(output, AOT_TYPED_OPERATION(JSONAppendNumber, Integer(cursorOf(output)), value).value());
    output.knownCapacity -= maxLengthOfNumberInJSON;
}

void Lowering::appendBoolean(JSONOutput& output, Word32 value)
{
    flushText(output);
    reserveCapacity(output, 5);
    LValue cursor = cursorOf(output);
    m_out.store32(m_out.select(value.value(), m_out.constInt32('t' | 'r' << 8 | 'u' << 16 | 'e' << 24), m_out.constInt32('f' | 'a' << 8 | 'l' << 16 | 's' << 24)), m_out.address(m_heaps.AOTTyped_argumentBuffer, cursor, 0));
    m_out.store32As8(m_out.constInt32('e'), m_out.address(m_heaps.AOTTyped_argumentBuffer, cursor, 4));
    setCursor(output, m_out.sub(m_out.add(cursor, m_out.constInt64(5)), m_out.zeroExtPtr(value.value())));
    output.knownCapacity -= 5;
}

void Lowering::appendString(JSONOutput& output, CellPointer string)
{
    appendText(output, "\""_s);
    flushText(output);
    didAppendWithRoomToSpare(output, AOT_TYPED_OPERATION(JSONAppendString, Integer(cursorOf(output)), string));
    appendText(output, "\""_s);
}

void Lowering::appendGenerally(JSONOutput& output, Boxed value, JSONPosition position, JSONExit& nothing)
{
    flushText(output);
    LValue before = cursorOf(output);
    Pointer cursor = AOT_TYPED_OPERATION(JSONAppendValue, Integer(before), value, word32(position == JSONPosition::Element));
    if (position == JSONPosition::Property) {
        LBasicBlock hasText = m_out.newBlock();
        m_out.branch(m_out.equal(cursor.value(), before), unsure(exitThrough(output, nothing)), unsure(hasText));
        m_out.appendTo(hasText);
    }
    didAppendWithRoomToSpare(output, cursor);
}

void Lowering::appendNilOrLeaveOut(JSONOutput& output, Word32 isNull, const Type& type, JSONPosition position, JSONExit& nothing, JSONExit& continuation)
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
    appendText(output, "null"_s);
    flushText(output);
    m_out.jump(exitThrough(output, continuation));
}

void Lowering::appendValue(JSONOutput& output, Operand operand, const Type& type, bool typeIsFact, unsigned depth, JSONPosition position, JSONExit& nothing)
{
    switch (operand.rep) {
    case Rep::Int32:
        appendBoolean(output, operand.as<Word32>());
        return;
    case Rep::Int64:
        appendInteger(output, operand.as<Integer>(), type.integers.isWithin(Range::int32()) ? maxLengthOfInt32InJSON : maxLengthOfIntegerInJSON);
        return;
    case Rep::Double:
        appendNumber(output, operand.as<Number>());
        return;
    case Rep::OptionalInt64:
    case Rep::OptionalDouble: {
        flushText(output);
        unsigned capacity = output.knownCapacity;
        JSONExit continuation { m_out.newBlock() };
        LBasicBlock isNumber = m_out.newBlock();
        LBasicBlock isNoNumber = m_out.newBlock();
        m_out.branch(isMarker(operand).value(), unsure(isNoNumber), unsure(isNumber));
        m_out.appendTo(isNumber);
        if (operand.rep == Rep::OptionalInt64)
            appendInteger(output, assumedInteger(operand.as<OptionalInteger>()), maxLengthOfIntegerInJSON);
        else
            appendNumber(output, assumedNumber(operand.as<OptionalNumber>()));
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(isNoNumber);
        output.knownCapacity = capacity;
        appendNilOrLeaveOut(output, equal(bitsOf(operand), integer(bitsOfMarker(Primitive::Null, operand.rep))), type, position, nothing, continuation);
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
        flushText(output);
        reserveCapacity(output, 5);
        unsigned capacity = output.knownCapacity;
        JSONExit continuation { m_out.newBlock() };
        LBasicBlock isBooleanCase = m_out.newBlock();
        LBasicBlock isNoBoolean = m_out.newBlock();
        m_out.branch(isBoolean(boxed).value(), unsure(isBooleanCase), unsure(isNoBoolean));
        m_out.appendTo(isBooleanCase);
        appendBoolean(output, Word32(m_out.bitAnd(low32(boxed).value(), m_out.constInt32(1))));
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(isNoBoolean);
        output.knownCapacity = capacity;
        if (typeIsFact)
            appendNilOrLeaveOut(output, haveEqualBits(boxed, boxedConstant(jsNull())), type, position, nothing, continuation);
        else {
            appendGenerally(output, boxed, position, nothing);
            m_out.jump(exitThrough(output, continuation));
        }
        m_out.appendTo(continuation.block);
        output.knownCapacity = continuation.knownCapacity;
        return;
    }
    if (expected.isNever() || repOf(expected) != Rep::Cell || expected.primitives & (Primitive::Symbol | Primitive::BigInt)) {
        if (!typeIsFact && expected.isNumber()) {
            flushText(output);
            reserveCapacity(output, maxLengthOfInt32InJSON);
            unsigned capacity = output.knownCapacity;
            JSONExit continuation { m_out.newBlock() };
            LBasicBlock isInt32Case = m_out.newBlock();
            LBasicBlock isOther = newUnlikelyBlock();
            m_out.branch(isInt32(boxed).value(), usually(isInt32Case), rarely(isOther));
            m_out.appendTo(isInt32Case);
            appendInteger(output, signExtended(low32(boxed)), maxLengthOfInt32InJSON);
            m_out.jump(exitThrough(output, continuation));
            m_out.appendTo(isOther);
            output.knownCapacity = capacity;
            appendGenerally(output, boxed, position, nothing);
            m_out.jump(exitThrough(output, continuation));
            m_out.appendTo(continuation.block);
            output.knownCapacity = continuation.knownCapacity;
            return;
        }
        appendGenerally(output, boxed, position, nothing);
        return;
    }
    flushText(output);
    unsigned capacity = output.knownCapacity;
    JSONExit continuation { m_out.newBlock() };
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock isNoCell = m_out.newBlock();
    m_out.branch(isCell(boxed).value(), unsure(isCellCase), unsure(isNoCell));
    m_out.appendTo(isCellCase);
    appendCell(output, asCell(boxed), expected, typeIsFact, depth, position, nothing);
    flushText(output);
    m_out.jump(exitThrough(output, continuation));
    m_out.appendTo(isNoCell);
    output.knownCapacity = capacity;
    if (typeIsFact)
        appendNilOrLeaveOut(output, haveEqualBits(boxed, boxedConstant(jsNull())), type, position, nothing, continuation);
    else {
        appendGenerally(output, boxed, position, nothing);
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
        flushText(output);
        unsigned capacity = output.knownCapacity;
        JSONExit continuation { m_out.newBlock() };
        LBasicBlock isString = m_out.newBlock();
        LBasicBlock isOther = newColdBlock();
        m_out.branch(equal(typeOfCell(cell), word32(StringType)).value(), usually(isString), rarely(isOther));
        m_out.appendTo(isString);
        appendString(output, cell);
        flushText(output);
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(isOther);
        output.knownCapacity = capacity;
        appendGenerally(output, box(cell), position, nothing);
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(continuation.block);
        output.knownCapacity = continuation.knownCapacity;
        return;
    }
    if (auto* engineClass = type.onlyEngineClass(); engineClass && engineClass->type == ArrayType) {
        appendArray(output, cell, type, depth, position, nothing);
        return;
    }
    if (type.onlyView()) {
        appendObject(output, cell, type, depth, position, nothing);
        return;
    }
    appendGenerally(output, box(cell), position, nothing);
}

void Lowering::appendField(JSONOutput& output, CellPointer cell, const Field& field, unsigned depth, JSONExit& nothing)
{
    if (field.store == Store::Int32OrElsewhere) {
        flushText(output);
        reserveCapacity(output, maxLengthOfInt32InJSON);
        unsigned capacity = output.knownCapacity;
        JSONExit continuation { m_out.newBlock() };
        LBasicBlock inPlace = m_out.newBlock();
        LBasicBlock elsewhere = newUnlikelyBlock();
        Integer word = signExtended(load(field.store, fieldAt(cell, field), safeIfAllMembersHave(cell, field)).as<Word32>());
        branchOnInt32InPlace(word, usually(inPlace), rarely(elsewhere));
        m_out.appendTo(inPlace);
        appendInteger(output, word, maxLengthOfInt32InJSON);
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(elsewhere);
        output.knownCapacity = capacity;
        appendGenerally(output, readField(cell, field, m_program.at(m_program.types.anything)).as<Boxed>(), JSONPosition::Property, nothing);
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
    flushText(output);
    LValue cursor = callFunction(serializer, Int64, { cell.value(), cursorOf(output) });
    orElse(Word32(m_out.notZero64(cursor)), output.givesUp);
    setCursor(output, cursor);
    output.knownCapacity = 0;
}

void Lowering::appendObjectBeyondPlace(JSONOutput& output, CellPointer cell, const Type& type, JSONPosition position, JSONExit& nothing)
{
    auto serializers = serializersFor(type);
    if (serializers.isEmpty()) {
        appendGenerally(output, box(cell), position, nothing);
        return;
    }
    if (serializers.size() == 1) {
        appendBySerializer(output, cell, serializers[0].second);
        return;
    }
    flushText(output);
    unsigned capacity = output.knownCapacity;
    JSONExit continuation { m_out.newBlock() };
    LBasicBlock otherwise = newColdBlock();
    Word32 shapeWord = shapeWordOf(cell);
    for (auto& [layout, serializer] : serializers) {
        LBasicBlock matches = m_out.newBlock();
        bool isLast = layout == serializers.last().first;
        LBasicBlock next = isLast ? otherwise : m_out.newBlock();
        Word32 isOfLayout = equal(shapeWord, word32(static_cast<int32_t>(Typed::shapeWordOf(m_program.at(layout).shape))));
        if (isLast)
            m_out.branch(isOfLayout.value(), usually(matches), rarely(next));
        else
            m_out.branch(isOfLayout.value(), unsure(matches), unsure(next));
        m_out.appendTo(matches);
        output.knownCapacity = capacity;
        appendBySerializer(output, cell, serializer);
        m_out.jump(exitThrough(output, continuation));
        m_out.appendTo(next);
    }
    output.knownCapacity = capacity;
    appendGenerally(output, box(cell), position, nothing);
    m_out.jump(exitThrough(output, continuation));
    m_out.appendTo(continuation.block);
    output.knownCapacity = continuation.knownCapacity;
}

void Lowering::appendObject(JSONOutput& output, CellPointer cell, const Type& type, unsigned depth, JSONPosition position, JSONExit& nothing)
{
    Vector<JSONKeyOrder, 4> orders;
    if (depth < maxDepthOfJSONInPlace)
        orders = keyOrdersForJSON(type);
    unsigned numberOfFields = 0;
    for (auto& order : orders)
        numberOfFields += order.fields.size() + 1;
    if (orders.isEmpty() || numberOfFields > output.fieldsLeft) {
        appendObjectBeyondPlace(output, cell, type, position, nothing);
        return;
    }
    output.fieldsLeft -= numberOfFields;
    flushText(output);
    unsigned capacity = output.knownCapacity;
    JSONExit continuation { m_out.newBlock() };
    LBasicBlock handsOverRestOfList = std::exchange(output.handsOverRestOfList, nullptr);
    LBasicBlock otherwise = handsOverRestOfList ? handsOverRestOfList : depth ? newColdBlock() : output.givesUp;
    appendObjectInPlace(output, cell, orders, depth, otherwise, continuation);
    if (depth && !handsOverRestOfList) {
        m_out.appendTo(otherwise);
        output.knownCapacity = capacity;
        appendGenerally(output, box(cell), position, nothing);
        m_out.jump(exitThrough(output, continuation));
    }
    m_out.appendTo(continuation.block);
    output.knownCapacity = continuation.knownCapacity;
}

template<typename Describe, typename Append>
void Lowering::appendMembers(JSONOutput& output, unsigned count, const Describe& describe, const Append& append)
{
    appendText(output, "{"_s);
    enum class Separator : uint8_t { None, Always, Unknown };
    Separator separator = Separator::None;
    for (unsigned i = 0; i < count; ++i) {
        auto [name, mayBeLeftOut] = describe(i);
        LValue separatorLength = nullptr;
        unsigned lengthOfKey = 0;
        if (mayBeLeftOut || separator == Separator::Unknown)
            flushText(output);
        if (separator == Separator::Unknown) {
            reserveCapacity(output, 1);
            LValue cursor = cursorOf(output);
            LValue previous = m_out.load8ZeroExt32(TypedPointer(m_heaps.root, m_out.add(cursor, m_out.constIntPtr(-1))));
            m_out.store32As8(m_out.constInt32(','), m_out.address(m_heaps.AOTTyped_argumentBuffer, cursor, 0));
            separatorLength = m_out.zeroExtPtr(m_out.notEqual(previous, m_out.constInt32('{')));
            setCursor(output, m_out.add(cursor, separatorLength));
            output.knownCapacity -= 1;
        } else if (separator == Separator::Always) {
            appendText(output, ","_s);
            ++lengthOfKey;
        }
        appendText(output, "\""_s);
        appendText(output, name);
        appendText(output, "\":"_s);
        lengthOfKey += name.size() + 3;
        if (!mayBeLeftOut) {
            JSONExit never { output.givesUp };
            append(i, never);
            separator = Separator::Always;
            continue;
        }
        flushText(output);
        JSONExit isLeftOut { m_out.newBlock() };
        JSONExit isDone { m_out.newBlock() };
        append(i, isLeftOut);
        flushText(output);
        m_out.jump(exitThrough(output, isDone));
        m_out.appendTo(isLeftOut.block);
        output.knownCapacity = isLeftOut.knownCapacity == UINT_MAX ? 0 : isLeftOut.knownCapacity;
        LValue rewound = m_out.sub(cursorOf(output), m_out.constInt64(lengthOfKey));
        setCursor(output, separatorLength ? m_out.sub(rewound, separatorLength) : rewound);
        m_out.jump(exitThrough(output, isDone));
        m_out.appendTo(isDone.block);
        output.knownCapacity = isDone.knownCapacity;
        if (separator == Separator::None)
            separator = Separator::Unknown;
    }
    appendText(output, "}"_s);
}

std::optional<Vector<Lowering::JSONLiteralMember, 8>> Lowering::membersOfFreshLiteral(ValueID argument, ValueID holder) const
{
    static constexpr unsigned maxMembers = 32;
    ValueID object = m_program.originOf(m_function, argument);
    if (m_function.at(object).opcode != Opcode::NewObject || !m_currentBlock)
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
    auto fields = fieldsMadeWithAllocation(m_program, m_function, *home, allocation, false, false);
    if (fields.size() > maxMembers)
        return std::nullopt;
    Vector<JSONLiteralMember, 8> members;
    for (size_t index : fields) {
        auto& field = m_function.at(home->values[index]);
        StringID name = field.opcode == Opcode::PutById ? field.string() : m_function.at(field.arguments[1]).string();
        if (!isKeyWrittenInPlace(m_program.string(name)))
            return std::nullopt;
        members.append({ name, field.opcode == Opcode::PutById ? field.child(1) : field.arguments[2], home->values[index] });
    }
    auto hasIt = [&](ValueID id) {
        if (id == argument || id == holder)
            return false;
        bool uses = false;
        m_function.at(id).forEachChild([&](ValueID child) {
            uses |= child == object || child == argument;
        });
        return uses;
    };
    auto isHandedOnBy = [&](const Terminator& terminator) {
        bool uses = terminator.value == object || terminator.value == argument;
        for (auto& target : terminator.targets) {
            for (ValueID passed : target.arguments)
                uses |= passed == object || passed == argument;
        }
        return uses;
    };
    size_t end = home == m_currentBlock ? m_currentIndexInBlock : home->values.size();
    for (size_t k = (fields.isEmpty() ? allocation : fields.last()) + 1; k < end; ++k) {
        if (hasIt(home->values[k]))
            return std::nullopt;
    }
    if (home == m_currentBlock)
        return members;
    if (isHandedOnBy(home->terminator))
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
        if (hasIt(m_currentBlock->values[k]))
            return std::nullopt;
    }
    return members;
}

void Lowering::appendFreshLiteral(JSONOutput& output, const Vector<JSONLiteralMember, 8>& literal, unsigned depth)
{
    Vector<std::optional<Vector<JSONLiteralMember, 8>>, 8> literalsWithin;
    for (auto& member : literal) {
        literalsWithin.append(depth + 1 < maxDepthOfJSONInPlace ? membersOfFreshLiteral(member.value, member.definition) : std::nullopt);
        if (literalsWithin.last() && literalsWithin.last()->size() + 1 > output.fieldsLeft)
            literalsWithin.last() = std::nullopt;
        if (literalsWithin.last())
            output.fieldsLeft -= literalsWithin.last()->size() + 1;
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
        appendValue(output, get(member), isPredicted ? expectedTypeOf(member) : Type(typeOf(member)), !isPredicted, depth + 1, JSONPosition::Property, isLeftOut);
    });
}

void Lowering::appendObjectInPlace(JSONOutput& output, CellPointer cell, const Vector<JSONKeyOrder, 4>& orders, unsigned depth, LBasicBlock otherwise, JSONExit& continuation)
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
    const JSONKeyOrder* lastOfEveryCount = nullptr;
    for (auto& order : orders) {
        bodies.append(m_out.newBlock());
        if (order.leastCount < order.count)
            lastOfEveryCount = &order;
    }
    for (auto& order : orders) {
        LValue isComplete = m_out.equal(words, integer(static_cast<int64_t>(shapeAndOrderOf(order) | Typed::orderWordOf(0, order.count))).value());
        if (&order == &orders.last() && !lastOfEveryCount) {
            m_out.branch(isComplete, usually(bodies.last()), rarely(otherwise));
            break;
        }
        LBasicBlock next = m_out.newBlock();
        m_out.branch(isComplete, unsure(bodies[&order - orders.begin()]), unsure(next));
        m_out.appendTo(next);
    }
    for (auto& order : orders) {
        if (order.leastCount == order.count)
            continue;
        LBasicBlock isInOrder = m_out.newBlock();
        LBasicBlock next = &order == lastOfEveryCount ? otherwise : m_out.newBlock();
        m_out.branch(m_out.equal(m_out.bitAnd(words, integer(static_cast<int64_t>(~count)).value()), integer(static_cast<int64_t>(shapeAndOrderOf(order))).value()), unsure(isInOrder), unsure(next));
        m_out.appendTo(isInOrder);
        m_out.branch(m_out.aboveOrEqual(m_out.bitAnd(words, integer(static_cast<int64_t>(count)).value()), integer(static_cast<int64_t>(Typed::orderWordOf(0, order.leastCount))).value()), unsure(bodies[&order - orders.begin()]), unsure(next));
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
        flushText(output);
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
        LBasicBlock isOfNone = m_out.newBlock();
        m_out.switchInstruction(shapeWordOf(cell).value(), cases, isOfNone, FTL::Weight());
        for (auto [block, serializer] : blocks) {
            m_out.appendTo(block);
            join.add(Integer(callFunction(serializer, Int64, { cell.value(), start.value() })));
        }
        m_out.appendTo(isOfNone);
        join.add(integer(isNotWrittenByDispatcher));
        return join.finish();
    }
    JSONOutput output;
    output.givesUp = newColdBlock();
    output.cursor = m_proc.addVariable(Int64);
    setCursor(output, start.value());
    JSONExit continuation { m_out.newBlock() };
    LBasicBlock otherwise = newColdBlock();
    orElse(Word32(m_out.aboveOrEqual(m_out.sub(m_out.framePointer(), m_out.constInt64(stackKeptFreeByJSONSerializers)), changing64(Instance::offsetOfSoftStackLimit()))), output.givesUp);
    LayoutID layout(static_cast<uint32_t>(written));
    output.fieldsLeft = maxFieldsOfJSONInPlaceInFunction;
    auto orders = keyOrdersForJSON(std::span<const LayoutID> { &layout, 1 });
    if (orders.isEmpty())
        m_out.jump(otherwise);
    else
        appendObjectInPlace(output, cell, orders, 0, otherwise, continuation);
    m_out.appendTo(otherwise);
    orElse(Word32(m_out.notEqual(start.value(), m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONBufferStart())))), output.givesUp);
    output.knownCapacity = 0;
    JSONExit never { output.givesUp };
    appendGenerally(output, box(cell), JSONPosition::Element, never);
    m_out.jump(exitThrough(output, continuation));
    m_out.appendTo(continuation.block);
    join.add(Integer(cursorOf(output)));
    m_out.appendTo(output.givesUp);
    join.add(integer(0));
    return join.finish();
}

void Lowering::appendArray(JSONOutput& output, CellPointer array, const Type& type, unsigned depth, JSONPosition position, JSONExit& nothing)
{
    bool isBeyondPlace = depth >= maxDepthOfJSONInPlace && (depth >= maxDepthOfArraysInPlace || !type.elements || serializersFor(m_program.at(type.elements)).isEmpty());
    if (isBeyondPlace || !type.elements || output.fieldsLeft < 2 || !hasCodeForObjectsIn(m_program.at(type.elements))) {
        appendGenerally(output, box(array), position, nothing);
        return;
    }
    output.fieldsLeft -= 2;
    auto& elements = m_program.at(type.elements);
    bool expectsNumbers = elements.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent).isNumber();
    flushText(output);
    reserveCapacity(output, 2);
    unsigned capacity = output.knownCapacity;
    JSONExit continuation { m_out.newBlock() };
    LBasicBlock otherwise = depth ? newColdBlock() : output.givesUp;
    orElse(isPlainArray(array), otherwise);
    Word32 shape = indexingModeOf(array, IndexingShapeMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    auto whereKeptAs = [&](IndexingType kept, bool isLast, const auto& appendElement) {
        LBasicBlock isKeptSo = m_out.newBlock();
        LBasicBlock body = m_out.newBlock();
        LBasicBlock done = m_out.newBlock();
        LBasicBlock isNot = isLast ? otherwise : m_out.newBlock();
        if (isLast)
            m_out.branch(equal(shape, word32(kept)).value(), usually(isKeptSo), rarely(isNot));
        else
            m_out.branch(equal(shape, word32(kept)).value(), unsure(isKeptSo), unsure(isNot));
        m_out.appendTo(isKeptSo);
        output.knownCapacity = capacity;
        appendText(output, "["_s);
        flushText(output);
        ValueFromBlock first = m_out.anchor(m_out.int64Zero);
        m_out.branch(isZero(length).value(), unsure(done), unsure(body));
        m_out.appendTo(body);
        LValue index = m_out.phi(Int64, first);
        output.knownCapacity = 0;
        appendElement(Integer(index));
        appendText(output, ","_s);
        flushText(output);
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
        whereKeptAs(Int32Shape, false, [&](Integer index) {
            Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedInt32Properties, butterfly.value(), index.value())));
            orElse(isNotEmpty(element), output.givesUp);
            appendInteger(output, signExtended(low32(element)), maxLengthOfInt32InJSON);
        });
        whereKeptAs(DoubleShape, true, [&](Integer index) {
            Number element(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
            orElse(equal(element, element), output.givesUp);
            appendNumber(output, element);
        });
    } else {
        whereKeptAs(ContiguousShape, true, [&](Integer index) {
            Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
            orElse(isNotEmpty(element), output.givesUp);
            JSONExit never { output.givesUp };
            LBasicBlock handsOverRest = newColdBlock();
            LBasicBlock ofListAround = std::exchange(output.handsOverRestOfList, handsOverRest);
            appendValue(output, element, elements, false, depth + 1, JSONPosition::Element, never);
            bool isReached = !std::exchange(output.handsOverRestOfList, ofListAround);
            flushText(output);
            LBasicBlock isWritten = m_out.newBlock();
            unsigned capacityBehind = output.knownCapacity;
            m_out.jump(isWritten);
            m_out.appendTo(handsOverRest);
            if (!isReached) {
                m_out.unreachable();
                m_out.appendTo(isWritten);
                return;
            }
            Pointer behindLast = AOT_TYPED_OPERATION(JSONAppendElementsFrom, Integer(cursorOf(output)), array, index);
            didAppendWithRoomToSpare(output, behindLast);
            appendText(output, "]"_s);
            flushText(output);
            ensureStillAliveHere(array);
            m_out.jump(exitThrough(output, continuation));
            m_out.appendTo(isWritten);
            output.knownCapacity = capacityBehind;
        });
    }
    if (depth) {
        m_out.appendTo(otherwise);
        output.knownCapacity = capacity;
        appendGenerally(output, box(array), position, nothing);
        m_out.jump(exitThrough(output, continuation));
    }
    m_out.appendTo(continuation.block);
    output.knownCapacity = continuation.knownCapacity;
}

bool Lowering::appendArgument(JSONOutput& output, ValueID argument)
{
    Type expected = expectedTypeOf(argument);
    auto literal = membersOfFreshLiteral(argument, { });
    bool goesByShape = !literal && !hasCodeForJSON(expected);
    if (Options::auditAOTTypedAssertions() || (goesByShape && !(m_program.jsonDispatcher && m_program.hasCode(m_program.jsonDispatcher) && expected.hasObjects())))
        return false;
    output.givesUp = newColdBlock();
    output.cursor = m_proc.addVariable(Int64);
    output.fieldsLeft = maxFieldsOfJSONInPlace;
    setCursor(output, m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONBufferStart())));
    JSONExit nothing { output.givesUp };
    if (literal)
        appendFreshLiteral(output, *literal, 0);
    else if (goesByShape) {
        uint32_t least = UINT32_MAX;
        uint32_t most = 0;
        for (auto [layout, serializer] : m_program.jsonSerializers) {
            if (uint32_t shape = m_program.at(layout).shape) {
                least = std::min(least, shape);
                most = std::max(most, shape);
            }
        }
        CellPointer cell = objectOrElse(argument, output.givesUp);
        orElse(below(add(shapeWordOf(cell), word32(static_cast<int32_t>(most))), word32(static_cast<int32_t>(most - least + 1))), output.givesUp);
        LValue cursor = callFunction(m_program.jsonDispatcher, Int64, { cell.value(), cursorOf(output) });
        orElse(Word32(m_out.above(cursor, m_out.constInt64(isNotWrittenByDispatcher))), output.givesUp);
        setCursor(output, cursor);
    } else
        appendValue(output, get(argument), expected, false, 0, JSONPosition::Property, nothing);
    flushText(output);
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
        m_out.store32(m_out.constInt32(0), m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONWideSegmentCount()));
        m_out.jump(calls);
        m_out.appendTo(calls);
    }
    auto& type = typeOf(result);
    Boxed yielded = callBuiltinFunction(value, effectsOfBuiltinCall(value));
    return vouchesForItsType(m_program, m_function, value) ? convert(yielded, type, repOf(type)) : ensure(yielded, type);
}

Operand Lowering::lowerJSONStringify(ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    auto generally = [&] {
        Boxed yielded = callBuiltinFunction(value, effectsOfBuiltinCall(value));
        return vouchesForItsType(m_program, m_function, value) ? convert(yielded, type, rep) : ensure(yielded, type);
    };
    JSONOutput output;
    if (!appendArgument(output, value.arguments[0]))
        return generally();
    Join join(m_out, rep, b3TypeOf(rep));
    LBasicBlock isFinished = m_out.newBlock();
    CellPointer string = AOT_TYPED_OPERATION(JSONFinish, Integer(cursorOf(output)));
    m_out.branch(m_out.notNull(string.value()), usually(isFinished), rarely(output.givesUp));
    m_out.appendTo(isFinished);
    join.add(ensure(string, m_program.at(m_program.types.string), type));
    m_out.appendTo(output.givesUp);
    m_out.store32(m_out.constInt32(0), m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONWideSegmentCount()));
    join.add(generally());
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
