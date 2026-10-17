/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedObject.h"
#include "ArrayPrototypeInlines.h"
#include "CommonSlowPaths.h"
#include "JSCInlines.h"
#include "JSStringJoiner.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySplice, EncodedJSValue, (Instance* instance, JSCell* cell, int32_t start, int32_t deleteCount, uint32_t count, const EncodedJSValue* items, uint32_t isUsed))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    if (!arraySpeciesWatchpointIsValid(vm, array)) [[unlikely]]
        OPERATION_RETURN(scope, encodedJSValue());
    int64_t length = array->length();
    int64_t from = start < 0 ? std::max<int64_t>(length + start, 0) : std::min<int64_t>(start, length);
    int64_t removed = std::clamp<int64_t>(deleteCount, 0, length - from);
    JSArray* result = isUsed ? JSArray::fastSlice(globalObject, array, from, removed) : array;
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!result)
        OPERATION_RETURN(scope, encodedJSValue());
    bool isDone = array->fastSplice(globalObject, length, from, removed, std::span { items, count });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, isDone ? JSValue::encode(result) : encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayFromTypedArray, JSCell*, (Instance* instance, JSCell* cell, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
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

static bool growVector(VM& vm, JSArray* array, unsigned length, unsigned expected = 0)
{
    static constexpr unsigned maxLength = 1024;
    static_assert(maxLength < MIN_SPARSE_ARRAY_INDEX && maxLength < MAX_STORAGE_VECTOR_LENGTH);
    Butterfly* butterfly = array->butterfly();
    unsigned oldVectorLength = butterfly->vectorLength();
    if (length > maxLength || length <= oldVectorLength || array->structure()->outOfLineCapacity()) [[unlikely]]
        return false;
    unsigned newVectorLength = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), expected >= length && expected <= maxLength ? expected : static_cast<unsigned>(nextLength(length)));
    void* base = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, newVectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!base) [[unlikely]]
        return false;
    memcpy(base, butterfly->base(0, 0), Butterfly::totalSize(0, 0, true, oldVectorLength * sizeof(EncodedJSValue)));
    Butterfly* grown = Butterfly::fromBase(base, 0, 0);
    uint64_t hole = hasDouble(array->indexingType()) ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
    for (unsigned i = oldVectorLength; i < newVectorLength; ++i)
        grown->indexingPayload<uint64_t>()[i] = hole;
    grown->setVectorLength(newVectorLength);
    array->setButterfly(vm, grown);
    return true;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedAllocateStorage, void*, (Instance* instance, uint64_t size))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    return vm.auxiliarySpace().allocate(vm, size, nullptr, AllocationFailureMode::ReturnNull);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayGrow, size_t, (Instance* instance, JSCell* cell, uint32_t length))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    return growVector(vm, uncheckedDowncast<JSArray>(cell), length);
}

unsigned pushToFullVector(VM& vm, JSArray* array, JSValue value, unsigned expected)
{
    IndexingType mode = array->indexingMode();
    bool isTaken = mode == ArrayWithContiguous || (mode == ArrayWithInt32 ? value.isInt32() : mode == ArrayWithDouble && value.isNumber() && value.asNumber() == value.asNumber());
    if (!isTaken)
        return 0;
    unsigned length = array->butterfly()->publicLength();
    if (length != array->butterfly()->vectorLength() || !growVector(vm, array, length + 1, expected ? length + expected : 0))
        return 0;
    Butterfly* butterfly = array->butterfly();
    if (mode == ArrayWithDouble)
        butterfly->contiguousDouble().at(array, length) = value.asNumber();
    else
        butterfly->contiguous().at(array, length).set(vm, array, value);
    butterfly->setPublicLength(length + 1);
    return length + 1;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayEntries, JSCell*, (Instance* instance, JSCell* array))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSCellButterfly* record = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 2);
    record->setIndex(vm, 0, array);
    record->setIndex(vm, 1, jsNumber(Typed::CollectionIteration::iteratesArrayEntriesByIndex));
    OPERATION_RETURN(scope, record);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayJoin, JSCell*, (Instance* instance, JSCell* cell, JSCell* separator))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    auto view = (separator ? uncheckedDowncast<JSString>(separator) : vm.smallStrings.singleCharacterString(','))->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSOnlyStringsAndInt32sJoiner joiner(view);
    Butterfly& butterfly = *array->butterfly();
    unsigned length = butterfly.publicLength();
    switch (array->indexingType()) {
    case ALL_INT32_INDEXING_TYPES:
        OPERATION_RETURN(scope, joiner.tryJoin<Int32Shape>(globalObject, butterfly.contiguous().data(), length));
    case ALL_DOUBLE_INDEXING_TYPES:
        OPERATION_RETURN(scope, joiner.tryJoin(globalObject, butterfly.contiguousDouble().data(), length));
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        auto* elements = butterfly.contiguous().data();
        uint64_t units = 0;
        bool areAllStrings = length >= 2;
        for (unsigned i = 0; i < length && areAllStrings; ++i) {
            JSValue element = elements[i].get();
            areAllStrings = element && element.isString();
            if (areAllStrings)
                units += asString(element)->length();
        }
        if (areAllStrings && units >= 16384 && units / length >= 256) {
            JSString* between = separator ? uncheckedDowncast<JSString>(separator) : vm.smallStrings.singleCharacterString(',');
            JSRopeString::RopeBuilder<RecordOverflow> builder(vm);
            for (unsigned i = 0; i < length; ++i) {
                if (i)
                    builder.append(between);
                builder.append(asString(elements[i].get()));
            }
            if (!builder.hasOverflowed())
                OPERATION_RETURN(scope, builder.release());
        }
        OPERATION_RETURN(scope, joiner.tryJoin<ContiguousShape>(globalObject, elements, length));
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static JSValue arrayLiteralConstant(VM& vm, Typed::State& state, uint64_t encoded)
{
    JSValue value = JSValue::decode(encoded);
    if (!value.isCell())
        return value;
    if (encoded & Typed::Data::encodedStringIsSmall)
        return encoded >> 3 ? vm.smallStrings.singleCharacterString(static_cast<unsigned char>(encoded >> 3)) : vm.smallStrings.emptyString();
    uint32_t string = Typed::Data::decodeStringConstant(encoded);
    return encoded & Typed::Data::encodedStringIsCached ? state.createStringConstant(vm, string) : state.string(vm, string);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayBufferFromData, JSCell*, (Instance* instance, uint32_t offset, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto& shared = *reinterpret_cast<JSCellButterfly**>(instance->typedStatics + offset);
    IndexingType mode = indexingType | CopyOnWrite;
    if (!shared) {
        auto& data = instance->typed->data;
        auto [valuesOffset, count] = data.at<std::array<uint32_t, 2>>(data.arrayLiteralsOffset)[(offset - data.copyOnWriteButterfliesOffset) / sizeof(JSCell*)];
        const uint64_t* values = data.at<uint64_t>(valuesOffset);
        JSCellButterfly* elements = JSCellButterfly::tryCreate(vm, mode, count);
        if (!elements) {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, nullptr);
        }
        for (uint32_t i = 0; i < count; ++i)
            elements->setIndex(vm, i, arrayLiteralConstant(vm, *instance->typed, values[i]));
        vm.mutatorFence();
        shared = elements;
        instance->didStoreRootInStatics(offset);
    }
    OPERATION_RETURN(scope, CommonSlowPaths::allocateNewArrayBuffer(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(mode), shared));
}

struct ArraySite {
    static constexpr unsigned allocationCountShift = arraySiteAllocationCountShift;
    static constexpr uint32_t capacityMask = (1 << allocationCountShift) - 1;
    static constexpr uint32_t interval = arraySiteInterval;
    static constexpr uint8_t initialReviewsToWait = 16;
    static_assert(BASE_CONTIGUOUS_VECTOR_LEN_MAX <= capacityMask && interval <= UINT8_MAX);

    unsigned capacity() const { return capacityAndAllocationCount & capacityMask ? capacityAndAllocationCount & capacityMask : BASE_CONTIGUOUS_VECTOR_LEN_EMPTY; }
    unsigned allocationsSinceReview() const { return capacityAndAllocationCount >> allocationCountShift & (interval - 1); }
    void change(unsigned capacity)
    {
        capacityAndAllocationCount = capacity;
        outgrown = 0;
    }
    void goBack()
    {
        change(capacityBefore);
        capacityBefore = 0;
        reviewsToWait = initialReviewsToWait;
    }

    uint32_t capacityAndAllocationCount;
    uint8_t outgrown;
    uint8_t capacityBefore;
    uint8_t outgrownBefore;
    uint8_t reviewsToWait;
};
static_assert(sizeof(ArraySite) == 2 * sizeof(uint32_t) && !OBJECT_OFFSETOF(ArraySite, capacityAndAllocationCount));

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedUpdateVectorLengthHint, void, (Instance* instance, JSCell* cell, uint32_t offset))
{
    IndexingType type = cell->indexingType();
    if (!hasInt32(type) && !hasDouble(type) && !hasContiguous(type))
        return;
    auto& site = *reinterpret_cast<ArraySite*>(instance->typedStatics + offset);
    Butterfly* butterfly = uncheckedDowncast<JSArray>(cell)->butterfly();
    unsigned capacity = butterfly->vectorLength();
    if (butterfly->publicLength() != capacity || capacity != site.capacity() || capacity >= BASE_CONTIGUOUS_VECTOR_LEN_MAX || site.outgrown == UINT8_MAX)
        return;
    if (++site.outgrown < 2 || site.outgrown * 2u <= site.allocationsSinceReview())
        return;
    if (site.capacityBefore)
        site.goBack();
    else
        site.change(capacity <= 9 ? capacity + 2 : std::min<unsigned>((capacity * 5 / 4) | 1, BASE_CONTIGUOUS_VECTOR_LEN_MAX));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedReviewVectorLengthHint, void, (Instance* instance, uint32_t offset))
{
    auto& site = *reinterpret_cast<ArraySite*>(instance->typedStatics + offset);
    unsigned capacity = site.capacity();
    unsigned outgrown = std::exchange(site.outgrown, 0);
    if (site.capacityBefore) {
        if (outgrown > site.outgrownBefore + ArraySite::interval / 8) {
            site.goBack();
            return;
        }
        site.capacityBefore = 0;
    }
    if (site.reviewsToWait) {
        --site.reviewsToWait;
        return;
    }
    if (capacity == 1)
        return;
    site.capacityBefore = capacity;
    site.outgrownBefore = outgrown;
    site.change(capacity <= 9 ? capacity - 2 : (capacity * 3 / 4) | 1);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayIndexOfDouble, size_t, (Instance*, JSCell* array, double searched))
{
    Butterfly& butterfly = *uncheckedDowncast<JSArray>(array)->butterfly();
    const double* data = butterfly.contiguousDouble().data();
    const double* found = WTF::findDouble(data, searched, butterfly.publicLength());
    return found ? found - data : static_cast<size_t>(-1);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayIndexOfInt32, size_t, (Instance*, JSCell* array, EncodedJSValue searched))
{
    Butterfly& butterfly = *uncheckedDowncast<JSArray>(array)->butterfly();
    auto* data = std::bit_cast<const uint64_t*>(butterfly.contiguous().data());
    const uint64_t* found = WTF::find64(data, searched, butterfly.publicLength());
    return found ? found - data : static_cast<size_t>(-1);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayUnshift, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue item))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
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

