/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayAllocationProfile.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeReporter.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSortedStorage.h"
#include "AOTState.h"
#include "ArrayPrototypeInlines.h"
#include "ButterflyInlines.h"
#include "CPUFeatureSet.h"
#include "DeferTermination.h"
#include "JSArrayInlines.h"
#include "JSArrayIterator.h"
#include "JSCInlines.h"
#include "JSStringInlines.h"
#include "JSStringJoiner.h"
#include "StringRecursionChecker.h"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringCommon.h>
#if CPU(X86_64)
#include <immintrin.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

enum class RemovedElements : uint8_t { MakeArray, AreDropped };
template<RemovedElements = RemovedElements::MakeArray>
static JSArray* trySpliceInPlace(JSGlobalObject*, JSArray*, JSValue start, JSValue deleteCount, std::span<const EncodedJSValue> items);

JSC_DEFINE_JIT_OPERATION(operationAOTArraySplice, EncodedJSValue, (Instance* instance, JSCell* cell, int32_t start, int32_t deleteCount, uint32_t count, const EncodedJSValue* items))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySplice);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySplice);
    if (!arraySpeciesWatchpointIsValid(vm, array)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    if (JSArray* removedItems = trySpliceInPlace(globalObject, array, jsNumber(start), jsNumber(deleteCount), std::span { items, count })) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(removedItems));
    int64_t length = array->length();
    int64_t from = start < 0 ? std::max<int64_t>(length + start, 0) : std::min<int64_t>(start, length);
    int64_t removed = std::clamp<int64_t>(deleteCount, 0, length - from);
    JSArray* result = JSArray::fastSlice(globalObject, array, from, removed);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!result)
        OPERATION_RETURN(scope, encodedJSValue());
    bool isDone = array->fastSplice(globalObject, length, from, removed, std::span { items, count });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, isDone ? JSValue::encode(result) : encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySpliceDiscarding, size_t, (Instance* instance, JSCell* cell, int32_t start, int32_t deleteCount, uint32_t count, const EncodedJSValue* items))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySpliceDiscarding);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySpliceDiscarding);
    if (!arraySpeciesWatchpointIsValid(vm, array)) [[unlikely]]
        OPERATION_RETURN(scope, false);
    if (trySpliceInPlace<RemovedElements::AreDropped>(globalObject, array, jsNumber(start), jsNumber(deleteCount), std::span { items, count })) [[likely]]
        OPERATION_RETURN(scope, true);
    int64_t length = array->length();
    int64_t from = start < 0 ? std::max<int64_t>(length + start, 0) : std::min<int64_t>(start, length);
    int64_t removed = std::clamp<int64_t>(deleteCount, 0, length - from);
    bool isDone = array->fastSplice(globalObject, length, from, removed, std::span { items, count });
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, isDone);
}

template<typename CharacterType, typename SourceCharacterType>
static ALWAYS_INLINE CharacterType* appendCharacters(CharacterType* destination, std::span<const SourceCharacterType> characters)
{
    static constexpr size_t maxLengthCopiedInPlace = 8;
    const SourceCharacterType* source = characters.data();
    size_t length = characters.size();
    if (length <= maxLengthCopiedInPlace) {
        if constexpr (sizeof(CharacterType) == 1 && sizeof(SourceCharacterType) == 1) {
            if (length >= sizeof(uint16_t))
                WTF::copySmallMemory<sizeof(uint16_t), maxLengthCopiedInPlace>(byteCast<uint8_t>(destination), byteCast<uint8_t>(source), length);
            else if (length)
                *destination = *source;
        } else {
            for (size_t i = 0; i < length; ++i)
                destination[i] = source[i];
        }
    } else
        StringImpl::copyCharacters(std::span { destination, length }, characters);
    return destination + length;
}

template<typename CharacterType>
static ALWAYS_INLINE CharacterType* appendString(CharacterType* destination, JSString* string)
{
    StringImpl* impl = string->tryGetValueImpl();
    if (!impl) [[unlikely]] {
        unsigned length = string->length();
        string->resolveToBuffer(std::span { destination, length });
        return destination + length;
    }
    if constexpr (std::is_same_v<CharacterType, Latin1Character>)
        return appendCharacters(destination, impl->span8());
    else {
        if (impl->is8Bit())
            return appendCharacters(destination, impl->span8());
        return appendCharacters(destination, impl->span16());
    }
}

template<typename CharacterType>
static RefPtr<StringImpl> joinStrings(const WriteBarrier<Unknown>* elements, unsigned count, StringView between, unsigned joinedLength)
{
    std::span<CharacterType> buffer;
    RefPtr<StringImpl> joined = StringImpl::tryCreateUninitialized(joinedLength, buffer);
    if (!joined) [[unlikely]]
        return nullptr;
    CharacterType* end = appendString(buffer.data(), asString(elements[0].get()));
    switch (between.length()) {
    case 0:
        for (unsigned i = 1; i < count; ++i)
            end = appendString(end, asString(elements[i].get()));
        break;
    case 1: {
        CharacterType unit = static_cast<CharacterType>(between[0]);
        for (unsigned i = 1; i < count; ++i) {
            *end++ = unit;
            end = appendString(end, asString(elements[i].get()));
        }
        break;
    }
    default:
        for (unsigned i = 1; i < count; ++i) {
            between.getCharacters(std::span { end, between.length() });
            end = appendString(end + between.length(), asString(elements[i].get()));
        }
        break;
    }
    RELEASE_ASSERT(end == std::to_address(buffer.end()));
    return joined;
}

template<typename CharacterType, bool hasSeparator>
static ALWAYS_INLINE JSString* joinFewStrings(VM& vm, StringImpl* const* string, StringImpl* const* endOfStrings, Latin1Character separator, unsigned joinedLength)
{
    std::span<CharacterType> buffer;
    RefPtr<StringImpl> joined = StringImpl::tryCreateUninitialized(joinedLength, buffer);
    if (!joined) [[unlikely]]
        return nullptr;
    CharacterType* end = buffer.data();
    for (;;) {
        const StringImpl& appended = **string;
        unsigned length = appended.length();
        auto append = [&](const auto* source) ALWAYS_INLINE_LAMBDA {
            _Pragma("clang loop vectorize(disable) unroll(disable)")
            for (unsigned unit = 0; unit < length; ++unit)
                end[unit] = source[unit];
        };
        if constexpr (std::is_same_v<CharacterType, Latin1Character>) {
            if (length > sizeof(uint64_t))
                memcpy(end, appended.span8().data(), length);
            else
                append(appended.span8().data());
        } else if (appended.is8Bit())
            append(appended.span8().data());
        else if (length == 2)
            WTF::unalignedStore<uint32_t>(end, WTF::unalignedLoad<uint32_t>(appended.span16().data()));
        else if (length > sizeof(uint32_t))
            memcpy(end, appended.span16().data(), length * sizeof(char16_t));
        else
            append(appended.span16().data());
        end += length;
        if (++string == endOfStrings)
            break;
        if constexpr (hasSeparator)
            *end++ = separator;
    }
    return jsNontrivialString(vm, String { joined.releaseNonNull() });
}

static ALWAYS_INLINE bool isJoinOfFewStrings(const JSString& text, StringImpl* const* string, StringImpl* const* endOfStrings, Latin1Character separator, unsigned joinedLength)
{
    const StringImpl* impl = text.tryGetValueImpl();
    if (!impl || impl->length() != joinedLength || !impl->is8Bit())
        return false;
    const Latin1Character* unit = impl->span8().data();
    for (;;) {
        std::span<const Latin1Character> joined = (*string)->span8();
        if (!WTF::equal(unit, joined))
            return false;
        unit += joined.size();
        if (++string == endOfStrings)
            return true;
        if (*unit++ != separator)
            return false;
    }
}

template<bool hasSeparator>
static NEVER_INLINE JSString* tryJoinFewStrings(JSGlobalObject* globalObject, JSArray* array, Latin1Character separator)
{
    static constexpr unsigned maxKeptStringUnits = 64;
    VM& vm = globalObject->vm();
    static constexpr unsigned mostStrings = 16;
    static constexpr unsigned mostUnits = 512;
    Butterfly& butterfly = *array->butterfly();
    unsigned count = butterfly.publicLength();
    if (count < 2 || count > mostStrings)
        return nullptr;
    std::array<StringImpl*, mostStrings> strings;
    StringImpl** endOfStrings = strings.data();
    unsigned units = hasSeparator ? count - 1 : 0;
    unsigned sharedFlags = StringImpl::flagIs8Bit();
    auto* element = butterfly.contiguous().data();
    auto* endOfElements = element + count;
    do {
        JSValue value = element->get();
        StringImpl* impl = nullptr;
        if (!value && holesMustForwardToPrototype(array))
            return nullptr;
        if (!value || value.isUndefinedOrNull())
            impl = StringImpl::empty();
        else if (value.isString())
            impl = asString(value)->tryGetValueImpl();
        else if (value.isInt32() && static_cast<uint32_t>(value.asInt32()) < NumericStrings::cacheSize)
            impl = vm.numericStrings.add(value.asInt32()).impl();
        if (!impl || impl->length() > mostUnits)
            return nullptr;
        *endOfStrings++ = impl;
        units += impl->length();
        sharedFlags &= impl->is8Bit() ? StringImpl::flagIs8Bit() : 0;
    } while (++element != endOfElements);
    if (units < 2 || units > mostUnits)
        return nullptr;
    if constexpr (hasSeparator) {
        if (sharedFlags && units <= maxKeptStringUnits) {
            auto& texts = State::of(globalObject).textsOfFewStrings;
            uintptr_t mixed = std::bit_cast<uintptr_t>(strings[0]) >> 4 ^ std::bit_cast<uintptr_t>(endOfStrings[-1]) >> 5;
            JSString*& kept = texts[(mixed ^ mixed >> 6) % texts.size()];
            if (kept && isJoinOfFewStrings(*kept, strings.data(), endOfStrings, separator, units))
                return kept;
            JSString* joined = joinFewStrings<Latin1Character, hasSeparator>(vm, strings.data(), endOfStrings, separator, units);
            if (joined)
                kept = joined;
            return joined;
        }
    }
    return sharedFlags ? joinFewStrings<Latin1Character, hasSeparator>(vm, strings.data(), endOfStrings, separator, units) : joinFewStrings<char16_t, hasSeparator>(vm, strings.data(), endOfStrings, separator, units);
}

