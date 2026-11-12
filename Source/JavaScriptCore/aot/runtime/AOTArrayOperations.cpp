/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCellFormat.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "AOTVector.h"
#include "ArrayConstructor.h"
#include "ButterflyInlines.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include "ExceptionHelpers.h"
#include "GCDeferralContextInlines.h"
#include "JSArrayBufferViewInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSObjectVisitInlines.h"
#include "TypedArrayAdaptors.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static NEVER_INLINE bool appendBoxedDoubles(JSGlobalObject* globalObject, VM& vm, JSArray* target, const double* numbers, unsigned length)
{
    Vector<EncodedJSValue, 16> boxed(length, [&](size_t i) {
        return JSValue::encode(JSValue(JSValue::EncodeAsDouble, numbers[i]));
    });
    return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithContiguous, boxed.span());
}

static bool holdsOnlyNumbersForDoubleStorage(std::span<const EncodedJSValue> values)
{
    return std::ranges::all_of(values, [](EncodedJSValue encoded) {
        JSValue value = JSValue::decode(encoded);
        return value.isInt32() || (value.isDouble() && !std::isnan(value.asDouble()));
    });
}

static bool holdsOnlyInt32s(std::span<const EncodedJSValue> values)
{
    return std::ranges::all_of(values, [](EncodedJSValue encoded) {
        return JSValue::decode(encoded).isInt32();
    });
}

static bool tryAppendArray(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSArray* source)
{
    IndexingType type = source->indexingType();
    unsigned length = source->length();
    if (hasDouble(type)) {
        if (containsHole(source->butterfly()->contiguousDouble().data(), length))
            return false;
        if (hasContiguous(target->indexingType()) && copiesIntoDenseStorage(vm.arrayStoragePolicy(), static_cast<uint64_t>(target->length()) + length))
            return appendBoxedDoubles(globalObject, vm, target, source->butterfly()->contiguousDouble().data(), length);
        if (hasInt32(target->indexingType()))
            target->tryMakeWritableDouble(vm);
    } else if (hasInt32(type) || hasContiguous(type)) {
        if (containsHole(source->butterfly()->contiguous().data(), length))
            return false;
        IndexingType targetType = target->indexingType();
        std::span<const EncodedJSValue> values { std::bit_cast<const EncodedJSValue*>(source->butterfly()->contiguous().data()), length };
        if (hasInt32(type) && (hasContiguous(targetType) || hasDouble(targetType)))
            return target->appendMemcpy(globalObject, vm, target->length(), hasDouble(targetType) ? ArrayWithDouble : ArrayWithContiguous, values);
        if (hasContiguous(type) && hasDouble(targetType) && holdsOnlyNumbersForDoubleStorage(values))
            return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithDouble, values);
        if (hasContiguous(type) && hasInt32(targetType) && holdsOnlyInt32s(values))
            return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithInt32, values);
        if (hasContiguous(type) && hasInt32(targetType) && holdsOnlyNumbersForDoubleStorage(values) && target->tryMakeWritableDouble(vm).data())
            return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithDouble, values);
    } else
        return false;
    return target->appendMemcpy(globalObject, vm, target->length(), source);
}

static ALWAYS_INLINE bool isPlainTarget(JSArray* target)
{
    IndexingType type = target->indexingType();
    return !isCopyOnWrite(target->indexingMode()) && (hasInt32(type) || hasDouble(type) || hasContiguous(type)) && !target->structure()->outOfLineCapacity() && !target->structure()->hasElementsStartOffset();
}

template<typename ValueAt>
static ALWAYS_INLINE bool tryFillEmptyTarget(VM& vm, JSArray* target, unsigned length, NOESCAPE const ValueAt& valueAt)
{
    IndexingType targetType = target->indexingType();
    if (!target->length() && length && isPlainTarget(target)) {
        if (hasContiguous(targetType) && target->butterfly()->vectorLength() >= length) {
            Butterfly* butterfly = target->butterfly();
            butterfly->setPublicLength(length);
            for (unsigned i = 0; i < length; ++i)
                butterfly->contiguous().atUnsafe(i).setWithoutWriteBarrier(valueAt(i));
            vm.writeBarrier(target);
            return true;
        }
        unsigned vectorLength = length | 1;
        StructureID oldStructureID = target->structureID();
        Structure* oldStructure = oldStructureID.decode();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = hasContiguous(targetType) ? oldStructure : Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateContiguous, &deferred);
        DeferGC deferGC(vm);
        if (void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull)) {
            Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
            butterfly->setVectorLength(vectorLength);
            butterfly->setPublicLength(length);
            for (unsigned i = 0; i < vectorLength; ++i)
                butterfly->contiguous().atUnsafe(i).setWithoutWriteBarrier(i < length ? valueAt(i) : JSValue());
            if (newStructure == oldStructure)
                target->setButterfly(vm, butterfly);
            else {
                target->nukeStructureAndSetButterfly(vm, oldStructureID, butterfly);
                target->setStructure(vm, newStructure);
            }
            vm.writeBarrier(target);
            return true;
        }
    }
    return false;
}

static NEVER_INLINE bool tryAppendArrayWithHoles(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSArray* source)
{
    IndexingType type = source->indexingType();
    unsigned length = source->length();
    if (!hasInt32(type) && !hasDouble(type) && !hasContiguous(type))
        return false;
    if (source->structure()->holesMustForwardToPrototype(source) || !copiesIntoDenseStorage(vm.arrayStoragePolicy(), static_cast<uint64_t>(target->length()) + length))
        return false;
    auto valueAt = [&](unsigned i) {
        if (hasDouble(type)) {
            double number = source->butterfly()->contiguousDouble().data()[i];
            return std::isnan(number) ? jsUndefined() : JSValue(JSValue::EncodeAsDouble, number);
        }
        JSValue value = source->butterfly()->contiguous().data()[i].get();
        return value ? value : jsUndefined();
    };
    if (tryFillEmptyTarget(vm, target, length, valueAt))
        return true;
    Vector<EncodedJSValue, 16> values(length, [&](size_t i) {
        return JSValue::encode(valueAt(i));
    });
    if (!target->tryMakeWritableContiguous(vm).data())
        return false;
    return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithContiguous, values.span());
}

static bool tryFillWithCodePoints(Instance& instance, JSArray* target, std::span<const char16_t> units)
{
    VM& vm = *instance.vm;
    if (target->length() || !isPlainTarget(target) || !target->tryMakeWritableContiguous(vm).data() || !target->ensureLength(vm, static_cast<unsigned>(units.size())))
        return false;
    unsigned count = 0;
    for (size_t i = 0; i < units.size(); ++i) {
        bool isPair = U16_IS_LEAD(units[i]) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1]);
        JSString* character = isPair ? jsNontrivialString(vm, String(units.subspan(i, 2))) : stringOfCharacter(instance, units[i]);
        i += isPair;
        target->butterfly()->contiguous().at(target, count++).set(vm, target, character);
    }
    target->butterfly()->setPublicLength(count);
    return true;
}