static constexpr uint32_t valuesAreRawNumbers = 1u << 8;

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayWithIndexingType, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* values, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    bool areRaw = indexingType & valuesAreRawNumbers;
    indexingType &= ~valuesAreRawNumbers;
    auto valueAt = [&](uint32_t i) {
        if (!areRaw)
            return JSValue::decode(values[i]);
        uint64_t bits = static_cast<uint64_t>(values[i]);
        if (bits == Typed::nullDouble)
            return jsNull();
        if (bits == Typed::undefinedDouble || bits == Typed::absentDouble)
            return jsUndefined();
        return jsNumber(purifyNaN(std::bit_cast<double>(bits)));
    };
    for (uint32_t i = 0; i < count; ++i) {
        JSValue value = valueAt(i);
        bool fits = indexingType == ArrayWithContiguous || (indexingType == ArrayWithInt32 ? value.isInt32() : value.isNumber() && value.asNumber() == value.asNumber());
        if (!fits)
            indexingType = ArrayWithContiguous;
    }
    ObjectInitializationScope initializationScope(vm);
    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    IndexingType made = array->indexingType();
    for (uint32_t i = 0; i < count; ++i)
        array->initializeIndex(initializationScope, i, valueAt(i), made);
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayFromDoubles, JSCell*, (Instance* instance, uint32_t countAndNumberMask, double first, double second, double third, double fourth))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    double given[] = { first, second, third, fourth };
    unsigned count = countAndNumberMask & ((1u << arrayLiteralWithDoubleIsNumberMaskShift) - 1);
    bool fit = true;
    for (unsigned i = 0; i < count; ++i)
        fit &= given[i] == given[i];
    if (fit && !globalObject->isHavingABadTime()) {
        unsigned capacity = arrayLiteralVectorLength(count);
        void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, capacity * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
        if (!storage) {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, nullptr);
        }
        Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
        butterfly->setVectorLength(capacity);
        butterfly->setPublicLength(count);
        for (unsigned i = 0; i < capacity; ++i)
            butterfly->contiguousDouble().atUnsafe(i) = i < count ? given[i] : PNaN;
        OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(ArrayWithDouble), butterfly));
    }
    ObjectInitializationScope initializationScope(vm);
    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, globalObject->arrayStructureForIndexingTypeDuringAllocation(fit ? ArrayWithDouble : ArrayWithContiguous), count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    IndexingType made = array->indexingType();
    for (unsigned i = 0; i < count; ++i) {
        uint64_t bits = std::bit_cast<uint64_t>(given[i]);
        bool isNumber = countAndNumberMask >> (arrayLiteralWithDoubleIsNumberMaskShift + i) & 1;
        JSValue value = !isNumber && bits == Typed::nullDouble ? jsNull() : !isNumber && (bits == Typed::undefinedDouble || bits == Typed::absentDouble) ? jsUndefined() : jsNumber(purifyNaN(given[i]));
        array->initializeIndex(initializationScope, i, value, made);
    }
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewConstantArray, JSCell*, (Instance* instance, uint32_t literal, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto& data = instance->typed->data;
    auto [valuesOffset, count] = data.at<std::array<uint32_t, 2>>(data.arrayLiteralsOffset)[literal];
    const uint64_t* values = data.at<uint64_t>(valuesOffset);
    auto valueAt = [&](uint32_t i) {
        return arrayLiteralConstant(vm, *instance->typed, values[i]);
    };
    for (uint32_t i = 0; i < count; ++i) {
        JSValue value = valueAt(i);
        bool fits = indexingType == ArrayWithContiguous || (indexingType == ArrayWithInt32 ? value.isInt32() : value.isNumber() && value.asNumber() == value.asNumber());
        if (!fits)
            indexingType = ArrayWithContiguous;
    }
    ObjectInitializationScope initializationScope(vm);
    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    IndexingType made = array->indexingType();
    for (uint32_t i = 0; i < count; ++i)
        array->initializeIndex(initializationScope, i, valueAt(i), made);
    OPERATION_RETURN(scope, array);
}

namespace Typed {

using namespace B3;

static constexpr unsigned minLengthForOutOfLineSearch = 32;
static constexpr unsigned maxLengthForElementwiseCopy = 64;

void sinkEmptyArrayLiterals(Function& function)
{
    for (auto& block : function.blocks) {
        size_t end = block.values.size() - (block.terminator.kind == TerminatorKind::Invoke && !block.values.isEmpty());
        for (size_t i = end; i--;) {
            ValueID id = block.values[i];
            auto& value = function.at(id);
            if (value.opcode != Opcode::NewArray || !value.arguments.isEmpty())
                continue;
            size_t firstUse = i + 1;
            bool isUsed = false;
            for (; firstUse < end; ++firstUse) {
                auto& user = function.at(block.values[firstUse]);
                if ((user.opcode == Opcode::PutById && user.isInitialStore()) || user.opcode == Opcode::IteratorOpen || user.opcode == Opcode::NewObjectWithShape)
                    break;
                user.forEachChild([&](ValueID child) {
                    isUsed |= child == id;
                });
                if (isUsed)
                    break;
            }
            if (!isUsed || firstUse == i + 1)
                continue;
            block.values.removeAt(i);
            block.values.insert(firstUse - 1, id);
        }
    }
}

static bool isInt32Type(const Type& type)
{
    return type.isInteger() && type.integers.isSubsetOf(Range::int32());
}

Type Lowering::arrayTypeAtCreation(ValueID id) const
{
    if (TypeID prediction = m_function.at(id).prediction(); prediction && m_program.at(prediction).elements)
        return m_program.at(prediction).withoutPrimitives(Primitive::UndefinedOrNull);
    if (m_function.at(id).opcode == Opcode::NewArray && m_function.at(id).arguments.isEmpty()) {
        for (ValueID user : const_cast<Lowering*>(this)->usersOf(id)) {
            auto& spread = m_function.at(user);
            if (languageOperationOf(m_program, spread) == LanguageOperation::AppendSpread && spread.arguments.size() == 2 && spread.arguments[0] == id && spread.type && m_program.at(spread.type).elements)
                return m_program.at(spread.type).withoutPrimitives(Primitive::UndefinedOrNull);
        }
    }
    return typeOf(id);
}

IndexingType Lowering::indexingTypeAtCreation(const Type& array) const
{
    if (!array.elements)
        return ArrayWithContiguous;
    auto& elements = m_program.at(array.elements);
    if (isInt32Type(elements))
        return ArrayWithInt32;
    return elements.isNumber() ? ArrayWithDouble : ArrayWithContiguous;
}

int64_t Lowering::holeIn(IndexingType indexingType)
{
    return indexingType == ArrayWithDouble ? std::bit_cast<int64_t>(PNaN) : JSValue::encode(JSValue());
}

const AbstractHeap& Lowering::elementsOf(IndexingType indexingType)
{
    return indexingType == ArrayWithDouble ? m_heaps.indexedDoubleProperties.atAnyIndex() : m_heaps.indexedContiguousProperties.atAnyIndex();
}

LValue Lowering::allocateStorage(LValue size, LBasicBlock slowPath)
{
    LBasicBlock hasRunOut = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    orElse(m_out.notZero64(size), slowPath);
    LValue storage = allocateHeapCell(allocatorForSize(fixedPointer(Instance::offsetOfAuxiliarySpace()), size, slowPath), hasRunOut);
    applyEffects(storage->as<PatchpointValue>(), { Writes::RuntimeState, false });
    ValueFromBlock inlinePath = m_out.anchor(storage);
    m_out.jump(continuation);
    m_out.appendTo(hasRunOut);
    Pointer found = AOT_TYPED_OPERATION(AllocateStorage, Integer(size));
    orElse(m_out.notNull(found.value()), slowPath);
    ValueFromBlock outOfLine = m_out.anchor(found.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), inlinePath, outOfLine);
}

CellPointer Lowering::allocateArrayWithHoles(Integer length, IndexingType indexingType, LBasicBlock slowPath, bool isUnderConstruction)
{
    if (isUnderConstruction) {
        static_assert(static_cast<unsigned>(Stub::NewArrayUnderConstructionWithContiguous) + 1 == static_cast<unsigned>(Stub::LeafNewArrayUnderConstructionWithContiguous) && static_cast<unsigned>(Stub::NewArrayUnderConstructionWithDouble) + 1 == static_cast<unsigned>(Stub::LeafNewArrayUnderConstructionWithDouble) && static_cast<unsigned>(Stub::NewArrayUnderConstructionWithInt32) + 1 == static_cast<unsigned>(Stub::LeafNewArrayUnderConstructionWithInt32));
        Stub stub = indexingType == ArrayWithDouble ? Stub::NewArrayUnderConstructionWithDouble : indexingType == ArrayWithInt32 ? Stub::NewArrayUnderConstructionWithInt32 : Stub::NewArrayUnderConstructionWithContiguous;
#if CPU(ARM64)
        constexpr GPRReg lengthGPR = stubTemporaryGPRs[5];
#else
        constexpr GPRReg lengthGPR = GPRInfo::argumentGPR3;
#endif
        LValue array = callAllocationStub(stub, { { length.value(), lengthGPR } }, effectsForOperation<operationAOTTypedNewNumberArray>(), true);
        orElse(m_out.notZero64(array), slowPath);
        return CellPointer(array);
    }
    LBasicBlock fill = m_out.newBlock();
    LBasicBlock filled = m_out.newBlock();
    LValue header = changing64(Instance::offsetOfNewArrayHeader(indexingType));
    orElse(m_out.notZero64(header), slowPath);
    LValue vectorLength = m_out.bitOr(length.value(), m_out.constInt64(1));
    LValue size = m_out.add(m_out.shl(vectorLength, m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader)));
    LValue storage = allocateStorage(size, slowPath);
    LValue end = m_out.add(storage, size);
    LValue hole = m_out.constInt64(holeIn(indexingType));
    ValueFromBlock start = m_out.anchor(storage);
    m_out.jump(fill);

    m_out.appendTo(fill);
    LValue pair = m_out.phi(pointerType(), start);
    m_out.store64(hole, TypedPointer(elementsOf(indexingType), pair));
    m_out.store64(hole, TypedPointer(elementsOf(indexingType), m_out.add(pair, m_out.intPtrEight)));
    LValue next = m_out.add(pair, m_out.constIntPtr(2 * sizeof(EncodedJSValue)));
    m_out.addIncomingToPhi(pair, m_out.anchor(next));
    m_out.branch(m_out.below(next, end), unsure(fill), unsure(filled));

    m_out.appendTo(filled);
    LValue butterfly = m_out.add(storage, m_out.constIntPtr(sizeof(IndexingHeader)));
    m_out.store32(m_out.castToInt32(length.value()), butterfly, m_heaps.Butterfly_publicLength);
    m_out.store32(m_out.castToInt32(vectorLength), butterfly, m_heaps.Butterfly_vectorLength);
    LValue array = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), slowPath);
    applyEffects(array->as<PatchpointValue>(), { Writes::RuntimeState, false });
    storeHeader(array, header);
    m_out.storePtr(butterfly, array, m_heaps.JSObject_butterfly);
    fenceAfterInitialization();
    return CellPointer(array);
}

IndexingType Lowering::literalIndexingType(const Arguments& elements, const Type& type) const
{
    IndexingType indexingType = indexingTypeAtCreation(type);
    for (ValueID element : elements) {
        if (indexingType == ArrayWithInt32 ? !isInt32Type(typeOf(element)) : indexingType == ArrayWithDouble && !typeOf(element).isNumber())
            return ArrayWithContiguous;
    }
    return indexingType;
}

LValue Lowering::allocateNumberArray(IndexingType indexingType, LValue lengthAndCapacity, LBasicBlock slowPath)
{
    static_assert(static_cast<unsigned>(Stub::NewArrayWithDouble) + 1 == static_cast<unsigned>(Stub::LeafNewArrayWithDouble) && static_cast<unsigned>(Stub::NewArrayWithInt32) + 1 == static_cast<unsigned>(Stub::LeafNewArrayWithInt32));
    static_assert(static_cast<unsigned>(Stub::NewEmptyArrayWithDouble) + 1 == static_cast<unsigned>(Stub::LeafNewEmptyArrayWithDouble) && static_cast<unsigned>(Stub::NewEmptyArrayWithInt32) + 1 == static_cast<unsigned>(Stub::LeafNewEmptyArrayWithInt32));
    RELEASE_ASSERT(indexingType == ArrayWithDouble || indexingType == ArrayWithInt32);
    bool areDoubles = indexingType == ArrayWithDouble;
    OperationEffects effects = effectsForOperation<operationAOTTypedNewNumberArray>();
    LValue array = lengthAndCapacity
        ? callAllocationStub(areDoubles ? Stub::NewArrayWithDouble : Stub::NewArrayWithInt32, { { lengthAndCapacity, stubTemporaryGPRs[0] } }, effects, true)
        : callAllocationStub(areDoubles ? Stub::NewEmptyArrayWithDouble : Stub::NewEmptyArrayWithInt32, { }, effects, true);
    if (lengthAndCapacity || !areDoubles)
        orElse(m_out.notZero64(array), slowPath);
    return array;
}