static ALWAYS_INLINE JSString* tryJoinFewStrings(JSGlobalObject* globalObject, JSArray* array, JSString* separatorOrNull)
{
    if (!hasContiguous(array->indexingType()) && !hasInt32(array->indexingType())) [[unlikely]]
        return nullptr;
    if (!separatorOrNull)
        return tryJoinFewStrings<true>(globalObject, array, ',');
    StringImpl* separator = separatorOrNull->tryGetValueImpl();
    if (!separator || separator->length() > 1)
        return nullptr;
    if (!separator->length())
        return tryJoinFewStrings<false>(globalObject, array, 0);
    return separator->is8Bit() ? tryJoinFewStrings<true>(globalObject, array, separator->span8()[0]) : nullptr;
}

static bool holdsOnlyHoles(JSArray* array)
{
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    if (hasDouble(array->indexingType()))
        return std::ranges::all_of(std::span { butterfly.contiguousDouble().data(), length }, [](double element) { return isHole(element); });
    return std::ranges::all_of(std::span { butterfly.contiguous().data(), length }, [](const WriteBarrier<Unknown>& element) { return !element; });
}

static NEVER_INLINE JSString* tryJoinFewPrimitives(JSGlobalObject* globalObject, JSArray* array, std::span<const Latin1Character> separator)
{
    static constexpr unsigned mostElements = 32;
    static constexpr unsigned mostUnits = 256;
    static constexpr unsigned mostUnitsOfNumber = 32;
    static constexpr unsigned maxKeptStringUnits = 64;
    VM& vm = globalObject->vm();
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    if (length > mostElements || separator.size() > 1)
        return nullptr;
    std::array<Latin1Character, mostUnits + mostUnitsOfNumber> units;
    Latin1Character* end = units.data();
    Latin1Character* const limit = end + mostUnits;
    auto append = [&](std::span<const Latin1Character> text) ALWAYS_INLINE_LAMBDA -> bool {
        if (end > limit || text.size() > static_cast<size_t>(limit - end))
            return false;
        end = appendCharacters(end, text);
        return true;
    };
    auto appendRope = [&](JSString* rope) -> bool {
        unsigned ropeLength = rope->length();
        if (!rope->is8Bit() || end > limit || ropeLength > static_cast<size_t>(limit - end))
            return false;
        rope->resolveToBuffer(std::span { end, ropeLength });
        end += ropeLength;
        return true;
    };
    auto appendInteger = [&](int32_t integer) ALWAYS_INLINE_LAMBDA -> bool {
        if (end >= limit)
            return false;
        if (static_cast<uint32_t>(integer) < 10) {
            *end++ = '0' + integer;
            return true;
        }
        unsigned digits = lengthOfIntegerAsString(integer);
        writeIntegerToBuffer(integer, std::span { end, digits });
        end += digits;
        return true;
    };
    auto appendDouble = [&](double number) ALWAYS_INLINE_LAMBDA -> bool {
        if (number != number)
            return append("NaN"_span8);
        if (int32_t integer = truncateDoubleToInt32(number); integer == number)
            return appendInteger(integer);
        return append(vm.numericStrings.add(number).span8());
    };
    bool holdsDoubles = hasDouble(array->indexingType());
    bool hasHoles = false;
    for (unsigned index = 0; index < length; ++index) {
        if (index && !append(separator))
            return nullptr;
        bool isAppended = true;
        if (holdsDoubles) {
            double number = butterfly.contiguousDouble().data()[index];
            if (!isHole(number)) {
                if (!appendDouble(number))
                    return nullptr;
                continue;
            }
        } else if (JSValue element = butterfly.contiguous().data()[index].get()) {
            if (element.isInt32())
                isAppended = appendInteger(element.asInt32());
            else if (element.isDouble())
                isAppended = appendDouble(element.asDouble());
            else if (element.isString()) {
                if (length == 1)
                    return asString(element);
                StringImpl* impl = asString(element)->tryGetValueImpl();
                isAppended = impl ? impl->is8Bit() && append(impl->span8()) : appendRope(asString(element));
            } else if (element.isBoolean())
                isAppended = append(element.asBoolean() ? "true"_span8 : "false"_span8);
            else
                isAppended = element.isUndefinedOrNull();
            if (!isAppended)
                return nullptr;
            continue;
        }
        if (!std::exchange(hasHoles, true) && holesMustForwardToPrototype(array))
            return nullptr;
    }
    std::span<const Latin1Character> text { units.data(), end };
    if (text.size() < 2)
        return text.empty() ? jsEmptyString(vm) : jsSingleCharacterString(vm, text.front());
    JSString** kept = nullptr;
    if (text.size() <= maxKeptStringUnits) {
        auto& texts = State::of(globalObject).textsOfFewStrings;
        unsigned mixed = text.front() ^ text[text.size() / 2] << 2 ^ text.back() << 4 ^ text.size();
        kept = &texts[(mixed ^ mixed >> 6) % texts.size()];
        if (*kept) {
            StringImpl* impl = (*kept)->tryGetValueImpl();
            if (impl && impl->is8Bit() && WTF::equal(impl->span8(), text))
                return *kept;
        }
    }
    JSString* joined = jsNontrivialString(vm, String { text });
    if (kept)
        *kept = joined;
    return joined;
}

static ALWAYS_INLINE bool holdsObjectAmongElements(JSArray* array)
{
    Butterfly& butterfly = *array->butterfly();
    for (unsigned index = butterfly.publicLength(); index--;) {
        JSValue element = butterfly.contiguous().data()[index].get();
        if (element && element.isObject())
            return true;
    }
    return false;
}

static ALWAYS_INLINE JSString* tryJoinFewPrimitives(JSGlobalObject* globalObject, JSArray* array, JSString* separatorOrNull)
{
    static constexpr Latin1Character comma = ',';
    static constexpr unsigned mostElementsLookedThrough = 32;
    if (!hasInt32(array->indexingType()) && !hasDouble(array->indexingType()) && !hasContiguous(array->indexingType())) [[unlikely]]
        return nullptr;
    if (hasContiguous(array->indexingType()) && array->butterfly()->publicLength() <= mostElementsLookedThrough && holdsObjectAmongElements(array))
        return nullptr;
    if (!separatorOrNull)
        return tryJoinFewPrimitives(globalObject, array, span(comma));
    StringImpl* separator = separatorOrNull->tryGetValueImpl();
    return separator && separator->is8Bit() ? tryJoinFewPrimitives(globalObject, array, separator->span8()) : nullptr;
}