bool tryAppendCharacters(Instance& instance, JSArray* target, JSString* string)
{
    JSGlobalObject* globalObject = instance.globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (view->isEmpty())
        return true;
    if (!copiesIntoDenseStorage(vm.arrayStoragePolicy(), static_cast<uint64_t>(target->length()) + view->length()))
        return false;
    if (!view->is8Bit()) {
        bool isFilled = tryFillWithCodePoints(instance, target, view->span16());
        ensureStillAliveHere(string);
        return isFilled;
    }
    std::span<const Latin1Character> characters = view->span8();
    bool isFilled = tryFillEmptyTarget(vm, target, static_cast<unsigned>(characters.size()), [&](unsigned i) {
        return JSValue(vm.smallStrings.singleCharacterString(characters[i]));
    });
    ensureStillAliveHere(string);
    return isFilled;
}

static void appendElements(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSArray* source)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (static_cast<uint64_t>(target->length()) + source->length() > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    if (!source->length())
        return;
    bool isAppended = tryAppendArray(globalObject, vm, target, source);
    RETURN_IF_EXCEPTION(scope, void());
    if (isAppended)
        return;
    isAppended = tryAppendArrayWithHoles(globalObject, vm, target, source);
    RETURN_IF_EXCEPTION(scope, void());
    if (isAppended)
        return;
    unsigned length = source->length();
    for (unsigned i = 0; i < length; ++i) {
        JSValue element = source->getIndex(globalObject, i);
        RETURN_IF_EXCEPTION(scope, void());
        target->push(globalObject, element);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void appendElementsOfList(JSGlobalObject* globalObject, JSArray* target, JSArray* source)
{
    appendElements(globalObject, globalObject->vm(), target, source);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTConvertToDoubleStorage, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConvertToDoubleStorage);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTConvertToDoubleStorage);
    switch (array->indexingMode()) {
    case ArrayWithInt32:
    case CopyOnWriteArrayWithDouble:
        array->tryMakeWritableDouble(vm);
        return;
    default:
        return;
    }
}

static void reserveArrayCapacity(VM& vm, JSArray* array, uint64_t room)
{
    IndexingType mode = array->indexingMode();
    if (mode != ArrayWithInt32 && mode != ArrayWithDouble && mode != ArrayWithContiguous)
        return;
    unsigned vectorLength = array->butterfly()->vectorLength();
    if (room > MAX_STORAGE_VECTOR_LENGTH || room <= vectorLength)
        return;
    unsigned publicLength = array->butterfly()->publicLength();
    unsigned lengthThatGrowsToRoom = static_cast<unsigned>(room - room / 4 - room / 16);
    ASSERT(nextLength(lengthThatGrowsToRoom) >= room);
    if (array->ensureLength(vm, std::max(lengthThatGrowsToRoom, vectorLength + 1)))
        array->butterfly()->setPublicLength(publicLength);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReserveArrayCapacity, void, (Instance* instance, JSCell* cell, uint64_t room))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTReserveArrayCapacity);
    reserveArrayCapacity(vm, cellArgument<JSArray>(cell, Entry::operationAOTReserveArrayCapacity), room);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGrowFilledArray, void, (Instance* instance, JSCell* cell, uint64_t room))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGrowFilledArray);
    reserveArrayCapacity(vm, cellArgument<JSArray>(cell, Entry::operationAOTGrowFilledArray), room);
}

static bool isViewOfNumbers(JSCell* cell)
{
    return isTypedView(cell->type()) && !isBigIntTypedView(typedArrayType(cell->type()));
}

