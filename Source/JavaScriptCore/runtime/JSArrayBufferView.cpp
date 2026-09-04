/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "JSArrayBufferView.h"

#include "GenericTypedArrayViewInlines.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSTypedArrays.h"
#include "TypedArrayController.h"
#include "TypedArrays.h"
#include <wtf/FastMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

const ClassInfo JSArrayBufferView::s_info = {
    "ArrayBufferView"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSArrayBufferView)
};

const ASCIILiteral typedArrayBufferHasBeenDetachedErrorMessage { "Underlying ArrayBuffer has been detached from the view or out-of-bounds"_s };

JSArrayBufferView::ConstructionContext::ConstructionContext(Structure* structure, size_t length, void* vector)
    : m_structure(structure)
    , m_vector(vector)
    , m_length(length)
    , m_byteOffset(0)
    , m_mode(FastTypedArray)
    , m_butterfly(nullptr)
{
    ASSERT(!isResizableOrGrowableSharedTypedArrayIncludingDataView(structure->classInfoForCells()));
    ASSERT(!Gigacage::isEnabled() || (Gigacage::contains(vector) && Gigacage::contains(static_cast<const uint8_t*>(vector) + length - 1)));
    RELEASE_ASSERT(length <= fastSizeLimit);
}

JSArrayBufferView::ConstructionContext::ConstructionContext(VM& vm, Structure* structure, size_t length, unsigned elementSize, InitializationMode mode)
    : m_structure(nullptr)
    , m_length(length)
    , m_byteOffset(0)
    , m_butterfly(nullptr)
{
    ASSERT(!isResizableOrGrowableSharedTypedArrayIncludingDataView(structure->classInfoForCells()));

    if (length <= fastSizeLimit) {
        // Attempt GC allocation.
        void* temp;
        size_t size = sizeOf(length, elementSize);
        temp = vm.primitiveGigacageAuxiliarySpace().allocate(vm, size, nullptr, AllocationFailureMode::ReturnNull);
        if (!temp)
            return;

        m_structure = structure;
        m_vector = VectorType(temp);
        m_mode = FastTypedArray;

        if (mode == ZeroFill) {
            uint64_t* asWords = static_cast<uint64_t*>(vector());
            for (unsigned i = size / sizeof(uint64_t); i--;)
                asWords[i] = 0;
        }
        
        return;
    }

    CheckedSize size = length;
    size *= elementSize;
    if (size.hasOverflowed() || size > MAX_ARRAY_BUFFER_SIZE)
        return;

    void* memory = nullptr;
    if (mode == ZeroFill)
        memory = Gigacage::tryZeroedMalloc(Gigacage::Primitive, size.value());
    else
        memory = Gigacage::tryMalloc(Gigacage::Primitive, size.value());
    m_vector = VectorType(memory);
    if (!m_vector)
        return;

    vm.heap.reportExtraMemoryAllocated(static_cast<JSCell*>(nullptr), size.value());
    
    m_structure = structure;
    m_mode = OversizeTypedArray;
}

JSArrayBufferView::ConstructionContext::ConstructionContext(VM& vm, Structure* structure, RefPtr<ArrayBuffer>&& arrayBuffer, size_t byteOffset, std::optional<size_t> length)
    : m_structure(structure)
    , m_length(length.value_or(0))
    , m_byteOffset(byteOffset)
    , m_mode(WastefulTypedArray)
{
    if (!arrayBuffer->isResizableOrGrowableShared())
        m_mode = WastefulTypedArray;
    else {
        if (arrayBuffer->isGrowableShared())
            m_mode = length ? GrowableSharedWastefulTypedArray : GrowableSharedAutoLengthWastefulTypedArray;
        else
            m_mode = length ? ResizableNonSharedWastefulTypedArray : ResizableNonSharedAutoLengthWastefulTypedArray;
    }
#if ASSERT_ENABLED
    if (!length)
        ASSERT(arrayBuffer->isResizableOrGrowableShared());
    if (JSC::isResizableOrGrowableShared(m_mode))
        ASSERT(isResizableOrGrowableSharedTypedArrayIncludingDataView(structure->classInfoForCells()));
    else
        ASSERT(!isResizableOrGrowableSharedTypedArrayIncludingDataView(structure->classInfoForCells()));
#endif

    m_vector = VectorType(static_cast<uint8_t*>(arrayBuffer->data()) + byteOffset);
    IndexingHeader indexingHeader;
    indexingHeader.setArrayBuffer(arrayBuffer.get());
    m_butterfly = Butterfly::create(vm, nullptr, 0, 0, true, indexingHeader, 0);
}

