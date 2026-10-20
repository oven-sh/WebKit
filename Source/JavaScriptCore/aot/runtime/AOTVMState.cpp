/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVMState.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTCallSiteCache.h"
#include "AOTFunctionTables.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHeapHooks.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleRecord.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCaches.h"
#include "AOTRegExpLiterals.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTStringBuilderStorage.h"
#include "JSCInlines.h"
#include "Options.h"

namespace JSC { namespace AOT {

VMState::VMState() = default;

VMState::~VMState()
{
    RELEASE_ASSERT(!hasCompiledCode);
}

void VMState::attachImage(VM& vm)
{
    RELEASE_ASSERT(!hasCompiledCode && !image);
    image = Image::current();
    RELEASE_ASSERT(image);
    RELEASE_ASSERT(vm.structureStructure && !vm.structureRareDataStructure);

    structureIDBase = JSC::structureIDBase();
    validatesRuntime = image->header().options.validates || Options::validateAOTRuntime();
    vm.updateSoftReservedZoneSize(Options::softReservedZoneSize() + BuiltinCells::stackKeptFromProgram);

    caches = makeUnique<CacheRegistry>();
    counters = makeUniqueWithoutFastMallocCheck<RuntimeCounters>();
    hasCompiledCode = true;

    addMarkingConstraints(vm);
    vm.heap.setValidatesCellVisits(validatesRuntime || Options::verifyGC());

    reporter = makeUniqueWithoutFastMallocCheck<RuntimeReporter>(vm);
    runtimeTable = makeUnique<RuntimeTable>(vm);
    identifiers = makeUnique<ProgramIdentifiers>(vm, *image);
    callSiteCache = makeUnique<CallSiteCache>();
    propertyCaches = makeUnique<VMPropertyCaches>(vm);
    regExpLiterals = makeUnique<RegExpLiterals>(*caches);
    regExpMatchOutput = makeUniqueArray<int>(image->header().counts.maxRegExpOutputSize);

    moduleRecordStructure.setWithoutWriteBarrier(ModuleRecord::createStructure(vm, nullptr, jsNull()));
    stringBuilderStorageStructure.setWithoutWriteBarrier(StringBuilderStorage::createStructure(vm, nullptr, jsNull()));

    if (validatesRuntime)
        caches->addObserver("validation after collection"_s, reporter->collectionValidator());
}

// The rows of Structures are words of one table of the process, so they are given back while the Structure IDs still belong to this VM.
void VMState::willShutDownHeap()
{
    if (propertyCaches)
        propertyCaches->front().clear();
}

void VMState::releaseImage(VM& vm)
{
    if (!hasCompiledCode)
        return;
    ASSERT(vm.heap.isShuttingDown());

    reporter->writeAtExit();
    Instance::destroyAll(vm);
    RELEASE_ASSERT(instances.isEmpty() && instancesToDestroy.isEmpty());

    regExpLiterals = nullptr;
    regExpMatchOutput = nullptr;
    propertyCaches = nullptr;
    functionTables = nullptr;
    callSiteCache = nullptr;
    identifiers = nullptr;
    runtimeTable = nullptr;
    reporter = nullptr;
    counters = nullptr;
    caches = nullptr;

    moduleRecordStructure.clear();
    stringBuilderStorageStructure.clear();
    image = nullptr;
    structureIDBase = 0;
    validatesRuntime = false;
    hasCompiledCode = false;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