static NEVER_INLINE JSString* tryJoinPrimitives(JSGlobalObject* globalObject, JSArray* array, JSString* separatorOrNull)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    EnsureStillAliveScope arrayScope(array);
    JSString* between = separatorOrNull ? separatorOrNull : vm.smallStrings.singleCharacterString(',');
    auto view = between->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    bool holdsDoubles = hasDouble(array->indexingType());
    auto elementAt = [&](unsigned index) -> JSValue {
        if (!holdsDoubles)
            return butterfly.contiguous().at(array, index).get();
        double element = butterfly.contiguousDouble().at(array, index);
        return element == element ? jsNumber(element) : JSValue();
    };
    JSStringJoiner joiner(view);
    joiner.reserveCapacity(globalObject, length);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool hasHoles = false;
    for (unsigned index = 0; index < length; ++index) {
        JSValue element = elementAt(index);
        if (!element) {
            if (!std::exchange(hasHoles, true) && holesMustForwardToPrototype(array))
                return nullptr;
            joiner.appendEmptyString();
            continue;
        }
        if (element.isCell() && !element.isString())
            return nullptr;
        joiner.append(globalObject, element);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    JSValue joined = joiner.join(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return asString(joined);
}

static NEVER_INLINE JSString* tryJoinElements(JSGlobalObject* globalObject, JSArray* array, JSString* separatorOrNull)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    EnsureStillAliveScope arrayScope(array);
    JSString* between = separatorOrNull ? separatorOrNull : vm.smallStrings.singleCharacterString(',');
    auto view = between->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSExactLengthJoiner joiner(view);
    if (!hasInt32(array->indexingType()) && !hasDouble(array->indexingType()) && !hasContiguous(array->indexingType()))
        return nullptr;
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    if (length && holdsOnlyHoles(array)) {
        if (holesMustForwardToPrototype(array))
            return nullptr;
        RELEASE_AND_RETURN(scope, repeatSeparator(globalObject, view, length - 1));
    }
    JSString* joinedAtExactLength = nullptr;
    switch (array->indexingType()) {
    case ALL_INT32_INDEXING_TYPES:
        joinedAtExactLength = joiner.tryJoin<Int32Shape>(globalObject, butterfly.contiguous().data(), length);
        break;
    case ALL_DOUBLE_INDEXING_TYPES:
        joinedAtExactLength = joiner.tryJoin(globalObject, butterfly.contiguousDouble().data(), length);
        break;
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        static constexpr uint64_t minLengthJoinedAsRope = 16384;
        static constexpr uint64_t minUnitsPerElementJoinedAsRope = 256;
        auto* elements = butterfly.contiguous().data();
        uint64_t units = 0;
        bool areAllStrings = length >= 2;
        bool areAll8Bit = view->is8Bit();
        for (unsigned i = 0; i < length && areAllStrings; ++i) {
            JSValue element = elements[i].get();
            areAllStrings = element && element.isString();
            if (!areAllStrings)
                break;
            JSString* string = asString(element);
            if (StringImpl* impl = string->tryGetValueImpl()) [[likely]] {
                units += impl->length();
                areAll8Bit &= impl->is8Bit();
            } else {
                units += string->length();
                areAll8Bit &= string->is8Bit();
            }
        }
        if (areAllStrings && units >= minLengthJoinedAsRope && units / length >= minUnitsPerElementJoinedAsRope) {
            JSRopeString::RopeBuilder<RecordOverflow> builder(vm);
            for (unsigned i = 0; i < length; ++i) {
                if (i)
                    builder.append(between);
                builder.append(asString(elements[i].get()));
            }
            if (!builder.hasOverflowed())
                RELEASE_AND_RETURN(scope, builder.release());
        }
        if (uint64_t joinedLength = units + static_cast<uint64_t>(view->length()) * (length - 1); areAllStrings && joinedLength && joinedLength <= JSString::MaxLength) {
            RefPtr<StringImpl> joined = areAll8Bit ? joinStrings<Latin1Character>(elements, length, view, joinedLength) : joinStrings<char16_t>(elements, length, view, joinedLength);
            if (joined) [[likely]]
                RELEASE_AND_RETURN(scope, jsString(vm, joined.releaseNonNull()));
        }
        joinedAtExactLength = joiner.tryJoin<ContiguousShape>(globalObject, elements, length);
        break;
    }
    default:
        return nullptr;
    }
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (joinedAtExactLength)
        return joinedAtExactLength;
    RELEASE_AND_RETURN(scope, tryJoinPrimitives(globalObject, array, separatorOrNull));
}

static ALWAYS_INLINE bool isEmptyDenseArray(JSArray* array)
{
    IndexingType type = array->indexingType();
    return (hasUndecided(type) || hasInt32(type) || hasDouble(type) || hasContiguous(type)) && !array->butterfly()->publicLength();
}

static ALWAYS_INLINE JSString* tryJoin(JSGlobalObject*, JSArray*, JSString* separatorOrNull);

static NEVER_INLINE JSString* tryJoinSharedElements(JSGlobalObject* globalObject, JSArray* array)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSCellButterfly* elements = JSCellButterfly::fromButterfly(array->butterfly());
    if (auto known = vm.immutableButterflyToStringCache.find(elements); known != vm.immutableButterflyToStringCache.end())
        return known->value;
    JSString* joined = tryJoin(globalObject, array, vm.smallStrings.singleCharacterString(','));
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool runsNoCode = joined && (hasContiguous(array->indexingType()) || hasInt32(array->indexingType())) && std::ranges::all_of(std::span { array->butterfly()->contiguous().data(), array->butterfly()->publicLength() }, [](const WriteBarrier<Unknown>& element) {
        return element && element.get().isPrimitive();
    });
    if (runsNoCode)
        vm.immutableButterflyToStringCache.add(elements, joined);
    return joined;
}

static ALWAYS_INLINE JSString* tryJoinAtOnce(JSGlobalObject* globalObject, JSArray* array, JSString* separatorOrNull)
{
    VM& vm = globalObject->vm();
    if (isEmptyDenseArray(array))
        return jsEmptyString(vm);
    if (vm.stringRecursionCheckFirstObject) [[unlikely]]
        return nullptr;
    if (!separatorOrNull && isCopyOnWrite(array->indexingMode()))
        return tryJoinSharedElements(globalObject, array);
    if (JSString* joined = tryJoinFewStrings(globalObject, array, separatorOrNull))
        return joined;
    if (JSString* joined = tryJoinFewPrimitives(globalObject, array, separatorOrNull))
        return joined;
    return tryJoinElements(globalObject, array, separatorOrNull);
}

static ALWAYS_INLINE std::span<const WriteBarrier<Unknown>> boxedElementsOf(JSArray* array)
{
    return { array->butterfly()->contiguous().data(), array->butterfly()->publicLength() };
}

static NEVER_INLINE bool elementConversionsRunNoCode(JSArray* array)
{
    return std::ranges::all_of(boxedElementsOf(array), [&](const WriteBarrier<Unknown>& element) {
        JSValue value = element.get();
        return value && (!value.isCell() || value.isString() || (isJSArray(value) && value != array && asArray(value)->isToPrimitiveFastAndNonObservable()));
    });
}

static NEVER_INLINE JSString* tryJoinList(JSGlobalObject* globalObject, JSArray* list)
{
    return tryJoinAtOnce(globalObject, list, nullptr);
}

static constexpr unsigned maxStringsHeldAtOnce = 16;

template<bool hasSeparator>
static NEVER_INLINE JSString* tryJoinFewTexts(JSGlobalObject* globalObject, JSArray* array, Latin1Character separator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::array<String, maxStringsHeldAtOnce> texts;
    std::array<StringImpl*, maxStringsHeldAtOnce> strings;
    unsigned length = array->butterfly()->publicLength();
    uint64_t units = hasSeparator ? length - 1 : 0;
    bool areAll8Bit = true;
    for (unsigned index = 0; index < length; ++index) {
        JSValue element = boxedElementsOf(array)[index].get();
        String& text = texts[index];
        if (element.isCell()) {
            JSString* string = element.isString() ? asString(element) : tryJoinList(globalObject, asArray(element));
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (!string)
                return nullptr;
            text = string->value(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
        } else if (element.isInt32())
            text = vm.numericStrings.add(element.asInt32());
        else if (element.isDouble()) {
            auto integer = tryConvertToStrictInt32(element.asDouble());
            text = integer ? vm.numericStrings.add(*integer) : vm.numericStrings.add(element.asDouble());
        } else if (element.isBoolean())
            text = element.isTrue() ? vm.propertyNames->trueKeyword.string() : vm.propertyNames->falseKeyword.string();
        else
            text = emptyString();
        strings[index] = text.impl();
        units += text.length();
        areAll8Bit &= text.is8Bit();
    }
    if (units < 2 || units > JSString::MaxLength)
        return nullptr;
    StringImpl** end = strings.data() + length;
    unsigned joinedLength = static_cast<unsigned>(units);
    scope.release();
    if (areAll8Bit) [[likely]]
        return joinFewStrings<Latin1Character, hasSeparator>(vm, strings.data(), end, separator, joinedLength);
    return joinFewStrings<char16_t, hasSeparator>(vm, strings.data(), end, separator, joinedLength);
}

static NEVER_INLINE JSString* tryJoinManyTexts(JSGlobalObject* globalObject, JSArray* array, StringView separator)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSStringJoiner joiner(separator);
    unsigned length = array->butterfly()->publicLength();
    joiner.reserveCapacity(globalObject, length);
    RETURN_IF_EXCEPTION(scope, nullptr);
    MarkedArgumentBuffer textsOfLists;
    for (unsigned index = 0; index < length; ++index) {
        JSValue element = boxedElementsOf(array)[index].get();
        if (element.isCell() && !element.isString()) {
            JSString* text = tryJoinList(globalObject, asArray(element));
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (!text)
                return nullptr;
            textsOfLists.append(text);
            if (textsOfLists.hasOverflowed()) [[unlikely]]
                return nullptr;
            element = text;
        }
        joiner.append(globalObject, element);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    RELEASE_AND_RETURN(scope, joiner.join(globalObject));
}

static NEVER_INLINE JSString* tryJoinOneByOne(JSGlobalObject* globalObject, JSArray* array, JSString* separatorOrNull)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasContiguous(array->indexingType()) || vm.stringRecursionCheckFirstObject || !elementConversionsRunNoCode(array))
        return nullptr;
    EnsureStillAliveScope arrayScope(array);
    JSString* between = separatorOrNull ? separatorOrNull : vm.smallStrings.singleCharacterString(',');
    auto view = between->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    unsigned length = array->butterfly()->publicLength();
    if (length >= 2 && length <= maxStringsHeldAtOnce && view->length() <= 1 && view->is8Bit()) {
        JSString* joined = view->length() ? tryJoinFewTexts<true>(globalObject, array, view->span8()[0]) : tryJoinFewTexts<false>(globalObject, array, 0);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (joined) [[likely]]
            return joined;
    }
    RELEASE_AND_RETURN(scope, tryJoinManyTexts(globalObject, array, view));
}