JSArrayBufferView::ConstructionContext::ConstructionContext(Structure* structure, RefPtr<ArrayBuffer>&& arrayBuffer, size_t byteOffset, std::optional<size_t> length, DataViewTag)
    : m_structure(structure)
    , m_length(length.value_or(0))
    , m_byteOffset(byteOffset)
    , m_mode(DataViewMode)
    , m_butterfly(nullptr)
{
    if (!arrayBuffer->isResizableOrGrowableShared())
        m_mode = DataViewMode;
    else {
        if (arrayBuffer->isGrowableShared())
            m_mode = length ? GrowableSharedDataViewMode : GrowableSharedAutoLengthDataViewMode;
        else
            m_mode = length ? ResizableNonSharedDataViewMode : ResizableNonSharedAutoLengthDataViewMode;
    }
#if ASSERT_ENABLED
    if (!length)
        ASSERT(arrayBuffer->isResizableOrGrowableShared());
    if (JSC::isResizableOrGrowableShared(m_mode))
        ASSERT(isResizableOrGrowableSharedTypedArrayIncludingDataView(structure->classInfoForCells()));
    else
        ASSERT(!isResizableOrGrowableSharedTypedArrayIncludingDataView(structure->classInfoForCells()));
#endif

    m_vector = VectorType(static_cast<uint8_t*>(arrayBuffer->data()) + byteOffset);
}

JSArrayBufferView::JSArrayBufferView(VM& vm, ConstructionContext& context)
    : Base(vm, context.structure(), nullptr)
    , m_length(context.length())
    , m_byteOffset(context.byteOffset())
    , m_mode(context.mode())
{
    // THREADS-INTEGRATE(objectmodel) §10.7 (audited, no guard): this is
    // ConstructionContext::butterfly() — a context-owned fresh allocation for
    // the view under construction, not a JSObject's tagged word. Statically flat.
    setButterfly(vm, context.butterfly());
    m_vector.setWithoutBarrier(context.vector());
}

void JSArrayBufferView::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(is<JSArrayBufferView>(this));
    switch (m_mode) {
    case FastTypedArray:
        return;
    case OversizeTypedArray:
        vm.heap.addFinalizer(this, finalize);
        return;
    case WastefulTypedArray:
    case ResizableNonSharedWastefulTypedArray:
    case ResizableNonSharedAutoLengthWastefulTypedArray:
    case GrowableSharedWastefulTypedArray:
    case GrowableSharedAutoLengthWastefulTypedArray:
        vm.heap.addReference(this, butterfly()->indexingHeader()->arrayBuffer());
        return;
    case DataViewMode:
    case ResizableNonSharedDataViewMode:
    case ResizableNonSharedAutoLengthDataViewMode:
    case GrowableSharedDataViewMode:
    case GrowableSharedAutoLengthDataViewMode:
        ASSERT(!butterfly());
        vm.heap.addReference(this, uncheckedDowncast<JSDataView>(this)->possiblySharedBuffer());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Visitor>
void JSArrayBufferView::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSArrayBufferView* thisObject = uncheckedDowncast<JSArrayBufferView>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(cell, visitor);

    if (thisObject->hasArrayBuffer()) {
        WTF::loadLoadFence();
        ArrayBuffer* buffer = thisObject->possiblySharedBuffer();
        RELEASE_ASSERT(buffer);
        visitor.addOpaqueRoot(buffer);
    }
}

DEFINE_VISIT_CHILDREN(JSArrayBufferView);