template<typename Function>
static void withAdaptorOfView(JSArrayBufferView* view, const Function& function)
{
    switch (typedArrayType(view->type())) {
    case TypeInt8:
        return function(Int8Adaptor());
    case TypeUint8:
        return function(Uint8Adaptor());
    case TypeUint8Clamped:
        return function(Uint8ClampedAdaptor());
    case TypeInt16:
        return function(Int16Adaptor());
    case TypeUint16:
        return function(Uint16Adaptor());
    case TypeInt32:
        return function(Int32Adaptor());
    case TypeUint32:
        return function(Uint32Adaptor());
    case TypeFloat16:
        return function(Float16Adaptor());
    case TypeFloat32:
        return function(Float32Adaptor());
    case TypeFloat64:
        return function(Float64Adaptor());
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

template<typename Adaptor>
static ALWAYS_INLINE typename Adaptor::Type nativeFromNumber(double number)
{
    if constexpr (Adaptor::isFloat)
        return Adaptor::toNativeFromDouble(purifyNaN(number));
    else if constexpr (Adaptor::typeValue == TypeUint8Clamped)
        return Adaptor::toNativeFromDouble(number);
    else {
        constexpr double limit = 0x1p63;
        if (number > -limit && number < limit) [[likely]]
            return static_cast<typename Adaptor::Type>(static_cast<int64_t>(number));
        return Adaptor::toNativeFromDouble(number);
    }
}

static constexpr size_t wordsTestedTogether = 8;

static size_t countPresentWords(const EncodedJSValue* words, size_t count)
{
    size_t present = 0;
    for (; present + wordsTestedTogether <= count; present += wordsTestedTogether) {
        bool someIsHole = false;
        for (size_t place = 0; place < wordsTestedTogether; ++place)
            someIsHole |= !words[present + place];
        if (someIsHole)
            break;
    }
    while (present < count && words[present])
        ++present;
    return present;
}

static size_t countPresentDoubles(const double* numbers, size_t count)
{
    size_t present = 0;
    for (; present + wordsTestedTogether <= count; present += wordsTestedTogether) {
        bool someIsHole = false;
        for (size_t place = 0; place < wordsTestedTogether; ++place)
            someIsHole |= numbers[present + place] != numbers[present + place];
        if (someIsHole)
            break;
    }
    while (present < count && numbers[present] == numbers[present])
        ++present;
    return present;
}

static size_t copyIntoView(JSArrayBufferView* target, size_t targetIndex, JSObject* source, size_t sourceIndex, size_t count)
{
    size_t length = target->length();
    if (targetIndex >= length)
        return 0;
    count = std::min(count, length - targetIndex);
    auto* sourceView = isViewOfNumbers(source) ? uncheckedDowncast<JSArrayBufferView>(source) : nullptr;
    if (sourceView && sourceView->type() == target->type()) {
        withAdaptorOfView(target, [&]<typename Adaptor>(Adaptor) {
            using Element = typename Adaptor::Type;
            auto* to = static_cast<Element*>(target->vector()) + targetIndex;
            const auto* from = static_cast<const Element*>(sourceView->vector()) + sourceIndex;
            bool readsStoredElements = from < to && to < from + count;
            if (!Adaptor::isFloat && !readsStoredElements) {
                memmove(to, from, count * sizeof(Element));
                return;
            }
            const size_t limit = count;
            for (size_t place = 0; place < limit; ++place) {
                Element element = from[place];
                if constexpr (Adaptor::isFloat) {
                    if (static_cast<double>(element) != static_cast<double>(element))
                        element = static_cast<Element>(PNaN);
                }
                to[place] = element;
            }
        });
        return count;
    }
    if (sourceView) {
        const auto* to = static_cast<const uint8_t*>(target->vector());
        const auto* from = static_cast<const uint8_t*>(sourceView->vector());
        if (from < to + target->byteLength() && to < from + sourceView->byteLength())
            return 0;
    }
    size_t copied = 0;
    withAdaptorOfView(target, [&]<typename Adaptor>(Adaptor) {
        auto* to = static_cast<typename Adaptor::Type*>(target->vector()) + targetIndex;
        const size_t limit = count;
        size_t place = 0;
        if (sourceView) {
            withAdaptorOfView(sourceView, [&]<typename SourceAdaptor>(SourceAdaptor) {
                const auto* from = static_cast<const typename SourceAdaptor::Type*>(sourceView->vector()) + sourceIndex;
                for (size_t each = 0; each < limit; ++each) {
                    if constexpr (SourceAdaptor::isFloat)
                        to[each] = nativeFromNumber<Adaptor>(static_cast<double>(from[each]));
                    else
                        to[each] = SourceAdaptor::template convertTo<Adaptor>(from[each]);
                }
            });
            copied = limit;
            return;
        }
        Butterfly& butterfly = *source->butterfly();
        if ((source->indexingType() & IndexingShapeMask) == DoubleShape) {
            const double* from = butterfly.contiguousDouble().data() + sourceIndex;
            for (; place < limit && from[place] == from[place]; ++place)
                to[place] = nativeFromNumber<Adaptor>(from[place]);
            copied = place;
            return;
        }
        const auto* from = butterfly.contiguous().data() + sourceIndex;
        for (; place < limit; ++place) {
            JSValue element = from[place].get();
            if (element.isInt32())
                to[place] = Adaptor::toNativeFromInt32(element.asInt32());
            else if (element && element.isDouble())
                to[place] = nativeFromNumber<Adaptor>(element.asDouble());
            else
                break;
        }
        copied = place;
    });
    return copied;
}

static size_t fillArray(VM& vm, JSArray* target, size_t targetIndex, JSValue value, size_t count)
{
    IndexingType shape = target->indexingType() & IndexingShapeMask;
    if ((shape != UndecidedShape && shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape) || mayRejectOrInterceptWrites(target))
        return 0;
    if (isCopyOnWrite(target->indexingMode()) && !target->tryMakeWritable(vm))
        return 0;
    bool isPlainDouble = value.isDouble() && value.asDouble() == value.asDouble();
    if (shape == UndecidedShape && (value.isInt32() ? !target->tryMakeWritableInt32(vm).data() : isPlainDouble ? !target->tryMakeWritableDouble(vm).data() : !target->tryMakeWritableContiguous(vm).data()))
        return 0;
    size_t lengthBefore = target->butterfly()->publicLength();
    if (targetIndex > lengthBefore || targetIndex + count > MAX_STORAGE_VECTOR_LENGTH)
        return 0;
    if (targetIndex + count > lengthBefore && !target->ensureLength(vm, static_cast<unsigned>(targetIndex + count)))
        return 0;
    size_t stored = 0;
    if (target->trySetIndexQuickly(vm, static_cast<unsigned>(targetIndex), value)) {
        shape = target->indexingType() & IndexingShapeMask;
        if (shape == DoubleShape) {
            double* to = target->butterfly()->contiguousDouble().data() + targetIndex;
            std::fill_n(to, count, value.asNumber());
            stored = count;
        } else if (shape == Int32Shape || shape == ContiguousShape) {
            auto to = target->butterfly()->contiguous();
            for (stored = 1; stored < count; ++stored)
                to.at(target, targetIndex + stored).setWithoutWriteBarrier(value);
            vm.writeBarrier(target, value);
        }
    }
    if (targetIndex + count > lengthBefore)
        target->butterfly()->setPublicLength(static_cast<unsigned>(std::max(lengthBefore, targetIndex + stored)));
    return stored;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFillElements, size_t, (Instance* instance, EncodedJSValue encodedTarget, EncodedJSValue encodedValue, uint64_t targetIndex, uint64_t count))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFillElements);
    JSValue target = JSValue::decode(encodedTarget);
    JSValue value = JSValue::decode(encodedValue);
    constexpr uint64_t limit = std::numeric_limits<int32_t>::max();
    if (!target.isObject() || targetIndex >= limit || static_cast<int64_t>(count) <= 0)
        return 0;
    count = std::min(count, limit - targetIndex);
    JSObject* to = asObject(target);
    if (isJSArray(to))
        return fillArray(vm, uncheckedDowncast<JSArray>(to), targetIndex, value, count);
    if (!isViewOfNumbers(to) || !value.isNumber())
        return 0;
    auto* view = uncheckedDowncast<JSArrayBufferView>(to);
    size_t length = view->length();
    if (targetIndex >= length)
        return 0;
    count = std::min<uint64_t>(count, length - targetIndex);
    withAdaptorOfView(view, [&]<typename Adaptor>(Adaptor) {
        double number = Adaptor::isFloat ? purifyNaN(value.asNumber()) : value.asNumber();
        std::fill_n(static_cast<typename Adaptor::Type*>(view->vector()) + targetIndex, count, Adaptor::toNativeFromDouble(number));
    });
    return count;
}