static ALWAYS_INLINE JSString* tryJoin(JSGlobalObject* globalObject, JSArray* array, JSString* separatorOrNull)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSString* joined = tryJoinAtOnce(globalObject, array, separatorOrNull);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (joined) [[likely]]
        return joined;
    RELEASE_AND_RETURN(scope, tryJoinOneByOne(globalObject, array, separatorOrNull));
}

static bool holdsCompiledObjects(JSArray* array)
{
    if (!hasContiguous(array->indexingType()))
        return false;
    bool holdsSome = false;
    for (const WriteBarrier<Unknown>& element : boxedElementsOf(array)) {
        JSValue value = element.get();
        if (!value)
            return false;
        if (!value.isCell() || value.isString())
            continue;
        if (value.asCell()->type() != CompiledObjectType)
            return false;
        holdsSome = true;
    }
    return holdsSome;
}

static NEVER_INLINE JSString* joinConvertedElements(Instance* instance, JSGlobalObject* globalObject, JSArray* array)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    StringRecursionChecker checker(globalObject, array);
    EXCEPTION_ASSERT(!scope.exception() || checker.earlyReturnValue());
    if (JSValue earlyReturnValue = checker.earlyReturnValue()) [[unlikely]] {
        RETURN_IF_EXCEPTION(scope, nullptr);
        return asString(earlyReturnValue);
    }
    const unsigned length = array->butterfly()->publicLength();
    MarkedArgumentBuffer texts;
    for (unsigned index = 0; index < length; ++index) {
        JSValue element = hasContiguous(array->indexingType()) && index < array->butterfly()->publicLength() ? boxedElementsOf(array)[index].get() : JSValue();
        if (!element) [[unlikely]] {
            element = array->get(globalObject, index);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        JSString* text;
        if (element.isString())
            text = asString(element);
        else if (element.isCell() && element.asCell()->type() == CompiledObjectType)
            text = toStringOfCompiledObject(instance, *uncheckedDowncast<Object>(element.asCell()));
        else
            text = element.isUndefinedOrNull() ? jsEmptyString(vm) : element.toString(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        texts.append(text);
    }
    if (texts.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    if (length == 1)
        return asString(texts.at(0));

    constexpr Latin1Character separator = ',';
    if (length <= maxStringsHeldAtOnce) {
        std::array<StringImpl*, maxStringsHeldAtOnce> strings;
        uint64_t units = length - 1;
        bool areAll8Bit = true;
        for (unsigned index = 0; index < length; ++index) {
            auto text = asString(texts.at(index))->value(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
            strings[index] = text->impl();
            units += text->length();
            areAll8Bit &= text->is8Bit();
        }
        if (units >= 2 && units <= JSString::MaxLength) {
            StringImpl** end = strings.data() + length;
            unsigned joinedLength = static_cast<unsigned>(units);
            JSString* joined = areAll8Bit ? joinFewStrings<Latin1Character, true>(vm, strings.data(), end, separator, joinedLength) : joinFewStrings<char16_t, true>(vm, strings.data(), end, separator, joinedLength);
            if (joined) [[likely]]
                return joined;
        }
    }
    JSStringJoiner joiner(StringView { std::span { &separator, 1 } });
    joiner.reserveCapacity(globalObject, length);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (unsigned index = 0; index < length; ++index) {
        joiner.append(globalObject, texts.at(index));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    RELEASE_AND_RETURN(scope, joiner.join(globalObject));
}

JSString* tryConvertListToString(Instance* instance, JSGlobalObject* globalObject, JSArray* array)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!array->isToPrimitiveFastAndNonObservable())
        return nullptr;
    JSString* joined = tryJoin(globalObject, array, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (joined || !holdsCompiledObjects(array))
        return joined;
    RELEASE_AND_RETURN(scope, joinConvertedElements(instance, globalObject, array));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayJoin, JSCell*, (Instance* instance, JSCell* cell, JSCell* separator))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayJoin);
    OPERATION_RETURN(scope, tryJoin(globalObject, cellArgument<JSArray>(cell, Entry::operationAOTArrayJoin), separator ? cellArgument<JSString>(separator, Entry::operationAOTArrayJoin) : nullptr));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayJoinOfAnyReceiver, EncodedJSValue, (Instance* instance, EncodedJSValue encodedReceiver, EncodedJSValue encodedSeparator, JSCell* function))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayJoinOfAnyReceiver);
    JSValue receiver = JSValue::decode(encodedReceiver);
    JSValue separator = JSValue::decode(encodedSeparator);
    if (isJSArray(receiver) && (separator.isUndefined() || separator.isString())) {
        JSString* joined = tryJoin(globalObject, asArray(receiver), separator.isString() ? asString(separator) : nullptr);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (joined)
            OPERATION_RETURN(scope, JSValue::encode(joined));
    }
    MarkedArgumentBuffer arguments;
    arguments.append(separator);
    ASSERT(!arguments.hasOverflowed());
    JSValue joined = call(globalObject, function, JSC::getCallData(function), receiver, arguments);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!joined.isString()) [[unlikely]]
        stopAtHostResult(*instance, AOT_OPERATION_FRAME_RECORD(), joined, function, kindsOfHostResult(HostResult::String));
    OPERATION_RETURN(scope, JSValue::encode(joined));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayToStringOfAnyReceiver, EncodedJSValue, (Instance* instance, EncodedJSValue encodedReceiver, JSCell* function))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayToStringOfAnyReceiver);
    JSValue receiver = JSValue::decode(encodedReceiver);
    if (isJSArray(receiver)) {
        JSString* joined = asArray(receiver)->isToPrimitiveFastAndNonObservable() ? tryJoin(globalObject, asArray(receiver), nullptr) : nullptr;
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (joined)
            OPERATION_RETURN(scope, JSValue::encode(joined));
    }
    OPERATION_RETURN(scope, JSValue::encode(call(globalObject, function, JSC::getCallData(function), receiver, *vm.emptyList)));
}

static ALWAYS_INLINE bool precedesInDefaultOrder(const StringImpl& first, const StringImpl& second)
{
    if (first.is8Bit() && second.is8Bit()) [[likely]]
        return is_lt(codePointCompare(first.span8(), second.span8()));
    return is_lt(codePointCompare(StringView { first }, StringView { second }));
}

SortedStorage& SortedStorage::ensure(State& state)
{
    if (SortedStorage* storage = state.instance.realm.sortedStorage) [[likely]]
        return *storage;
    SortedStorage* storage = state.tableArena.allocate<SortedStorage>(1);
    storage->cageBase = state.instance.vm->heap.cage().base();
    state.caches->addClearedEachCollection("sorted storage"_s, asMutableByteSpan(std::span { storage->entries }));
    state.instance.realm.sortedStorage = storage;
    return *storage;
}

static bool trySortStrings(JSGlobalObject* globalObject, State& state, JSArray* array)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasContiguous(array->indexingType()) || array->structure()->hasImmutableProperties())
        return false;
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    if (length > maxLengthOfInsertionSortedArray)
        return false;
    JSCellButterfly* shared = isCopyOnWrite(array->indexingMode()) && !JSCellButterfly::isOutsideHeap(&butterfly) ? JSCellButterfly::fromButterfly(&butterfly) : nullptr;
    if (const SortedStorage* known = shared ? state.instance.realm.sortedStorage : nullptr) {
        if (JSCellButterfly* sorted = known->tryFind(shared)) {
            countEvent(vm, RuntimeCounterID::SortedStorageCacheHits);
            if (sorted != shared)
                array->setButterfly(vm, sorted->toButterfly());
            return true;
        }
    }

    const WriteBarrier<Unknown>* elements = butterfly.contiguous().data();
    std::array<EncodedJSValue, maxLengthOfInsertionSortedArray> sorted;
    std::array<const StringImpl*, maxLengthOfInsertionSortedArray> impls;
    bool isSorted = true;
    for (unsigned i = 0; i < length; ++i) {
        JSValue element = elements[i].get();
        if (!element || !element.isString())
            return false;
        const StringImpl* impl = asString(element)->tryGetValueImpl();
        if (!impl) [[unlikely]] {
            asString(element)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            impl = asString(element)->tryGetValueImpl();
        }
        unsigned position = i;
        if (i && precedesInDefaultOrder(*impl, *impls[i - 1])) {
            unsigned end = i - 1;
            position = 0;
            while (position < end) {
                unsigned middle = position + (end - position) / 2;
                if (precedesInDefaultOrder(*impl, *impls[middle]))
                    end = middle;
                else
                    position = middle + 1;
            }
            std::copy_backward(sorted.begin() + position, sorted.begin() + i, sorted.begin() + i + 1);
            std::copy_backward(impls.begin() + position, impls.begin() + i, impls.begin() + i + 1);
            isSorted = false;
        }
        sorted[position] = JSValue::encode(element);
        impls[position] = impl;
    }
    if (isSorted) {
        if (shared)
            SortedStorage::ensure(state).remember(shared, shared);
        return true;
    }
    if (JSCellButterfly* made = shared ? JSCellButterfly::tryCreate(vm, WTF::opaque(shared->structure()), length) : nullptr) {
        for (unsigned i = 0; i < length; ++i)
            made->setIndex(vm, i, JSValue::decode(sorted[i]));
        array->setButterfly(vm, made->toButterfly());
        SortedStorage::ensure(state).remember(shared, made);
        return true;
    }
    RELEASE_AND_RETURN(scope, array->appendMemcpy(globalObject, vm, 0, ArrayWithContiguous, std::span<const EncodedJSValue> { sorted }.first(length)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySortInDefaultOrder, EncodedJSValue, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySortInDefaultOrder);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySortInDefaultOrder);
    bool isSorted = trySortStrings(globalObject, *instance->state, array);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (isSorted)
        OPERATION_RETURN(scope, JSValue::encode(array));
    OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeSort, array)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTArrayIndexOfDouble, size_t, (Instance*, JSCell* array, double searched))
{
    Butterfly& butterfly = *cellArgument<JSArray>(array, Entry::operationAOTArrayIndexOfDouble)->butterfly();
    const double* data = butterfly.contiguousDouble().data();
    const double* found = WTF::findDouble(data, searched, butterfly.publicLength());
    return found ? found - data : static_cast<size_t>(-1);
}

