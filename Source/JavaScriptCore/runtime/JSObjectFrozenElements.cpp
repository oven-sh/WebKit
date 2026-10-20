/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "JSObject.h"

#include "DeferTermination.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"

namespace JSC {

void JSObject::moveFrozenElementsToSparseMap(VM& vm)
{
    RELEASE_ASSERT(hasFrozenElementsInPlace());
    DeferTerminationForAWhile deferScope(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    {
        Structure* frozenStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, frozenStructure);
        frozenStructure->didTransitionFromThisStructureWithoutFiringWatchpoint();
        frozenStructure->fireStructureTransitionWatchpoint(&deferred);
        setStructure(vm, frozenStructure->previousID());
    }
    convertFromCopyOnWrite(vm);
    switch (indexingType()) {
    case ALL_INT32_INDEXING_TYPES:
        convertInt32ToArrayStorage(vm);
        break;
    case ALL_DOUBLE_INDEXING_TYPES:
        convertDoubleToArrayStorage(vm);
        break;
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        convertContiguousToArrayStorage(vm);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    objectConstructorFreeze(realm(), this);
    scope.releaseAssertNoException();
    RELEASE_ASSERT(inSparseIndexingMode());
}

bool JSObject::tryFreezeArrayInPlace(VM& vm)
{
    if (hasFrozenElementsInPlace())
        return true;
    JSGlobalObject* globalObject = realm();
    if (classInfo() != JSArray::info() || !globalObject->isOriginalArrayStructure(structure()) || globalObject->isHavingABadTime() || mayBePrototype())
        return false;
    JSCellButterfly* copyOnWriteButterfly = nullptr;
    if (!isCopyOnWrite(indexingMode())) {
        copyOnWriteButterfly = tryCreateCopyOnWriteButterfly(vm, this);
        if (!copyOnWriteButterfly)
            return false;
    }
    StructureID oldStructureID = structureID();
    Structure* copyOnWriteStructure = globalObject->originalArrayStructureForIndexingType(indexingMode() | CopyOnWrite);
    DeferredStructureTransitionWatchpointFire deferred(vm, copyOnWriteStructure);
    Structure* newStructure = Structure::nonPropertyTransition(vm, copyOnWriteStructure, TransitionKind::FreezeElementsInPlace, &deferred);
    RELEASE_ASSERT(isCopyOnWrite(newStructure->indexingMode()));
    RELEASE_ASSERT(newStructure->hasFrozenElementsInPlace());
    RELEASE_ASSERT(newStructure->hasImmutableProperties());
    Structure* oldStructure = oldStructureID.decode();
    oldStructure->didTransitionFromThisStructureWithoutFiringWatchpoint();
    oldStructure->fireStructureTransitionWatchpoint(nullptr);
    if (copyOnWriteButterfly)
        nukeStructureAndSetButterfly(vm, oldStructureID, copyOnWriteButterfly->toButterfly());
    setStructure(vm, newStructure);
    return true;
}

} // namespace JSC
