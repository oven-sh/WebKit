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

#pragma once

#include "GenericArgumentsImpl.h"
#include "JSCInlines.h"
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// =============================================================================
// UNGIL annex N7 RESOLVED-3 (AUD1.N3, BINDING; U-T8b) — the lazy
// modified-arguments bitmap is CAS-PUBLISH state.
//
// m_modifiedArgumentsDescriptor is a lazily-allocated aux bitmap readable by
// FOREIGN threads: DFG GetFromArguments / the inlined
// offsetOfModifiedArgumentsDescriptor null-check load it on any thread that
// can see a shared arguments object, while the OWNER's first
// `delete arguments[i]` / defineOwnProperty allocates and publishes it. OM
// annex 15.6 audited only the butterfly() callers of this file — this state
// is ruled HERE, per N7 RESOLVED-3:
//   - WRITER: allocate + fill the bitmap COMPLETELY, then release-CAS the
//     single pointer word; CAS losers discard (the loser's aux allocation is
//     unreferenced garbage the GC reclaims).
//   - READERS: load-acquire the pointer word (slow paths below); the
//     tier-inlined null-check stays as-is (address-dependent load, jit F2
//     shape — a reader either sees null and takes the generic path, or sees
//     the released pointer and therefore the filled bitmap).
//   - Per-index bit WRITES after publication (setModifiedArgumentDescriptor)
//     are single-byte stores ordered by the OM property-slot rules of the
//     put/defineOwnProperty that carries them; cross-thread visibility of an
//     individual bit is SAB-grade (racy-tolerated) exactly like the property
//     write it shadows.
// Flag-off / GIL-on identity: under the GIL the CAS cannot lose and the
// acquire loads are plain-load-equivalent; no behavior change.
//
// The companion rulings RESOLVED-3 (DirectArguments::m_mappedArguments
// overrideThings publication) and RESOLVED-4 (ScopedArguments::
// m_overrodeThings / ClonedArguments::m_callee release-after-puts flags)
// live in DirectArguments.cpp / ScopedArguments.cpp / ClonedArguments.cpp —
// OUTSIDE U-T8b's owned file set; recorded OPEN for their owning slices.
// =============================================================================

namespace GenericArgumentsImplInternal {

template<typename PtrType>
ALWAYS_INLINE bool* acquireLoadDescriptor(const PtrType& field)
{
    static_assert(sizeof(PtrType) == sizeof(bool*), "the descriptor is published as one pointer word");
    return WTF::atomicLoad(std::bit_cast<bool**>(const_cast<PtrType*>(&field)), std::memory_order_acquire);
}

} // namespace GenericArgumentsImplInternal

template<typename Type>
template<typename Visitor>
void GenericArgumentsImpl<Type>::visitChildrenImpl(JSCell* thisCell, Visitor& visitor)
{
    Type* thisObject = static_cast<Type*>(thisCell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisCell, visitor);

    // Concurrent markers read the publication word unfenced: a marker either
    // sees null (pre-publication; the loser's/winner's in-flight buffer is
    // kept alive by the allocating thread's conservative roots, heap I7) or
    // the published pointer. The bitmap holds no cells; markAuxiliary needs
    // no contents ordering.
    if (auto* pointer = thisObject->m_modifiedArgumentsDescriptor.getUnsafe())
        visitor.markAuxiliary(pointer);
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(template<typename Type>, GenericArgumentsImpl<Type>);

template<typename Type>
bool GenericArgumentsImpl<Type>::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName ident, PropertySlot& slot)
{
    Type* thisObject = uncheckedDowncast<Type>(object);
    VM& vm = globalObject->vm();

    if (!thisObject->overrodeThings()) {
        if (ident == vm.propertyNames->length) {
            slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontEnum), jsNumber(thisObject->internalLength()));
            return true;
        }
        if (ident == vm.propertyNames->callee) {
            slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontEnum), thisObject->callee());
            return true;
        }
        if (ident == vm.propertyNames->iteratorSymbol) {
            slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontEnum), thisObject->realm()->arrayProtoValuesFunction());
            return true;
        }
    }

    if (std::optional<uint32_t> index = parseIndex(ident))
        return GenericArgumentsImpl<Type>::getOwnPropertySlotByIndex(thisObject, globalObject, *index, slot);

    return Base::getOwnPropertySlot(thisObject, globalObject, ident, slot);
}

