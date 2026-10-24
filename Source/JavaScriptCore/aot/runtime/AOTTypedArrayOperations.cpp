/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "JSArrayBufferViewInlines.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSGenericTypedArrayViewPrototypeFunctions.h"
#include "JSTypedArrays.h"
#include "ObjectConstructor.h"
#include "StructureCache.h"
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringCommon.h>

namespace JSC { namespace AOT {

template<typename View>
static JSCell* newTypedArrayView(Instance* instance, JSGlobalObject* globalObject, size_t length)
{
    static_assert(View::TypedArrayStorageType < AllocationWords::numberOfTypedArrayAllocations && sizeof(View) == sizeof(JSArrayBufferView));
    VM& vm = *instance->vm;
    Structure* structure = globalObject->typedArrayStructure(View::TypedArrayStorageType, false);
    auto& allocation = instance->allocation.typedArrayAllocations[View::TypedArrayStorageType];
    structure->tryMarkPristine(vm);
    allocation.header = cellHeaderFor(structure);
    if (!allocation.allocator) {
        allocation.elementSize = View::elementSize;
        allocation.allocator = subspaceFor<View>(vm)->allocatorFor(sizeof(View), AllocatorForMode::EnsureAllocator).localAllocator();
    }
    return View::create(globalObject, structure, length);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewTypedArray, JSCell*, (Instance* instance, uint32_t type, double length))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewTypedArray);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    // As the constructor converts its operand. A length that no memory holds is refused by the allocation.
    size_t elementCount = jsNumber(purifyNaN(length)).toIndex(globalObject, "length"_s);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    switch (static_cast<TypedArrayType>(type)) {
#define AOT_NEW_TYPED_ARRAY_VIEW(name) \
    case Type ## name: \
        OPERATION_RETURN(scope, newTypedArrayView<JS ## name ## Array>(instance, globalObject, elementCount));
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_NEW_TYPED_ARRAY_VIEW)
#undef AOT_NEW_TYPED_ARRAY_VIEW
    case NotTypedArray:
    case TypeDataView:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The length is fixed and the memory is there: the view is neither detached, nor empty without memory, nor over a buffer that can change its size.
static ALWAYS_INLINE bool isFixed(const JSArrayBufferView* view)
{
    return !isResizableOrGrowableShared(view->mode()) && view->vector();
}

static ALWAYS_INLINE JSUint8Array* asFixedBytes(JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != Uint8ArrayType)
        return nullptr;
    auto* view = uncheckedDowncast<JSUint8Array>(value.asCell());
    return isFixed(view) ? view : nullptr;
}

// The characters of a text that has 8 bits a character. UTF-8 writes those below 128 as they are and each of the others as two bytes.
static ALWAYS_INLINE std::optional<std::span<const Latin1Character>> latin1CharactersOf(JSGlobalObject* globalObject, JSValue value)
{
    if (!value.isString())
        return std::nullopt;
    const StringImpl* impl = asString(value)->tryGetValueImpl();
    if (!impl) {
        static_cast<void>(asString(value)->value(globalObject));
        impl = asString(value)->tryGetValueImpl();
        if (!impl)
            return std::nullopt;
    }
    if (!impl->is8Bit())
        return std::nullopt;
    return impl->span8();
}

struct ReadAndWritten {
    size_t read { 0 };
    size_t written { 0 };
};

// As many whole characters as there is room for.
static NEVER_INLINE ReadAndWritten writeLatin1AsUTF8(std::span<const Latin1Character> characters, uint8_t* bytes, size_t room)
{
    ReadAndWritten counts;
    for (Latin1Character character : characters) {
        uint8_t unit = static_cast<uint8_t>(character);
        size_t size = unit < 0x80 ? 1 : 2;
        if (room - counts.written < size)
            break;
        if (size == 1)
            bytes[counts.written] = unit;
        else {
            bytes[counts.written] = 0xC0 | unit >> 6;
            bytes[counts.written + 1] = 0x80 | (unit & 0x3F);
        }
        counts.written += size;
        ++counts.read;
    }
    return counts;
}

// What the constructor makes of a length for which the collector keeps the bytes beside the view, without its questions about the other lengths. Nothing where there is no memory.
static ALWAYS_INLINE JSUint8Array* tryCreateFewBytes(VM& vm, JSGlobalObject* globalObject, size_t size)
{
    ASSERT(size && size <= JSArrayBufferView::fastSizeLimit);
    void* bytes = vm.primitiveGigacageAuxiliarySpace().allocate(vm, JSArrayBufferView::sizeOf(size, 1), nullptr, AllocationFailureMode::ReturnNull);
    if (!bytes) [[unlikely]]
        return nullptr;
    return JSUint8Array::createWithFastVector(globalObject, globalObject->typedArrayStructure(TypeUint8, false), size, bytes);
}

// `encoder.encode(text)`, the function known and the receiver a TextEncoder.
JSC_DEFINE_JIT_OPERATION(operationAOTEncodeTextToBytes, EncodedJSValue, (Instance* instance, EncodedJSValue encodedSource))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTEncodeTextToBytes);
    JSValue source = JSValue::decode(encodedSource);
    std::optional<std::span<const Latin1Character>> characters = latin1CharactersOf(globalObject, source);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (characters) [[likely]] {
        size_t size = characters->size();
        const uint8_t* from = byteCast<uint8_t>(characters->data());
        // A tag, a name, a number.
        if (size >= 8 && size <= 16) {
            uint64_t first = WTF::unalignedLoad<uint64_t>(from);
            uint64_t last = WTF::unalignedLoad<uint64_t>(from + size - 8);
            if (!((first | last) & 0x8080808080808080ull)) [[likely]] {
                if (JSUint8Array* bytes = tryCreateFewBytes(vm, globalObject, size)) [[likely]] {
                    WTF::unalignedStore<uint64_t>(bytes->typedVector(), first);
                    WTF::unalignedStore<uint64_t>(bytes->typedVector() + size - 8, last);
                    OPERATION_RETURN(scope, JSValue::encode(bytes));
                }
            }
        } else if (size && size < 8) {
            // The room for the bytes is a multiple of eight.
            uint64_t all = 0;
            for (size_t i = 0; i < size; ++i)
                all |= static_cast<uint64_t>(from[i]) << (8 * i);
            if (!(all & 0x8080808080808080ull)) [[likely]] {
                if (JSUint8Array* bytes = tryCreateFewBytes(vm, globalObject, size)) [[likely]] {
                    WTF::unalignedStore<uint64_t>(bytes->typedVector(), all);
                    OPERATION_RETURN(scope, JSValue::encode(bytes));
                }
            }
        }
        bool isASCII = charactersAreAllASCII(*characters);
        if (!isASCII) [[unlikely]] {
            for (Latin1Character character : *characters)
                size += static_cast<uint8_t>(character) >> 7;
        }
        JSUint8Array* bytes = size && size <= JSArrayBufferView::fastSizeLimit ? tryCreateFewBytes(vm, globalObject, size) : nullptr;
        if (!bytes) [[unlikely]] {
            bytes = JSUint8Array::createUninitialized(globalObject, globalObject->typedArrayStructure(TypeUint8, false), size);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        if (isASCII) [[likely]]
            memcpy(bytes->typedVector(), from, size);
        else
            writeLatin1AsUTF8(*characters, bytes->typedVector(), size);
        ensureStillAliveHere(source);
        OPERATION_RETURN(scope, JSValue::encode(bytes));
    }
    OPERATION_RETURN(scope, encodedJSValue());
}