CellPointer Lowering::newArrayWithType(const Arguments& elements, const Type& type)
{
    constexpr unsigned maxInlineLength = Program::maxInlineArrayLiteralLength;
    unsigned length = elements.size();
    IndexingType indexingType = literalIndexingType(elements, type);
    bool areNumbersOrMarkers = indexingTypeAtCreation(type) == ArrayWithDouble && std::ranges::all_of(elements, [&](ValueID element) {
        return typeOf(element).isNumber() || get(element).rep == Rep::OptionalDouble;
    });
    if (areNumbersOrMarkers)
        indexingType = ArrayWithDouble;
    if (indexingType == ArrayWithContiguous && length <= maxInlineLength)
        return newArray(elements);
    auto numberOrMarker = [&](ValueID element) {
        Operand operand = get(element);
        return operand.rep == Rep::OptionalDouble ? assumedNumber(operand.as<OptionalNumber>()) : in<Number>(element);
    };
    auto outOfLine = [&] {
        if (indexingType == ArrayWithDouble && length) {
            Vector<LValue, 16> raw;
            for (ValueID element : elements)
                raw.append((get(element).rep == Rep::OptionalDouble || typeOf(element).isInteger() ? numberOrMarker(element) : purified(numberOrMarker(element))).value());
            LValue slot = transientBuffer(length * sizeof(EncodedJSValue));
            for (unsigned i = 0; i < length; ++i)
                m_out.storeDouble(raw[i], m_out.address(m_heaps.AOTTyped_argumentBuffer, slot, i * sizeof(EncodedJSValue)));
            return AOT_TYPED_OPERATION(NewArrayWithIndexingType, word32(length), Pointer(slot), word32(indexingType | valuesAreRawNumbers));
        }
        auto [count, list] = argumentBuffer(elements);
        return AOT_TYPED_OPERATION(NewArrayWithIndexingType, count, list, word32(indexingType));
    };
    if (length > maxInlineLength)
        return outOfLine();
    if (indexingType == ArrayWithDouble && length && length <= maxArrayElementsInRegisters) {
        Vector<LValue, 4> numbers;
        uint32_t countAndNumberMask = length;
        for (unsigned i = 0; i < length; ++i) {
            numbers.append(numberOrMarker(elements[i]).value());
            countAndNumberMask |= static_cast<uint32_t>(get(elements[i]).rep != Rep::OptionalDouble) << (arrayLiteralWithDoubleIsNumberMaskShift + i);
        }
        return CellPointer(callAllocationStub(doubleArrayLiteralStub(length), { { m_out.constInt64(countAndNumberMask), stubTemporaryGPRs[4] } }, effectsForOperation<operationAOTTypedNewArrayFromDoubles>(), true, false, false, numbers[0], std::nullopt, Stub::TypedString, 0, numbers.span().subspan(1)));
    }
    if (indexingType == ArrayWithDouble && !length)
        return CellPointer(allocateNumberArray(indexingType, nullptr, nullptr));

    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<LValue, 16> stored;
    for (ValueID element : elements) {
        if (indexingType == ArrayWithInt32) {
            stored.append(boxInt32(low32(in<Integer>(element))).value());
            continue;
        }
        Number number = numberOrMarker(element);
        if (!typeOf(element).isInteger())
            orElse(equal(number, number), slowPath);
        stored.append(number.value());
    }
    LValue array = allocateNumberArray(indexingType, length ? m_out.constInt64(length | arrayLiteralVectorLength(length) << numberArrayCapacityShift) : nullptr, slowPath);
    if (length) {
        LValue butterfly = m_out.loadPtr(array, m_heaps.JSObject_butterfly);
        for (unsigned i = 0; i < length; ++i) {
            if (indexingType == ArrayWithDouble)
                m_out.storeDouble(stored[i], butterfly, m_heaps.indexedDoubleProperties[i]);
            else
                m_out.store64(stored[i], butterfly, m_heaps.indexedContiguousProperties[i]);
        }
    }
    ValueFromBlock fast = m_out.anchor(array);
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(outOfLine().value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

bool Lowering::isAvailableAtCurrentValue(ValueID id) const
{
    BlockID block = m_definingBlock[id.index()];
    if (!block)
        return false;
    if (block != m_currentBlockID)
        return dominates(block, m_currentBlockID);
    if (m_currentBlock->parameters.contains(id))
        return true;
    for (size_t i = 0; i < m_currentIndexInBlock; ++i) {
        if (m_currentBlock->values[i] == id)
            return true;
    }
    return false;
}

std::optional<Lowering::CountedLoop> Lowering::countedLoopContaining(BlockID block) const
{
    unsigned index = m_innermostLoop[block.index()];
    if (index == noLoop)
        return std::nullopt;
    auto& loop = m_loops[index];
    auto& header = m_function.at(loop.header);
    if (header.terminator.kind != TerminatorKind::Branch || m_definingBlock[header.terminator.value.index()] != loop.header)
        return std::nullopt;
    auto& test = m_function.at(header.terminator.value);
    if (!loop.blocks.get(header.terminator.targets[0].block.index()) || loop.blocks.get(header.terminator.targets[1].block.index()))
        return std::nullopt;
    CountedLoop result;
    result.index = index;
    if (test.opcode == Opcode::Less) {
        result.counter = test.child(0);
        result.bound = test.child(1);
        if (!typeOf(result.bound).isInteger())
            return std::nullopt;
    } else if (test.opcode == Opcode::IteratorHasNext && keyedAccessOf(m_program, m_function, test) == KeyedAccess::Array) {
        result.counter = test.child(1);
        result.iteratedArray = m_function.at(test.child(0)).child(0);
    } else
        return std::nullopt;
    result.position = header.parameters.find(result.counter);
    if (result.position == notFound || !typeOf(result.counter).isInteger())
        return std::nullopt;
    for (size_t blockIndex : loop.blocks) {
        auto& terminator = m_function.blocks[blockIndex].terminator;
        if (terminator.kind == TerminatorKind::Return)
            return std::nullopt;
        for (auto& target : successorsOf(terminator)) {
            if (terminator.kind == TerminatorKind::Invoke && &target == &terminator.targets.last())
                continue;
            if (target.block != loop.header) {
                if (!loop.blocks.get(target.block.index()) && BlockID(blockIndex) != loop.header)
                    return std::nullopt;
                continue;
            }
            auto& next = m_function.at(originOf(target.arguments[result.position]));
            ValueID stepped = next.opcode == Opcode::Add ? originOf(next.child(0)) : ValueID();
            if (stepped && m_function.at(stepped).opcode == Opcode::ToNumeric)
                stepped = originOf(m_function.at(stepped).child(0));
            if (stepped != result.counter || m_function.at(next.child(1)).opcode != Opcode::Integer || m_function.at(next.child(1)).integer() != 1)
                return std::nullopt;
            if (!dominates(block, BlockID(blockIndex)))
                return std::nullopt;
        }
    }
    return result;
}

Integer Lowering::arrayLengthOrBail(ValueID array, LBasicBlock otherwise)
{
    Operand operand = get(array);
    if (operand.rep == Rep::Cell && !typeOf(array).mayBeUndefinedOrNull())
        return arrayLength(operand.as<CellPointer>());
    Boxed boxed = convertTo<Boxed>(operand, typeOf(array));
    orElse(isCell(boxed, typeOf(array)), otherwise);
    return arrayLength(asCell(boxed));
}

std::optional<Lowering::InitialCapacity> Lowering::inferInitialCapacity(ValueID literal)
{
    auto isAvailableArray = [&](ValueID id) {
        auto* engineClass = typeOf(id).singleEngineClass();
        return engineClass && engineClass->type == ArrayType && isAvailableAtCurrentValue(id) && get(id).value;
    };
    auto isAppendSpread = [&](const Value& value) {
        auto isOperation = [&](const Value& call) {
            if (call.opcode != Opcode::CallBuiltin)
                return false;
            auto& implementation = m_builtins.at(call.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::AppendSpread;
        };
        if (isOperation(value))
            return true;
        if (value.opcode != Opcode::CallDirect || value.arguments.size() != 2)
            return false;
        auto& body = m_program.at(value.function());
        return body.blocks.size() == 1 && body.blocks[0].parameters.size() == 2 && body.blocks[0].values.size() == 1 && isOperation(body.at(body.blocks[0].values[0]));
    };
    if (!std::exchange(m_didFindArrayPushes, true)) {
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (ValueID id : m_function.blocks[b].values) {
                auto& value = m_function.at(id);
                if (isAppendSpread(value))
                    m_blocksWithAppendSpread.set(b);
                if (isInlineBuiltin(id, InlineBuiltin::ArrayPush))
                    m_arrayPushesByReceiver.add(originOf(value.child(0)).index(), Vector<std::pair<BlockID, unsigned>, 1> { }).iterator->value.append({ BlockID(b), static_cast<unsigned>(value.arguments.size()) });
                if (value.opcode == Opcode::PutByVal)
                    m_arrayStoresByReceiver.add(originOf(value.child(0)).index(), Vector<std::pair<BlockID, ValueID>, 1> { }).iterator->value.append({ BlockID(b), value.child(1) });
            }
        }
    }
    InitialCapacity room;
    ValueID array = literal;
    for (size_t i = m_currentIndexInBlock + 1; i < m_currentBlock->values.size() && m_blocksWithAppendSpread.get(m_currentBlockID.index()); ++i) {
        ValueID id = m_currentBlock->values[i];
        auto& value = m_function.at(id);
        if (!isAppendSpread(value) || originOf(value.arguments[0]) != array)
            continue;
        if (!isAvailableArray(value.arguments[1]))
            return std::nullopt;
        room.arrays.append(value.arguments[1]);
        room.constant += numberOfElementsPushedTo(id);
        array = id;
    }
    if (array != literal)
        return room;

    Vector<BlockID, 4> pushBlocks;
    unsigned pushedCount = 0;
    if (auto pushes = m_arrayPushesByReceiver.find(literal.index()); pushes != m_arrayPushesByReceiver.end()) {
        for (auto [block, numberOfElements] : pushes->value) {
            pushBlocks.append(block);
            pushedCount += numberOfElements;
        }
    }
    ValueID keyStoredAt;
    if (auto stores = m_arrayStoresByReceiver.find(literal.index()); pushBlocks.isEmpty() && stores != m_arrayStoresByReceiver.end() && stores->value.size() == 1) {
        pushBlocks.append(stores->value[0].first);
        pushedCount = 1;
        keyStoredAt = originOf(stores->value[0].second);
    }
    if (pushBlocks.isEmpty())
        return std::nullopt;
    bool isPushedInLoop = pushBlocks.containsIf([&](BlockID block) {
        return m_innermostLoop[block.index()] != m_innermostLoop[m_currentBlockID.index()];
    });
    if (!isPushedInLoop) {
        if (keyStoredAt)
            return std::nullopt;
        bool isUnconditional = !pushBlocks.containsIf([&](BlockID block) {
            return block != m_currentBlockID;
        });
        if (isUnconditional ? pushedCount > maxNumberArrayCapacity : pushedCount > 1)
            return std::nullopt;
        room.constant = pushedCount;
        return room;
    }
    if (pushBlocks.size() != 1 || pushedCount != 1)
        return std::nullopt;
    auto counted = countedLoopContaining(pushBlocks[0]);
    if (!counted)
        return std::nullopt;
    bool isStoredAtCounter = keyStoredAt && keyStoredAt == originOf(counted->counter);
    if (keyStoredAt && !isStoredAtCounter && !(isInlineBuiltin(keyStoredAt, InlineBuiltin::ArrayLength) && originOf(m_function.at(keyStoredAt).child(0)) == literal))
        return std::nullopt;
    auto& loop = m_loops[counted->index];
    if (loop.blocks.get(m_currentBlockID.index()) || !dominates(m_currentBlockID, loop.header))
        return std::nullopt;
    for (auto& other : m_loops) {
        if (&other != &loop && other.blocks.get(loop.header.index()) && !other.blocks.get(m_currentBlockID.index()))
            return std::nullopt;
    }
    std::optional<int64_t> start;
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        if (loop.blocks.get(b))
            continue;
        for (auto& target : successorsOf(m_function.blocks[b].terminator)) {
            if (target.block != loop.header)
                continue;
            ValueID first = m_program.constantOf(m_function, target.arguments[counted->position]);
            if (!first || m_function.at(first).opcode != Opcode::Integer || (start && *start != m_function.at(first).integer()))
                return std::nullopt;
            start = m_function.at(first).integer();
        }
    }
    if (!start || *start < 0)
        return std::nullopt;
    room.constant = isStoredAtCounter ? 0 : -*start;
    ValueID iteratedArray = counted->iteratedArray;
    if (!iteratedArray) {
        ValueID bound = originOf(counted->bound);
        if (isInlineBuiltin(bound, InlineBuiltin::ArrayLength))
            iteratedArray = m_function.at(bound).child(0);
        else if (Range range = typeOf(bound).integers; range.minimum == range.maximum && range.minimum >= 0 && range.minimum <= maxNumberArrayCapacity) {
            room.constant += range.minimum;
            return room;
        } else if (isAvailableAtCurrentValue(bound) && get(bound).value && get(bound).rep == Rep::Int64) {
            room.count = bound;
            return room;
        } else
            return std::nullopt;
    }
    if (!isAvailableArray(iteratedArray))
        return std::nullopt;
    room.arrays.append(iteratedArray);
    return room;
}

CellPointer Lowering::newArrayLiteral(ValueID id)
{
    std::optional<InitialCapacity> inferred;
    if (m_function.at(id).arguments.isEmpty() && m_function.at(id).copyOnWriteButterflyOffset() == Format::none)
        inferred = inferInitialCapacity(id);
    if (!inferred)
        return newArrayLiteralWithAllocationSite(id);
    auto isAppendSpread = [&](const Value& value) {
        if (value.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = m_builtins.at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::AppendSpread;
    };
    static_assert(static_cast<unsigned>(Stub::NewEmptyArrayWithCapacity) + 1 == static_cast<unsigned>(Stub::LeafNewEmptyArrayWithCapacity));
    static_assert(static_cast<unsigned>(Stub::NewEmptyArrayCapacity1) + 1 == static_cast<unsigned>(Stub::LeafNewEmptyArrayCapacity1));
    static_assert(static_cast<unsigned>(Stub::NewEmptyArrayCapacity3) + 1 == static_cast<unsigned>(Stub::LeafNewEmptyArrayCapacity3));
    IndexingType indexingType = indexingTypeAtCreation(arrayTypeAtCreation(id));
    unsigned constantCapacity = !inferred->count && inferred->arrays.isEmpty() ? inferred->constant | 1 : 0;
    if (constantCapacity == arrayLiteralVectorLength(0))
        return newArrayWithType(m_function.at(id).arguments, arrayTypeAtCreation(id));
    if (indexingType == ArrayWithContiguous && (constantCapacity == 1 || constantCapacity == 3))
        return CellPointer(callAllocationStub(constantCapacity == 1 ? Stub::NewEmptyArrayCapacity1 : Stub::NewEmptyArrayCapacity3, { }, effectsForOperation<operationAOTTypedNewArray>(), true));
    if (indexingType == ArrayWithDouble && (constantCapacity == 1 || constantCapacity == 3))
        return CellPointer(callAllocationStub(constantCapacity == 1 ? Stub::NewEmptyArrayWithDoubleCapacity1 : Stub::NewEmptyArrayWithDoubleCapacity3, { }, effectsForOperation<operationAOTTypedNewNumberArray>(), true));
    LBasicBlock fallback = newUnlikelyBlock();
    LBasicBlock continuation = m_out.newBlock();
    unsigned numberOfSpreadsAtCreation = 0;
    if ((indexingType == ArrayWithContiguous || indexingType == ArrayWithDouble) && !inferred->count) {
        ValueID target = id;
        for (size_t next = m_currentIndexInBlock + 1; next < m_currentBlock->values.size() && numberOfSpreadsAtCreation < inferred->arrays.size(); ++next) {
            auto& spread = m_function.at(m_currentBlock->values[next]);
            if (!isAppendSpread(spread) || originOf(spread.arguments[0]) != target || spread.arguments[1] != inferred->arrays[numberOfSpreadsAtCreation])
                break;
            target = m_currentBlock->values[next];
            ++numberOfSpreadsAtCreation;
            if (indexingType == ArrayWithDouble)
                break;
        }
    }
    if (numberOfSpreadsAtCreation) {
        static_assert(static_cast<unsigned>(Stub::NewArrayWithContiguous) + 1 == static_cast<unsigned>(Stub::LeafNewArrayWithContiguous));
        Vector<Integer, 4> counts;
        Integer length = integer(0);
        Integer room = integer(inferred->constant);
        for (size_t i = 0; i < inferred->arrays.size(); ++i) {
            Integer count = arrayLengthOrBail(inferred->arrays[i], fallback);
            room = add(room, count);
            if (i >= numberOfSpreadsAtCreation)
                continue;
            Operand operand = get(inferred->arrays[i]);
            Word32 shape = indexingModeOf(operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>()), IndexingShapeMask);
            if (indexingType == ArrayWithDouble)
                orElse(equal(shape, word32(DoubleShape)), fallback);
            else
                orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), fallback);
            orElse(belowOrEqual(count, integer(maxLengthForElementwiseCopy)), fallback);
            counts.append(count);
            length = i ? add(length, count) : count;
        }
        orElse(belowOrEqual(room, integer(maxNumberArrayCapacity)), fallback);
        LValue lengthAndCapacity = m_out.bitOr(m_out.shl(m_out.bitOr(room.value(), m_out.constInt64(1)), m_out.constInt32(numberArrayCapacityShift)), length.value());
        LValue made;
        if (indexingType == ArrayWithDouble)
            made = allocateNumberArray(indexingType, lengthAndCapacity, fallback);
        else {
            LBasicBlock isShort = m_out.newBlock();
            LBasicBlock isLong = m_out.newBlock();
            LBasicBlock isMade = m_out.newBlock();
            m_out.branch(belowOrEqual(room, integer(arrayLiteralVectorLength(0))).value(), unsure(isShort), unsure(isLong));
            m_out.appendTo(isShort);
            LValue shortArray = callAllocationStub(Stub::NewEmptyArrayWithCapacity, { { room.value(), stubTemporaryGPRs[0] } }, effectsForOperation<operationAOTTypedNewArray>(), true);
            m_out.store32(m_out.castToInt32(length.value()), butterflyOf(CellPointer(shortArray)).value(), m_heaps.Butterfly_publicLength);
            ValueFromBlock shortArrayValue = m_out.anchor(shortArray);
            m_out.jump(isMade);
            m_out.appendTo(isLong);
            LValue longArray = callAllocationStub(Stub::NewArrayWithContiguous, { { lengthAndCapacity, stubTemporaryGPRs[0] } }, effectsForOperation<operationAOTTypedNewNumberArray>(), true);
            orElse(m_out.notZero64(longArray), fallback);
            ValueFromBlock longArrayValue = m_out.anchor(longArray);
            m_out.jump(isMade);
            m_out.appendTo(isMade);
            made = m_out.phi(Int64, shortArrayValue, longArrayValue);
        }
        m_isFillingArrayAtCreation = true;
        auto& elements = indexingType == ArrayWithDouble ? m_heaps.indexedDoubleProperties : m_heaps.indexedContiguousProperties;
        LValue to = butterflyOf(CellPointer(made)).value();
        for (unsigned i = 0; i < numberOfSpreadsAtCreation; ++i) {
            Operand operand = get(inferred->arrays[i]);
            CellPointer source = operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>());
            Pointer from = butterflyOf(source);
            LBasicBlock copyLoop = m_out.newBlock();
            LBasicBlock copy = m_out.newBlock();
            LBasicBlock copied = m_out.newBlock();
            ValueFromBlock firstCopied = m_out.anchor(m_out.intPtrZero);
            m_out.jump(copyLoop);
            m_out.appendTo(copyLoop);
            LValue index = m_out.phi(pointerType(), firstCopied);
            m_out.branch(m_out.below(index, counts[i].value()), unsure(copy), unsure(copied));
            m_out.appendTo(copy);
            LValue element = m_out.load64(m_out.baseIndex(elements, from.value(), index));
            if (indexingType == ArrayWithDouble)
                orElse(Word32(m_out.notEqual(element, m_out.constInt64(holeIn(ArrayWithDouble)))), fallback);
            else
                element = m_out.select(m_out.isZero64(element), undefined().value(), element);
            m_out.store64(element, m_out.baseIndex(elements, to, index));
            m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
            m_out.jump(copyLoop);
            m_out.appendTo(copied);
            ensureStillAliveHere(source);
            if (i + 1 < numberOfSpreadsAtCreation)
                to = m_out.add(to, m_out.shl(counts[i].value(), m_out.constInt32(3)));
        }
        if (indexingType != ArrayWithDouble)
            fenceAfterInitialization();
        m_isFillingArrayAtCreation = false;
        ValueFromBlock sized = m_out.anchor(made);
        ValueFromBlock isSized = m_out.anchor(m_out.int32One);
        m_out.jump(continuation);
        m_out.appendTo(fallback);
        ValueFromBlock usual = m_out.anchor(newArrayWithType(m_function.at(id).arguments, arrayTypeAtCreation(id)).value());
        ValueFromBlock defaultCase = m_out.anchor(m_out.int32Zero);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        LValue isPresizedForSpreads = m_out.phi(Int32, isSized, defaultCase);
        for (unsigned i = 0; i < numberOfSpreadsAtCreation; ++i)
            m_spreadsDoneAtCreation.add(m_currentBlock->values[m_currentIndexInBlock + 1 + i].index(), isPresizedForSpreads);
        return CellPointer(m_out.phi(Int64, sized, usual));
    }
    Integer room = inferred->count ? add(in<Integer>(inferred->count), integer(inferred->constant)) : integer(inferred->constant);
    for (ValueID array : inferred->arrays)
        room = add(room, arrayLengthOrBail(array, fallback));
    LValue made;
    if (indexingType == ArrayWithContiguous)
        made = callAllocationStub(Stub::NewEmptyArrayWithCapacity, { { room.value(), stubTemporaryGPRs[0] } }, effectsForOperation<operationAOTTypedNewArray>(), true);
    else {
        orElse(belowOrEqual(room, integer(maxNumberArrayCapacity)), fallback);
        made = allocateNumberArray(indexingType, m_out.shl(m_out.bitOr(room.value(), m_out.constInt64(1)), m_out.constInt32(numberArrayCapacityShift)), fallback);
    }
    ValueFromBlock sized = m_out.anchor(made);
    m_out.jump(continuation);
    m_out.appendTo(fallback);
    ValueFromBlock usual = m_out.anchor(newArrayWithType(m_function.at(id).arguments, arrayTypeAtCreation(id)).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, sized, usual));
}