template<typename Type>
bool GenericArgumentsImpl<Type>::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    Type* thisObject = uncheckedDowncast<Type>(object);

    if (!thisObject->isModifiedArgumentDescriptor(index) && thisObject->isMappedArgument(index)) {
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), thisObject->getIndexQuickly(index));
        return true;
    }

    bool result = Base::getOwnPropertySlotByIndex(object, globalObject, index, slot);

    if (thisObject->isMappedArgument(index)) {
        ASSERT(result);
        slot.setValue(thisObject, slot.attributes(), thisObject->getIndexQuickly(index));
        return true;
    }

    return result;
}

template<typename Type>
void GenericArgumentsImpl<Type>::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& array, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    Type* thisObject = uncheckedDowncast<Type>(object);

    if (array.includeStringProperties()) {
        for (unsigned i = 0; i < thisObject->internalLength(); ++i) {
            if (!thisObject->isMappedArgument(i))
                continue;
            array.add(Identifier::from(vm, i));
        }
        thisObject->getOwnIndexedPropertyNames(globalObject, array, mode);
    }

    if (mode == DontEnumPropertiesMode::Include && !thisObject->overrodeThings()) {
        array.add(vm.propertyNames->length);
        array.add(vm.propertyNames->callee);
        array.add(vm.propertyNames->iteratorSymbol);
    }
    thisObject->getOwnNonIndexPropertyNames(globalObject, array, mode);
}

template<typename Type>
bool GenericArgumentsImpl<Type>::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName ident, JSValue value, PutPropertySlot& slot)
{
    Type* thisObject = uncheckedDowncast<Type>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!thisObject->overrodeThings()
        && (ident == vm.propertyNames->length
            || ident == vm.propertyNames->callee
            || ident == vm.propertyNames->iteratorSymbol)) {
        thisObject->overrideThings(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        PutPropertySlot dummy = slot; // This put is not cacheable, so we shadow the slot that was given to us.
        RELEASE_AND_RETURN(scope, Base::put(thisObject, globalObject, ident, value, dummy));
    }

    // https://tc39.github.io/ecma262/#sec-arguments-exotic-objects-set-p-v-receiver
    // Fall back to the OrdinarySet when the receiver is altered from the thisObject.
    if (slot.thisValue() != thisObject) [[unlikely]]
        RELEASE_AND_RETURN(scope, Base::put(thisObject, globalObject, ident, value, slot));

    std::optional<uint32_t> index = parseIndex(ident);
    if (index && thisObject->isMappedArgument(index.value())) {
        thisObject->setIndexQuickly(vm, index.value(), value);
        return true;
    }

    RELEASE_AND_RETURN(scope, Base::put(thisObject, globalObject, ident, value, slot));
}

template<typename Type>
bool GenericArgumentsImpl<Type>::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    Type* thisObject = uncheckedDowncast<Type>(cell);
    VM& vm = globalObject->vm();

    if (thisObject->isMappedArgument(index)) {
        thisObject->setIndexQuickly(vm, index, value);
        return true;
    }

    return Base::putByIndex(cell, globalObject, index, value, shouldThrow);
}

template<typename Type>
bool GenericArgumentsImpl<Type>::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName ident, DeletePropertySlot& slot)
{
    Type* thisObject = uncheckedDowncast<Type>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!thisObject->overrodeThings()
        && (ident == vm.propertyNames->length
            || ident == vm.propertyNames->callee
            || ident == vm.propertyNames->iteratorSymbol)) {
        thisObject->overrideThings(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }

    if (std::optional<uint32_t> index = parseIndex(ident))
        RELEASE_AND_RETURN(scope, GenericArgumentsImpl<Type>::deletePropertyByIndex(thisObject, globalObject, *index));

    RELEASE_AND_RETURN(scope, Base::deleteProperty(thisObject, globalObject, ident, slot));
}

