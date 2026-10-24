/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompactStorage.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTImage.h"
#include "AOTRuntimeTable.h"
#include "JSCInlines.h"

namespace JSC {
namespace AOT::CompactStorage {

JSCell* tryCreate(JSGlobalObject* globalObject, bool hasValues)
{
    using Width = CompactTable::Width;
    RELEASE_ASSERT(!Options::useJIT());
    VM& vm = globalObject->vm();
    Width width = vm.aotState().image->header().options.useWideReferences() ? Width::Wide : Width::Narrow;
    CompactTable::ensureInitialTableAllocators(vm);
    return CompactTable::tryCreate(globalObject, width, hasValues ? width : Width::None, CompactTable::initialCapacity);
}

// A number needs a wide column. A table that starts with one is not replaced by its first entry.
JSCell* tryCreateForFirstEntry(JSGlobalObject* globalObject, bool hasValues, JSValue key, JSValue value)
{
    RELEASE_ASSERT(!Options::useJIT());
    CompactTable::ensureInitialTableAllocators(globalObject->vm());
    return CompactTable::tryCreateForFirstEntry(globalObject, hasValues, key, value);
}

JSCell* copy(JSGlobalObject* globalObject, JSCell* storage)
{
    return uncheckedDowncast<CompactTable>(storage)->tryCopy(globalObject);
}

bool has(JSGlobalObject* globalObject, JSCell* storage, JSValue key)
{
    return uncheckedDowncast<CompactTable>(storage)->find(globalObject, key).has_value();
}

JSValue find(JSGlobalObject* globalObject, JSCell* storage, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* table = uncheckedDowncast<CompactTable>(storage);
    std::optional<uint32_t> entry = table->find(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!entry)
        return { };
    return table->hasValues() ? table->valueAt(*entry) : table->keyAt(*entry);
}

void add(JSGlobalObject* globalObject, JSObject* owner, JSCell* storage, JSValue key, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT_UNUSED(storage, CompactTable::tableOf(owner) == storage);
    bool isAdded = CompactTable::add(globalObject, owner, key, value);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_ASSERT(isAdded);
}

bool remove(JSGlobalObject* globalObject, JSObject* owner, JSCell* storage, JSValue key)
{
    ASSERT_UNUSED(storage, CompactTable::tableOf(owner) == storage);
    return CompactTable::remove(globalObject, owner, key);
}

void clear(JSGlobalObject* globalObject, JSObject* owner, JSCell* storage)
{
    ASSERT_UNUSED(storage, CompactTable::tableOf(owner) == storage);
    CompactTable::clear(globalObject, owner);
}

uint32_t aliveEntryCount(JSCell* storage)
{
    return uncheckedDowncast<CompactTable>(storage)->size();
}

Position transitAndNext(VM& vm, JSCell* storage, uint32_t entry)
{
    return CompactTable::next(vm, storage, entry);
}

void setIterationEntry(JSCell* storage, uint32_t entry)
{
    storage->vm().aotState().runtimeTable->setCompactTableIterationEntry(entry);
}

uint32_t iterationEntry(JSCell* storage)
{
    return storage->vm().aotState().runtimeTable->compactTableIterationEntry();
}

JSValue keyAt(JSCell* storage, uint32_t entry)
{
    return uncheckedDowncast<CompactTable>(storage)->keyAt(entry);
}

JSValue valueAt(JSCell* storage, uint32_t entry)
{
    return uncheckedDowncast<CompactTable>(storage)->valueAt(entry);
}

} // namespace AOT::CompactStorage
} // namespace JSC

#endif // ENABLE(AOT)
