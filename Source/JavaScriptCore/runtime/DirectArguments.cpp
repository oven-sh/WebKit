/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
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
#include "DirectArguments.h"

#include "CodeBlock.h"
#include "GenericArgumentsImplInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(DirectArguments);

const ClassInfo DirectArguments::s_info = { "Arguments"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(DirectArguments) };

uint32_t DirectArguments::length(JSGlobalObject* globalObject) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // overrodeThings() (not a raw m_mappedArguments test) so a foreign reader
    // acquires before reading the materialized length property (RESOLVED-3).
    if (overrodeThings()) [[unlikely]] {
        JSValue value = get(globalObject, vm.propertyNames->length);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, value.toUInt32(globalObject));
    }
    return internalLength();
}

DirectArguments::DirectArguments(VM& vm, Structure* structure, unsigned length, unsigned capacity)
    : GenericArgumentsImpl(vm, structure)
{
    // THREADS/TSAN: relaxed stores — the cell address may be GC-recycled, so
    // even constructor writes must be atomic to pair with stale readers'
    // relaxed loads of these words.
    WTF::atomicStore(&m_length, length, std::memory_order_relaxed);
    WTF::atomicStore(&m_minCapacity, capacity, std::memory_order_relaxed);
    // When we construct the object from C++ code, we expect the capacity to be at least as large as
    // length. JIT-allocated DirectArguments objects play evil tricks, though.
    ASSERT(capacity >= length);
}

DirectArguments* DirectArguments::createUninitialized(
    VM& vm, Structure* structure, unsigned length, unsigned capacity)
{
    DirectArguments* result =
        new (NotNull, allocateCell<DirectArguments>(vm, allocationSize(capacity)))
        DirectArguments(vm, structure, length, capacity);
    result->finishCreation(vm);
    return result;
}

DirectArguments* DirectArguments::create(VM& vm, Structure* structure, unsigned length, unsigned capacity)
{
    DirectArguments* result = createUninitialized(vm, structure, length, capacity);
    
    for (unsigned i = capacity; i--;)
        result->storage()[i].setUndefined();
    
    return result;
}

DirectArguments* DirectArguments::createByCopying(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    
    unsigned length = callFrame->argumentCount();
    unsigned capacity = std::max(length, static_cast<unsigned>(callFrame->codeBlock()->numParameters() - 1));
    DirectArguments* result = createUninitialized(
        vm, globalObject->directArgumentsStructure(), length, capacity);
    
    for (unsigned i = capacity; i--;)
        result->storage()[i].set(vm, result, callFrame->getArgumentUnsafe(i));
    
    result->setCallee(vm, uncheckedDowncast<JSFunction>(callFrame->jsCallee()));
    
    return result;
}

size_t DirectArguments::estimatedSize(JSCell* cell, VM& vm)
{
    DirectArguments* thisObject = uncheckedDowncast<DirectArguments>(cell);
    size_t mappedArgumentsSize = thisObject->m_mappedArguments ? thisObject->mappedArgumentsSize() * sizeof(bool) : 0;
    size_t modifiedArgumentsSize = thisObject->m_modifiedArgumentsDescriptor ? thisObject->internalLength() * sizeof(bool) : 0;
    return Base::estimatedSize(cell, vm) + mappedArgumentsSize + modifiedArgumentsSize;
}

template<typename Visitor>
void DirectArguments::visitChildrenImpl(JSCell* thisCell, Visitor& visitor)
{
    DirectArguments* thisObject = static_cast<DirectArguments*>(thisCell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    GenericArgumentsImpl::visitChildren(thisCell, visitor); // Including Base::visitChildren.

    visitor.appendValues(thisObject->storage(), std::max(thisObject->internalLength(), WTF::atomicLoad(&thisObject->m_minCapacity, std::memory_order_relaxed)));
    visitor.append(thisObject->m_callee);

    if (thisObject->m_mappedArguments)
        visitor.markAuxiliary(thisObject->m_mappedArguments.get());
}

DEFINE_VISIT_CHILDREN(DirectArguments);

Structure* DirectArguments::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(DirectArgumentsType, StructureFlags), info());
}