ArrayBuffer* JSArrayBufferView::unsharedBuffer()
{
    ArrayBuffer* result = possiblySharedBuffer();
    RELEASE_ASSERT(!result || !result->isShared());
    return result;
}
    
void JSArrayBufferView::finalize(JSCell* cell)
{
    JSArrayBufferView* thisObject = static_cast<JSArrayBufferView*>(cell);

    // This JSArrayBufferView could be an OversizeTypedArray that was converted
    // to a WastefulTypedArray via slowDownAndWasteMemory(). Hence, it is possible
    // to get to this finalizer and found the mode to be WastefulTypedArray.
    ASSERT(thisObject->m_mode == OversizeTypedArray || thisObject->hasArrayBuffer());
    if (thisObject->m_mode == OversizeTypedArray)
        Gigacage::free(Gigacage::Primitive, thisObject->vector());
}

JSArrayBuffer* JSArrayBufferView::unsharedJSBuffer(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (ArrayBuffer* buffer = unsharedBuffer())
        return vm.m_typedArrayController->toJS(globalObject, this->realm(), *buffer);
    scope.throwException(globalObject, createOutOfMemoryError(globalObject));
    return nullptr;
}

JSArrayBuffer* JSArrayBufferView::possiblySharedJSBuffer(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (ArrayBuffer* buffer = possiblySharedBuffer())
        return vm.m_typedArrayController->toJS(globalObject, this->realm(), *buffer);
    scope.throwException(globalObject, createOutOfMemoryError(globalObject));
    return nullptr;
}

void JSArrayBufferView::detachFromArrayBuffer()
{
    {
        Locker locker { cellLock() };
        RELEASE_ASSERT(hasArrayBuffer());
        RELEASE_ASSERT(!isShared());
        if (detachKeepsVector()) [[unlikely]] {
            m_detachedKeepingVector = true;
            WTF::storeStoreFence();
            m_length = 0;
            m_byteOffset = 0;
        } else {
            m_length = 0;
            m_byteOffset = 0;
            m_vector.clear();
        }
    }
    // With threads, the notification can fire a watchpoint under a
    // stop-the-world, and a thread must not hold a cell lock across a stop:
    // another thread that waits for the lock could not park.
    realm()->notifyArrayBufferDetaching();
}

