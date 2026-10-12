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
    v(operationAOTNewObject) \
    v(operationAOTNewObjectLiteral) \
    v(operationAOTNewObjectOfRow) \
    v(operationAOTCreateThisWithProperties) \
    v(operationAOTCreateThis) \
    v(operationAOTNewArray) \
    v(operationAOTNewArrayWithSize) \
    v(operationAOTNewArrayBuffer) \
    v(operationAOTNewArrayWithSpread) \
    v(operationAOTNewArrayWithSpecies) \
    v(operationAOTCloneObject) \
    v(operationAOTNewFunction) \
    v(operationAOTNewInternalFieldObject) \
    v(operationAOTNewMapOrSet) \
    v(operationAOTCreateLexicalEnvironment) \
    v(operationAOTCreateRest) \
    v(operationAOTThrowNotAFunction) \
    v(operationAOTToThis) \
    v(operationAOTStrcat) \
    v(operationAOTInstanceofAndCache) \
    v(operationAOTDefaultHasInstance) \
    v(operationAOTGetByIdWellKnown) \
    v(operationAOTPutByIdReallocating) \
    v(operationAOTWriteBarrierAfterPut) \
    v(operationAOTPutByValDirect) \
    v(operationAOTInById) \
    v(operationAOTMapSet) \
    v(operationAOTSetAdd) \
    v(operationAOTInByVal) \
    v(operationAOTGetPrivateName) \
    v(operationAOTPutPrivateName) \
    v(operationAOTCheckPrivateBrand) \
    v(operationAOTSetPrivateBrand) \
    v(operationAOTIteratorOpenTryFast) \
    v(operationAOTIteratorNextTryFast) \
    v(operationAOTIteratorNextWithIndex) \
    v(operationAOTMaterializeArrayIterator) \
    v(operationAOTThrowIteratorResultIsNotObject) \
    v(operationAOTSizeOfVarargs) \
    v(operationAOTLoadVarargs) \
    v(operationAOTLinkFunction) \
    v(operationAOTConstructViaCall) \
    v(operationAOTCacheCallee) \
    v(operationAOTCacheHostCallee) \
    v(operationAOTEnsureData) \
    v(operationAOTCountMissOfCalleeCache) \
    v(operationAOTNoteCallee) \

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

enum class InternalFieldObjectKind : uint32_t {
    Promise,
    Generator,
    AsyncGenerator,
    AsyncFunctionGenerator,
};

JSC_DECLARE_JIT_OPERATION(operationAOTNewObject, JSObject*, (Instance*, uint32_t inlineCapacity, Slot*));
bool tryCopyDataProperties(Instance*, VM&, JSObject* target, JSObject* source, const IdentifierSet* excluded, const char* operation);
JSC_DECLARE_JIT_OPERATION(operationAOTCloneObject, JSObject*, (Instance*, EncodedJSValue source, uint32_t layoutID));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateThisWithProperties, JSObject*, (Instance*, JSObject* callee, EncodedJSValue* values, uint32_t count, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTNewObjectLiteral, JSObject*, (Instance*, EncodedJSValue* values, uint32_t count, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTNewObjectOfRow, JSObject*, (Instance*, EncodedJSValue* values, uint32_t count, Slot* row));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateThis, JSObject*, (Instance*, JSObject* callee, uint32_t layoutIDAndInlineCapacity));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArray, JSObject*, (Instance*, const EncodedJSValue* values, uint32_t count, uint32_t indexingType));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayWithSize, JSObject*, (Instance*, EncodedJSValue size));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayBuffer, JSObject*, (Instance*, JSCell* immutableButterfly));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayWithSpread, JSObject*, (Instance*, EncodedJSValue*, uint32_t count, uint32_t pendingSpreads));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayWithSpecies, JSObject*, (Instance*, EncodedJSValue length, JSObject* array));
JSCell* spread(JSGlobalObject*, JSValue iterable);
JSArray* copyableArray(JSValue iterable);
JSC_DECLARE_JIT_OPERATION(operationAOTNewFunction, JSObject*, (Instance*, JSScope*, uint32_t index, uint32_t isExpression, uint32_t functionKind, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTNewInternalFieldObject, JSObject*, (Instance*, uint32_t kind));
JSC_DECLARE_JIT_OPERATION(operationAOTNewMapOrSet, JSObject*, (Instance*, uint32_t isSet));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateLexicalEnvironment, JSObject*, (Instance*, JSScope*, JSCell* symbolTable, EncodedJSValue initialValue, uint32_t numberOfVariables));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateRest, JSObject*, (Instance*, uint32_t count, EncodedJSValue* arguments, uint32_t numParametersToSkip));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowNotAFunction, void, (Instance*, EncodedJSValue callee));