static ALWAYS_INLINE Structure* structureOfReadAndWrittenCounts(Instance* instance, VM& vm, JSGlobalObject* globalObject)
{
    Structure*& kept = instance->realm.readAndWrittenCountsStructure;
    if (!kept) [[unlikely]] {
        PropertyOffset offset;
        Structure* structure = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), 2);
        structure = Structure::addPropertyTransition(vm, structure, vm.propertyNames->read, 0, offset);
        RELEASE_ASSERT(!offset);
        structure = Structure::addPropertyTransition(vm, structure, vm.propertyNames->written, 0, offset);
        RELEASE_ASSERT(offset == 1);
        kept = structure;
    }
    return kept;
}

// As many whole characters as there is room for. Nothing for a text that has 16 bits a character. May leave an exception.
static ALWAYS_INLINE JSObject* tryEncodeInto(Instance* instance, VM& vm, JSGlobalObject* globalObject, JSValue source, uint8_t* bytes, size_t room)
{
    std::optional<std::span<const Latin1Character>> characters = latin1CharactersOf(globalObject, source);
    if (!characters) [[unlikely]]
        return nullptr;
    auto answer = [&](size_t read, size_t written) ALWAYS_INLINE_LAMBDA {
        JSObject* counts = constructEmptyObject(vm, structureOfReadAndWrittenCounts(instance, vm, globalObject));
        counts->putDirectOffset(vm, 0, jsNumber(read));
        counts->putDirectOffset(vm, 1, jsNumber(written));
        return counts;
    };
    size_t usual = std::min(characters->size(), room);
    const uint8_t* from = byteCast<uint8_t>(characters->data());
    // A tag, a name, a number: nothing is written before all of it is known to be ASCII, because what lies behind the last byte written stays as it is.
    if (usual <= 16) {
        bool isASCII;
        if (usual >= 8) {
            uint64_t first = WTF::unalignedLoad<uint64_t>(from);
            uint64_t last = WTF::unalignedLoad<uint64_t>(from + usual - 8);
            isASCII = !((first | last) & 0x8080808080808080ull);
            if (isASCII) [[likely]] {
                WTF::unalignedStore<uint64_t>(bytes, first);
                WTF::unalignedStore<uint64_t>(bytes + usual - 8, last);
            }
        } else if (usual >= 4) {
            uint32_t first = WTF::unalignedLoad<uint32_t>(from);
            uint32_t last = WTF::unalignedLoad<uint32_t>(from + usual - 4);
            isASCII = !((first | last) & 0x80808080u);
            if (isASCII) [[likely]] {
                WTF::unalignedStore<uint32_t>(bytes, first);
                WTF::unalignedStore<uint32_t>(bytes + usual - 4, last);
            }
        } else {
            uint8_t all = 0;
            for (size_t i = 0; i < usual; ++i)
                all |= from[i];
            isASCII = !(all & 0x80);
            for (size_t i = 0; isASCII && i < usual; ++i)
                bytes[i] = from[i];
        }
        if (isASCII) [[likely]]
            return answer(usual, usual);
    } else if (charactersAreAllASCII(std::span { characters->data(), usual })) [[likely]] {
        memcpy(bytes, from, usual);
        return answer(usual, usual);
    }
    ReadAndWritten done = writeLatin1AsUTF8(*characters, bytes, room);
    return answer(done.read, done.written);
}

