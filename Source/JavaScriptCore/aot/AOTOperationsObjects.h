/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "OperationResult.h"

namespace JSC {

class CallFrame;
class JSArray;
class JSCell;
class JSGlobalObject;
class JSObject;
class JSScope;
class VM;

namespace AOT {

struct Slot;

#define FOR_EACH_AOT_OBJECT_OPERATION(v) \
    v(operationAOTNewObjectLiteral) \
    v(operationAOTNewObjectFromDescriptor) \
    v(operationAOTNewObjectFromFilledDescriptor) \
    v(operationAOTInstanceofAndCache) \
    v(operationAOTDefaultHasInstance) \
    v(operationAOTGetByIdWellKnown) \
    v(operationAOTPutByIdReallocating) \
    v(operationAOTWriteBarrierAfterPut) \
    v(operationAOTMapSet) \
    v(operationAOTSetAdd) \
    v(operationAOTWeakMapSet) \
    v(operationAOTStringStartsWith) \
    v(operationAOTStringIndexOf) \
    v(operationAOTEnsureData) \
    v(operationAOTDidFillSlots) \

enum class WellKnownIdentifier : uint32_t {
    Length,
    Next,
    Done,
    Value,
    HasInstanceSymbol,
    Prototype,
};

enum class FunctionKind : uint32_t {
    Normal,
    Generator,
    Async,
    AsyncGenerator,
};

enum class PrototypesOfTarget : uint8_t { DoNotMatter, MayIntercept };
bool tryCopyDataProperties(Instance*, VM&, JSObject* target, JSObject* source, const IdentifierSet* excluded, const char* operation, PrototypesOfTarget = PrototypesOfTarget::DoNotMatter);
bool tryCopyRecordDataProperties(Instance*, VM&, JSObject* target, JSObject* record, const char* operation);
JSC_DECLARE_JIT_OPERATION(operationAOTNewObjectLiteral, JSObject*, (Instance*, EncodedJSValue* values, uint32_t count, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTNewObjectFromDescriptor, JSObject*, (Instance*, EncodedJSValue* values, uint32_t count, uint32_t slotIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTNewObjectFromFilledDescriptor, JSObject*, (Instance*, EncodedJSValue* values, uint32_t count, Slot* row));

JSC_DECLARE_JIT_OPERATION(operationAOTInstanceofAndCache, size_t, (Instance*, EncodedJSValue value, EncodedJSValue constructor, uint32_t, Slot* cache, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTDefaultHasInstance, size_t, (Instance*, EncodedJSValue value, EncodedJSValue prototype));

JSC_DECLARE_JIT_OPERATION(operationAOTGetByIdWellKnown, EncodedJSValue, (Instance*, EncodedJSValue base, uint32_t wellKnownIdentifier, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByIdReallocating, void, (VM*, JSObject* base, EncodedJSValue value, const void* megamorphicCacheStoreEntry));
JSC_DECLARE_JIT_OPERATION(operationAOTWriteBarrierAfterPut, void, (VM*, JSCell*));
JSC_DECLARE_JIT_OPERATION(operationAOTMapSet, EncodedJSValue, (Instance*, JSCell* map, EncodedJSValue key, EncodedJSValue value, int32_t hash));
JSC_DECLARE_JIT_OPERATION(operationAOTSetAdd, EncodedJSValue, (Instance*, JSCell* set, EncodedJSValue key, int32_t hash));
JSC_DECLARE_JIT_OPERATION(operationAOTWeakMapSet, EncodedJSValue, (Instance*, JSCell* map, JSCell* key, EncodedJSValue value));
JSC_DECLARE_JIT_OPERATION(operationAOTStringStartsWith, EncodedJSValue, (Instance*, JSCell* string, JSCell* other));
JSC_DECLARE_JIT_OPERATION(operationAOTStringIndexOf, EncodedJSValue, (Instance*, JSCell* string, JSCell* other));

JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureData, void, (Instance*, uint32_t index));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTDidFillSlots, void, (Data*));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
