/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "JSObject.h"

#include "ButterflyInlines.h"
#include "JSCInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

static bool canHaveElementsStartOffset(JSObject* object, Structure* structure)
{
    if (object->type() != ArrayType && object->type() != DerivedArrayType)
        return false;
    if (structure->hijacksIndexingHeader() || structure->hasImmutableProperties())
        return false;
    switch (structure->indexingMode()) {
    case ArrayWithInt32:
    case ArrayWithDouble:
    case ArrayWithContiguous:
        return true;
    default:
        return false;
    }
}

bool JSObject::tryReallocateWithElementsStartOffset(VM& vm, unsigned vectorLength, unsigned numberOfFreeSlots)
{
    ASSERT(numberOfFreeSlots);
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    size_t propertyCapacity = oldStructure->outOfLineCapacity();
    if (vectorLength > MAX_STORAGE_VECTOR_LENGTH)
        return false;

    Structure* newStructure = oldStructure;
    DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
    if (!oldStructure->hasElementsStartOffset()) {
        if (!canHaveElementsStartOffset(this, oldStructure))
            return false;
        newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateElementsStartOffset, &deferred);
    }

    Butterfly* oldButterfly = this->butterfly();
    unsigned numberOfCopiedElements = std::min(oldButterfly->vectorLength(), vectorLength);
    Butterfly* newButterfly = Butterfly::tryCreateUninitialized(vm, this, numberOfFreeSlots, propertyCapacity, true, vectorLength * sizeof(EncodedJSValue));
    if (!newButterfly)
        return false;

    memcpy(newButterfly->base(0, propertyCapacity), oldButterfly->base(0, propertyCapacity), Butterfly::totalSize(0, propertyCapacity, true, numberOfCopiedElements * sizeof(EncodedJSValue)));
    newButterfly->setVectorLength(vectorLength);
    Butterfly::clearRange(oldStructure->indexingType(), newButterfly, numberOfCopiedElements, vectorLength);
    newButterfly->setPublicLength(std::min(oldButterfly->publicLength(), vectorLength));
    newButterfly->indexingHeader()->elementsStartOffset(propertyCapacity) = numberOfFreeSlots;

    if (newStructure != oldStructure) {
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
        return true;
    }

    Locker locker { cellLock() };
    WTF::storeStoreFence();
    butterflyRef().set(vm, this, newButterfly);
    return true;
}

bool JSObject::tryMoveElementsStartForward(VM& vm, unsigned count)
{
    ASSERT(count);
    if (!canHaveElementsStartOffset(this, structure()))
        return false;
    // The out-of-line properties lie in front of the elements and move with their start.
    size_t propertyCapacity = structure()->outOfLineCapacity();
    if (propertyCapacity >= butterfly()->publicLength() - count)
        return false;
    if (!structure()->hasElementsStartOffset() && !tryReallocateWithElementsStartOffset(vm, butterfly()->vectorLength(), 1))
        return false;

    Butterfly* oldButterfly = this->butterfly();
    unsigned publicLength = oldButterfly->publicLength();
    unsigned vectorLength = oldButterfly->vectorLength();
    uint64_t numberOfFreeSlots = oldButterfly->indexingHeader()->elementsStartOffset(propertyCapacity);
    RELEASE_ASSERT(count <= publicLength && publicLength <= vectorLength);

    Locker locker { cellLock() };
    auto* oldBase = static_cast<EncodedJSValue*>(oldButterfly->base(0, propertyCapacity));
    memmove(oldBase + count, oldBase, propertyCapacity * sizeof(EncodedJSValue));
    Butterfly* newButterfly = Butterfly::fromBase(oldBase + count, 0, propertyCapacity);
    newButterfly->setPublicLength(publicLength - count);
    newButterfly->setVectorLength(vectorLength - count);
    newButterfly->indexingHeader()->elementsStartOffset(propertyCapacity) = numberOfFreeSlots + count;
    WTF::storeStoreFence();
    butterflyRef().set(vm, this, newButterfly);
    return true;
}

bool JSObject::tryMoveElementsStartBackward(VM& vm, unsigned count)
{
    ASSERT(count);
    Structure* structure = this->structure();
    if (!canHaveElementsStartOffset(this, structure))
        return false;

    unsigned publicLength = butterfly()->publicLength();
    unsigned vectorLength = butterfly()->vectorLength();
    if (count > MAX_STORAGE_VECTOR_LENGTH - vectorLength)
        return false;
    size_t propertyCapacity = structure->outOfLineCapacity();
    if (propertyCapacity >= publicLength)
        return false;

    bool hasRoom = structure->hasElementsStartOffset() && butterfly()->indexingHeader()->elementsStartOffset(propertyCapacity) > count;
    if (!hasRoom) {
        size_t numberOfFreeSlots = static_cast<size_t>(count) + std::max<unsigned>(publicLength / 2, 16) + 1;
        if (numberOfFreeSlots + publicLength > MAX_STORAGE_VECTOR_LENGTH)
            return false;
        if (!tryReallocateWithElementsStartOffset(vm, vectorLength, numberOfFreeSlots))
            return false;
    }

    Butterfly* oldButterfly = this->butterfly();
    uint64_t numberOfFreeSlots = oldButterfly->indexingHeader()->elementsStartOffset(propertyCapacity);
    RELEASE_ASSERT(numberOfFreeSlots > count);

    Locker locker { cellLock() };
    auto* oldBase = static_cast<EncodedJSValue*>(oldButterfly->base(0, propertyCapacity));
    memmove(oldBase - count, oldBase, propertyCapacity * sizeof(EncodedJSValue));
    Butterfly* newButterfly = Butterfly::fromBase(oldBase - count, 0, propertyCapacity);
    newButterfly->indexingHeader()->elementsStartOffset(propertyCapacity) = numberOfFreeSlots - count;
    newButterfly->setPublicLength(publicLength + count);
    newButterfly->setVectorLength(vectorLength + count);
    Butterfly::clearRange(indexingType(), newButterfly, 0, count);
    WTF::storeStoreFence();
    butterflyRef().set(vm, this, newButterfly);
    return true;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