void DirectArguments::overrideThings(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // THREADS (AUD1.N3 RESOLVED-3): GIL-off, a foreign overrideThings() can
    // win between the caller's unfenced overrideThingsIfNecessary() check and
    // here — race-tolerate it instead of RELEASE_ASSERTing (losing a
    // COMPLETED race is fine: the winner's puts plus its release-published
    // bitmap are exactly what we needed; overrodeThings() supplied the
    // acquire). GIL-on (including useJSThreads=0) no other mutator can run in
    // that window, so the old invariant stays enforced verbatim.
    if (!vm.gilOff())
        RELEASE_ASSERT(!m_mappedArguments);
    else if (overrodeThings())
        return;

    // GIL-off, two threads can both reach here and run these puts
    // concurrently before the CAS below picks the bitmap winner. That is
    // deliberate, per the RESOLVED-3 ruling: "the materialize-properties half
    // follows OM property rules" — same-key concurrent adds are serialized by
    // the SPEC-objectmodel transition/put protocol (no lost properties), and
    // every racer computes IDENTICAL values (internalLength() and m_callee
    // are creation-frozen here; arrayProtoValuesFunction is a per-realm
    // singleton eagerly initialized under useJSThreads(), see
    // JSGlobalObject.cpp), so the racing stores are idempotent.
    putDirect(vm, vm.propertyNames->length, jsNumber(internalLength()), static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirect(vm, vm.propertyNames->callee, m_callee.get(), static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirect(vm, vm.propertyNames->iteratorSymbol, globalObject->arrayProtoValuesFunction(), static_cast<unsigned>(PropertyAttribute::DontEnum));

    void* backingStore = vm.gigacageAuxiliarySpace(m_mappedArguments.kind).allocate(vm, mappedArgumentsSize(), nullptr, AllocationFailureMode::ReturnNull);
    if (!backingStore) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    bool* overrides = static_cast<bool*>(backingStore);
    // THREADS (RESOLVED-3 companion): fill the bitmap COMPLETELY before
    // publishing the pointer word — concurrent readers (isMappedArgument on a
    // shared arguments object) take the pointer's address dependency to the
    // bits; relaxed atomic per-bit stores keep the recycled-address races
    // defined. The fence orders fill before the publication store.
    for (unsigned i = internalLength(); i--;)
        WTF::atomicStore(&overrides[i], false, std::memory_order_relaxed);
    WTF::storeStoreFence();
    if (vm.gilOff()) [[unlikely]] {
        // RESOLVED-3 CAS-PUBLISH: allocate + fill complete (above), CAS the
        // pointer in, losers discard. A losing racer's bitmap is unreferenced
        // gigacage auxiliary memory — the GC reclaims it. The winner runs the
        // write barrier the plain set() would have run; the loser's
        // subsequent at(index) accesses re-load the pointer and land in the
        // winner's bitmap (null -> non-null happens exactly once).
        static_assert(sizeof(MappedArguments) == sizeof(bool*));
        bool* prior = WTF::atomicCompareExchangeStrong(std::bit_cast<bool**>(&m_mappedArguments), static_cast<bool*>(nullptr), overrides);
        if (prior)
            return;
        vm.writeBarrier(this);
        return;
    }
    m_mappedArguments.set(vm, this, overrides);
}

void DirectArguments::overrideThingsIfNecessary(JSGlobalObject* globalObject)
{
    // overrodeThings() (not a raw m_mappedArguments test) so the
    // already-overridden path acquires before callers touch the materialized
    // properties (RESOLVED-3 reader-acquire half).
    if (!overrodeThings())
        overrideThings(globalObject);
}

void DirectArguments::unmapArgument(JSGlobalObject* globalObject, unsigned index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    overrideThingsIfNecessary(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

    WTF::atomicStore(&m_mappedArguments.at(index), true, std::memory_order_relaxed); // THREADS: SAB-grade per-bit store (see isMappedArgument).
}

void DirectArguments::copyToArguments(JSGlobalObject* globalObject, JSValue* firstElementDest, unsigned offset, unsigned length)
{
    // overrodeThings() (not a raw m_mappedArguments test): the overridden
    // path reads materialized properties via GenericArgumentsImpl, which are
    // not address-dependent on the bitmap pointer (RESOLVED-3 acquire).
    if (!overrodeThings()) {
        unsigned limit = std::min(length + offset, internalLength());
        unsigned i;
        for (i = offset; i < limit; ++i)
            firstElementDest[i - offset] = storage()[i].get();
        for (; i < length; ++i)
            firstElementDest[i - offset] = get(globalObject, i);
        return;
    }

    GenericArgumentsImpl::copyToArguments(globalObject, firstElementDest, offset, length);
}

unsigned DirectArguments::mappedArgumentsSize()
{
    // We always allocate something; in the relatively uncommon case of overriding an empty argument we
    // still allocate so that m_mappedArguments is non-null. We use that to indicate that the other properties
    // (length, etc) are overridden.
    return WTF::roundUpToMultipleOf<8>(internalLength() ? internalLength() : 1);
}

bool DirectArguments::isIteratorProtocolFastAndNonObservable()
{
    Structure* structure = this->structure();
    JSGlobalObject* globalObject = structure->realm();
    if (!globalObject->isArgumentsPrototypeIteratorProtocolFastAndNonObservable())
        return false;

    if (m_mappedArguments) [[unlikely]]
        return false;

    if (structure->didTransition())
        return false;

    return true;
}

JSArray* DirectArguments::fastSlice(JSGlobalObject* globalObject, DirectArguments* arguments, uint64_t startIndex, uint64_t count)
{
    if (count >= MIN_SPARSE_ARRAY_INDEX)
        return nullptr;

    if (arguments->m_mappedArguments) [[unlikely]]
        return nullptr;

    if (startIndex + count > arguments->internalLength())
        return nullptr;

    Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    if (hasAnyArrayStorage(resultStructure->indexingType())) [[unlikely]]
        return nullptr;

    ObjectInitializationScope scope(globalObject->vm());
    JSArray* resultArray = JSArray::tryCreateUninitializedRestricted(scope, resultStructure, static_cast<uint32_t>(count));
    if (!resultArray) [[unlikely]]
        return nullptr;

    ASSERT(!resultArray->mayBeSegmentedButterfly()); // THREADS-INTEGRATE(objectmodel) §10.7 [assert-only]: fresh private array
    auto& resultButterfly = *resultArray->butterfly();
    gcSafeMemcpy(resultButterfly.contiguous().data(), arguments->storage() + startIndex, sizeof(JSValue) * static_cast<uint32_t>(count));

    ASSERT(resultButterfly.publicLength() == count);
    return resultArray;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