#if CPU(X86_64)
__attribute__((target("avx2"))) static const uint64_t* findWithAVX2(const uint64_t* data, uint64_t searched, size_t length)
{
    const __m256i target = _mm256_set1_epi64x(static_cast<long long>(searched));
    size_t index = 0;
    for (; index + 16 <= length; index += 16) {
        auto* cursor = reinterpret_cast<const __m256i*>(data + index);
        __m256i first = _mm256_cmpeq_epi64(_mm256_loadu_si256(cursor), target);
        __m256i second = _mm256_cmpeq_epi64(_mm256_loadu_si256(cursor + 1), target);
        __m256i third = _mm256_cmpeq_epi64(_mm256_loadu_si256(cursor + 2), target);
        __m256i fourth = _mm256_cmpeq_epi64(_mm256_loadu_si256(cursor + 3), target);
        __m256i any = _mm256_or_si256(_mm256_or_si256(first, second), _mm256_or_si256(third, fourth));
        if (_mm256_testz_si256(any, any)) [[likely]]
            continue;
        uint32_t found = _mm256_movemask_pd(_mm256_castsi256_pd(first)) | _mm256_movemask_pd(_mm256_castsi256_pd(second)) << 4
            | _mm256_movemask_pd(_mm256_castsi256_pd(third)) << 8 | _mm256_movemask_pd(_mm256_castsi256_pd(fourth)) << 12;
        return data + index + std::countr_zero(found);
    }
    for (; index + 4 <= length; index += 4) {
        __m256i equal = _mm256_cmpeq_epi64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + index)), target);
        if (uint32_t found = _mm256_movemask_pd(_mm256_castsi256_pd(equal)))
            return data + index + std::countr_zero(found);
    }
    for (; index < length; ++index) {
        if (data[index] == searched)
            return data + index;
    }
    return nullptr;
}
#endif

static const uint64_t* findBox(const uint64_t* data, uint64_t searched, size_t length)
{
#if CPU(X86_64)
    if (CPUFeatureSet::host().contains(CPUFeature::AVX2))
        return findWithAVX2(data, searched, length);
#endif
    return WTF::find64(data, searched, length);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTArrayIndexOfInt32, size_t, (Instance*, JSCell* array, EncodedJSValue searched))
{
    Butterfly& butterfly = *cellArgument<JSArray>(array, Entry::operationAOTArrayIndexOfInt32)->butterfly();
    auto* data = std::bit_cast<const uint64_t*>(butterfly.contiguous().data());
    const uint64_t* found = findBox(data, searched, butterfly.publicLength());
    return found ? found - data : static_cast<size_t>(-1);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayIndexOfString, size_t, (Instance* instance, JSCell* cell, JSCell* searched, uint64_t start))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayIndexOfString);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayIndexOfString);
    EnsureStillAliveScope arrayScope(array);
    JSString* searchString = cellArgument<JSString>(searched, Entry::operationAOTArrayIndexOfString);
    constexpr size_t notFound = static_cast<size_t>(-1);
    const WriteBarrier<Unknown>* elements = hasContiguous(array->indexingType()) ? array->butterfly()->contiguous().data() : nullptr;
    if (!elements && !hasAnyArrayStorage(array->indexingType()))
        OPERATION_RETURN(scope, notFound);
    if (elements && isCopyOnWrite(array->indexingMode()) && JSCellButterfly::isOnlyAtomStringsStructure(vm, array->butterfly())) {
        auto searched = searchString->toExistingAtomString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, notFound);
        AtomStringImpl* atom = searched.data;
        bool isKnownAtomicWord = atom && ((atom->length() == 1 && atom->at(0) <= maxSingleCharacterString) || vm.atomStringToJSStringMap.contains(atom));
        for (uint64_t index = start, length = isKnownAtomicWord ? array->length() : 0; index < length; ++index) {
            if (asString(elements[index].get())->getValueImpl() == atom)
                OPERATION_RETURN(scope, index);
        }
        OPERATION_RETURN(scope, notFound);
    }
    unsigned searchLength = searchString->length();
    for (uint64_t index = start, length = array->length(); index < length; ++index) {
        JSValue element = elements ? elements[index].get() : array->getDirectIndex(globalObject, static_cast<unsigned>(index));
        if (!element || !element.isString())
            continue;
        JSString* string = asString(element);
        if (string == searchString)
            OPERATION_RETURN(scope, index);
        if (string->length() != searchLength)
            continue;
        bool isEqual = string->equalInline(globalObject, searchString);
        OPERATION_RETURN_IF_EXCEPTION(scope, notFound);
        if (isEqual)
            OPERATION_RETURN(scope, index);
    }
    OPERATION_RETURN(scope, notFound);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySearchOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedReceiver, EncodedJSValue encodedSearched, EncodedJSValue encodedFromIndex, uint32_t answersIndex))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySearchOfAnyValue);
    JSValue receiver = JSValue::decode(encodedReceiver);
    JSValue searched = JSValue::decode(encodedSearched);
    JSValue fromIndex = JSValue::decode(encodedFromIndex);
    bool startsInArray = fromIndex.isUndefined() || (fromIndex.isInt32() && fromIndex.asInt32() >= 0);
    if (searched.isString() && startsInArray && isJSArray(receiver) && hasContiguous(asArray(receiver)->indexingType())) {
        JSArray* array = asArray(receiver);
        JSString* text = asString(searched);
        unsigned textLength = text->length();
        unsigned length = array->butterfly()->publicLength();
        const WriteBarrier<Unknown>* elements = array->butterfly()->contiguous().data();
        bool hasMetHole = false;
        std::optional<unsigned> found;
        for (unsigned index = fromIndex.isUndefined() ? 0 : fromIndex.asInt32(); index < length; ++index) {
            JSValue element = elements[index].get();
            if (!element) [[unlikely]] {
                hasMetHole = true;
                continue;
            }
            if (element == searched) {
                found = index;
                break;
            }
            if (!element.isString() || asString(element)->length() != textLength)
                continue;
            bool isEqual = asString(element)->equal(globalObject, text);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (isEqual) {
                found = index;
                break;
            }
        }
        if (!hasMetHole || !array->structure()->holesMustForwardToPrototype(array)) [[likely]] {
            if (answersIndex)
                OPERATION_RETURN(scope, JSValue::encode(found ? jsNumber(*found) : jsNumber(-1)));
            OPERATION_RETURN(scope, JSValue::encode(jsBoolean(!!found)));
        }
    }
    OPERATION_RETURN(scope, encodedJSValue());
}

static bool tryUnshiftItemFast(VM& vm, JSArray* array, JSValue item)
{
    IndexingType mode = array->indexingMode();
    bool itemFits = mode == ArrayWithContiguous || (mode == ArrayWithInt32 && item.isInt32()) || (mode == ArrayWithDouble && item.isNumber() && item.asNumber() == item.asNumber());
    if (!itemFits)
        return false;
    Butterfly* butterfly = array->butterfly();
    unsigned length = butterfly->publicLength();
    if (length > JSArray::shiftThreshold || length >= butterfly->vectorLength())
        return false;
    butterfly->setPublicLength(length + 1);
    WriteBarrier<Unknown>* elements = butterfly->contiguous().data();
    if (mode == ArrayWithContiguous && vm.heap.mutatorShouldBeFenced())
        gcSafeMemmove(elements + 1, elements, length * sizeof(JSValue));
    else
        memmove(static_cast<void*>(elements + 1), static_cast<const void*>(elements), length * sizeof(JSValue));
    if (mode == ArrayWithDouble)
        butterfly->contiguousDouble().atUnsafe(0) = item.asNumber();
    else
        elements[0].setWithoutWriteBarrier(item);
    if (mode == ArrayWithContiguous)
        vm.writeBarrier(array);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayUnshift, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue item))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayUnshift);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayUnshift);
    if (mayRejectOrInterceptWrites(array) || array->structure()->holesMustForwardToPrototype(array)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    if (tryUnshiftItemFast(vm, array, JSValue::decode(item))) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(array->length())));
    uint64_t length = array->length();
    unshift(globalObject, array, 0, 0, 1, length);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    array->putByIndexInline(globalObject, static_cast<unsigned>(0), JSValue::decode(item), true);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    setLength(globalObject, vm, array, length + 1);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(length + 1)));
}

