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
    v(operationAOTCreateThisWithProperties) \
    v(operationAOTPutProperties) \
    v(operationAOTCreateThis) \
    v(operationAOTNewArray) \
    v(operationAOTNewArrayWithSize) \
    v(operationAOTNewArrayBuffer) \
    v(operationAOTNewArrayWithSpread) \
    v(operationAOTNewArrayWithSpecies) \
    v(operationAOTSpread) \
    v(operationAOTNewRegExp) \
    v(operationAOTNewRegExpForReceiver) \
    v(operationAOTNewRegExpForArgument) \
    v(operationAOTIsMadeFromFunction) \
    v(operationAOTLinkTimeConstant) \
    v(operationAOTArrayIteratorMethod) \
    v(operationAOTValidateNewObject) \
    v(operationAOTNewTypedObject) \
    v(operationAOTCloneObject) \
    v(operationAOTTryCopyDataProperties) \
    v(operationAOTNoteClass) \
    v(operationAOTMakeAtom) \
    v(operationAOTNewFunction) \
    v(operationAOTNewFunctionWithCaptures) \
    v(operationAOTSetFunctionName) \
    v(operationAOTNewInternalFieldObject) \
    v(operationAOTNewMapOrSet) \
    v(operationAOTAsyncFunctionDrive) \
    v(operationAOTCreateInternalFieldObject) \
    v(operationAOTCreateLexicalEnvironment) \
    v(operationAOTPushWithScope) \
    v(operationAOTResolveScopeForHoistingFuncDeclInEval) \
    v(operationAOTCreateDirectArguments) \
    v(operationAOTCreateScopedArguments) \
    v(operationAOTCreateClonedArguments) \
    v(operationAOTCreateRest) \
    v(operationAOTThrowNotAFunction) \
    v(operationAOTThrowNotAConstructor) \
    v(operationAOTToThis) \
    v(operationAOTToObject) \
    v(operationAOTToPrimitive) \
    v(operationAOTToPropertyKey) \
    v(operationAOTTypeof) \
    v(operationAOTTypeofIsObject) \
    v(operationAOTTypeofIsFunction) \
    v(operationAOTIsCallable) \
    v(operationAOTIsConstructor) \
    v(operationAOTStrcat) \
    v(operationAOTGetPrototypeOf) \
    v(operationAOTInstanceof) \
    v(operationAOTInstanceofCustom) \
    v(operationAOTInstanceofAndCache) \
    v(operationAOTDefaultHasInstance) \
    v(operationAOTThrowTDZError) \
    v(operationAOTThrowThisTDZError) \
    v(operationAOTThrowStaticError) \
    v(operationAOTGetByIdWellKnown) \
    v(operationAOTPutByIdReallocating) \
    v(operationAOTWriteBarrierAfterPut) \
    v(operationAOTGetByIdDirect) \
    v(operationAOTGetByIdWithThis) \
    v(operationAOTGetByValWithThis) \
    v(operationAOTPutByIdWithThis) \
    v(operationAOTPutByValWithThis) \
    v(operationAOTPutByValDirect) \
    v(operationAOTInById) \
    v(operationAOTMapSet) \
    v(operationAOTSetAdd) \
    v(operationAOTInByVal) \
    v(operationAOTDelById) \
    v(operationAOTDelByVal) \
    v(operationAOTGetPrivateName) \
    v(operationAOTPutPrivateName) \
    v(operationAOTHasPrivateName) \
    v(operationAOTHasPrivateBrand) \
    v(operationAOTCheckPrivateBrand) \
    v(operationAOTSetPrivateBrand) \
    v(operationAOTPutAccessorById) \
    v(operationAOTPutGetterSetterById) \
    v(operationAOTPutAccessorByVal) \
    v(operationAOTDefineDataProperty) \
    v(operationAOTDefineDataPropertyOnSingleton) \
    v(operationAOTDefineAccessorProperty) \
    v(operationAOTGetPropertyEnumerator) \
    v(operationAOTEnumeratorNext) \
    v(operationAOTEnumeratorGetByVal) \
    v(operationAOTEnumeratorInByVal) \
    v(operationAOTEnumeratorPutByVal) \
    v(operationAOTEnumeratorHasOwnProperty) \
    v(operationAOTIteratorOpenTryFast) \
    v(operationAOTAsyncIteratorOpenTryFast) \
    v(operationAOTIteratorNextTryFast) \
    v(operationAOTIteratorNextWithIndex) \
    v(operationAOTAsyncIteratorNextWithDriver) \
    v(operationAOTMaterializeArrayIterator) \
    v(operationAOTThrowIteratorResultIsNotObject) \
    v(operationAOTSizeOfVarargs) \
    v(operationAOTLoadVarargs) \
    v(operationAOTLinkFunction) \
    v(operationAOTConstructViaCall) \
    v(operationAOTNoteFilled) \
    v(operationAOTCacheCallee) \
    v(operationAOTCacheHostCallee) \
    v(operationAOTHasOwnProperty) \
    v(operationAOTEnsureData) \
    v(operationAOTCallDirectEval) \

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
JSC_DECLARE_JIT_OPERATION(operationAOTNewTypedObject, JSObject*, (Instance*, uint32_t layoutID, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTCloneObject, JSObject*, (Instance*, EncodedJSValue source, uint32_t layoutID));
JSC_DECLARE_JIT_OPERATION(operationAOTTryCopyDataProperties, size_t, (Instance*, EncodedJSValue target, EncodedJSValue source, EncodedJSValue excludedSetIndex, uint32_t whose));
JSC_DECLARE_JIT_OPERATION(operationAOTNoteClass, void, (Instance*, EncodedJSValue constructor, EncodedJSValue prototype, uint32_t layoutID));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTMakeAtom, void, (EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateThisWithProperties, JSObject*, (Instance*, JSObject* callee, EncodedJSValue* values, uint32_t count, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTPutProperties, void, (Instance*, EncodedJSValue base, EncodedJSValue* values, uint32_t count, Slot* cache));
JSC_DECLARE_JIT_OPERATION(operationAOTNewObjectLiteral, JSObject*, (Instance*, EncodedJSValue* values, uint32_t count, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateThis, JSObject*, (Instance*, JSObject* callee, uint32_t inlineCapacity));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArray, JSObject*, (Instance*, const EncodedJSValue* values, uint32_t count, uint32_t indexingType));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayWithSize, JSObject*, (Instance*, EncodedJSValue size));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayBuffer, JSObject*, (Instance*, JSCell* immutableButterfly));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayWithSpread, JSObject*, (Instance*, EncodedJSValue*, uint32_t count, uint32_t pendingSpreads));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayWithSpecies, JSObject*, (Instance*, EncodedJSValue length, JSObject* array));
JSC_DECLARE_JIT_OPERATION(operationAOTSpread, JSCell*, (Instance*, EncodedJSValue iterable));
JSCell* spread(JSGlobalObject*, JSValue iterable);
JSArray* copyableArray(JSValue iterable);
JSC_DECLARE_JIT_OPERATION(operationAOTNewRegExp, JSObject*, (Instance*, JSCell* regExp));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTValidateNewObject, void, (Instance*, JSObject*));
JSC_DECLARE_JIT_OPERATION(operationAOTArrayIteratorMethod, EncodedJSValue, (Instance*, JSCell*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLinkTimeConstant, EncodedJSValue, (Instance*, uint32_t which));
JSC_DECLARE_JIT_OPERATION(operationAOTNewRegExpForReceiver, JSObject*, (Instance*, JSCell* regExp, uint32_t forTest, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTNewRegExpForArgument, JSObject*, (Instance*, JSCell* regExp, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTIsMadeFromFunction, size_t, (Instance*, EncodedJSValue, uint32_t number, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTNewFunction, JSObject*, (Instance*, JSScope*, uint32_t index, uint32_t isExpression, uint32_t functionKind, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTNewFunctionWithCaptures, JSObject*, (Instance*, JSScope*, uint32_t index, uint32_t isExpression, EncodedJSValue* captures, uint32_t count, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTSetFunctionName, void, (Instance*, JSObject* function, EncodedJSValue name));
JSC_DECLARE_JIT_OPERATION(operationAOTNewInternalFieldObject, JSObject*, (Instance*, uint32_t kind));
JSC_DECLARE_JIT_OPERATION(operationAOTNewMapOrSet, JSObject*, (Instance*, uint32_t isSet));
JSC_DECLARE_JIT_OPERATION(operationAOTAsyncFunctionDrive, void, (Instance*, EncodedJSValue resolution, JSCell* generator));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateInternalFieldObject, JSObject*, (Instance*, JSObject* callee, uint32_t kind));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateLexicalEnvironment, JSObject*, (Instance*, JSScope*, JSCell* symbolTable, EncodedJSValue initialValue, uint32_t numberOfVariables));
JSC_DECLARE_JIT_OPERATION(operationAOTPushWithScope, JSObject*, (Instance*, JSScope*, EncodedJSValue object));
JSC_DECLARE_JIT_OPERATION(operationAOTResolveScopeForHoistingFuncDeclInEval, EncodedJSValue, (Instance*, JSScope*, uint32_t identifierIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateDirectArguments, JSObject*, (Instance*, JSObject* callee, uint32_t count, EncodedJSValue* arguments, uint32_t numberOfParameters));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateScopedArguments, JSObject*, (Instance*, JSObject* scope, JSObject* callee, uint32_t count, EncodedJSValue* arguments));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateClonedArguments, JSObject*, (Instance*, JSObject* callee, uint32_t count, EncodedJSValue* arguments));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateRest, JSObject*, (Instance*, uint32_t count, EncodedJSValue* arguments, uint32_t numParametersToSkip));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowNotAFunction, void, (Instance*, EncodedJSValue callee));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowNotAConstructor, void, (Instance*, EncodedJSValue callee));