static size_t copyNumbersIntoArray(VM& vm, JSArray* target, size_t targetIndex, JSObject* source, size_t sourceIndex, size_t count)
{
    size_t copied = 0;
    auto copyFrom = [&](const auto& readNumber, const auto& readDouble) {
        const size_t limit = count;
        while (copied < limit) {
            IndexingType shape = target->indexingType() & IndexingShapeMask;
            size_t place = copied;
            if (shape == DoubleShape) {
                double* to = target->butterfly()->contiguousDouble().data() + targetIndex;
                for (; place < limit; ++place) {
                    double number = readDouble(place);
                    if (number != number)
                        break;
                    to[place] = number;
                }
            } else {
                auto* to = target->butterfly()->contiguous().data() + targetIndex;
                for (; place < limit; ++place) {
                    JSValue number = readNumber(place);
                    if (!number.isInt32() && shape == Int32Shape)
                        break;
                    to[place].setWithoutWriteBarrier(number);
                }
            }
            copied = place;
            if (copied < count && !target->trySetIndexQuickly(vm, static_cast<unsigned>(targetIndex + copied), readNumber(copied)))
                break;
            copied += copied < count;
        }
        vm.writeBarrier(target);
    };
    if (isViewOfNumbers(source)) {
        auto* view = uncheckedDowncast<JSArrayBufferView>(source);
        withAdaptorOfView(view, [&]<typename Adaptor>(Adaptor) {
            const auto* from = static_cast<const typename Adaptor::Type*>(view->vector()) + sourceIndex;
            copyFrom([&](size_t place) {
                if constexpr (Adaptor::isFloat)
                    return jsNumber(purifyNaN(static_cast<double>(from[place])));
                else
                    return jsNumber(from[place]);
            }, [&](size_t place) { return static_cast<double>(from[place]); });
        });
    } else if ((source->indexingType() & IndexingShapeMask) == DoubleShape) {
        const double* from = source->butterfly()->contiguousDouble().data() + sourceIndex;
        count = countPresentDoubles(from, count);
        copyFrom([&](size_t place) { return jsNumber(from[place]); }, [&](size_t place) { return from[place]; });
    } else {
        const auto* from = source->butterfly()->contiguous().data() + sourceIndex;
        count = countPresentWords(std::bit_cast<const EncodedJSValue*>(from), count);
        copyFrom([&](size_t place) { return from[place].get(); }, [&](size_t place) { return from[place].get().asNumber(); });
    }
    return copied;
}

static size_t copyIntoArray(VM& vm, JSArray* target, size_t targetIndex, JSObject* source, size_t sourceIndex, size_t count)
{
    IndexingType shape = target->indexingType() & IndexingShapeMask;
    if ((shape != UndecidedShape && shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape) || mayRejectOrInterceptWrites(target))
        return 0;
    IndexingType shapeOfSource = isViewOfNumbers(source) ? static_cast<IndexingType>(NoIndexingShape) : static_cast<IndexingType>(source->indexingType() & IndexingShapeMask);
    if (isCopyOnWrite(target->indexingMode()) && !target->tryMakeWritable(vm))
        return 0;
    if (shape == UndecidedShape || (shape == Int32Shape && shapeOfSource == DoubleShape)) {
        bool holdsDoubles = shapeOfSource == DoubleShape || (shapeOfSource == NoIndexingShape && !isInt(typedArrayType(source->type())));
        if (holdsDoubles ? !target->tryMakeWritableDouble(vm).data() : shapeOfSource == ContiguousShape ? !target->tryMakeWritableContiguous(vm).data() : !target->tryMakeWritableInt32(vm).data())
            return 0;
        shape = target->indexingType() & IndexingShapeMask;
    }
    size_t lengthBefore = target->butterfly()->publicLength();
    if (targetIndex > lengthBefore || targetIndex + count > MAX_STORAGE_VECTOR_LENGTH)
        return 0;
    if (targetIndex + count > lengthBefore && !target->ensureLength(vm, static_cast<unsigned>(targetIndex + count)))
        return 0;
    size_t copied = 0;
    Butterfly& butterfly = *target->butterfly();
    if (shape == DoubleShape && shapeOfSource == DoubleShape) {
        double* to = butterfly.contiguousDouble().data() + targetIndex;
        const double* from = source->butterfly()->contiguousDouble().data() + sourceIndex;
        if (from < to && to < from + count) {
            for (; copied < count && from[copied] == from[copied]; ++copied)
                to[copied] = from[copied];
        } else {
            copied = countPresentDoubles(from, count);
            memmove(to, from, copied * sizeof(double));
        }
    } else if ((shape == ContiguousShape && (shapeOfSource == ContiguousShape || shapeOfSource == Int32Shape)) || (shape == Int32Shape && shapeOfSource == Int32Shape)) {
        auto* to = std::bit_cast<EncodedJSValue*>(butterfly.contiguous().data()) + targetIndex;
        const auto* from = std::bit_cast<const EncodedJSValue*>(source->butterfly()->contiguous().data()) + sourceIndex;
        if (from < to && to < from + count) {
            for (; copied < count && from[copied]; ++copied)
                to[copied] = from[copied];
        } else {
            copied = countPresentWords(from, count);
            gcSafeMemmove(to, from, copied * sizeof(EncodedJSValue));
        }
        vm.writeBarrier(target);
    } else if (shapeOfSource == ContiguousShape) {
        for (; copied < count; ++copied) {
            JSValue element = source->butterfly()->contiguous().at(source, sourceIndex + copied).get();
            if (!element || !target->trySetIndexQuickly(vm, static_cast<unsigned>(targetIndex + copied), element))
                break;
        }
    } else {
        copied = copyNumbersIntoArray(vm, target, targetIndex, source, sourceIndex, count);
    }
    if (targetIndex + count > lengthBefore)
        target->butterfly()->setPublicLength(static_cast<unsigned>(std::max(lengthBefore, targetIndex + copied)));
    return copied;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCopyElements, size_t, (Instance* instance, EncodedJSValue encodedTarget, EncodedJSValue encodedSource, uint64_t targetIndex, uint64_t sourceIndex, uint64_t count))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyElements);
    JSValue target = JSValue::decode(encodedTarget);
    JSValue source = JSValue::decode(encodedSource);
    constexpr uint64_t limit = std::numeric_limits<int32_t>::max();
    if (!target.isObject() || !source.isObject() || targetIndex >= limit || sourceIndex >= limit || static_cast<int64_t>(count) <= 0)
        return 0;
    count = std::min({ count, limit - targetIndex, limit - sourceIndex });
    JSObject* from = asObject(source);
    size_t length = 0;
    if (isViewOfNumbers(from))
        length = uncheckedDowncast<JSArrayBufferView>(from)->length();
    else if (isJSArray(from)) {
        IndexingType shape = from->indexingType() & IndexingShapeMask;
        if (shape == Int32Shape || shape == DoubleShape || shape == ContiguousShape)
            length = from->butterfly()->publicLength();
    }
    if (sourceIndex >= length)
        return 0;
    count = std::min<uint64_t>(count, length - sourceIndex);
    JSObject* to = asObject(target);
    if (isViewOfNumbers(to))
        return copyIntoView(uncheckedDowncast<JSArrayBufferView>(to), targetIndex, from, sourceIndex, count);
    return isJSArray(to) ? copyIntoArray(vm, uncheckedDowncast<JSArray>(to), targetIndex, from, sourceIndex, count) : 0;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCopyElementsDown, size_t, (Instance* instance, EncodedJSValue encodedTarget, EncodedJSValue encodedSource, uint64_t targetIndex, uint64_t sourceIndex, uint64_t count))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyElementsDown);
    JSValue targetValue = JSValue::decode(encodedTarget);
    JSValue sourceValue = JSValue::decode(encodedSource);
    constexpr uint64_t limit = std::numeric_limits<int32_t>::max();
    if (!targetValue.isObject() || !sourceValue.isObject() || targetIndex >= limit || sourceIndex >= limit || static_cast<int64_t>(count) <= 0)
        return 0;
    count = std::min({ count, targetIndex + 1, sourceIndex + 1 });
    JSObject* target = asObject(targetValue);
    JSObject* source = asObject(sourceValue);
    size_t copied = 0;
    if (isViewOfNumbers(target) && target->type() == source->type()) {
        auto* to = uncheckedDowncast<JSArrayBufferView>(target);
        auto* from = uncheckedDowncast<JSArrayBufferView>(source);
        if (targetIndex >= to->length() || sourceIndex >= from->length())
            return 0;
        withAdaptorOfView(to, [&]<typename Adaptor>(Adaptor) {
            using Element = typename Adaptor::Type;
            auto* stored = static_cast<Element*>(to->vector()) + targetIndex;
            const auto* read = static_cast<const Element*>(from->vector()) + sourceIndex;
            for (; copied < count; ++copied) {
                Element element = *(read - copied);
                if constexpr (Adaptor::isFloat) {
                    if (static_cast<double>(element) != static_cast<double>(element))
                        element = static_cast<Element>(PNaN);
                }
                *(stored - copied) = element;
            }
        });
        return copied;
    }
    if (!isJSArray(target) || !isJSArray(source))
        return 0;
    auto* array = uncheckedDowncast<JSArray>(target);
    IndexingType shape = array->indexingType() & IndexingShapeMask;
    IndexingType shapeOfSource = source->indexingType() & IndexingShapeMask;
    bool takesWords = (shape == ContiguousShape && (shapeOfSource == ContiguousShape || shapeOfSource == Int32Shape)) || (shape == Int32Shape && shapeOfSource == Int32Shape);
    if ((!takesWords && (shape != DoubleShape || shapeOfSource != DoubleShape)) || mayRejectOrInterceptWrites(array) || (isCopyOnWrite(array->indexingMode()) && !array->tryMakeWritable(vm)))
        return 0;
    if (targetIndex >= array->butterfly()->publicLength() || sourceIndex >= source->butterfly()->publicLength())
        return 0;
    if (takesWords) {
        auto to = array->butterfly()->contiguous();
        auto from = source->butterfly()->contiguous();
        for (; copied < count; ++copied) {
            JSValue element = from.at(source, sourceIndex - copied).get();
            if (!element)
                break;
            to.at(array, targetIndex - copied).setWithoutWriteBarrier(element);
        }
        vm.writeBarrier(array);
        return copied;
    }
    double* to = array->butterfly()->contiguousDouble().data() + targetIndex;
    const double* from = source->butterfly()->contiguousDouble().data() + sourceIndex;
    for (; copied < count && *(from - copied) == *(from - copied); ++copied)
        *(to - copied) = *(from - copied);
    return copied;
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetArrayLength, void, (Instance* instance, JSCell* cell, uint32_t length))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetArrayLength);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTSetArrayLength);
    PutPropertySlot slot(array, true);
    array->methodTable()->put(array, globalObject, vm.propertyNames->length, jsNumber(length), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetArrayLengthInSloppyCode, void, (Instance* instance, JSCell* cell, uint32_t length))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetArrayLengthInSloppyCode);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTSetArrayLengthInSloppyCode);
    PutPropertySlot slot(array, false);
    array->methodTable()->put(array, globalObject, vm.propertyNames->length, jsNumber(length), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTAppendElements, void, (Instance* instance, JSCell* cell, JSCell* sourceCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAppendElements);
    JSArray* target = cellArgument<JSArray>(cell, Entry::operationAOTAppendElements);
    JSArray* source = cellArgument<JSArray>(sourceCell, Entry::operationAOTAppendElements);
    if (hasAnyArrayStorage(target->indexingType()) || mayRejectOrInterceptWrites(target)) [[unlikely]] {
        MarkedArgumentBuffer elements;
        for (unsigned i = 0, length = source->length(); i < length; ++i) {
            elements.append(source->getIndex(globalObject, i));
            OPERATION_RETURN_IF_EXCEPTION(scope);
        }
        if (elements.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope);
        }
        callMethodInEngine(instance, Builtins::ArrayPrototypePush, target, elements);
        OPERATION_RETURN(scope);
    }
    appendElements(globalObject, vm, target, source);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTAppendHole, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAppendHole);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTAppendHole);
    array->setLength(globalObject, array->length() + 1, true);
    OPERATION_RETURN(scope, array);
}