static bool tryMoveStartForward(VM& vm, JSArray* array)
{
    Structure* structure = array->structure();
    if ((array->type() != ArrayType && array->type() != DerivedArrayType) || structure->hijacksIndexingHeader() || structure->hasImmutableProperties() || structure->outOfLineCapacity())
        return false;
    DeferredStructureTransitionWatchpointFire deferred(vm, structure);
    Structure* newStructure = structure->hasElementsStartOffset() ? nullptr : Structure::nonPropertyTransition(vm, structure, TransitionKind::AllocateElementsStartOffset, &deferred);
    if (vm.heap.mutatorShouldBeFenced()) [[unlikely]]
        return false;
    Butterfly* butterfly = array->butterfly();
    unsigned publicLength = butterfly->publicLength() - 1;
    unsigned vectorLength = butterfly->vectorLength() - 1;
    uint64_t slotsInFront = newStructure ? 0 : butterfly->indexingHeader()->elementsStartOffset(0);
    Butterfly* moved = Butterfly::fromBase(static_cast<EncodedJSValue*>(butterfly->base(0, 0)) + 1, 0, 0);
    moved->setPublicLength(publicLength);
    moved->setVectorLength(vectorLength);
    moved->indexingHeader()->elementsStartOffset(0) = slotsInFront + 1;
    array->setButterfly(vm, moved);
    if (newStructure)
        array->setStructure(vm, newStructure);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTShiftArrayStart, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTShiftArrayStart);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTShiftArrayStart);
    StructureID before = array->structureID();
    if (tryMoveStartForward(vm, array)) {
        if (StructureID behind = array->structureID(); behind != before) {
            instance->arrays.structuresAroundFirstShift[0] = before.bits();
            instance->arrays.structuresAroundFirstShift[1] = behind.bits();
        }
        OPERATION_RETURN(scope);
    }
    if (array->fastShift(vm)) [[likely]]
        OPERATION_RETURN(scope);
    Butterfly& butterfly = *array->butterfly();
    unsigned last = butterfly.publicLength() - 1;
    if (hasDouble(array->indexingType())) {
        memmove(butterfly.contiguousDouble().data(), butterfly.contiguousDouble().data() + 1, sizeof(double) * last);
        butterfly.contiguousDouble().at(array, last) = PNaN;
    } else {
        gcSafeMemmove(butterfly.contiguous().data(), butterfly.contiguous().data() + 1, sizeof(JSValue) * last);
        butterfly.contiguous().at(array, last).clear();
    }
    butterfly.setPublicLength(last);
    vm.writeBarrier(array);
    OPERATION_RETURN(scope);
}

static constexpr unsigned radixDigitBits = 11;
static constexpr unsigned radixDigitCount = 1 << radixDigitBits;

template<typename Key> static constexpr size_t minRadixSortedLength = sizeof(Key) > sizeof(uint32_t) ? 2048 : 512;

template<typename Key>
static std::span<Key> radixSortKeys(std::span<Key> keys, std::span<Key> scratch)
{
    constexpr unsigned numberOfPasses = (sizeof(Key) * CHAR_BIT + radixDigitBits - 1) / radixDigitBits;
    using CountsOfPass = std::array<uint32_t, radixDigitCount>;
    auto digitOf = [](Key key, unsigned pass) -> unsigned {
        return static_cast<unsigned>(key >> (pass * radixDigitBits)) & (radixDigitCount - 1);
    };
    void* memory = nullptr;
    if (!tryFastZeroedMalloc(numberOfPasses * sizeof(CountsOfPass)).getValue(memory))
        return { };
    auto counts = std::span { static_cast<CountsOfPass*>(memory), numberOfPasses };
    for (Key key : keys) {
        for (unsigned pass = 0; pass < numberOfPasses; ++pass)
            ++counts[pass][digitOf(key, pass)];
    }
    for (unsigned pass = 0; pass < numberOfPasses; ++pass) {
        CountsOfPass& places = counts[pass];
        if (places[digitOf(keys[0], pass)] == keys.size())
            continue;
        uint32_t place = 0;
        for (uint32_t& count : places)
            place += std::exchange(count, place);
        for (Key key : keys)
            scratch[places[digitOf(key, pass)]++] = key;
        std::swap(keys, scratch);
    }
    fastFree(memory);
    return keys;
}

template<typename Key, typename Element>
static bool sortElementsByKey(std::span<Element> elements, NOESCAPE const auto& tryMakeKey, NOESCAPE const auto& elementOf)
{
    bool usesDigits = elements.size() >= minRadixSortedLength<Key> && elements.size() <= std::numeric_limits<uint32_t>::max();
    void* memory = nullptr;
    if (!tryFastMalloc((usesDigits ? 2 : 1) * elements.size() * sizeof(Key)).getValue(memory))
        return false;
    auto keys = std::span { static_cast<Key*>(memory), elements.size() };
    bool hasKeys = true;
    for (size_t i = 0; i < elements.size() && hasKeys; ++i)
        hasKeys = tryMakeKey(elements[i], keys[i]);
    std::span<Key> sorted;
    if (hasKeys && usesDigits)
        sorted = radixSortKeys(keys, std::span { static_cast<Key*>(memory) + elements.size(), elements.size() });
    else if (hasKeys) {
        std::sort(keys.begin(), keys.end());
        sorted = keys;
    }
    for (size_t i = 0; i < sorted.size(); ++i)
        elements[i] = elementOf(sorted[i]);
    fastFree(memory);
    return !sorted.empty();
}

static bool tryMakeSortKeyOfDouble(double number, uint64_t mask, uint64_t& key)
{
    constexpr uint64_t signBit = 1ULL << 63;
    uint64_t bits = std::bit_cast<uint64_t>(number);
    if (number != number || bits == signBit) [[unlikely]]
        return false;
    key = (bits ^ (static_cast<uint64_t>(static_cast<int64_t>(bits) >> 63) | signBit)) ^ mask;
    return true;
}

static double doubleOfSortKey(uint64_t key, uint64_t mask)
{
    constexpr uint64_t signBit = 1ULL << 63;
    key ^= mask;
    return std::bit_cast<double>(key ^ (key & signBit ? signBit : ~0ULL));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSortNumbers, size_t, (Instance*, JSCell* cell, int32_t direction))
{
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTSortNumbers);
    Butterfly& butterfly = *array->butterfly();
    size_t length = butterfly.publicLength();
    uint64_t mask = direction < 0 ? ~0ULL : 0;
    switch (array->indexingMode()) {
    case ArrayWithDouble:
        return sortElementsByKey<uint64_t>(std::span { butterfly.contiguousDouble().data(), length }, [&](double number, uint64_t& key) {
            return tryMakeSortKeyOfDouble(number, mask, key);
        }, [&](uint64_t key) {
            return doubleOfSortKey(key, mask);
        });
    case ArrayWithInt32:
    case ArrayWithContiguous: {
        auto elements = std::span { std::bit_cast<EncodedJSValue*>(butterfly.contiguous().data()), length };
        bool holdsOnlyInt32s = std::ranges::all_of(elements, [](EncodedJSValue element) {
            return JSValue::decode(element).isInt32();
        });
        if (holdsOnlyInt32s) {
            auto flipped = [&](uint32_t bits) {
                return bits ^ static_cast<uint32_t>(mask) ^ (1U << 31);
            };
            return sortElementsByKey<uint32_t>(elements, [&](EncodedJSValue element, uint32_t& key) {
                key = flipped(static_cast<uint32_t>(JSValue::decode(element).asInt32()));
                return true;
            }, [&](uint32_t key) {
                return JSValue::encode(jsNumber(static_cast<int32_t>(flipped(key))));
            });
        }
        if (array->indexingMode() == ArrayWithInt32)
            return false;
        return sortElementsByKey<uint64_t>(elements, [&](EncodedJSValue element, uint64_t& key) {
            JSValue number = JSValue::decode(element);
            return number.isNumber() && tryMakeSortKeyOfDouble(number.asNumber(), mask, key);
        }, [&](uint64_t key) {
            return JSValue::encode(jsNumber(doubleOfSortKey(key, mask)));
        });
    }
    default:
        return false;
    }
}

static constexpr size_t maxLengthSortedByInsertion = 32;
static constexpr size_t maxLengthSortedByCounting = 8;

static ALWAYS_INLINE void sortElementsByInsertion(EncodedJSValue* elements, int64_t* keys, size_t length)
{
    for (size_t i = 1; i < length; ++i) {
        EncodedJSValue element = elements[i];
        int64_t key = keys[i];
        size_t place = i;
        for (; place && keys[place - 1] > key; --place) {
            keys[place] = keys[place - 1];
            elements[place] = elements[place - 1];
        }
        keys[place] = key;
        elements[place] = element;
    }
}

template<size_t length>
static void sortElementsByCounting(EncodedJSValue* elements, const int64_t* keys)
{
    std::array<size_t, length> places;
    std::array<EncodedJSValue, length> moved;
    for (size_t i = 0; i < length; ++i) {
        places[i] = i;
        for (size_t before = 0; before < i; ++before) {
            size_t isAhead = keys[i] < keys[before];
            places[i] -= isAhead;
            places[before] += isAhead;
        }
    }
    for (size_t i = 0; i < length; ++i)
        moved[places[i]] = elements[i];
    gcSafeMemcpy(elements, moved.data(), sizeof(moved));
}