CellPointer Lowering::newArrayLiteralWithAllocationSite(ValueID id)
{
    auto& value = m_function.at(id);
    Type type = arrayTypeAtCreation(id);
    LValue statics = staticsBase();
    if (value.copyOnWriteButterflyOffset() != Format::none) {
        IndexingType indexingType = literalIndexingType(value.arguments, type);
        for (ValueID element : value.arguments) {
            if (indexingType == ArrayWithDouble && m_function.at(element).opcode == Opcode::Double && std::isnan(std::bit_cast<double>(m_function.at(element).bits())))
                indexingType = ArrayWithContiguous;
        }
        if (m_program.isStaticallyRare(m_id) && !m_isInLoop)
            return AOT_TYPED_OPERATION(NewArrayBufferFromData, word32(value.copyOnWriteButterflyOffset()), word32(indexingType));
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue shared = m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, statics, value.copyOnWriteButterflyOffset()));
        orElse(m_out.notZero64(shared), slowPath);
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        LValue header = changing64(Instance::offsetOfNewCopyOnWriteArrayHeader(indexingType));
        orElse(m_out.notZero64(header), slowPath);
        LValue array = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), slowPath);
        applyEffects(array->as<PatchpointValue>(), { Writes::RuntimeState, false });
        storeHeader(array, header);
        m_out.storePtr(m_out.add(shared, m_out.constIntPtr(JSCellButterfly::offsetOfData())), array, m_heaps.JSObject_butterfly);
        fenceAfterInitialization();
        ValueFromBlock fast = m_out.anchor(array);
        m_out.jump(continuation);
        m_out.appendTo(slowPath);
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArrayBufferFromData, word32(value.copyOnWriteButterflyOffset()), word32(indexingType)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return CellPointer(m_out.phi(Int64, fast, slow));
    }
    if (value.unsharedConstantLiteral() != Format::none)
        return AOT_TYPED_OPERATION(NewConstantArray, word32(value.unsharedConstantLiteral()), word32(literalIndexingType(value.arguments, type)));
    if (value.vectorLengthHintOffset() == Format::none)
        return newArrayWithType(value.arguments, type);
    static_assert(static_cast<unsigned>(Stub::NewEmptyArrayAtSite) + 1 == static_cast<unsigned>(Stub::LeafNewEmptyArrayAtSite));
    bool hasStubs = indexingTypeAtCreation(type) == ArrayWithContiguous;
    auto entries = maxPushesInCollectionLoop(id);
    if (hasStubs && !entries) {
        ++m_context.numberOfStubEmptyArrayLiterals;
        return CellPointer(callAllocationStub(Stub::NewEmptyArrayAtSite, { { m_out.constInt64(value.vectorLengthHintOffset()), stubTemporaryGPRs[0] } }, effectsForOperation<operationAOTTypedNewArray>(), true));
    }
    m_context.numberOfInlineEmptyArrayLiterals += hasStubs;

    LBasicBlock isDueForReview = newColdBlock();
    LBasicBlock isCounted = m_out.newBlock();
    LBasicBlock hasOwnCapacity = m_out.newBlock();
    LBasicBlock defaultPath = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    TypedPointer site = m_out.address(m_heaps.AOTTyped_collectorState, statics, value.vectorLengthHintOffset() + OBJECT_OFFSETOF(ArraySite, capacityAndAllocationCount));
    LValue counted = m_out.add(m_out.load32(site), m_out.constInt32(1 << ArraySite::allocationCountShift));
    m_out.store32(counted, site);
    ValueFromBlock unchangedCount = m_out.anchor(counted);
    m_out.branch(m_out.testIsZero32(counted, m_out.constInt32((ArraySite::interval - 1) << ArraySite::allocationCountShift)), rarely(isDueForReview), usually(isCounted));
    m_out.appendTo(isDueForReview);
    AOT_TYPED_OPERATION(ReviewVectorLengthHint, word32(value.vectorLengthHintOffset()));
    ValueFromBlock reviewed = m_out.anchor(m_out.load32(site));
    m_out.jump(isCounted);
    m_out.appendTo(isCounted);
    LValue capacity = m_out.bitAnd(m_out.phi(Int32, unchangedCount, reviewed), m_out.constInt32(ArraySite::capacityMask));
    if (entries) {
        LValue room = m_out.select(m_out.isZero32(capacity), m_out.constInt32(arrayLiteralVectorLength(0)), capacity);
        LValue bound = m_out.bitOr(entries->value(), m_out.int32One);
        capacity = m_out.select(m_out.below(bound, room), bound, room);
    }
    Vector<ValueFromBlock, 2> smallArrays;
    if (hasStubs) {
        for (auto [room, stub] : { std::pair { 1, Stub::NewEmptyArrayCapacity1 }, std::pair { 3, Stub::NewEmptyArrayCapacity3 } }) {
            LBasicBlock matchesCapacity = m_out.newBlock();
            LBasicBlock isNot = m_out.newBlock();
            m_out.branch(m_out.equal(capacity, m_out.constInt32(room)), unsure(matchesCapacity), unsure(isNot));
            m_out.appendTo(matchesCapacity);
            smallArrays.append(m_out.anchor(callAllocationStub(stub, { }, effectsForOperation<operationAOTTypedNewArray>(), true)));
            m_out.jump(continuation);
            m_out.appendTo(isNot);
        }
    } else if (indexingTypeAtCreation(type) == ArrayWithDouble) {
        static_assert(static_cast<unsigned>(Stub::NewEmptyArrayWithDoubleCapacity1) + 1 == static_cast<unsigned>(Stub::LeafNewEmptyArrayWithDoubleCapacity1));
        static_assert(static_cast<unsigned>(Stub::NewEmptyArrayWithDoubleCapacity3) + 1 == static_cast<unsigned>(Stub::LeafNewEmptyArrayWithDoubleCapacity3));
        for (auto [room, stub] : { std::pair { 1, Stub::NewEmptyArrayWithDoubleCapacity1 }, std::pair { 3, Stub::NewEmptyArrayWithDoubleCapacity3 } }) {
            LBasicBlock matchesCapacity = m_out.newBlock();
            LBasicBlock isNot = m_out.newBlock();
            m_out.branch(m_out.equal(capacity, m_out.constInt32(room)), unsure(matchesCapacity), unsure(isNot));
            m_out.appendTo(matchesCapacity);
            LValue made = callAllocationStub(stub, { }, effectsForOperation<operationAOTTypedNewNumberArray>(), true);
            smallArrays.append(m_out.anchor(made));
            m_out.jump(continuation);
            m_out.appendTo(isNot);
        }
    }
    static_assert(arrayLiteralVectorLength(0) == 5);
    m_out.branch(hasStubs ? m_out.above(capacity, m_out.constInt32(arrayLiteralVectorLength(0))) : m_out.notZero32(capacity), unsure(hasOwnCapacity), unsure(defaultPath));
    m_out.appendTo(hasOwnCapacity);
    ValueFromBlock hinted = [&] {
        if (!hasStubs)
            return m_out.anchor(allocateNumberArray(indexingTypeAtCreation(type), m_out.shl(m_out.zeroExtPtr(m_out.bitOr(capacity, m_out.int32One)), m_out.constInt32(numberArrayCapacityShift)), defaultPath));
        CellPointer presizedArray = allocateArrayWithHoles(Integer(m_out.zeroExtPtr(capacity)), ArrayWithContiguous, defaultPath);
        storeField(butterflyOf(presizedArray), m_heaps.Butterfly_publicLength, word32(0));
        return m_out.anchor(presizedArray.value());
    }();
    m_out.jump(continuation);
    m_out.appendTo(defaultPath);
    ValueFromBlock usual = m_out.anchor(newArrayWithType(value.arguments, type).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    LValue made = m_out.phi(Int64, hinted, usual);
    for (ValueFromBlock& one : smallArrays)
        m_out.addIncomingToPhi(made, one);
    return CellPointer(made);
}