// `encoder.encodeInto(text, bytes)`, the function known and the receiver a TextEncoder.
JSC_DEFINE_JIT_OPERATION(operationAOTEncodeTextIntoBytes, EncodedJSValue, (Instance* instance, EncodedJSValue encodedSource, EncodedJSValue encodedDestination))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTEncodeTextIntoBytes);
    if (JSUint8Array* destination = asFixedBytes(JSValue::decode(encodedDestination))) [[likely]] {
        JSObject* counts = tryEncodeInto(instance, vm, globalObject, JSValue::decode(encodedSource), destination->typedVector(), destination->lengthRaw());
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (counts) [[likely]]
            OPERATION_RETURN(scope, JSValue::encode(counts));
    }
    OPERATION_RETURN(scope, encodedJSValue());
}

// `target.set(source, offset)`, the function known. The elements of a typed array go into one of the same kind, at a place where they fit, as they lie in memory. Nothing is answered for anything else.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySetOfAnyValue, EncodedJSValue, (Instance*, EncodedJSValue encodedReceiver, EncodedJSValue encodedSource, EncodedJSValue encodedOffset))
{
    JSValue receiver = JSValue::decode(encodedReceiver);
    JSValue given = JSValue::decode(encodedSource);
    JSValue offset = JSValue::decode(encodedOffset);
    if (receiver.isCell() && given.isCell() && isTypedArrayType(receiver.asCell()->type()) && receiver.asCell()->type() == given.asCell()->type() && (offset.isUndefined() || (offset.isInt32() && offset.asInt32() >= 0))) [[likely]] {
        auto* target = uncheckedDowncast<JSArrayBufferView>(receiver.asCell());
        auto* source = uncheckedDowncast<JSArrayBufferView>(given.asCell());
        size_t position = offset.isUndefined() ? 0 : offset.asInt32();
        size_t count = source->lengthRaw();
        size_t room = target->lengthRaw();
        if (isFixed(target) && isFixed(source) && position <= room && count <= room - position) [[likely]] {
            unsigned shift = logElementSize(target->type());
            memmove(static_cast<uint8_t*>(target->vector()) + (position << shift), source->vector(), count << shift);
            return { JSValue::encode(jsUndefined()), nullptr };
        }
    }
    return { encodedJSValue(), nullptr };
}