JSArray* tryCreateArrayWithStorage(VM& vm, Structure* structure, unsigned publicLength, unsigned vectorLength, unsigned propertyRoom, NewElements elements)
{
    IndexingType indexingType = structure->indexingType();
    RELEASE_ASSERT(publicLength <= vectorLength && !structure->outOfLineCapacity() && !structure->hasElementsStartOffset() && structure->classInfoForCells() == JSArray::info());
    RELEASE_ASSERT(hasInt32(indexingType) || hasDouble(indexingType) || hasContiguous(indexingType));
    if (!vectorLength || vectorLength > MarkedSpace::largeCutoff / sizeof(EncodedJSValue))
        return nullptr;
    size_t size = sizeof(JSArray) + propertyRoom + Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue));
    if (size > MarkedSpace::largeCutoff)
        return nullptr;
    char* cell = static_cast<char*>(vm.cellSpace().allocate(vm, size, nullptr, AllocationFailureMode::Assert));
    std::fill_n(std::bit_cast<uint64_t*>(cell + sizeof(JSArray)), propertyRoom / sizeof(uint64_t), 0);
    Butterfly* butterfly = Butterfly::fromBase(cell + sizeof(JSArray) + propertyRoom, 0, 0);
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(publicLength);
    uint64_t hole = hasDouble(indexingType) ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
    unsigned firstHole = elements == NewElements::WrittenByCaller ? publicLength : 0;
    std::fill_n(butterfly->indexingPayload<uint64_t>() + firstHole, vectorLength - firstHole, hole);
    *std::bit_cast<Butterfly**>(cell + JSObject::butterflyOffset()) = butterfly;
    *std::bit_cast<uint64_t*>(cell) = cellHeaderFor(structure);
    vm.mutatorFence();
    return std::bit_cast<JSArray*>(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewArrayOfList, JSCell*, (Instance* instance, JSCell* cell, uint32_t capacityAndKind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayOfList);
    JSArray* list = cellArgument<JSArray>(cell, Entry::operationAOTNewArrayOfList);
    unsigned capacity = capacityAndKind >> 1;
    IndexingType kindOfList = list->indexingType();
    if (hasDouble(kindOfList)) {
        size_t count = list->butterfly()->publicLength();
        bool holdsValues = capacityAndKind & 1 || std::ranges::any_of(std::span { list->butterfly()->contiguousDouble().data(), count }, [](double fraction) {
            return fraction != fraction;
        });
        Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(holdsValues ? ArrayWithContiguous : ArrayWithDouble);
        if (capacity < count || capacity > maxHoleArrayCapacity || (holdsValues ? !hasContiguous(structure->indexingType()) : !hasDouble(structure->indexingType())) || structure->outOfLineCapacity())
            OPERATION_RETURN(scope, nullptr);
        JSArray* array = tryCreateArrayWithStorage(vm, structure, count, capacity, 0, NewElements::WrittenByCaller);
        if (!array)
            OPERATION_RETURN(scope, nullptr);
        for (size_t index = 0; index < count; ++index) {
            double fraction = list->butterfly()->contiguousDouble().atUnsafe(index);
            if (!holdsValues)
                array->butterfly()->contiguousDouble().atUnsafe(index) = fraction;
            else
                array->butterfly()->contiguous().atUnsafe(index).setWithoutWriteBarrier(fraction != fraction ? jsUndefined() : JSValue(JSValue::EncodeAsDouble, fraction));
        }
        vm.mutatorFence();
        OPERATION_RETURN(scope, array);
    }
    if (!hasInt32(kindOfList) && !hasContiguous(kindOfList))
        OPERATION_RETURN(scope, nullptr);
    std::span<const WriteBarrier<Unknown>> elements { list->butterfly()->contiguous().data(), list->butterfly()->publicLength() };
    if (capacity < elements.size() || capacity > maxHoleArrayCapacity)
        OPERATION_RETURN(scope, nullptr);
    bool isContiguous = capacityAndKind & 1 || std::ranges::any_of(elements, [](const WriteBarrier<Unknown>& element) {
        return !element || !element.get().isInt32();
    });
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(isContiguous ? ArrayWithContiguous : ArrayWithInt32);
    if ((isContiguous ? !hasContiguous(structure->indexingType()) : !hasInt32(structure->indexingType())) || structure->outOfLineCapacity())
        OPERATION_RETURN(scope, nullptr);
    JSArray* array = tryCreateArrayWithStorage(vm, structure, elements.size(), capacity, 0, NewElements::WrittenByCaller);
    if (!array)
        OPERATION_RETURN(scope, nullptr);
    elements = { list->butterfly()->contiguous().data(), elements.size() };
    for (size_t index = 0; index < elements.size(); ++index)
        array->butterfly()->contiguous().atUnsafe(index).setWithoutWriteBarrier(elements[index] ? elements[index].get() : jsUndefined());
    vm.mutatorFence();
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArray, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArray);
    static_assert(maxValuesInArrayLiteral < roomInArrayLiteralCount && roomInArrayLiteralCount < int32ValuesInArrayLiteralCount && int32ValuesInArrayLiteralCount < 1u << capacityShiftInArrayLiteralCount);
    bool choosesStorage = count >= numbersInArrayLiteralCount;
    unsigned emptyCapacity = choosesStorage ? 0 : count >> capacityShiftInArrayLiteralCount;
    bool holdsInt32 = count & int32ValuesInArrayLiteralCount;
    unsigned propertyRoom = count & roomInArrayLiteralCount ? propertyRoomOfArray : 0;
    count &= roomInArrayLiteralCount - 1;
    if (count > maxValuesInArrayLiteral || (emptyCapacity && count)) [[unlikely]]
        crashOnWrongOperand(Entry::operationAOTNewArray);
    auto valueAt = [&](uint32_t i) {
        return holdsInt32 ? jsNumber(static_cast<int32_t>(values[i])) : JSValue::decode(values[i]);
    };
    IndexingType storage = holdsInt32 ? ArrayWithInt32 : ArrayWithContiguous;
    if (choosesStorage) {
        std::span written { values, count };
        bool areInt32 = std::ranges::all_of(written, [](EncodedJSValue value) { return JSValue::decode(value).isInt32(); });
        bool areNumbers = std::ranges::all_of(written, [](EncodedJSValue value) {
            JSValue number = JSValue::decode(value);
            return number.isNumber() && !std::isnan(number.asNumber());
        });
        storage = areInt32 ? ArrayWithInt32 : areNumbers ? ArrayWithDouble : ArrayWithContiguous;
    }
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(storage);
    unsigned vectorLength = emptyCapacity ? emptyCapacity : arrayLiteralVectorLength(count);
    bool hasWantedStorage = storage == ArrayWithInt32 ? hasInt32(structure->indexingType()) : storage == ArrayWithContiguous && hasContiguous(structure->indexingType());
    if (hasWantedStorage && !structure->outOfLineCapacity()) {
        if (JSArray* array = vectorLength <= maxHoleArrayCapacity ? tryCreateArrayWithStorage(vm, structure, count, vectorLength, propertyRoom) : nullptr) {
            for (uint32_t i = 0; i < count; ++i)
                array->butterfly()->contiguous().atUnsafe(i).setWithoutWriteBarrier(valueAt(i));
            vm.mutatorFence();
            instance->findMissingShortArrayAllocator(vectorLength + propertyRoom / sizeof(EncodedJSValue));
            OPERATION_RETURN(scope, array);
        }
        if (void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull)) {
            Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
            butterfly->setVectorLength(vectorLength);
            butterfly->setPublicLength(count);
            for (uint32_t i = 0; i < vectorLength; ++i)
                butterfly->contiguous().atUnsafe(i).setWithoutWriteBarrier(i < count ? valueAt(i) : JSValue());
            OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, structure, butterfly));
        }
    }
    JSArray* array = JSArray::tryCreate(vm, structure, count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    for (uint32_t i = 0; i < count; ++i) {
        array->putDirectIndex(globalObject, i, valueAt(i));
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayOfLengthOrElement, JSCell*, (Instance* instance, EncodedJSValue lengthOrElement))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayOfLengthOrElement);
    OPERATION_RETURN(scope, constructArrayWithSizeQuirk(globalObject, nullptr, JSValue::decode(lengthOrElement)));
}