ValueID Lowering::emptyLiteralSourceOf(ValueID receiver)
{
    ValueID origin = originOf(receiver);
    if (m_function.at(origin).opcode == Opcode::NewArray)
        return origin;
    BlockID meeting = m_definingBlock[origin.index()];
    size_t position = m_function.at(meeting).parameters.find(origin);
    if (position == notFound)
        return ValueID();
    return m_emptyLiteralSourcesOfParameters.ensure(origin.index(), [&] {
        ValueID found;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                if (target.block != meeting || position >= target.arguments.size())
                    continue;
                ValueID incoming = originOf(target.arguments[position]);
                if (m_function.at(incoming).opcode != Opcode::NewArray || incoming == found)
                    continue;
                if (found)
                    return ValueID();
                found = incoming;
            }
        }
        return found;
    }).iterator->value;
}

void Lowering::updateVectorLengthHint(CellPointer array, ValueID receiver)
{
    ValueID literal = emptyLiteralSourceOf(receiver);
    if (!literal || m_function.at(literal).vectorLengthHintOffset() == Format::none)
        return;
    LBasicBlock isFull = newColdBlock();
    LBasicBlock continuation = newColdBlock();
    Pointer butterfly = butterflyOf(array);
    m_out.branch(aboveOrEqual(publicLength(butterfly), vectorLength(butterfly)).value(), unsure(isFull), unsure(continuation));
    m_out.appendTo(isFull);
    AOT_TYPED_OPERATION(UpdateVectorLengthHint, array, word32(m_function.at(literal).vectorLengthHintOffset()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

Word32 Lowering::remainingLoopIterations()
{
    static constexpr int64_t maxTurns = 1 << 20;
    auto counted = countedLoopContaining(m_currentBlockID);
    if (!counted)
        return word32(0);
    ValueID boundSource = counted->iteratedArray ? m_function.at(m_function.at(m_loops[counted->index].header).terminator.value).child(0) : counted->bound;
    if (!isAvailableAtCurrentValue(boundSource) || !get(boundSource).value)
        return word32(0);
    Integer bound = counted->iteratedArray ? arrayLength(asCell(in<Boxed>(m_function.at(m_function.at(m_loops[counted->index].header).terminator.value).child(0)))) : in<Integer>(counted->bound);
    Integer turns = sub(bound, in<Integer>(counted->counter));
    return low32(select(below(turns, integer(maxTurns)), turns, integer(0)));
}

LValue Lowering::expectedIndexingOfStoredArray(ValueID receiver, ValueID key, bool isNumberAmongNumbers)
{
    ValueID literal = emptyLiteralSourceOf(receiver);
    bool hasSite = literal && m_function.at(literal).vectorLengthHintOffset() != Format::none;
    auto counted = countedLoopContaining(m_currentBlockID);
    auto isReceiverLength = [&](ValueID id) {
        return id && isInlineBuiltin(originOf(id), InlineBuiltin::ArrayLength) && originOf(m_function.at(originOf(id)).child(0)) == originOf(receiver);
    };
    bool staysWithinLength = counted && (isReceiverLength(counted->bound) || (counted->iteratedArray && originOf(counted->iteratedArray) == originOf(receiver)));
    bool appendsInLoopOrder = counted && !staysWithinLength && (originOf(key) == originOf(counted->counter) || isReceiverLength(key));
    Word32 turns = appendsInLoopOrder ? remainingLoopIterations() : word32(0);
    if (!hasSite && turns.value()->isInt32(0))
        return nullptr;
    LValue expectedAndSite = m_out.shl(m_out.zeroExtPtr(turns.value()), m_out.constInt32(32));
    if (hasSite)
        expectedAndSite = m_out.bitOr(expectedAndSite, m_out.constInt64((static_cast<uint64_t>(m_function.at(literal).vectorLengthHintOffset()) + 1) << 1));
    return isNumberAmongNumbers ? m_out.bitOr(expectedAndSite, m_out.constInt64(1)) : expectedAndSite;
}

std::optional<Word32> Lowering::maxPushesInCollectionLoop(ValueID literal)
{
    unsigned index = m_innermostLoop[m_currentBlockID.index()];
    if (index == noLoop)
        return std::nullopt;
    ValueID collection;
    bool isMap = false;
    for (size_t block : m_loops[index].blocks) {
        if (m_innermostLoop[block] != index || collection)
            continue;
        for (ValueID id : m_function.blocks[block].values) {
            isMap = isInlineBuiltin(id, InlineBuiltin::MapNext);
            if (!isMap && !isInlineBuiltin(id, InlineBuiltin::SetNext))
                continue;
            collection = m_function.at(id).arguments[0];
            break;
        }
    }
    if (!collection || !isAvailableAtCurrentValue(collection) || !get(collection).value)
        return std::nullopt;
    unsigned pushedInTurn = 0;
    for (unsigned block = 0; block < m_function.blocks.size(); ++block) {
        for (ValueID id : m_function.blocks[block].values) {
            if (!isInlineBuiltin(id, InlineBuiltin::ArrayPush) || emptyLiteralSourceOf(m_function.at(id).child(0)) != literal)
                continue;
            if (m_innermostLoop[block] != index)
                return std::nullopt;
            pushedInTurn += m_function.at(id).arguments.size();
        }
    }
    if (!pushedInTurn)
        return std::nullopt;
    auto unknown = [&] {
        return word32(std::numeric_limits<int32_t>::max());
    };
    auto ofCell = [&](CellPointer cell) {
        return either<Word32>(equal(loadCellType(cell), word32(isMap ? JSMapType : JSSetType)), [&] {
            return low32(mul(mapOrSetSize(cell, isMap, false), integer(pushedInTurn)));
        }, unknown);
    };
    Operand operand = get(collection);
    if (operand.rep == Rep::Cell && !typeOf(collection).mayBeUndefinedOrNull())
        return ofCell(operand.as<CellPointer>());
    Boxed boxed = convertTo<Boxed>(operand, typeOf(collection));
    return either<Word32>(isCell(boxed, typeOf(collection)), [&] {
        return ofCell(asCell(boxed));
    }, unknown);
}

unsigned Lowering::numberOfElementsPushedTo(ValueID array)
{
    unsigned count = 0;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::CallBuiltin)
                continue;
            auto& builtin = m_builtins.at(value.builtin());
            if (builtin.hasReceiver() && value.child(0) && builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::ArrayPush && originOf(value.child(0)) == array)
                count += value.arguments.size();
        }
    }
    return count;
}