template<size_t... lengths>
static ALWAYS_INLINE void sortFewElementsByCounting(EncodedJSValue* elements, const int64_t* keys, size_t length, std::index_sequence<lengths...>)
{
    using Sort = void (*)(EncodedJSValue*, const int64_t*);
    static constexpr std::array<Sort, sizeof...(lengths)> sorts { sortElementsByCounting<lengths + 2>... };
    sorts[length - 2](elements, keys);
}

struct DoubleSortKey {
    uint64_t key;
    uint64_t index;

    friend bool operator<(const DoubleSortKey& a, const DoubleSortKey& b) { return a.key != b.key ? a.key < b.key : a.index < b.index; }
};

static ALWAYS_INLINE size_t indexOfSortKey(uint64_t key) { return static_cast<uint32_t>(key); }
static ALWAYS_INLINE size_t indexOfSortKey(const DoubleSortKey& key) { return key.index; }

template<typename Key>
static ALWAYS_INLINE void moveElementsToSortedPlaces(std::span<EncodedJSValue> elements, std::span<const Key> sorted, EncodedJSValue* moved)
{
    for (size_t i = 0; i < elements.size(); ++i)
        moved[i] = elements[indexOfSortKey(sorted[i])];
    gcSafeMemcpy(elements.data(), moved, elements.size_bytes());
}

template<typename Key>
static bool sortElementsByKeys(std::span<EncodedJSValue> elements, NOESCAPE const auto& tryMakeKey)
{
    size_t length = elements.size();
    void* memory = nullptr;
    if (length > std::numeric_limits<uint32_t>::max() || !tryFastMalloc(length * (sizeof(Key) + sizeof(EncodedJSValue))).getValue(memory))
        return false;
    auto keys = std::span { static_cast<Key*>(memory), length };
    bool hasKeys = true;
    for (size_t i = 0; i < length && hasKeys; ++i)
        hasKeys = tryMakeKey(i, keys[i]);
    if (hasKeys) {
        std::sort(keys.begin(), keys.end());
        moveElementsToSortedPlaces(elements, std::span<const Key>(keys), reinterpret_cast<EncodedJSValue*>(static_cast<Key*>(memory) + length));
    }
    fastFree(memory);
    return hasKeys;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSortByKeys, size_t, (Instance* instance, JSCell* cell, JSCell* keyCell, int32_t direction))
{
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTSortByKeys);
    JSArray* keyArray = cellArgument<JSArray>(keyCell, Entry::operationAOTSortByKeys);
    IndexingType mode = array->indexingMode();
    if ((mode != ArrayWithContiguous && mode != ArrayWithInt32) || array->structure()->hasImmutableProperties() || !hasContiguous(keyArray->indexingType())) [[unlikely]]
        return false;
    size_t length = array->butterfly()->publicLength();
    if (keyArray->butterfly()->publicLength() < length) [[unlikely]]
        return false;
    auto elements = std::span { std::bit_cast<EncodedJSValue*>(array->butterfly()->contiguous().data()), length };
    auto boxedKeys = std::span { std::bit_cast<EncodedJSValue*>(keyArray->butterfly()->contiguous().data()), length };
    EncodedJSValue sharedBits = JSValue::NumberTag;
    bool hasHole = false;
    _Pragma("clang loop vectorize(disable)")
    for (size_t i = 0; i < length; ++i) {
        sharedBits &= boxedKeys[i];
        hasHole |= !elements[i];
    }
    if (hasHole) [[unlikely]]
        return false;
    bool areInt32s = sharedBits == JSValue::NumberTag;
    uint64_t mask = direction < 0 ? ~0ULL : 0;
    if (length <= maxLengthSortedByInsertion) [[likely]] {
        std::array<int64_t, maxLengthSortedByInsertion> keys;
        if (areInt32s) [[likely]] {
            _Pragma("clang loop vectorize(disable)")
            for (size_t i = 0; i < length; ++i)
                keys[i] = static_cast<int32_t>(boxedKeys[i]) ^ static_cast<int64_t>(mask);
        } else {
            for (size_t i = 0; i < length; ++i) {
                JSValue number = JSValue::decode(boxedKeys[i]);
                uint64_t key;
                if (!number.isNumber() || !tryMakeSortKeyOfDouble(number.asNumber() + 0.0, mask, key))
                    return false;
                keys[i] = static_cast<int64_t>(key ^ (uint64_t { 1 } << 63));
            }
        }
        if (length < 2)
            return true;
        if (length <= maxLengthSortedByCounting)
            sortFewElementsByCounting(elements.data(), keys.data(), length, std::make_index_sequence<maxLengthSortedByCounting - 1> { });
        else
            sortElementsByInsertion(elements.data(), keys.data(), length);
        if (mode == ArrayWithContiguous)
            instance->vm->writeBarrier(array);
        return true;
    }
    bool isSorted = areInt32s ? sortElementsByKeys<uint64_t>(elements, [&](size_t index, uint64_t& key) {
        uint32_t flipped = static_cast<uint32_t>(JSValue::decode(boxedKeys[index]).asInt32()) ^ static_cast<uint32_t>(mask) ^ (1U << 31);
        key = static_cast<uint64_t>(flipped) << 32 | index;
        return true;
    }) : sortElementsByKeys<DoubleSortKey>(elements, [&](size_t index, DoubleSortKey& key) {
        JSValue number = JSValue::decode(boxedKeys[index]);
        key.index = index;
        return number.isNumber() && tryMakeSortKeyOfDouble(number.asNumber() + 0.0, mask, key.key);
    });
    if (isSorted && mode == ArrayWithContiguous)
        instance->vm->writeBarrier(array);
    return isSorted;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewArrayIterator, JSCell*, (Instance* instance, JSCell* array, uint32_t kind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayIterator);
    DeferTerminationForAWhile deferTermination(vm);
    RELEASE_ASSERT(kind <= static_cast<uint32_t>(IterationKind::Entries));
    prepareInlineAllocation<JSArrayIterator>(instance, InlineAllocation::ArrayIterator, globalObject->arrayIteratorStructure());
    return JSArrayIterator::create(vm, globalObject->arrayIteratorStructure(), cellArgument<JSObject>(array, Entry::operationAOTNewArrayIterator), static_cast<IterationKind>(kind));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayAtGeneric, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue position))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayAtGeneric);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayAtGeneric);
    double length = array->length();
    double place = JSValue::decode(position).toIntegerOrInfinity(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (place < 0)
        place += length;
    if (!(place >= 0 && place < length))
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    OPERATION_RETURN(scope, JSValue::encode(array->getIndex(globalObject, static_cast<unsigned>(place))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPopGeneric, EncodedJSValue, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayPopGeneric);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayPopGeneric);
    unsigned length = array->length();
    bool canPopDirectly = length ? array->canGetIndexQuickly(length - 1) : hasIndexedProperties(array->indexingType()) && !hasAnyArrayStorage(array->indexingType());
    if (!canPopDirectly || mayRejectOrInterceptWrites(array)) [[unlikely]]
        OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypePop, array)));
    OPERATION_RETURN(scope, JSValue::encode(array->pop(globalObject)));
}

static constexpr uint64_t mostElementsConcatenated = 4 * maxLengthForElementwiseCopy;