// `view.subarray(begin, end)`, the function known. A Uint8Array as its constructor left it, while nobody has changed what a new one is made by, gives a Uint8Array over the same buffer.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySubarrayOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedReceiver, EncodedJSValue encodedBegin, EncodedJSValue encodedEnd))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTypedArraySubarrayOfAnyValue);
    JSValue begin = JSValue::decode(encodedBegin);
    JSValue end = JSValue::decode(encodedEnd);
    JSUint8Array* view = asFixedBytes(JSValue::decode(encodedReceiver));
    bool makesPlainView = view && (begin.isInt32() || begin.isUndefined()) && (end.isInt32() || end.isUndefined()) && view->realm() == globalObject && speciesWatchpointIsValid(globalObject, view);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (makesPlainView) [[likely]] {
        int64_t length = view->lengthRaw();
        auto placeOf = [&](JSValue position, int64_t otherwise) {
            if (position.isUndefined())
                return otherwise;
            int64_t place = position.asInt32();
            return place < 0 ? std::max<int64_t>(length + place, 0) : std::min(place, length);
        };
        int64_t first = placeOf(begin, 0);
        int64_t last = std::max(first, placeOf(end, length));
        if (RefPtr<ArrayBuffer> buffer = view->possiblySharedBuffer()) [[likely]] {
            Structure* structure = globalObject->typedArrayStructure(TypeUint8, false);
            OPERATION_RETURN(scope, JSValue::encode(JSUint8Array::create(globalObject, structure, WTF::move(buffer), view->byteOffsetRaw() + first, static_cast<size_t>(last - first))));
        }
    }
    OPERATION_RETURN(scope, encodedJSValue());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayLength, uint64_t, (JSCell* view))
{
    return cellArgument<JSArrayBufferView>(view, Entry::operationAOTTypedArrayLength)->length();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedArrayPut, void, (Instance* instance, JSCell* cell, uint64_t index, double number))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTypedArrayPut);
    DeferTerminationForAWhile deferTermination(vm);
    auto* view = cellArgument<JSArrayBufferView>(cell, Entry::operationAOTTypedArrayPut);
    JSType type = view->type();
    RELEASE_ASSERT(type != BigInt64ArrayType && type != BigUint64ArrayType);
    JSValue value(JSValue::EncodeAsDouble, purifyNaN(number));
    switch (type) {
#define AOT_PUT_ELEMENT(name) \
    case name ## ArrayType: \
        uncheckedDowncast<JS ## name ## Array>(view)->setIndex(globalObject, index, value); \
        scope.assertNoException(); \
        return;
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_PUT_ELEMENT)
#undef AOT_PUT_ELEMENT
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowIfTypedArrayIsDetached, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowIfTypedArrayIsDetached);
    DeferGCForAWhile deferGC(vm);
    auto* view = cellArgument<JSArrayBufferView>(cell, Entry::operationAOTThrowIfTypedArrayIsDetached);
    if (view->isDetached() || view->isOutOfBounds())
        throwTypeError(globalObject, scope, typedArrayBufferHasBeenDetachedErrorMessage);
    OPERATION_RETURN(scope);
}

// A view over the buffer that has lost its memory with it. The constructor refuses such a buffer, so the view loses the memory of a buffer of its own, and is then handed the other.
static JSValue makeRangeWithoutMemory(JSGlobalObject* globalObject, Structure* structure, ArrayBuffer& buffer)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSUint8Array* range = JSUint8Array::create(globalObject, structure, 1);
    RETURN_IF_EXCEPTION(scope, { });
    RefPtr<ArrayBuffer> own = range->possiblySharedBuffer();
    if (!own) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    ArrayBufferContents taken;
    bool isTaken = own->transferTo(vm, taken);
    RELEASE_ASSERT(isTaken && range->isDetached());
    range->butterfly()->indexingHeader()->setArrayBuffer(&buffer);
    vm.heap.addReference(range, &buffer);
    return range;
}