static JSArray* newArrayOfHoles(Instance* instance, JSGlobalObject* globalObject, VM& vm, unsigned length, unsigned capacity, IndexingType indexingType)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT(length <= capacity && (indexingType == ArrayWithDouble || indexingType == ArrayWithInt32 || indexingType == ArrayWithContiguous));
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType);
    if (structure->indexingType() != indexingType) [[unlikely]] {
        if (!length) {
            JSArray* array = JSArray::tryCreate(vm, structure, length);
            if (!array)
                throwOutOfMemoryError(globalObject, scope);
            return array;
        }
        structure = globalObject->originalArrayStructureForIndexingType(indexingType);
    }
    if (JSArray* array = tryCreateArrayWithStorage(vm, structure, length, capacity)) {
        instance->findMissingShortArrayAllocator(capacity);
        return array;
    }
    void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, capacity * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!storage) {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
    butterfly->setVectorLength(capacity);
    butterfly->setPublicLength(length);
    for (unsigned i = 0; i < capacity; ++i) {
        if (indexingType == ArrayWithDouble)
            butterfly->contiguousDouble().atUnsafe(i) = PNaN;
        else
            butterfly->contiguous().atUnsafe(i).clear();
    }
    return JSArray::createWithButterfly(vm, nullptr, structure, butterfly);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayOfHoles, JSCell*, (Instance* instance, uint32_t lengthAndCapacity, uint32_t flags))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayOfHoles);
    unsigned length = lengthAndCapacity & ((1u << holeArrayCapacityShift) - 1);
    unsigned capacity = lengthAndCapacity >> holeArrayCapacityShift;
    OPERATION_RETURN(scope, newArrayOfHoles(instance, globalObject, vm, length, capacity, static_cast<IndexingType>(flags)));
}