template<typename Type>
bool GenericArgumentsImpl<Type>::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Type* thisObject = uncheckedDowncast<Type>(cell);

    bool propertyMightBeInJSObjectStorage = thisObject->isModifiedArgumentDescriptor(index) || !thisObject->isMappedArgument(index);
    bool deletedProperty = true;
    if (propertyMightBeInJSObjectStorage) {
        deletedProperty = Base::deletePropertyByIndex(cell, globalObject, index);
        RETURN_IF_EXCEPTION(scope, true);
    }

    if (deletedProperty) {
        // Deleting an indexed property unconditionally unmaps it.
        if (thisObject->isMappedArgument(index)) {
            // We need to check that the property was mapped so we don't write to random memory.
            thisObject->unmapArgument(globalObject, index);
            RETURN_IF_EXCEPTION(scope, true);
        }
        thisObject->setModifiedArgumentDescriptor(globalObject, index);
        RETURN_IF_EXCEPTION(scope, true);
    }

    return deletedProperty;
}

// https://tc39.es/ecma262/#sec-arguments-exotic-objects-defineownproperty-p-desc
template<typename Type>
bool GenericArgumentsImpl<Type>::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName ident, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    Type* thisObject = uncheckedDowncast<Type>(object);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (ident == vm.propertyNames->length
        || ident == vm.propertyNames->callee
        || ident == vm.propertyNames->iteratorSymbol) {
        thisObject->overrideThingsIfNecessary(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    } else if (std::optional<uint32_t> optionalIndex = parseIndex(ident)) {
        uint32_t index = optionalIndex.value();
        bool isMapped = thisObject->isMappedArgument(index);
        PropertyDescriptor newDescriptor = descriptor;

        if (isMapped) {
            if (thisObject->isModifiedArgumentDescriptor(index)) {
                if (!descriptor.value() && descriptor.writablePresent() && !descriptor.writable())
                    newDescriptor.setValue(thisObject->getIndexQuickly(index));
            } else
                thisObject->putDirectIndex(globalObject, index, thisObject->getIndexQuickly(index));

            scope.assertNoException();
        }

        bool status = thisObject->defineOwnIndexedProperty(globalObject, index, newDescriptor, shouldThrow);
        RETURN_IF_EXCEPTION(scope, false);
        if (!status) {
            ASSERT(!isMapped || thisObject->isModifiedArgumentDescriptor(index));
            return false;
        }

        thisObject->setModifiedArgumentDescriptor(globalObject, index);
        RETURN_IF_EXCEPTION(scope, false);

        if (isMapped) {
            if (descriptor.isAccessorDescriptor())
                thisObject->unmapArgument(globalObject, index);
            else {
                if (descriptor.value())
                    thisObject->setIndexQuickly(vm, index, descriptor.value());
                if (descriptor.writablePresent() && !descriptor.writable())
                    thisObject->unmapArgument(globalObject, index);
            }

            RETURN_IF_EXCEPTION(scope, false);
        }

        return true;
    }

    RELEASE_AND_RETURN(scope, Base::defineOwnProperty(object, globalObject, ident, descriptor, shouldThrow));
}