static JSArray* tryConcatFast(VM& vm, JSGlobalObject* globalObject, JSArray* first, JSValue argument)
{
    auto isPlain = [&](JSArray* array) {
        return array->isPristine() && array->structure()->realm() == globalObject;
    };
    JSArray* second = argument.isCell() && argument.asCell()->type() == ArrayType ? asArray(argument) : nullptr;
    if ((!second && argument.isObject()) || !isPlain(first) || (second && !isPlain(second)) || globalObject->isHavingABadTime() || globalObject->arrayIsConcatSpreadableWatchpointSet().state() != IsWatched)
        return nullptr;

    struct Part {
        IndexingType shape;
        unsigned length;
        const uint64_t* elements;
    };
    auto partOf = [](JSArray* array) -> std::optional<Part> {
        IndexingType shape = array->indexingType() & IndexingShapeMask;
        if (shape != UndecidedShape && shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape)
            return std::nullopt;
        unsigned length = array->butterfly()->publicLength();
        if (shape == UndecidedShape && length)
            return std::nullopt;
        return Part { shape, length, array->butterfly()->indexingPayload<uint64_t>() };
    };
    uint64_t element = JSValue::encode(argument);
    std::optional<Part> head = partOf(first);
    std::optional<Part> tail;
    if (second)
        tail = partOf(second);
    else if (argument.isInt32())
        tail = Part { Int32Shape, 1, &element };
    else if (argument.isDouble() && argument.asDouble() == argument.asDouble()) {
        element = std::bit_cast<uint64_t>(argument.asDouble());
        tail = Part { DoubleShape, 1, &element };
    } else
        tail = Part { ContiguousShape, 1, &element };
    if (!head || !tail)
        return nullptr;
    uint64_t count = static_cast<uint64_t>(head->length) + tail->length;
    if (count > mostElementsConcatenated)
        return nullptr;
    static_assert(UndecidedShape < Int32Shape && Int32Shape < DoubleShape && DoubleShape < ContiguousShape);
    IndexingType shape = std::max<IndexingType>(Int32Shape, !head->length ? tail->shape : !tail->length ? head->shape : std::max(head->shape, tail->shape));

    Structure* structure = globalObject->originalArrayStructureForIndexingType(static_cast<IndexingType>(IsArray | shape));
    unsigned vectorLength = std::max(static_cast<unsigned>(count) | 1, Butterfly::optimalContiguousVectorLength(structure, static_cast<unsigned>(count)));
    JSArray* result = tryCreateArrayWithStorage(vm, structure, static_cast<unsigned>(count), vectorLength, 0, NewElements::WrittenByCaller);
    if (!result)
        return nullptr;
    uint64_t* target = result->butterfly()->indexingPayload<uint64_t>();
    for (const Part* part : { &*head, &*tail }) {
        if (part->shape == DoubleShape && shape == ContiguousShape) {
            std::transform(part->elements, part->elements + part->length, target, [](uint64_t element) {
                double number = std::bit_cast<double>(element);
                return number == number ? static_cast<uint64_t>(JSValue::encode(JSValue(JSValue::EncodeAsDouble, number))) : JSValue::encode(JSValue());
            });
        } else if (part->shape == Int32Shape && shape == DoubleShape) {
            std::transform(part->elements, part->elements + part->length, target, [](uint64_t element) {
                return std::bit_cast<uint64_t>(element ? static_cast<double>(static_cast<int32_t>(element)) : PNaN);
            });
        } else
            std::copy_n(part->elements, part->length, target);
        target += part->length;
    }
    vm.mutatorFence();
    ensureStillAliveHere(first);
    ensureStillAliveHere(argument);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayConcatGeneric, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue argument))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayConcatGeneric);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayConcatGeneric);
    if (JSArray* result = tryConcatFast(vm, globalObject, array, JSValue::decode(argument))) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(result));
    OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeConcat, array, JSValue::decode(argument))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayShiftGeneric, EncodedJSValue, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayShiftGeneric);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayShiftGeneric);
    if (JSValue first = array->fastShift(vm))
        OPERATION_RETURN(scope, JSValue::encode(first));
    OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeShift, array)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayUnshiftGeneric, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue item))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayUnshiftGeneric);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayUnshiftGeneric);
    if (mayRejectOrInterceptWrites(array) || array->structure()->holesMustForwardToPrototype(array)) [[unlikely]]
        OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeUnshift, array, JSValue::decode(item))));
    if (tryUnshiftItemFast(vm, array, JSValue::decode(item))) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(array->length())));
    uint64_t length = array->length();
    unshift(globalObject, array, 0, 0, 1, length);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    array->putByIndexInline(globalObject, static_cast<unsigned>(0), JSValue::decode(item), true);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    setLength(globalObject, vm, array, length + 1);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(length + 1)));
}

static ALWAYS_INLINE bool speciesOfListIsAsBuilt(VM& vm, JSArray* array)
{
    if (array->isPristine()) [[likely]]
        return array->realm()->arraySpeciesWatchpointSet().state() == IsWatched;
    return arraySpeciesWatchpointIsValid(vm, array);
}

static JSArray* trySpliceFast(JSGlobalObject* globalObject, VM& vm, JSArray* array, JSValue start, JSValue deleteCount, std::span<const EncodedJSValue> items)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!start.isInt32() || !deleteCount.isInt32() || !speciesOfListIsAsBuilt(vm, array)) [[unlikely]]
        return nullptr;
    int64_t length = array->length();
    int64_t from = start.asInt32() < 0 ? std::max<int64_t>(length + start.asInt32(), 0) : std::min<int64_t>(start.asInt32(), length);
    int64_t removed = std::clamp<int64_t>(deleteCount.asInt32(), 0, length - from);
    JSArray* result = JSArray::fastSlice(globalObject, array, from, removed);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!result)
        return nullptr;
    bool isDone = array->fastSplice(globalObject, length, from, removed, items);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return isDone ? result : nullptr;
}

template<RemovedElements removedElements>
static JSArray* trySpliceInPlace(JSGlobalObject* globalObject, JSArray* array, JSValue start, JSValue deleteCount, std::span<const EncodedJSValue> items)
{
    VM& vm = globalObject->vm();
    IndexingType mode = array->indexingMode();
    if ((mode != ArrayWithContiguous && mode != ArrayWithInt32 && mode != ArrayWithDouble) || !start.isInt32() || !deleteCount.isInt32())
        return nullptr;
    for (EncodedJSValue item : items) {
        JSValue value = JSValue::decode(item);
        if ((mode == ArrayWithInt32 && !value.isInt32()) || (mode == ArrayWithDouble && !(value.isNumber() && value.asNumber() == value.asNumber())))
            return nullptr;
    }
    Butterfly* butterfly = array->butterfly();
    unsigned length = butterfly->publicLength();
    unsigned from = start.asInt32() < 0 ? static_cast<unsigned>(std::max<int64_t>(static_cast<int64_t>(length) + start.asInt32(), 0)) : std::min<unsigned>(start.asInt32(), length);
    unsigned numberOfRemoved = std::min<unsigned>(std::max(deleteCount.asInt32(), 0), length - from);
    uint64_t newLength = static_cast<uint64_t>(length) - numberOfRemoved + items.size();
    if (newLength > MAX_STORAGE_VECTOR_LENGTH || length - from > MIN_SPARSE_ARRAY_INDEX) [[unlikely]]
        return nullptr;
    if (!(from + numberOfRemoved) && length > JSArray::shiftThreshold) [[unlikely]]
        return nullptr;
    if (!speciesOfListIsAsBuilt(vm, array) || globalObject->isHavingABadTime() || (!array->isPristine() && array->structure()->holesMustForwardToPrototype(array))) [[unlikely]]
        return nullptr;
    if (newLength > length) {
        if (!array->ensureLength(vm, static_cast<unsigned>(newLength))) [[unlikely]]
            return nullptr;
        butterfly = array->butterfly();
    }
    JSArray* removed = array;
    if (removedElements == RemovedElements::MakeArray) {
        Structure* structure = globalObject->originalArrayStructureForIndexingType(numberOfRemoved ? mode : ArrayWithContiguous);
        unsigned vectorLengthOfRemoved = numberOfRemoved < 2 ? 1 : std::max(numberOfRemoved | 1, Butterfly::optimalContiguousVectorLength(structure, numberOfRemoved));
        removed = tryCreateArrayWithStorage(vm, structure, numberOfRemoved, vectorLengthOfRemoved, 0, NewElements::WrittenByCaller);
        if (!removed) [[unlikely]] {
            butterfly->setPublicLength(length);
            return nullptr;
        }
    }
    WriteBarrier<Unknown>* elements = butterfly->contiguous().data();
    if (numberOfRemoved && removedElements == RemovedElements::MakeArray) {
        std::copy_n(butterfly->indexingPayload<uint64_t>() + from, numberOfRemoved, removed->butterfly()->indexingPayload<uint64_t>());
        vm.mutatorFence();
    }
    unsigned numberOfMoved = length - from - numberOfRemoved;
    unsigned numberOfItems = static_cast<unsigned>(items.size());
    if (numberOfMoved && numberOfItems != numberOfRemoved) {
        WriteBarrier<Unknown>* target = elements + from + numberOfItems;
        WriteBarrier<Unknown>* source = elements + from + numberOfRemoved;
        if (mode == ArrayWithContiguous)
            gcSafeMemmove(target, source, numberOfMoved * sizeof(JSValue));
        else
            memmove(static_cast<void*>(target), static_cast<const void*>(source), numberOfMoved * sizeof(JSValue));
    }
    for (unsigned index = 0; index < numberOfItems; ++index) {
        if (mode == ArrayWithDouble)
            butterfly->contiguousDouble().atUnsafe(from + index) = JSValue::decode(items[index]).asNumber();
        else
            elements[from + index].setWithoutWriteBarrier(JSValue::decode(items[index]));
    }
    for (unsigned index = static_cast<unsigned>(newLength); index < length; ++index) {
        if (mode == ArrayWithDouble)
            butterfly->contiguousDouble().atUnsafe(index) = PNaN;
        else
            elements[index].clear();
    }
    if (newLength < length)
        butterfly->setPublicLength(static_cast<unsigned>(newLength));
    vm.writeBarrier(array);
    return removed;
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySpliceGeneric, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue start, EncodedJSValue deleteCount))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySpliceGeneric);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySpliceGeneric);
    if (JSArray* removed = trySpliceInPlace(globalObject, array, JSValue::decode(start), JSValue::decode(deleteCount), { })) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(removed));
    JSArray* result = trySpliceFast(globalObject, vm, array, JSValue::decode(start), JSValue::decode(deleteCount), { });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (result) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(result));
    OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeSplice, array, JSValue::decode(start), JSValue::decode(deleteCount))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySpliceItemGeneric, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue start, EncodedJSValue deleteCount, EncodedJSValue item))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySpliceItemGeneric);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySpliceItemGeneric);
    if (JSArray* removed = trySpliceInPlace(globalObject, array, JSValue::decode(start), JSValue::decode(deleteCount), singleElementSpan(item))) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(removed));
    JSArray* result = trySpliceFast(globalObject, vm, array, JSValue::decode(start), JSValue::decode(deleteCount), singleElementSpan(item));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (result) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(result));
    MarkedArgumentBuffer arguments;
    arguments.append(JSValue::decode(start));
    arguments.append(JSValue::decode(deleteCount));
    arguments.append(JSValue::decode(item));
    OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeSplice, array, arguments)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