static bool growFullVector(VM& vm, JSArray* array, unsigned length)
{
    static_assert(2 * MIN_SPARSE_ARRAY_INDEX <= MAX_STORAGE_VECTOR_LENGTH);
    Structure* structure = array->structure();
    if (length >= MIN_SPARSE_ARRAY_INDEX || structure->outOfLineCapacity() || structure->hasElementsStartOffset()) [[unlikely]]
        return false;

    GCDeferralContext deferralContext(vm);
    AssertNoGC assertNoGC;
    Butterfly* butterfly = array->butterfly();
    unsigned newVectorLength = isStorageInOwnCell(array, butterfly->base(0, 0)) ? length : Butterfly::availableContiguousVectorLength(static_cast<size_t>(0), length);
    bool hasRoomBehindVector = newVectorLength > length;
    if (!hasRoomBehindVector) {
        newVectorLength = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), static_cast<unsigned>(nextLength(length + 1)));
        butterfly = butterfly->reallocArrayRightIfPossible(vm, deferralContext, array, structure, 0, true, length * sizeof(EncodedJSValue), newVectorLength * sizeof(EncodedJSValue));
        if (!butterfly) [[unlikely]]
            return false;
    }

    uint64_t hole = hasDouble(array->indexingType()) ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
    std::fill_n(butterfly->indexingPayload<uint64_t>() + length, newVectorLength - length, hole);
    if (hasRoomBehindVector) {
        WTF::storeStoreFence();
        butterfly->setVectorLength(newVectorLength);
        return true;
    }
    butterfly->setVectorLength(newVectorLength);
    array->setButterfly(vm, butterfly);
    return true;
}

unsigned pushToFullVector(VM& vm, JSArray* array, JSValue value)
{
    IndexingType mode = array->indexingMode();
    bool isTaken = mode == ArrayWithContiguous || (mode == ArrayWithInt32 ? value.isInt32() : mode == ArrayWithDouble && value.isNumber() && value.asNumber() == value.asNumber());
    if (!isTaken)
        return 0;
    unsigned length = array->butterfly()->publicLength();
    if (length != array->butterfly()->vectorLength() || !growFullVector(vm, array, length))
        return 0;
    Butterfly* butterfly = array->butterfly();
    if (mode == ArrayWithDouble)
        butterfly->contiguousDouble().at(array, length) = value.asNumber();
    else
        butterfly->contiguous().at(array, length).setWithoutWriteBarrier(value);
    butterfly->setPublicLength(length + 1);
    vm.writeBarrier(array, value);
    return length + 1;
}

static EncodedJSValue pushToArray(Instance* instance, JSArray* array, JSValue value)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    if (unsigned newLength = pushToFullVector(vm, array, value))
        return JSValue::encode(jsNumber(newLength));
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (mayRejectOrInterceptWrites(array)) [[unlikely]]
        RELEASE_AND_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypePush, array, value)));
    array->push(globalObject, value);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    return JSValue::encode(jsNumber(array->length()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPush, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayPush);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayPush);
    OPERATION_RETURN(scope, pushToArray(instance, array, JSValue::decode(value)));
}

static JSArray* tryFastSlice(VM& vm, JSGlobalObject* globalObject, JSArray* array, int64_t from, int64_t to)
{
    if (!array->isPristine() || globalObject->isHavingABadTime())
        return nullptr;
    IndexingType shape = array->indexingType() & IndexingShapeMask;
    if (shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape)
        return nullptr;
    int64_t length = array->butterfly()->publicLength();
    from = from < 0 ? std::max<int64_t>(from + length, 0) : std::min(from, length);
    to = to < 0 ? std::max<int64_t>(to + length, 0) : std::min(to, length);
    int64_t count = std::max<int64_t>(to - from, 0);
    if (count > maxLengthForElementwiseCopy)
        return JSArray::fastSlice(globalObject, array, from, count);
    unsigned vectorLength = static_cast<unsigned>(count) | 1;
    auto hasCellsFor = [&](unsigned propertyRoom) {
        Allocator allocator = vm.cellSpace().allocatorFor(sizeOfArrayWithStorage(vectorLength, propertyRoom), AllocatorForMode::AllocatorIfExists);
        return allocator && !std::bit_cast<FreeList*>(std::bit_cast<char*>(allocator.localAllocator()) + LocalAllocator::offsetOfFreeList())->allocationWillFail();
    };
    unsigned propertyRoom = hasCellsFor(0) && !hasCellsFor(propertyRoomOfArray) ? propertyRoomOfArray : 0;
    JSArray* result = tryCreateArrayWithStorage(vm, globalObject->originalArrayStructureForIndexingType(static_cast<IndexingType>(IsArray | shape)), static_cast<unsigned>(count), vectorLength, propertyRoom, NewElements::WrittenByCaller);
    if (!result)
        return nullptr;
    std::copy_n(array->butterfly()->indexingPayload<uint64_t>() + from, count, result->butterfly()->indexingPayload<uint64_t>());
    vm.mutatorFence();
    ensureStillAliveHere(array);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayCopy, EncodedJSValue, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayCopy);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArrayCopy);
    if (JSArray* copy = tryFastSlice(vm, globalObject, array, 0, std::numeric_limits<int64_t>::max()))
        OPERATION_RETURN(scope, JSValue::encode(copy));
    OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeSlice, array)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySlice, EncodedJSValue, (Instance* instance, JSCell* cell, int64_t from, int64_t to))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySlice);
    JSArray* array = cellArgument<JSArray>(cell, Entry::operationAOTArraySlice);
    if (JSArray* part = tryFastSlice(vm, globalObject, array, from, to))
        OPERATION_RETURN(scope, JSValue::encode(part));
    OPERATION_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypeSlice, array, jsNumber(static_cast<double>(from)), jsNumber(static_cast<double>(to)))));
}