template<typename Type>
void GenericArgumentsImpl<Type>::initModifiedArgumentsDescriptor(JSGlobalObject* globalObject, unsigned argsLength)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // AUD1.N3: GIL-off, the old RELEASE_ASSERT(!m_modifiedArgumentsDescriptor)
    // precondition becomes the CAS failure arm below (a racing thread may
    // publish between the caller's check and here). Flag-off/GIL-on the CAS
    // cannot lose, so the failure arm re-asserts the old precondition there
    // (see the post-CAS check) — the single-threaded crash-on-misuse
    // guarantee is preserved, not deleted.
    if (!argsLength)
        return;

    void* backingStore = vm.gigacageAuxiliarySpace(m_modifiedArgumentsDescriptor.kind).allocate(vm, WTF::roundUpToMultipleOf<8>(argsLength), nullptr, AllocationFailureMode::ReturnNull);
    if (!backingStore) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    bool* modifiedArguments = static_cast<bool*>(backingStore);
    // Fill BEFORE publication. (The pre-ungil code published first and
    // zeroed after — single-thread-correct, but torn for a foreign acquire
    // reader; AUD1.N3 makes fill-then-release-CAS the rule.)
    for (unsigned i = argsLength; i--;)
        modifiedArguments[i] = false;

    static_assert(sizeof(ModifiedArgumentsPtr) == sizeof(bool*), "release-CAS publication treats the caged barrier as one pointer word");
    bool* prior = WTF::atomicCompareExchangeStrong(std::bit_cast<bool**>(&m_modifiedArgumentsDescriptor), static_cast<bool*>(nullptr), modifiedArguments, std::memory_order_release);
    if (prior) {
        // gilOff: legitimate AUD1.N3 CAS-loser arm — a racing thread
        // published a fully-filled bitmap; ours is unreferenced aux garbage
        // the GC reclaims. Flag-off/GIL-on: the CAS cannot lose to a race,
        // so a non-null prior is the OLD RELEASE_ASSERT's precondition
        // violation (a direct caller bypassing
        // initModifiedArgumentsDescriptorIfNecessary) — keep its fail-stop.
        RELEASE_ASSERT(vm.gilOff());
        return;
    }
    // The raw-word publish bypassed AuxiliaryBarrier::set's write barrier;
    // re-issue it (the bitmap is aux storage owned by this cell).
    vm.writeBarrier(this);
}

template<typename Type>
void GenericArgumentsImpl<Type>::initModifiedArgumentsDescriptorIfNecessary(JSGlobalObject* globalObject, unsigned argsLength)
{
    // AUD1.N3 reader side: load-acquire pairs with the writer's release-CAS,
    // so a non-null observation implies a fully-filled bitmap.
    if (!GenericArgumentsImplInternal::acquireLoadDescriptor(m_modifiedArgumentsDescriptor))
        initModifiedArgumentsDescriptor(globalObject, argsLength);
}

template<typename Type>
void GenericArgumentsImpl<Type>::setModifiedArgumentDescriptor(JSGlobalObject* globalObject, unsigned index, unsigned length)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    initModifiedArgumentsDescriptorIfNecessary(globalObject, length);
    RETURN_IF_EXCEPTION(scope, void());
    if (index < length) {
        // The acquire load is the ordering device (pairs with the
        // release-CAS); the access itself goes through the caged accessor so
        // the Gigacage rebase/hardening is preserved. The word transitions
        // null -> pointer exactly once, so the accessor's own re-read cannot
        // observe an older value than the acquire load did.
        ASSERT(GenericArgumentsImplInternal::acquireLoadDescriptor(m_modifiedArgumentsDescriptor)); // length > 0 => the init above published (or threw, returned above).
        WTF::atomicStore(&m_modifiedArgumentsDescriptor.at(index), true, std::memory_order_relaxed); // Per-bit store: SAB-grade racy-tolerated (AUD1.N3 banner); relaxed atomic so the race is defined.
    }
}

template<typename Type>
bool GenericArgumentsImpl<Type>::isModifiedArgumentDescriptor(unsigned index, unsigned length)
{
    // Acquire null-check (ordering device; see setModifiedArgumentDescriptor),
    // then the caged accessor for the hardened read.
    if (!GenericArgumentsImplInternal::acquireLoadDescriptor(m_modifiedArgumentsDescriptor))
        return false;
    if (index < length)
        return WTF::atomicLoad(&m_modifiedArgumentsDescriptor.at(index), std::memory_order_relaxed); // Relaxed per-bit read (see setModifiedArgumentDescriptor).
    return false;
}

template<typename Type>
void GenericArgumentsImpl<Type>::copyToArguments(JSGlobalObject* globalObject, JSValue* firstElementDest, unsigned offset, unsigned length)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Type* thisObject = static_cast<Type*>(this);
    for (unsigned i = 0; i < length; ++i) {
        if (thisObject->isMappedArgument(i + offset))
            firstElementDest[i] = thisObject->getIndexQuickly(i + offset);
        else {
            firstElementDest[i] = get(globalObject, i + offset);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