JSC_DECLARE_JIT_OPERATION(operationAOTToThis, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTStrcat, EncodedJSValue, (Instance*, const EncodedJSValue* values, uint32_t count));
JSC_DECLARE_JIT_OPERATION(operationAOTInstanceofAndCache, size_t, (Instance*, EncodedJSValue value, EncodedJSValue constructor, uint32_t, Slot* cache, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTDefaultHasInstance, size_t, (Instance*, EncodedJSValue value, EncodedJSValue prototype));

JSC_DECLARE_JIT_OPERATION(operationAOTGetByIdWellKnown, EncodedJSValue, (Instance*, EncodedJSValue base, uint32_t wellKnownIdentifier, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByIdReallocating, void, (VM*, JSObject* base, EncodedJSValue value, const void* megamorphicCacheStoreEntry));
JSC_DECLARE_JIT_OPERATION(operationAOTWriteBarrierAfterPut, void, (VM*, JSCell*));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByValDirect, void, (Instance*, JSObject* base, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTInById, size_t, (Instance*, EncodedJSValue base, uint32_t identifierIndex))
JSC_DECLARE_JIT_OPERATION(operationAOTMapSet, EncodedJSValue, (Instance*, JSCell* map, EncodedJSValue key, EncodedJSValue value, int32_t hash));
JSC_DECLARE_JIT_OPERATION(operationAOTSetAdd, EncodedJSValue, (Instance*, JSCell* set, EncodedJSValue key, int32_t hash));
JSC_DECLARE_JIT_OPERATION(operationAOTInByVal, size_t, (Instance*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTGetPrivateName, EncodedJSValue, (Instance*, EncodedJSValue base, EncodedJSValue property, uint32_t, Slot*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTPutPrivateName, void, (Instance*, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value, uint32_t, Slot*, uint32_t isDefine));
JSC_DECLARE_JIT_OPERATION(operationAOTCheckPrivateBrand, void, (Instance*, EncodedJSValue base, EncodedJSValue brand, uint32_t, Slot*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTSetPrivateBrand, void, (Instance*, JSObject* base, EncodedJSValue brand, uint32_t, Slot*, uint32_t));

JSC_DECLARE_JIT_OPERATION(operationAOTIteratorOpenTryFast, EncodedJSValue, (Instance*, EncodedJSValue iterable, EncodedJSValue symbolIterator, EncodedJSValue* next));
JSC_DECLARE_JIT_OPERATION(operationAOTIteratorNextTryFast, EncodedJSValue, (Instance*, JSObject* iterator));
JSC_DECLARE_JIT_OPERATION(operationAOTIteratorNextWithIndex, EncodedJSValue, (Instance*, EncodedJSValue iterable, EncodedJSValue* index));
JSC_DECLARE_JIT_OPERATION(operationAOTMaterializeArrayIterator, JSObject*, (Instance*, EncodedJSValue iterable, EncodedJSValue index));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowIteratorResultIsNotObject, void, (Instance*));

JSC_DECLARE_JIT_OPERATION(operationAOTSizeOfVarargs, size_t, (Instance*, EncodedJSValue listOrItems, uint32_t descriptor));
JSC_DECLARE_JIT_OPERATION(operationAOTLoadVarargs, void, (Instance*, EncodedJSValue* where, EncodedJSValue listOrItems, uint32_t descriptor, uint32_t length));

JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTConstructViaCall, UGPRPair, (CallFrame*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLinkFunction, void, (Instance*, void* addressInFunction));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTCacheCallee, void, (Instance*, Slot* cache, JSCell* callee, uint64_t entryWord, uint32_t count));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTCacheHostCallee, void, (Instance*, Slot* cache, EncodedJSValue callee, void* callHostFunction, void* callInternalFunction));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureData, void, (Instance*, uint32_t index));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTCountMissOfCalleeCache, void, (Instance*, const void* returnAddress));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTNoteCallee, void, (Instance*, Slot* cache, EncodedJSValue callee));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