LBasicBlock Lowering::appendSpread(const Value& value)
{
    if (auto* engineClass = typeOf(value.arguments[1]).singleEngineClass(); engineClass && engineClass->typedArrayType != NotTypedArray)
        return appendTypedArraySpread(value);
    ValueID literal = originOf(value.arguments[0]);
    while (m_function.at(literal).opcode == Opcode::CallBuiltin && m_function.at(literal).builtin() == value.builtin())
        literal = originOf(m_function.at(literal).arguments[0]);
    Operand iterable = get(value.arguments[1]);
    if (!holdsCells(iterable.rep) || !typeOf(value.arguments[1]).hasObjects())
        return nullptr;
    LBasicBlock otherwise = newUnlikelyBlock();
    if (auto presizedSpread = m_spreadsDoneAtCreation.getOptional(m_currentBlock->values[m_currentIndexInBlock].index()); presizedSpread && m_function.at(literal).opcode == Opcode::NewArray) {
        orElse(Word32(*presizedSpread), otherwise);
        didEmitWrite(builtinCallEffects(value).writes);
        m_spreadUsesStub = true;
        return otherwise;
    }
    if (iterable.rep != Rep::Cell)
        orElse(isCell(iterable.as<Boxed>(), typeOf(value.arguments[1])), otherwise);
    CellPointer array = iterable.rep == Rep::Cell ? iterable.as<CellPointer>() : asCell(iterable.as<Boxed>());
    orElse(equal(loadCellType(array), word32(ArrayType)), otherwise);
    orElse(isZero(publicLength(butterflyOf(array))), otherwise);
    didEmitWrite(builtinCallEffects(value).writes);
    m_spreadUsesStub = true;
    return otherwise;
}

void Lowering::addIfNumber(Join& join, Boxed element, const Type& type)
{
    Rep rep = repOf(type);
    if ((rep != Rep::Double && rep != Rep::OptionalDouble) || Options::auditAOTTypedAssertions())
        return;
    LBasicBlock isNumberCase = m_out.newBlock();
    LBasicBlock isOther = newUnlikelyBlock();
    m_out.branch(isNumber(element).value(), usually(isNumberCase), rarely(isOther));
    m_out.appendTo(isNumberCase);
    join.add(Operand(rep, select(isInt32(element), toNumber(low32(element)), unboxedDouble(element)).value()));
    m_out.appendTo(isOther);
}

Operand Lowering::doubleArrayElement(Number element, const Type& type)
{
    Rep rep = repOf(type);
    if (rep == Rep::Double || rep == Rep::OptionalDouble)
        return Operand(rep, element.value());
    return ensure(element, m_program.at(m_program.types.number), type);
}

Boxed Lowering::callArrayMethodOnSlowPath(const Value& value, CellPointer array)
{
    if (m_predictedMethodAlreadyRead == value.builtin())
        return callBuiltinFunction(value, { }, box(array));
    return callMethodGeneric(box(array), m_program.stringOf(m_builtins.at(value.builtin()).nameName), value.arguments);
}

Operand Lowering::lowerArrayAt(const Value& value, const Type& type)
{
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    CellPointer array = nonNullish(value.child(0), name);
    Rep rep = repOf(type);
    bool mayReturnNumber = type.primitives & Primitive::Number;
    LBasicBlock doubles = mayReturnNumber ? m_out.newBlock() : nullptr;
    LBasicBlock notDoubles = m_out.newBlock();
    LBasicBlock absent = m_out.newBlock();
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    Integer given = in<Integer>(value.arguments[0]);
    Word32 shape = indexingModeOf(array, IndexingShapeMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    Integer index = select(lessThan(given, integer(0)), add(given, length), given);
    if (mayReturnNumber) {
        m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(doubles), unsure(notDoubles));

        m_out.appendTo(doubles);
        orElse(below(index, length), absent);
        Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
        orElse(equal(asDouble, asDouble), absent);
        join.add(doubleArrayElement(asDouble, type));
    } else
        m_out.jump(notDoubles);

    m_out.appendTo(notDoubles);
    orElse(mayReturnNumber ? logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))) : equal(shape, word32(ContiguousShape)), slowPath);
    orElse(below(index, length), absent);
    Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
    addIfNumber(join, element, type);
    orElse(isNotEmpty(element), absent);
    join.add(ensure(element, type));

    m_out.appendTo(absent);
    join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));

    m_out.appendTo(slowPath);
    join.add(ensure(callArrayMethodOnSlowPath(value, array), type));
    return join.finish();
}