// The range as it would be by now, had it been made when its start was taken.
static JSValue makeRangeOfBytes(JSGlobalObject* globalObject, JSValue view, int32_t start)
{
    auto* bytes = uncheckedDowncast<JSUint8Array>(view.asCell());
    RefPtr<ArrayBuffer> buffer = bytes->possiblySharedBuffer();
    if (!buffer) [[unlikely]] {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    Structure* structure = globalObject->typedArrayStructure(TypeUint8, false);
    if (bytes->isDetached()) [[unlikely]]
        return makeRangeWithoutMemory(globalObject, structure, *buffer);
    return JSUint8Array::create(globalObject, structure, WTF::move(buffer), bytes->byteOffsetRaw() + start, bytes->lengthRaw() - start);
}

} } // namespace JSC::AOT

// view.subarray(begin) that goes into encoder.encodeInto(text, ..) and nowhere else on its way there, in parts: see %CompilerHelpers% in the table of built-ins.
extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__startOfRangeOfBytes(JSC::JSGlobalObject*, JSC::EncodedJSValue encodedView, JSC::EncodedJSValue encodedBegin)
{
    using namespace JSC;
    JSValue begin = JSValue::decode(encodedBegin);
    JSUint8Array* view = AOT::asFixedBytes(JSValue::decode(encodedView));
    // As its constructor left it, which is what the call was resolved for: a range of it is a Uint8Array of this realm.
    if (view && TypeInfo::isPristine(view->inlineTypeFlags()) && begin.isInt32() && static_cast<uint32_t>(begin.asInt32()) <= view->lengthRaw() && begin.asInt32() >= 0) [[likely]]
        return encodedBegin;
    return JSValue::encode(jsNumber(-1));
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__viewOrRangeOfBytes(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedView, JSC::EncodedJSValue encodedBegin, JSC::EncodedJSValue encodedStart)
{
    using namespace JSC;
    if (JSValue::decode(encodedStart).asInt32() >= 0) [[likely]]
        return encodedView;
    return JSValue::encode(AOT::callMethodInEngine(AOT::Instance::of(globalObject), AOT::Builtins::TypedArrayPrototypeSubarray, JSValue::decode(encodedView), JSValue::decode(encodedBegin)));
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__rangeOfBytes(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedView, JSC::EncodedJSValue encodedStart)
{
    using namespace JSC;
    int32_t start = JSValue::decode(encodedStart).asInt32();
    if (start < 0)
        return encodedView;
    return JSValue::encode(AOT::makeRangeOfBytes(globalObject, JSValue::decode(encodedView), start));
}

extern "C" JS_EXPORT_PRIVATE JSC::EncodedJSValue SYSV_ABI AOT__encodeIntoRangeOfBytes(JSC::JSGlobalObject* globalObject, JSC::EncodedJSValue encodedEncoder, JSC::EncodedJSValue encodedSource, JSC::EncodedJSValue encodedDestination, JSC::EncodedJSValue encodedStart)
{
    using namespace JSC;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue destination = JSValue::decode(encodedDestination);
    int32_t start = JSValue::decode(encodedStart).asInt32();
    if (JSUint8Array* bytes = AOT::asFixedBytes(destination)) [[likely]] {
        size_t first = std::max(start, 0);
        JSObject* counts = AOT::tryEncodeInto(AOT::Instance::of(globalObject), vm, globalObject, JSValue::decode(encodedSource), bytes->typedVector() + first, bytes->lengthRaw() - first);
        RETURN_IF_EXCEPTION(scope, { });
        if (counts) [[likely]]
            return JSValue::encode(counts);
    }
    if (start >= 0) {
        destination = AOT::makeRangeOfBytes(globalObject, destination, start);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(AOT::callMethodInEngine(AOT::Instance::of(globalObject), AOT::Builtins::TextEncoderPrototypeEncodeInto, JSValue::decode(encodedEncoder), JSValue::decode(encodedSource), destination)));
}

#endif // ENABLE(AOT)
