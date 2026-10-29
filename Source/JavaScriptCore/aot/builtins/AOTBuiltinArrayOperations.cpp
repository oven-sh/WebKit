/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayAllocationProfile.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTThreshold.h"
#include "ArrayPrototypeInlines.h"
#include "ButterflyInlines.h"
#include "DeferTermination.h"
#include "JSArrayInlines.h"
#include "JSArrayIterator.h"
#include "JSCInlines.h"
#include "JSStringInlines.h"
#include "JSStringJoiner.h"
#include <algorithm>
#include <bit>
#include <span>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTArraySplice, EncodedJSValue, (Instance* instance, JSCell* cell, int32_t start, int32_t deleteCount, uint32_t count, const EncodedJSValue* items))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySplice);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySplice);
    if (!arraySpeciesWatchpointIsValid(vm, array)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
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

JSC_DEFINE_JIT_OPERATION(operationAOTArrayFromTypedArray, JSCell*, (Instance* instance, JSCell* cell, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayFromTypedArray);
    auto* view = cellArgument<JSArrayBufferView>(cell, Entry::operationAOTArrayFromTypedArray);
    markPristineIfOriginal(*instance, view);
    size_t count = view->length();
    JSArray* array = count <= MAX_STORAGE_VECTOR_LENGTH ? JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), 0, count) : nullptr;
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    for (size_t i = 0; i < count; ++i) {
        array->push(globalObject, view->getIndexQuicklyForTypedArray(i));
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAllocateStorage, void*, (Instance* instance, uint64_t size))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAllocateStorage);
    return vm.cellSpace().allocate(vm, size, nullptr, AllocationFailureMode::ReturnNull);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTArrayGrow, size_t, (Instance* instance, JSCell* cell, uint32_t length))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayGrow);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayGrow);
    IndexingType mode = array->indexingMode();
    if (mode != ArrayWithInt32 && mode != ArrayWithDouble && mode != ArrayWithContiguous) [[unlikely]]
        return false;
    if (length > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
        return false;
    unsigned publicLength = array->butterfly()->publicLength();
    if (!array->ensureLength(vm, length)) [[unlikely]]
        return false;
    array->butterfly()->setPublicLength(publicLength);
    return true;
}

template<typename CharacterType, typename SourceCharacterType>
static ALWAYS_INLINE CharacterType* appendCharacters(CharacterType* destination, std::span<const SourceCharacterType> characters)
{
    AOT_THRESHOLD(size_t, maxLengthCopiedInPlace, 8);
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

static bool holdsOnlyHoles(JSArray* array)
{
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    if (hasDouble(array->indexingType()))
        return std::ranges::all_of(std::span { butterfly.contiguousDouble().data(), length }, [](double element) { return isHole(element); });
    return std::ranges::all_of(std::span { butterfly.contiguous().data(), length }, [](const WriteBarrier<Unknown>& element) { return !element; });
}

static JSString* tryJoin(JSGlobalObject* globalObject, JSArray* array, JSString* separatorOrNull)
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
    switch (array->indexingType()) {
    case ALL_INT32_INDEXING_TYPES:
        RELEASE_AND_RETURN(scope, joiner.tryJoin<Int32Shape>(globalObject, butterfly.contiguous().data(), length));
    case ALL_DOUBLE_INDEXING_TYPES:
        RELEASE_AND_RETURN(scope, joiner.tryJoin(globalObject, butterfly.contiguousDouble().data(), length));
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
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
        if (areAllStrings && units >= runtimeLimitValue(vm, LimitID::MinRopeJoinLength) && units / length >= runtimeLimitValue(vm, LimitID::MinRopeJoinUnitsPerElement)) {
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
        RELEASE_AND_RETURN(scope, joiner.tryJoin<ContiguousShape>(globalObject, elements, length));
    }
    default:
        return nullptr;
    }
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
    OPERATION_RETURN(scope, JSValue::encode(call(globalObject, function, JSC::getCallData(function), receiver, arguments)));
}

static ALWAYS_INLINE bool precedesInDefaultOrder(const StringImpl& first, const StringImpl& second)
{
    if (first.is8Bit() && second.is8Bit()) [[likely]]
        return is_lt(codePointCompare(first.span8(), second.span8()));
    return is_lt(codePointCompare(StringView { first }, StringView { second }));
}

// Comparing strings runs no code, so every stable sort finds the order that sortImpl() in ArrayPrototype.cpp finds. False, and nothing that the program can see is done, for any other array.
static bool trySortStrings(JSGlobalObject* globalObject, JSArray* array)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The host function throws when it stores into a frozen array, also where the order stays the same.
    if (!hasContiguous(array->indexingType()) || array->structure()->hasImmutableProperties())
        return false;
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    if (length > maxLengthOfInsertionSortedArray)
        return false;

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
    if (isSorted)
        return true;
    RELEASE_AND_RETURN(scope, array->appendMemcpy(globalObject, vm, 0, ArrayWithContiguous, std::span<const EncodedJSValue> { sorted }.first(length)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySortInDefaultOrder, EncodedJSValue, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySortInDefaultOrder);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySortInDefaultOrder);
    bool isSorted = trySortStrings(globalObject, array);
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTArrayIndexOfInt32, size_t, (Instance*, JSCell* array, EncodedJSValue searched))
{
    Butterfly& butterfly = *cellArgument<JSArray>(array, Entry::operationAOTArrayIndexOfInt32)->butterfly();
    auto* data = std::bit_cast<const uint64_t*>(butterfly.contiguous().data());
    const uint64_t* found = WTF::find64(data, searched, butterfly.publicLength());
    return found ? found - data : static_cast<size_t>(-1);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayIndexOfString, size_t, (Instance* instance, JSCell* cell, JSCell* searched, uint64_t start))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayIndexOfString);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayIndexOfString);
    JSString* searchString = cellArgument<JSString>(searched, Entry::operationAOTArrayIndexOfString);
    constexpr size_t notFound = static_cast<size_t>(-1);
    const WriteBarrier<Unknown>* elements = hasContiguous(array->indexingType()) ? array->butterfly()->contiguous().data() : nullptr;
    if (!elements && !hasAnyArrayStorage(array->indexingType()))
        OPERATION_RETURN(scope, notFound);
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

JSC_DEFINE_JIT_OPERATION(operationAOTArrayUnshift, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue item))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayUnshift);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayUnshift);
    if (shouldUseSlowPut(array->indexingType()) || array->structure()->holesMustForwardToPrototype(array)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    uint64_t length = array->length();
    unshift(globalObject, array, 0, 0, 1, length);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    array->putByIndexInline(globalObject, static_cast<unsigned>(0), JSValue::decode(item), true);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    setLength(globalObject, vm, array, length + 1);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(length + 1)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewArrayIterator, JSCell*, (Instance* instance, JSCell* array, uint32_t kind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayIterator);
    DeferTerminationForAWhile deferTermination(vm);
    RELEASE_ASSERT(kind <= static_cast<uint32_t>(IterationKind::Entries));
    return JSArrayIterator::create(vm, globalObject->arrayIteratorStructure(), cellArgument<JSObject>(array, Entry::operationAOTNewArrayIterator), static_cast<IterationKind>(kind));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