static unsigned capacityInUse(const ArrayAllocationProfile& profile)
{
    return profile.capacity() ? profile.capacity() : arrayLiteralVectorLength(0);
}

static void startWindow(ArrayAllocationProfile& profile)
{
    profile.reviews = 0;
    std::ranges::fill(profile.largestLengths, 0);
}

static IndexingType narrowestShapeThatHoldsContentsOf(JSArray* array)
{
    IndexingType shape = Int32Shape;
    Butterfly* butterfly = array->butterfly();
    for (unsigned position = 0; position < butterfly->publicLength(); ++position) {
        JSValue value = butterfly->contiguous().at(array, position).get();
        if (!value || value.isInt32())
            continue;
        if (!value.isDouble() || value.asDouble() != value.asDouble())
            return ContiguousShape;
        shape = DoubleShape;
    }
    return shape;
}

void reviewArrayAllocationProfile(ArrayAllocationProfile& profile, JSArray* lastArrayOrNull, bool readsContentsAtFirst)
{
    if (!lastArrayOrNull) {
        if (profile.intervalDoublings)
            --profile.intervalDoublings;
        return;
    }
    IndexingType shape = lastArrayOrNull->indexingType() & IndexingShapeMask;
    if (readsContentsAtFirst && !profile.shape() && shape == ContiguousShape)
        shape = narrowestShapeThatHoldsContentsOf(lastArrayOrNull);
    if (shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape) {
        if (!profile.shape())
            profile.set(profile.capacity(), Int32Shape);
        return;
    }

    IndexingType learnedShape = std::max(profile.shape(), shape);
    unsigned length = std::min<unsigned>(lastArrayOrNull->butterfly()->publicLength(), BASE_CONTIGUOUS_VECTOR_LEN_MAX);
    unsigned inUse = capacityInUse(profile);
    uint8_t entering = static_cast<uint8_t>(length);
    for (uint8_t& kept : profile.largestLengths) {
        if (entering > kept)
            std::swap(entering, kept);
    }

    if (length > inUse || learnedShape != profile.shape())
        profile.intervalDoublings = 0;

    unsigned wanted = profile.largestLengths[ArrayAllocationProfile::lengthsKept - 1] | 1;
    if (wanted > inUse) {
        profile.set(std::min<unsigned>(wanted, BASE_CONTIGUOUS_VECTOR_LEN_MAX), learnedShape);
        startWindow(profile);
        return;
    }
    if (profile.reviews + 1 < ArrayAllocationProfile::reviewsInWindow) {
        ++profile.reviews;
        profile.set(profile.capacity(), learnedShape);
        return;
    }

    for (uint8_t kept : profile.largestLengths)
        wanted = kept ? kept | 1 : wanted;
    bool takesLessRoom = wanted < inUse && profile.largestLengths[0];
    if (takesLessRoom)
        profile.intervalDoublings = 0;
    else if (profile.intervalDoublings < ArrayAllocationProfile::maxIntervalDoublings)
        ++profile.intervalDoublings;
    profile.set(takesLessRoom ? wanted : profile.capacity(), learnedShape);
    startWindow(profile);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayAtReviewedSite, JSCell*, (Instance* instance, void* profileAddress, uint32_t seedIndexingType))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayAtReviewedSite);
    uint8_t* statics = instance->statics();
    uint8_t* address = static_cast<uint8_t*>(profileAddress);
    uint64_t distanceToLastArray = instance->arrays.distanceFromProfileToLastArray;
    RELEASE_ASSERT(address >= statics && distanceToLastArray >= sizeof(ArrayAllocationProfile));
    RELEASE_ASSERT(address + distanceToLastArray + ArrayAllocationProfile::sizeOfLastArrayWord <= statics + instance->tables().instanceSizes.sizeOfStatics);
    RELEASE_ASSERT(seedIndexingType == ArrayWithInt32 || seedIndexingType == ArrayWithDouble || seedIndexingType == ArrayWithContiguous);
    auto& profile = *reinterpret_cast<ArrayAllocationProfile*>(address);
    auto& lastArray = *reinterpret_cast<JSArray**>(address + distanceToLastArray);
    ASSERT(profile.isDueForReview());

    reviewArrayAllocationProfile(profile, lastArray);
    profile.startInterval();

    IndexingType shape = std::max<IndexingType>(profile.shape(), seedIndexingType & IndexingShapeMask);
    JSArray* array = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(IsArray | shape), 0, capacityInUse(profile));
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    // Not a root: every collection zeroes this word, so it neither keeps the array alive nor dangles.
    lastArray = array;
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewArrayWithLengthAtReviewedSite, JSCell*, (Instance* instance, void* profileAddress, uint32_t seedAndLengthAndCapacity))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewArrayWithLengthAtReviewedSite);
    uint8_t* statics = instance->statics();
    uint8_t* address = static_cast<uint8_t*>(profileAddress);
    uint64_t distanceToLastArray = instance->arrays.distanceFromProfileToLastArray;
    uint32_t seedIndexingType = seedAndLengthAndCapacity >> seedShiftBesideLengthAndCapacity;
    unsigned capacity = (seedAndLengthAndCapacity & ((1u << seedShiftBesideLengthAndCapacity) - 1)) >> holeArrayCapacityShift;
    unsigned length = seedAndLengthAndCapacity & ((1u << holeArrayCapacityShift) - 1);
    RELEASE_ASSERT(address >= statics && distanceToLastArray >= sizeof(ArrayAllocationProfile));
    RELEASE_ASSERT(address + distanceToLastArray + ArrayAllocationProfile::sizeOfLastArrayWord <= statics + instance->tables().instanceSizes.sizeOfStatics);
    RELEASE_ASSERT(seedIndexingType == ArrayWithInt32 || seedIndexingType == ArrayWithDouble);
    RELEASE_ASSERT(capacity & 1 && capacity <= maxHoleArrayCapacity && length <= capacity);
    auto& profile = *reinterpret_cast<ArrayAllocationProfile*>(address);
    auto& lastArray = *reinterpret_cast<JSArray**>(address + distanceToLastArray);
    ASSERT(profile.isDueForReview());

    reviewArrayAllocationProfile(profile, lastArray);
    profile.startInterval();
    lastArray = nullptr;
    if (globalObject->isHavingABadTime())
        OPERATION_RETURN(scope, nullptr);

    IndexingType shape = std::max<IndexingType>(profile.shape(), seedIndexingType & IndexingShapeMask);
    JSArray* array = tryCreateArrayWithStorage(vm, globalObject->originalArrayStructureForIndexingType(IsArray | shape), length, capacity);
    lastArray = array;
    OPERATION_RETURN(scope, array);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