ArrayBuffer* JSArrayBufferView::slowDownAndWasteMemory()
{
    // r47: flag-on, the caller's m_mode==Fast/Oversize observation is a relaxed
    // load that may already be stale by the time we get here (a racing thread
    // may have published the wastage transition); the cell-locked re-check
    // below is the witness instead. Flag-off the original entry assert holds.
    ASSERT(Options::useJSThreads() || !hasArrayBuffer());

    // We play this game because we want this to be callable even from places that
    // don't have access to CallFrame* or the VM, and we only allocate so little
    // memory here that it's not necessary to trigger a GC - just accounting what
    // we have done is good enough. The sort of bizarre exception to the "allocating
    // little memory" is when we transfer a backing buffer into the C heap; this
    // will temporarily get counted towards heap footprint (incorrectly, in the case
    // of adopting an oversize typed array) but we don't GC here anyway. That's
    // almost certainly fine. The worst case is if you created a ton of fast typed
    // arrays, and did nothing but caused all of them to slow down and waste memory.
    // In that case, your memory footprint will double before the GC realizes what's
    // up. But if you do *anything* to trigger a GC watermark check, it will know
    // that you *had* done those allocations and it will GC appropriately.
    Heap* heap = Heap::heap(this);
    VM& vm = heap->vm();
    DeferGCForAWhile deferGC(vm);

    // Flag-on form; the legacy block below stays byte-identical for flag-off.
    // The wastage transition gives the view an IndexingHeader (its
    // ArrayBuffer*), so it dispatches on the tagged butterfly word:
    //   - None / owner Flat: copy-grow into a header-bearing flat butterfly
    //     and publish it with a cell-locked seq_cst CAS. Under the cell lock
    //     the only actors that can move a typed-array view's word without
    //     the lock are lock-free tag flips and foreign installs; a failed CAS
    //     re-dispatches on the fresh word instead of asserting.
    //   - Flat with a foreign TID or SW=1: under the GIL nothing races, so the
    //     copy-grow publishes with the tag preserved. GIL-off the object
    //     model forbids copying a shared-written flat butterfly (a racing
    //     lock-free out-of-line store between the copy and the CAS would be
    //     lost), so the view is converted to a segmented butterfly first -
    //     outside the cell lock, as the conversion protocol requires - and
    //     re-dispatched.
    //   - Segmented: the spine has no header fragment (it was header-less at
    //     conversion). Publish a replacement spine that aliases every
    //     out-of-line fragment and adds one indexed fragment whose slot 0 is
    //     the IndexingHeader, the location existingBufferInButterfly() and
    //     the JIT read for a segmented Wasteful view. Nothing is copied by
    //     value, so a racing store is never lost.
    // Publication order: the header slot is filled before the word is
    // published, except for the adopted Oversize vector, whose ArrayBuffer is
    // created only after the CAS succeeds (a failed attempt must be able to
    // drop its buffer, and an adopted buffer cannot be dropped without
    // freeing the vector). Readers only consult the header after observing
    // m_mode == Wasteful, which is flipped last behind a storeStoreFence that
    // pairs with the loadLoadFence in existingBufferInButterfly().
    // The cell-locked re-check makes the transition idempotent: two threads
    // may both observe a stale m_mode==Fast/Oversize and enter; the loser
    // returns the winner's buffer instead of double-adopting (Oversize) or
    // leaking (Fast) a second ArrayBuffer.
    if (Options::useJSThreads()) [[unlikely]] {
        RefPtr<ArrayBuffer> buffer;
        for (;;) {
            {
                Locker locker { cellLock() };
                if (hasArrayBuffer())
                    return existingBufferInButterfly();
                // Re-checked under the lock; the !hasIndexingHeader witness below
                // is only sound AFTER that re-check (a racing winner's m_mode flip
                // makes hasIndexingHeader() true on the loser's pre-lock probe).
                RELEASE_ASSERT(!hasIndexingHeader());
                RELEASE_ASSERT(m_mode == FastTypedArray || m_mode == OversizeTypedArray);

                Atomic<uint64_t>* word = std::bit_cast<Atomic<uint64_t>*>(butterflyAddress());
                uint64_t expected = word->load(std::memory_order_seq_cst);
                bool segmented = isSegmentedButterfly(expected);
                bool sharedFlat = !segmented && (expected & butterflyPointerMask)
                    && (butterflyTID(expected) != currentButterflyTID() || butterflySharedWrite(expected));
                if (sharedFlat && vm.gilOff()) {
                    buffer = nullptr; // Only ever a Fast copy here; the Oversize adopt happens after publication.
                    // Fall out of the lock scope and convert below.
                } else {
                    if (m_mode == FastTypedArray && !buffer) {
                        buffer = ArrayBuffer::tryCreate(span());
                        if (!buffer)
                            return nullptr;
                    }

                    uint64_t desired;
                    Butterfly* newButterfly = nullptr;
                    ButterflyFragment* headerFragment = nullptr;
                    if (segmented) {
                        ButterflySpine* spine = butterflySpine(expected);
                        spine->tsanConsume();
                        RELEASE_ASSERT(!spine->indexedFragmentCountConcurrent());
                        uint32_t outOfLineFragments = spine->outOfLineFragmentCountConcurrent();
                        auto* newSpine = static_cast<ButterflySpine*>(vm.auxiliarySpace().allocate(
                            vm, ButterflySpine::allocationSize(outOfLineFragments + 1), nullptr, AllocationFailureMode::Assert));
                        butterflyConcurrentStore(&newSpine->outOfLineFragmentCount, outOfLineFragments);
                        butterflyConcurrentStore(&newSpine->indexedFragmentCount, 1u);
                        butterflyConcurrentStore(&newSpine->vectorLength, 0u);
                        butterflyConcurrentStore(&newSpine->spineEpoch, butterflyConcurrentLoad(&spine->spineEpoch) + 1);
                        butterflyConcurrentStore(&newSpine->aliasedAllocationBase, butterflyConcurrentLoad(&spine->aliasedAllocationBase));
                        butterflyConcurrentStore(&newSpine->aliasedAllocationSize, butterflyConcurrentLoad(&spine->aliasedAllocationSize));
                        for (uint32_t j = 0; j < outOfLineFragments; ++j)
                            butterflyConcurrentStore(&newSpine->fragments()[j], spine->outOfLineFragment(j));
                        headerFragment = static_cast<ButterflyFragment*>(
                            vm.auxiliarySpace().allocate(vm, sizeof(ButterflyFragment), nullptr, AllocationFailureMode::Assert));
                        for (size_t slotIndex = 0; slotIndex < butterflyFragmentSlots; ++slotIndex)
                            headerFragment->slots[slotIndex].clear();
                        std::bit_cast<IndexingHeader*>(&headerFragment->slots[0])->setArrayBuffer(buffer.get());
                        butterflyConcurrentStore(&newSpine->fragments()[outOfLineFragments], headerFragment);
                        newSpine->validateConsistency();
                        newSpine->tsanPublish();
                        desired = encodeSegmentedButterfly(newSpine);
                    } else {
                        StructureID id = structureIDConcurrently();
                        if (id.isNuked())
                            continue; // A racing publication is mid-flight; re-dispatch on the settled state.
                        Structure* structure = id.decode();
                        newButterfly = Butterfly::createOrGrowArrayRight(
                            untaggedButterfly(expected), vm, this, structure,
                            structure->outOfLineCapacity(), false, 0, 0);
                        newButterfly->indexingHeader()->setArrayBuffer(buffer.get());
                        // None word => N3 first install (currentTID, SW=0); otherwise
                        // preserve TID/SW verbatim - never re-stamp an installer's tag.
                        desired = (expected & butterflyPointerMask)
                            ? encodeButterfly(newButterfly, butterflyTID(expected), butterflySharedWrite(expected))
                            : encodeButterfly(newButterfly, currentButterflyTID(), false);
                    }
                    WTF::storeStoreFence(); // Header slot and spine contents before the butterfly word.

                    uint64_t observed = word->compareExchangeStrong(expected, desired, std::memory_order_seq_cst);
                    if (observed != expected)
                        continue; // The word moved under us; the allocations drop unreferenced.
                    vm.writeBarrier(this);

                    if (!buffer) {
                        ASSERT(m_mode == OversizeTypedArray);
                        buffer = ArrayBuffer::createAdopted(span());
                        if (segmented)
                            std::bit_cast<IndexingHeader*>(&headerFragment->slots[0])->setArrayBuffer(buffer.get());
                        else
                            newButterfly->indexingHeader()->setArrayBuffer(buffer.get());
                    }

                    m_vector.setWithoutBarrier(buffer->data());
                    WTF::storeStoreFence(); // Butterfly, header slot and vector before m_mode (pairs with existingBufferInButterfly's loadLoadFence).
                    m_mode = WastefulTypedArray;
                    break;
                }
            }
            // GIL-off, shared-written flat word: convert to segmented outside the
            // cell lock (the conversion takes it itself and may stop the world),
            // then re-dispatch. A null return means the conversion fired the
            // thread-local sets or lost a race and asks for a restart; the next
            // pass re-classifies the word either way.
            convertToSegmentedButterfly(vm, this, nullptr, nullptr, invalidOffset, JSValue());
        }
        heap->addReference(this, buffer.get());
        return buffer.unsafeGet();
    }

    RELEASE_ASSERT(!hasIndexingHeader());
    Structure* structure = this->structure();

    RefPtr<ArrayBuffer> buffer;

    switch (m_mode) {
    case FastTypedArray: {
        buffer = ArrayBuffer::tryCreate(span());
        if (!buffer)
            return nullptr;
        break;
    }

    case OversizeTypedArray: {
        // FIXME: consider doing something like "subtracting" from extra memory
        // cost, since right now this case will cause the GC to think that we reallocated
        // the whole buffer.
        buffer = ArrayBuffer::createAdopted(span());
        break;
    }

    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    RELEASE_ASSERT(buffer);
    // Don't create bufferfly until we know we have an ArrayBuffer.
    setButterfly(vm, Butterfly::createOrGrowArrayRight(
        butterfly(), vm, this, structure,
        structure->outOfLineCapacity(), false, 0, 0));

    {
        Locker locker { cellLock() };
        butterfly()->indexingHeader()->setArrayBuffer(buffer.get());
        m_vector.setWithoutBarrier(buffer->data());
        WTF::storeStoreFence();
        m_mode = WastefulTypedArray; // There is no possibility that FastTypedArray or OversizeTypedArray becomes resizable ones since resizable ones do not start with FastTypedArray or OversizeTypedArray.
    }
    heap->addReference(this, buffer.get());

    return buffer.unsafeGet();
}

// Allocates the full-on native buffer and moves data into the C heap if
// necessary. Note that this never allocates in the GC heap.
RefPtr<ArrayBufferView> JSArrayBufferView::possiblySharedImpl()
{
    ArrayBuffer* buffer = possiblySharedBuffer();
    if (!buffer)
        return nullptr;
    size_t byteOffset = this->byteOffsetRaw();
    size_t length = this->lengthRaw();
    switch (type()) {
#define FACTORY(type) \
    case type ## ArrayType: \
        return type ## Array::wrappedAs(*buffer, byteOffset, isAutoLength() ? std::nullopt : std::optional { length });
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(FACTORY)
#undef FACTORY
    case DataViewType:
        return DataView::wrappedAs(*buffer, byteOffset, isAutoLength() ? std::nullopt : std::optional { length });
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

bool JSArrayBufferView::isIteratorProtocolFastAndNonObservable()
{
    // Excluding DataView.
    if (!isTypedArrayType(type()))
        return false;

    JSGlobalObject* globalObject = this->realm();
    TypedArrayType typedArrayType = JSC::typedArrayType(type());
    if (!globalObject->isTypedArrayPrototypeIteratorProtocolFastAndNonObservable(typedArrayType))
        return false;

    VM& vm = globalObject->vm();
    Structure* structure = this->structure();
    // This is the fast case. Many TypedArrays will be an original typed array structure.
    if (globalObject->isOriginalTypedArrayStructure(structure, true) || globalObject->isOriginalTypedArrayStructure(structure, false))
        return true;

    if (getPrototypeDirect() != globalObject->typedArrayPrototype(typedArrayType))
        return false;

    if (getDirectOffset(vm, vm.propertyNames->iteratorSymbol) != invalidOffset)
        return false;

    return true;
}

} // namespace JSC

namespace WTF {

using namespace JSC;

void printInternal(PrintStream& out, TypedArrayMode mode)
{
    switch (mode) {
    case FastTypedArray:
        out.print("FastTypedArray");
        return;
    case OversizeTypedArray:
        out.print("OversizeTypedArray");
        return;
    case WastefulTypedArray:
        out.print("WastefulTypedArray");
        return;
    case ResizableNonSharedWastefulTypedArray:
        out.print("ResizableNonSharedWastefulTypedArray");
        return;
    case ResizableNonSharedAutoLengthWastefulTypedArray:
        out.print("ResizableNonSharedAutoLengthWastefulTypedArray");
        return;
    case GrowableSharedWastefulTypedArray:
        out.print("GrowableSharedWastefulTypedArray");
        return;
    case GrowableSharedAutoLengthWastefulTypedArray:
        out.print("GrowableSharedAutoLengthWastefulTypedArray");
        return;
    case DataViewMode:
        out.print("DataViewMode");
        return;
    case ResizableNonSharedDataViewMode:
        out.print("ResizableNonSharedDataViewMode");
        return;
    case ResizableNonSharedAutoLengthDataViewMode:
        out.print("ResizableNonSharedAutoLengthDataViewMode");
        return;
    case GrowableSharedDataViewMode:
        out.print("GrowableSharedDataViewMode");
        return;
    case GrowableSharedAutoLengthDataViewMode:
        out.print("GrowableSharedAutoLengthDataViewMode");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace WTF

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