Operand Lowering::lowerArrayJoin(const Value& value, const Type& type, ValueID separator)
{
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    CellPointer array = nonNullish(value.child(0), name);
    LBasicBlock slowPath = newUnlikelyBlock();
    Join join(m_out, repOf(type), b3TypeOf(repOf(type)));
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    orElse(belowOrEqual(binary<Word32>(&Output::sub, indexingModeOf(array, IndexingShapeMask), word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
    CellPointer joined = AOT_TYPED_OPERATION(ArrayJoin, array, separator ? in<CellPointer>(separator) : CellPointer(m_out.int64Zero));
    orElse(Word32(m_out.notZero64(joined.value())), slowPath);
    join.add(ensure(joined, m_program.at(m_program.types.string), type));

    m_out.appendTo(slowPath);
    join.add(ensure(callArrayMethodOnSlowPath(value, array), type));
    return join.finish();
}

Operand Lowering::lowerArrayConcat(const Value& value, const Type& type)
{
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<CellPointer, 4> arrays { nonNullish(value.child(0), name) };
    bool areElementsNumbers = true;
    bool areElementsInt32 = true;
    for (ValueID argument : value.arguments) {
        if (!typeOf(argument).hasObjects()) {
            areElementsNumbers &= typeOf(argument).isNumber();
            areElementsInt32 &= isInt32Type(typeOf(argument));
            arrays.append(CellPointer());
            continue;
        }
        Operand operand = get(argument);
        if (operand.rep != Rep::Cell)
            orElse(isCell(operand.as<Boxed>(), typeOf(argument)), slowPath);
        arrays.append(operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>()));
    }
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    Word32 shape = anyArrayIndexingMode(arrays[0], IndexingShapeMask);
    orElse(belowOrEqual(binary<Word32>(&Output::sub, shape, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
    static_assert(Instance::arrayKindShift == 1);
    auto originalStructureID = [&](Word32 mode) {
        return Word32(m_out.load32(TypedPointer(m_heaps.AOTInstance_fixedFields, m_out.add(m_instance, m_out.add(m_out.shl(m_out.zeroExtPtr(mode.value()), m_out.constInt32(1)), m_out.constIntPtr(Instance::offsetOfOriginalArrayStructureIDs()))))));
    };
    Vector<Pointer, 4> butterflies;
    Vector<Integer, 4> lengths;
    Integer length = integer(0);
    for (CellPointer array : arrays) {
        if (!array) {
            butterflies.append(Pointer());
            lengths.append(integer(1));
            length = add(length, integer(1));
            continue;
        }
        Word32 mode = anyArrayIndexingMode(array, IndexingShapeMask | CopyOnWrite);
        orElse(equal(binary<Word32>(&Output::bitAnd, mode, word32(IndexingShapeMask)), shape), slowPath);
        orElse(equal(loadField<Word32>(array, m_heaps.JSCell_structureID), originalStructureID(mode)), slowPath);
        butterflies.append(butterflyOf(array));
        lengths.append(publicLength(butterflies.last()));
        length = add(length, lengths.last());
    }
    orElse(belowOrEqual(length, integer(maxLengthForElementwiseCopy)), slowPath);

    orElse(m_out.notZero32(changing32(Instance::offsetOfNewArrayWithContiguousStructureID())), slowPath);
    LValue capacity = m_out.bitOr(length.value(), m_out.constInt64(1));
    LValue size = m_out.add(m_out.shl(capacity, m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader)));
    LValue storage = allocateStorage(size, slowPath);
    LValue made = m_out.add(storage, m_out.constIntPtr(sizeof(IndexingHeader)));
    m_out.store32(m_out.castToInt32(length.value()), made, m_heaps.Butterfly_publicLength);
    m_out.store32(m_out.castToInt32(capacity), made, m_heaps.Butterfly_vectorLength);
    LBasicBlock copied = m_out.newBlock();
    auto copyElements = [&](IndexingType indexingType) {
        auto& elements = elementsOf(indexingType);
        m_out.store64(m_out.constInt64(holeIn(indexingType)), TypedPointer(elements, m_out.add(storage, m_out.shl(capacity, m_out.constInt32(3)))));
        LValue to = made;
        for (unsigned i = 0; i < arrays.size(); ++i) {
            if (!arrays[i]) {
                ValueID element = value.arguments[i - 1];
                if (indexingType != ArrayWithDouble)
                    m_out.store64(in<Boxed>(element).value(), TypedPointer(elements, to));
                else {
                    Number number = in<Number>(element);
                    if (!typeOf(element).isInteger())
                        orElse(equal(number, number), slowPath);
                    m_out.storeDouble(number.value(), TypedPointer(elements, to));
                }
                to = m_out.add(to, m_out.intPtrEight);
                continue;
            }
            LBasicBlock loop = m_out.newBlock();
            LBasicBlock element = m_out.newBlock();
            LBasicBlock next = m_out.newBlock();
            ValueFromBlock start = m_out.anchor(m_out.intPtrZero);
            m_out.jump(loop);
            m_out.appendTo(loop);
            LValue index = m_out.phi(pointerType(), start);
            m_out.branch(m_out.below(index, lengths[i].value()), unsure(element), unsure(next));
            m_out.appendTo(element);
            m_out.store64(m_out.load64(m_out.baseIndex(elements, butterflies[i].value(), index, FTL::ScaleEight)), m_out.baseIndex(elements, to, index, FTL::ScaleEight));
            m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
            m_out.jump(loop);
            m_out.appendTo(next);
            to = m_out.add(to, m_out.shl(lengths[i].value(), m_out.constInt32(3)));
        }
        m_out.jump(copied);
    };
    LBasicBlock doubles = m_out.newBlock();
    LBasicBlock values = m_out.newBlock();
    m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(doubles), unsure(values));
    m_out.appendTo(doubles);
    if (areElementsNumbers)
        copyElements(ArrayWithDouble);
    else
        m_out.jump(slowPath);
    m_out.appendTo(values);
    if (!areElementsInt32)
        orElse(equal(shape, word32(ContiguousShape)), slowPath);
    copyElements(ArrayWithContiguous);

    m_out.appendTo(copied);
    for (CellPointer array : arrays) {
        if (array)
            ensureStillAliveHere(array);
    }
    LValue result = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), slowPath);
    applyEffects(result->as<PatchpointValue>(), { Writes::RuntimeState, false });
    m_out.store32(originalStructureID(shape).value(), result, m_heaps.JSCell_structureID);
    m_out.store32(m_out.bitOr(shape.value(), m_out.constInt32(arrayTypeInfoBlob(ArrayClass))), result, m_heaps.JSCell_usefulBytes);
    m_out.storePtr(made, result, m_heaps.JSObject_butterfly);
    fenceAfterInitialization();
    ValueFromBlock fast = m_out.anchor(result);
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(callArrayMethodOnSlowPath(value, arrays[0]).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return ensure(Boxed(m_out.phi(Int64, fast, slow)), type);
}

Operand Lowering::lowerArrayShift(const Value& value, const Type& type)
{
    constexpr unsigned maxLengthForElementwiseMove = 32;
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    CellPointer array = nonNullish(value.child(0), name);
    Rep rep = repOf(type);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock absent = m_out.newBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    didEmitWrite(Writes::Heap);
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
    orElse(belowOrEqual(binary<Word32>(&Output::sub, mode, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    orElse(belowOrEqual(sub(length, integer(1)), integer(maxLengthForElementwiseMove - 1)), slowPath);
    Integer last = sub(length, integer(1));
    auto shift = [&](IndexingType indexingType) {
        auto& elements = elementsOf(indexingType);
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock element = m_out.newBlock();
        LBasicBlock moved = m_out.newBlock();
        LValue first = m_out.load64(TypedPointer(elements, butterfly.value()));
        ValueFromBlock start = m_out.anchor(m_out.intPtrZero);
        m_out.jump(loop);
        m_out.appendTo(loop);
        LValue index = m_out.phi(pointerType(), start);
        m_out.branch(m_out.below(index, last.value()), unsure(element), unsure(moved));
        m_out.appendTo(element);
        LValue next = m_out.add(index, m_out.intPtrOne);
        m_out.store64(m_out.load64(m_out.baseIndex(elements, butterfly.value(), next, FTL::ScaleEight)), m_out.baseIndex(elements, butterfly.value(), index, FTL::ScaleEight));
        m_out.addIncomingToPhi(index, m_out.anchor(next));
        m_out.jump(loop);
        m_out.appendTo(moved);
        m_out.store64(m_out.constInt64(holeIn(indexingType)), m_out.baseIndex(elements, butterfly.value(), last.value(), FTL::ScaleEight));
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(last));
        if (indexingType != ArrayWithDouble)
            barrier(array);
        orElse(m_out.notEqual(first, m_out.constInt64(holeIn(indexingType))), absent);
        return first;
    };
    LBasicBlock doubles = m_out.newBlock();
    LBasicBlock values = m_out.newBlock();
    m_out.branch(equal(mode, word32(DoubleShape)).value(), unsure(doubles), unsure(values));
    m_out.appendTo(doubles);
    join.add(doubleArrayElement(Number(m_out.bitCast(shift(ArrayWithDouble), Double)), type));
    m_out.appendTo(values);
    Boxed first(shift(ArrayWithContiguous));
    addIfNumber(join, first, type);
    join.add(ensure(first, type));

    m_out.appendTo(absent);
    join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));

    m_out.appendTo(slowPath);
    join.add(ensure(callArrayMethodOnSlowPath(value, array), type));
    return join.finish();
}

Operand Lowering::lowerArraySlice(const Value& value, const Type& type)
{
    auto& arguments = value.arguments;
    CellPointer array = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    LBasicBlock slowPath = newBlockForGenericCall(value);
    LBasicBlock continuation = m_out.newBlock();
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    m_context.emitsCalls = true;
    ValueFromBlock byStub;
    if (arguments.isEmpty())
        byStub = m_out.anchor(callAllocationStub(Stub::ArrayCopy, { { array.value(), GPRInfo::toArgumentRegister(1) } }, effectsForOperation<operationAOTTypedArrayCopy>(), true));
    else {
        Integer first = indexOrElse(arguments[0], slowPath);
        Integer afterLast = arguments.size() > 1 ? indexOrElse(arguments[1], slowPath) : integer(static_cast<int64_t>(1) << 32);
        byStub = m_out.anchor(callAllocationStub(Stub::ArraySlice, { { array.value(), GPRInfo::toArgumentRegister(1) }, { first.value(), GPRInfo::toArgumentRegister(2) }, { afterLast.value(), GPRInfo::toArgumentRegister(3) } }, effectsForOperation<operationAOTTypedArraySlice>(), true));
    }
    Vector<ValueFromBlock, 2> made { byStub };
    m_out.jump(continuation);
    if (!isSharedGenericPath(slowPath)) {
        m_out.appendTo(slowPath);
        made.append(m_out.anchor(callBuiltinFunction(value, OperationEffects { Writes::Heap, true }).value()));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
    return ensure(Boxed(m_out.phi(Int64, made)), Type::engineClass(m_builtins.classes()[value.engineClass()]), type);
}

Operand Lowering::lowerArraySplice(ValueID result, const Value& value, const Type& type)
{
    constexpr unsigned maxLengthForElementwiseMove = 32;
    constexpr unsigned maxItems = 2;
    constexpr int64_t maxRemoved = 2;
    auto& arguments = value.arguments;
    CellPointer array = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    Type newArrayType = Type::engineClass(m_builtins.classes()[value.engineClass()]);
    bool isUsed = useCountOf(result);
    Arguments items;
    if (arguments.size() > 2)
        items.append(arguments.span().subspan(2));
    Rep rep = repOf(type);
    Join join(m_out, rep, b3TypeOf(rep));
    LBasicBlock isNot = newBlockForGenericCall(value);
    Integer start = indexOrElse(arguments[0], isNot);
    if (!isInt32Type(typeOf(arguments[0])))
        orElse(equal(signExtended(low32(start)), start), isNot);

    Range given = arguments.size() > 1 ? typeOf(arguments[1]).integers : Range::full();
    if (given.minimum == given.maximum && given.minimum >= 0 && given.minimum <= maxRemoved && items.size() <= maxItems && (!isUsed || !given.minimum)) {
        int64_t removed = given.minimum;
        int64_t added = items.size();
        bool areNumbers = true;
        bool areInt32 = true;
        for (ValueID item : items) {
            areNumbers &= typeOf(item).isNumber();
            areInt32 &= isInt32Type(typeOf(item));
        }
        LBasicBlock slowPath = newColdBlock();
        didEmitWrite(Writes::Heap);
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
        orElse(belowOrEqual(binary<Word32>(&Output::sub, mode, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
        Pointer butterfly = butterflyOf(array);
        Integer length = publicLength(butterfly);
        Integer fromEnd = add(start, length);
        Integer from = select(lessThan(start, integer(0)), select(lessThan(fromEnd, integer(0)), integer(0), fromEnd), select(lessThan(start, length), start, length));
        Integer tail = sub(length, add(from, integer(removed)));
        orElse(belowOrEqual(tail, integer(maxLengthForElementwiseMove)), slowPath);
        Integer newLength = add(length, integer(added - removed));
        if (added > removed) {
            LBasicBlock hasRoom = m_out.newBlock();
            LBasicBlock isFull = newColdBlock();
            ValueFromBlock unchanged = m_out.anchor(butterfly.value());
            m_out.branch(belowOrEqual(newLength, vectorLength(butterfly)).value(), usually(hasRoom), rarely(isFull));
            m_out.appendTo(isFull);
            orElse(logicalNot(isZero(AOT_TYPED_OPERATION(ArrayGrow, array, low32(newLength)))), slowPath);
            ValueFromBlock grown = m_out.anchor(butterflyOf(array).value());
            m_out.jump(hasRoom);
            m_out.appendTo(hasRoom);
            butterfly = Pointer(m_out.phi(pointerType(), unchanged, grown));
        }
        auto splice = [&](IndexingType indexingType) {
            auto& elements = elementsOf(indexingType);
            Vector<LValue, maxItems> stored;
            for (ValueID item : items) {
                if (indexingType != ArrayWithDouble) {
                    stored.append(in<Boxed>(item).value());
                    continue;
                }
                Number number = in<Number>(item);
                if (!typeOf(item).isInteger())
                    orElse(equal(number, number), slowPath);
                stored.append(m_out.bitCast(number.value(), Int64));
            }
            auto addressOf = [&](Integer index) {
                return m_out.add(butterfly.value(), m_out.shl(index.value(), m_out.constInt32(3)));
            };
            if (added != removed) {
                bool movesUp = added > removed;
                LValue source = addressOf(add(from, integer(removed)));
                LValue target = addressOf(add(from, integer(added)));
                LBasicBlock loop = m_out.newBlock();
                LBasicBlock element = m_out.newBlock();
                LBasicBlock moved = m_out.newBlock();
                ValueFromBlock first = m_out.anchor(movesUp ? tail.value() : m_out.int64Zero);
                m_out.jump(loop);
                m_out.appendTo(loop);
                LValue index = m_out.phi(Int64, first);
                m_out.branch(movesUp ? m_out.notZero64(index) : m_out.below(index, tail.value()), unsure(element), unsure(moved));
                m_out.appendTo(element);
                LValue place = movesUp ? m_out.sub(index, m_out.constInt64(1)) : index;
                m_out.store64(m_out.load64(m_out.baseIndex(elements, source, place, FTL::ScaleEight)), m_out.baseIndex(elements, target, place, FTL::ScaleEight));
                m_out.addIncomingToPhi(index, m_out.anchor(movesUp ? place : m_out.add(index, m_out.constInt64(1))));
                m_out.jump(loop);
                m_out.appendTo(moved);
                for (int64_t i = 0; i < removed - added; ++i)
                    m_out.store64(m_out.constInt64(holeIn(indexingType)), TypedPointer(elements, addressOf(add(newLength, integer(i)))));
            }
            for (unsigned i = 0; i < stored.size(); ++i)
                m_out.store64(stored[i], TypedPointer(elements, addressOf(add(from, integer(i)))));
            if (added != removed)
                storeField(butterfly, m_heaps.Butterfly_publicLength, low32(newLength));
            if (indexingType != ArrayWithDouble)
                barrier(array);
            join.add(ensure(isUsed ? newArray(Arguments { }) : array, newArrayType, type));
        };
        LBasicBlock doubles = m_out.newBlock();
        LBasicBlock values = m_out.newBlock();
        m_out.branch(equal(mode, word32(DoubleShape)).value(), unsure(doubles), unsure(values));
        m_out.appendTo(doubles);
        if (areNumbers)
            splice(ArrayWithDouble);
        else
            m_out.jump(slowPath);
        m_out.appendTo(values);
        if (!areInt32)
            orElse(equal(mode, word32(ContiguousShape)), slowPath);
        splice(ArrayWithContiguous);
        m_out.appendTo(slowPath);
    }

    auto [count, buffer] = argumentBuffer(items);
    Word32 deleteCount = arguments.size() > 1 ? low32(in<Integer>(arguments[1])) : word32(INT32_MAX);
    LValue done = AOT_TYPED_OPERATION(ArraySplice, array, low32(start), deleteCount, count, buffer, word32(isUsed)).value();
    LBasicBlock isDone = m_out.newBlock();
    m_out.branch(m_out.notZero64(done), usually(isDone), rarely(isNot));
    m_out.appendTo(isDone);
    join.add(ensure(Boxed(done), newArrayType, type));
    if (!isSharedGenericPath(isNot)) {
        m_out.appendTo(isNot);
        join.add(ensure(callBuiltinFunction(value, OperationEffects { Writes::Heap, true }), newArrayType, type));
    }
    return join.finish();
}

std::optional<Operand> Lowering::lowerArraySearchAsSingleCall(ValueID result, const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    if (builtin.implementation.kind != BuiltinImplementation::Inline || (builtin.implementation.inlined != InlineBuiltin::ArrayIndexOf && builtin.implementation.inlined != InlineBuiltin::ArrayIncludes))
        return std::nullopt;
    if (value.arguments.size() != 1 || typeOf(value.arguments[0]).isNumber() || typeOf(value.child(0)).singleEngineClass() != &m_builtins.arrayClass())
        return std::nullopt;
    bool isIndexOf = builtin.implementation.inlined == InlineBuiltin::ArrayIndexOf;
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    CellPointer array = nonNullish(value.child(0), m_program.stringOf(builtin.nameName));
    LBasicBlock isCallResult = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    Vector<std::pair<LValue, GPRReg>, 4> operands { { array.value(), GPRInfo::argumentGPR1 }, { mapKey(value.arguments[0]).value(), GPRInfo::argumentGPR2 } };
    if (value.methodGetById())
        operands.append({ in<Boxed>(value.methodGetById()).value(), GPRInfo::argumentGPR3 });
    Stub stub = value.methodGetById() ? (isIndexOf ? Stub::ArrayIndexOfOrCallMethodRead : Stub::ArrayIncludesOrCallMethodRead) : (isIndexOf ? Stub::ArrayIndexOfOrCall : Stub::ArrayIncludesOrCall);
    LValue answer = callICStub(stub, Int64, operands, std::nullopt, OperationEffects { Writes::Heap, true });
    ensureStillAliveHere(array);
    orElse(Word32(isIndexOf ? m_out.isZero64(m_out.lShr(answer, m_out.constInt32(32))) : m_out.belowOrEqual(answer, m_out.constInt64(1))), isCallResult);
    Word32 found(m_out.castToInt32(answer));
    if (isIndexOf)
        join.add(ensure(signExtended(found), Type::integer({ -1, INT32_MAX }), type));
    else
        join.add(booleanResult(found, type));
    m_out.appendTo(isCallResult);
    LValue yielded = answer;
    if (isIndexOf) {
        static_assert(!(arraySearchDeclined & JSValue::NumberTag) && arraySearchDeclined >> 32);
        LValue returnedValue = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfVM()), VM::offsetOfEncodedHostCallReturnValue()));
        yielded = m_out.select(m_out.equal(answer, m_out.constInt64(arraySearchDeclined)), returnedValue, answer);
    }
    join.add(ensure(Boxed(yielded), type));
    return join.finish();
}

Integer Lowering::indexOfNumber(CellPointer array, Number searched)
{
    LBasicBlock isLong = newUnlikelyBlock();
    LBasicBlock loop = m_out.newBlock();
    LBasicBlock element = m_out.newBlock();
    LBasicBlock next = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    ValueFromBlock start = m_out.anchor(m_out.int64Zero);
    m_out.branch(below(length, integer(minLengthForOutOfLineSearch)).value(), usually(loop), rarely(isLong));
    m_out.appendTo(isLong);
    ValueFromBlock outOfLine = m_out.anchor(signExtended(AOT_TYPED_OPERATION(ArrayIndexOfDouble, array, searched)).value());
    m_out.jump(continuation);
    m_out.appendTo(loop);
    LValue index = m_out.phi(Int64, start);
    ValueFromBlock none = m_out.anchor(m_out.constInt64(-1));
    m_out.branch(m_out.below(index, length.value()), unsure(element), unsure(continuation));
    m_out.appendTo(element);
    ValueFromBlock found = m_out.anchor(index);
    m_out.branch(m_out.doubleEqual(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index)), searched.value()), unsure(continuation), unsure(next));
    m_out.appendTo(next);
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.constInt64(1))));
    m_out.jump(loop);
    m_out.appendTo(continuation);
    ensureStillAliveHere(array);
    return Integer(m_out.phi(Int64, none, found, outOfLine));
}