JSC_DECLARE_JIT_OPERATION(operationAOTToThis, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTToObject, JSObject*, (Instance*, EncodedJSValue, uint32_t messageIdentifierIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTToPrimitive, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTToPropertyKey, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTTypeof, JSCell*, (Instance*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTTypeofIsObject, size_t, (Instance*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTTypeofIsFunction, size_t, (Instance*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTIsCallable, size_t, (EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTIsConstructor, size_t, (EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTStrcat, EncodedJSValue, (Instance*, const EncodedJSValue* values, uint32_t count));
JSC_DECLARE_JIT_OPERATION(operationAOTGetPrototypeOf, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTInstanceof, size_t, (Instance*, EncodedJSValue value, EncodedJSValue constructor));
JSC_DECLARE_JIT_OPERATION(operationAOTInstanceofAndCache, size_t, (Instance*, EncodedJSValue value, EncodedJSValue constructor, uint32_t, Slot* cache, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTInstanceofCustom, size_t, (Instance*, EncodedJSValue value, JSObject* constructor, EncodedJSValue hasInstance));
JSC_DECLARE_JIT_OPERATION(operationAOTDefaultHasInstance, size_t, (Instance*, EncodedJSValue value, EncodedJSValue prototype));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowTDZError, void, (Instance*));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowThisTDZError, void, (Instance*));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowStaticError, void, (Instance*, EncodedJSValue message, uint32_t errorType));

JSC_DECLARE_JIT_OPERATION(operationAOTGetByIdWellKnown, EncodedJSValue, (Instance*, EncodedJSValue base, uint32_t wellKnownIdentifier, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByIdReallocating, void, (VM*, JSObject* base, EncodedJSValue value, const void* megamorphicCacheStoreEntry));
JSC_DECLARE_JIT_OPERATION(operationAOTWriteBarrierAfterPut, void, (VM*, JSCell*));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByIdDirect, EncodedJSValue, (Instance*, EncodedJSValue base, uint32_t identifierIndex, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByIdWithThis, EncodedJSValue, (Instance*, EncodedJSValue base, EncodedJSValue thisValue, uint32_t identifierIndex, Slot* cache));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByValWithThis, EncodedJSValue, (Instance*, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByIdWithThis, void, (Instance*, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue value, uint32_t identifierIndex, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByValWithThis, void, (Instance*, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByValDirect, void, (Instance*, JSObject* base, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTInById, size_t, (Instance*, EncodedJSValue base, uint32_t identifierIndex))
JSC_DECLARE_JIT_OPERATION(operationAOTMapSet, EncodedJSValue, (Instance*, JSCell* map, EncodedJSValue key, EncodedJSValue value, int32_t hash));
JSC_DECLARE_JIT_OPERATION(operationAOTSetAdd, EncodedJSValue, (Instance*, JSCell* set, EncodedJSValue key, int32_t hash));
JSC_DECLARE_JIT_OPERATION(operationAOTInByVal, size_t, (Instance*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTDelById, size_t, (Instance*, EncodedJSValue base, uint32_t identifierIndex, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTDelByVal, size_t, (Instance*, EncodedJSValue base, EncodedJSValue property, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTGetPrivateName, EncodedJSValue, (Instance*, EncodedJSValue base, EncodedJSValue property, uint32_t, Slot*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTPutPrivateName, void, (Instance*, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value, uint32_t, Slot*, uint32_t isDefine));
JSC_DECLARE_JIT_OPERATION(operationAOTHasPrivateName, size_t, (Instance*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTHasPrivateBrand, size_t, (Instance*, EncodedJSValue base, EncodedJSValue brand));
JSC_DECLARE_JIT_OPERATION(operationAOTCheckPrivateBrand, void, (Instance*, EncodedJSValue base, EncodedJSValue brand, uint32_t, Slot*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTSetPrivateBrand, void, (Instance*, JSObject* base, EncodedJSValue brand, uint32_t, Slot*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTPutAccessorById, void, (Instance*, JSObject* base, uint32_t identifierIndex, uint32_t attributes, JSObject* accessor, uint32_t isSetter));
JSC_DECLARE_JIT_OPERATION(operationAOTPutGetterSetterById, void, (Instance*, JSObject* base, uint32_t identifierIndex, uint32_t attributes, EncodedJSValue getter, EncodedJSValue setter));
JSC_DECLARE_JIT_OPERATION(operationAOTPutAccessorByVal, void, (Instance*, JSObject* base, EncodedJSValue property, uint32_t attributes, JSObject* accessor, uint32_t isSetter));
JSC_DECLARE_JIT_OPERATION(operationAOTDefineDataProperty, void, (Instance*, JSObject* base, EncodedJSValue property, EncodedJSValue value, int32_t attributes));
JSC_DECLARE_JIT_OPERATION(operationAOTDefineDataPropertyOnSingleton, void, (Instance*, JSObject* base, EncodedJSValue property, EncodedJSValue value, int32_t attributes));
JSC_DECLARE_JIT_OPERATION(operationAOTDefineAccessorProperty, void, (Instance*, JSObject* base, EncodedJSValue property, EncodedJSValue getter, EncodedJSValue setter, int32_t attributes));

JSC_DECLARE_JIT_OPERATION(operationAOTGetPropertyEnumerator, JSCell*, (Instance*, EncodedJSValue base));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorNext, JSCell*, (Instance*, EncodedJSValue base, JSCell* enumerator, EncodedJSValue* modeAndIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorGetByVal, EncodedJSValue, (Instance*, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorInByVal, size_t, (Instance*, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorPutByVal, void, (Instance*, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue value, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorHasOwnProperty, size_t, (Instance*, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator));

JSC_DECLARE_JIT_OPERATION(operationAOTIteratorOpenTryFast, EncodedJSValue, (Instance*, EncodedJSValue iterable, EncodedJSValue symbolIterator, EncodedJSValue* next));
JSC_DECLARE_JIT_OPERATION(operationAOTAsyncIteratorOpenTryFast, EncodedJSValue, (Instance*, EncodedJSValue iterable, EncodedJSValue symbolIterator, EncodedJSValue* next));
JSC_DECLARE_JIT_OPERATION(operationAOTIteratorNextTryFast, EncodedJSValue, (Instance*, JSObject* iterator));
JSC_DECLARE_JIT_OPERATION(operationAOTIteratorNextWithIndex, EncodedJSValue, (Instance*, EncodedJSValue iterable, EncodedJSValue* index));
JSC_DECLARE_JIT_OPERATION(operationAOTAsyncIteratorNextWithDriver, EncodedJSValue, (Instance*, JSObject* iterator, JSObject* driver, EncodedJSValue resumeValue));
JSC_DECLARE_JIT_OPERATION(operationAOTMaterializeArrayIterator, JSObject*, (Instance*, EncodedJSValue iterable, EncodedJSValue index));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowIteratorResultIsNotObject, void, (Instance*));

JSC_DECLARE_JIT_OPERATION(operationAOTSizeOfVarargs, size_t, (Instance*, EncodedJSValue listOrItems, uint32_t descriptor));
JSC_DECLARE_JIT_OPERATION(operationAOTLoadVarargs, void, (Instance*, EncodedJSValue* where, EncodedJSValue listOrItems, uint32_t descriptor, uint32_t length));

JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTConstructViaCall, UGPRPair, (CallFrame*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLinkFunction, void, (Instance*, void* addressInFunction));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTNoteFilled, void, (Data*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTCacheCallee, void, (Instance*, Slot* cache, JSCell* callee, uint64_t entryWord, uint32_t count));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTCacheHostCallee, void, (Instance*, Slot* cache, EncodedJSValue callee, void* callHostFunction, void* callInternalFunction));
JSC_DECLARE_JIT_OPERATION(operationAOTHasOwnProperty, size_t, (Instance*, JSObject*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureData, void, (Instance*, uint32_t index));
JSC_DECLARE_JIT_OPERATION(operationAOTCallDirectEval, EncodedJSValue, (Instance*, EncodedJSValue callee, uint32_t count, EncodedJSValue firstArgument, JSScope*, EncodedJSValue thisValue, uint32_t bytecodeIndexBits, uint32_t lexicallyScopedFeatures));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
