/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinFamilyState.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTJSONRuntime.h"
#include "AOTObjectWords.h"
#include "AOTState.h"
#include "JSObject.h"
#include <bit>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

BuiltinFamilyState::BuiltinFamilyState(State& state)
    : instance(state.instance)
    , json(makeUnique<JSONRuntime>(state.instance, *state.caches))
{
    state.caches->addClearedEachCollection("Object.create"_s, asMutableByteSpan(instance.builtins.objects.objectCreate));
}

BuiltinFamilyState::~BuiltinFamilyState() = default;

void BuiltinFamilyState::cacheObjectCreate(JSObject* prototype, Structure* structureOfNewObject)
{
    if (structureOfNewObject->inlineCapacity() != JSFinalObject::defaultInlineCapacity)
        return;
    unsigned index = ObjectBuiltinWords::objectCreateIndex(std::bit_cast<uintptr_t>(prototype));
    instance.builtins.objects.objectCreate[index] = { prototype, cellHeaderFor(structureOfNewObject) };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