static bool isTypedArrayWithInlineElementReads(const Type& type)
{
    auto* engineClass = type.singleEngineClass();
    if (!engineClass)
        return false;
    switch (engineClass->typedArrayType) {
    case TypeInt8:
    case TypeUint8:
    case TypeUint8Clamped:
    case TypeInt16:
    case TypeUint16:
    case TypeInt32:
    case TypeUint32:
    case TypeFloat32:
    case TypeFloat64:
        return true;
    default:
        return false;
    }
}

std::optional<Writes> writesOfInlineArray(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::ArrayFrom:
        return writesIf(isTypedArrayWithInlineElementReads(arguments[0]), Writes::RuntimeState);
    case InlineBuiltin::ArrayLength:
        return Writes::Nothing;
    case InlineBuiltin::ArrayEntries:
        return writesIf(arguments.empty(), Writes::RuntimeState);
    case InlineBuiltin::ArrayPush:
        return writesIf(!arguments.empty() && arguments.size() <= 8, Writes::Heap);
    case InlineBuiltin::ArraySetLength:
    case InlineBuiltin::ArrayPop:
        return Writes::Heap;
    case InlineBuiltin::ArrayAt:
        return writesIf(arguments[0].isInteger(), Writes::Heap);
    case InlineBuiltin::ArrayJoin:
        return writesIf(arguments.empty() || arguments[0].isString(), Writes::Heap);
    case InlineBuiltin::ArrayToString:
    case InlineBuiltin::ArrayShift:
        return Writes::Heap;
    case InlineBuiltin::ArrayConcat:
        return writesIf(!arguments.empty() && arguments.size() <= 3 && std::ranges::all_of(arguments, [](auto& argument) {
            return !argument.hasObjects() || argument.singleEngineClass() == &Builtins::singleton().arrayClass();
        }), Writes::Heap);
    case InlineBuiltin::ArrayUnshift:
        return writesIf(arguments.size() == 1, Writes::Heap);
    case InlineBuiltin::ArrayIndexOf:
        if (arguments.size() == 2 && !arguments[0].isNumber() && arguments[1].isInteger() && arguments[1].integers.minimum >= 0 && arguments[1].integers.maximum <= INT32_MAX)
            return Writes::RuntimeState;
        [[fallthrough]];
    case InlineBuiltin::ArrayIncludes:
        return writesIf(arguments.size() == 1, Writes::RuntimeState);
    case InlineBuiltin::ArraySlice:
        return writesIf(std::ranges::all_of(arguments, [](auto& argument) {
            return argument.isNumber();
        }), Writes::RuntimeState);
    case InlineBuiltin::ArraySplice:
        return writesIf(!arguments.empty() && arguments[0].isNumber() && (arguments.size() == 1 || isInt32Type(arguments[1])), Writes::Heap);
    case InlineBuiltin::ArrayWithLength:
    case InlineBuiltin::ArrayOf:
        return Writes::RuntimeState;
    case InlineBuiltin::ArrayIsArray:
        return arguments[0].hasObjects() ? Writes::RuntimeState : Writes::Pure;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Operand> Lowering::lowerInlineArray(InlineBuiltin which, ValueID result, const Value& value, Writes inlinePath)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto receiver = [&] {
        return nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    };
    auto genericPath = [&] {
        return callBuiltinFunction(value, OperationEffects { Writes::Heap, true });
    };
    auto orGeneric = [&](LValue done, const Type& guarantee) {
        LBasicBlock isDone = m_out.newBlock();
        LBasicBlock isNot = newBlockForGenericCall(value);
        Join join(m_out, Rep::JSValue, Int64);
        m_out.branch(m_out.notZero64(done), usually(isDone), rarely(isNot));
        m_out.appendTo(isDone);
        join.add(Boxed(done));
        if (!isSharedGenericPath(isNot)) {
            m_out.appendTo(isNot);
            join.add(genericPath());
        }
        return ensure(join.finish(), guarantee, type);
    };

    switch (which) {
    case InlineBuiltin::ArrayEntries:
        if (!m_entriesCreatedAsRecords.get(result.index()))
            return ensure(genericPath(), type);
        return convert(AOT_TYPED_OPERATION(NewArrayEntries, receiver()), Type::engineClass(m_builtins.arrayClass()), repOf(type));
    case InlineBuiltin::ArrayFrom:
        return lowerArrayFromTypedArray(result, value, type);
    case InlineBuiltin::ArrayAt:
        return lowerArrayAt(value, type);
    case InlineBuiltin::ArrayJoin:
        return lowerArrayJoin(value, type, arguments.isEmpty() ? ValueID() : arguments[0]);
    case InlineBuiltin::ArrayToString:
        return lowerArrayJoin(value, type, { });
    case InlineBuiltin::ArrayConcat:
        return lowerArrayConcat(value, type);
    case InlineBuiltin::ArrayShift:
        return lowerArrayShift(value, type);
    case InlineBuiltin::ArrayPop: {
        CellPointer array = receiver();
        Rep rep = repOf(type);
        LBasicBlock hasElements = m_out.newBlock();
        LBasicBlock hasNone = m_out.newBlock();
        bool mayReturnNumber = type.primitives & Primitive::Number;
        LBasicBlock values = m_out.newBlock();
        LBasicBlock doubles = mayReturnNumber ? m_out.newBlock() : nullptr;
        LBasicBlock slowPath = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        didEmitWrite(Writes::Heap);
        Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
        Word32 isDouble = equal(mode, word32(DoubleShape));
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        orElse(mayReturnNumber ? belowOrEqual(binary<Word32>(&Output::sub, mode, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)) : equal(mode, word32(ContiguousShape)), slowPath);
        Pointer butterfly = butterflyOf(array);
        Integer length = publicLength(butterfly);
        m_out.branch(isZero(length).value(), rarely(hasNone), usually(hasElements));

        m_out.appendTo(hasElements);
        Integer last = add(length, integer(-1));
        if (mayReturnNumber)
            m_out.branch(isDouble.value(), unsure(doubles), unsure(values));
        else
            m_out.jump(values);

        m_out.appendTo(values);
        TypedPointer slot = m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), last.value());
        Boxed element(m_out.load64(slot));
        orElse(isNotEmpty(element), slowPath);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(last));
        m_out.store64(m_out.constInt64(JSValue::encode(JSValue())), slot);
        addIfNumber(join, element, type);
        join.add(ensure(element, type));

        if (mayReturnNumber) {
            m_out.appendTo(doubles);
            TypedPointer doubleSlot = m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), last.value());
            Number asDouble(m_out.loadDouble(doubleSlot));
            orElse(equal(asDouble, asDouble), slowPath);
            storeField(butterfly, m_heaps.Butterfly_publicLength, low32(last));
            m_out.storeDouble(m_out.constDouble(PNaN), doubleSlot);
            join.add(doubleArrayElement(asDouble, type));
        }

        m_out.appendTo(hasNone);
        join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));

        m_out.appendTo(slowPath);
        join.add(ensure(genericPath(), type));
        return join.finish();
    }
    case InlineBuiltin::ArraySlice:
        return lowerArraySlice(value, type);
    case InlineBuiltin::ArraySplice:
        return lowerArraySplice(result, value, type);
    case InlineBuiltin::ArrayIndexOf:
    case InlineBuiltin::ArrayIncludes: {
        bool isIndexOf = which == InlineBuiltin::ArrayIndexOf;
        CellPointer array = receiver();
        Rep rep = repOf(type);
        LBasicBlock slowPath = newBlockForGenericCall(value);
        Join join(m_out, rep, b3TypeOf(rep));
        Word32 shape = indexingModeOf(array, IndexingShapeMask);
        if (typeOf(arguments[0]).isNumber()) {
            LBasicBlock doubles = m_out.newBlock();
            LBasicBlock notDoubles = m_out.newBlock();
            m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(doubles), unsure(notDoubles));
            m_out.appendTo(doubles);
            auto returnIndex = [&](Integer index) {
                if (isIndexOf)
                    join.add(ensure(index, Type::integer({ -1, INT32_MAX }), type));
                else
                    join.add(booleanResult(greaterThanOrEqual(index, integer(0)), type));
            };
            returnIndex(indexOfNumber(array, in<Number>(arguments[0])));
            m_out.appendTo(notDoubles);
            LBasicBlock longInt32s = newUnlikelyBlock();
            LBasicBlock others = m_out.newBlock();
            Word32 isLong = aboveOrEqual(publicLength(butterflyOf(array)), integer(minLengthForOutOfLineSearch));
            m_out.branch(logicalAnd(equal(shape, word32(Int32Shape)), isLong).value(), rarely(longInt32s), usually(others));
            m_out.appendTo(longInt32s);
            returnIndex(signExtended(AOT_TYPED_OPERATION(ArrayIndexOfInt32, array, mapKey(arguments[0]))));
            m_out.appendTo(others);
        }
        orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), slowPath);
        Vector<std::pair<LValue, GPRReg>, 4> operands { { butterflyOf(array).value(), GPRInfo::argumentGPR1 }, { mapKey(arguments[0]).value(), GPRInfo::argumentGPR2 } };
        bool hasStart = arguments.size() == 2;
        if (hasStart)
            operands.append({ in<Integer>(arguments[1]).value(), GPRInfo::argumentGPR4 });
        Word32 found(m_out.castToInt32(callICStub(hasStart ? Stub::ArrayIndexOfFrom : isIndexOf ? Stub::ArrayIndexOf : Stub::ArrayIncludes, Int64, operands, std::nullopt, OperationEffects { Writes::RuntimeState, true })));
        ensureStillAliveHere(array);
        if (isIndexOf)
            join.add(ensure(signExtended(found), Type::integer({ -1, INT32_MAX }), type));
        else
            join.add(booleanResult(logicalNot(isZero(found)), type));
        if (!isSharedGenericPath(slowPath)) {
            m_out.appendTo(slowPath);
            join.add(ensure(callBuiltinFunction(value, OperationEffects { Writes::RuntimeState, true }), type));
        }
        return join.finish();
    }
    case InlineBuiltin::ArrayUnshift:
        return orGeneric(AOT_TYPED_OPERATION(ArrayUnshift, receiver(), in<Boxed>(arguments[0])).value(), m_program.at(m_program.types.anything));
    default:
        return lowerContainerBuiltin(which, result, value, inlinePath);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
