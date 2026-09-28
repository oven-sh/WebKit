/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "OperationResult.h"

namespace JSC {

class CallFrame;
class JSCell;
class JSGlobalObject;
class JSObject;
class JSScope;
class VM;

namespace AOT {

struct Slot;

// The operations behind every instruction that is not arithmetic, a plain property access or a variable access. What they have
// in common is what they do without: operands and results are arguments and return values, never bytecode registers in the
// frame, and nothing looks at the instruction stream, at the interpreter's metadata or at a profile. Part of
// FOR_EACH_AOT_OPERATION (AOTRuntime.h).
#define FOR_EACH_AOT_OBJECT_OPERATION(v) \
    v(operationAOTNewObject) \
    v(operationAOTNewObjectLiteral) \
    v(operationAOTCreateThisWithProperties) \
    v(operationAOTCreateThis) \
    v(operationAOTNewArray) \
    v(operationAOTNewArrayWithSize) \
    v(operationAOTNewArrayBuffer) \
    v(operationAOTNewArrayWithSpread) \
    v(operationAOTNewArrayWithSpecies) \
    v(operationAOTSpread) \
    v(operationAOTNewRegExp) \
    v(operationAOTNewFunction) \
    v(operationAOTSetFunctionName) \
    v(operationAOTNewInternalFieldObject) \
    v(operationAOTCreateInternalFieldObject) \
    v(operationAOTCreateLexicalEnvironment) \
    v(operationAOTPushWithScope) \
    v(operationAOTResolveScopeForHoistingFuncDeclInEval) \
    v(operationAOTCreateDirectArguments) \
    v(operationAOTCreateScopedArguments) \
    v(operationAOTCreateClonedArguments) \
    v(operationAOTCreateRest) \
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
    v(operationAOTThrowTDZError) \
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
    v(operationAOTSizeFrameForVarargs) \
    v(operationAOTSetupVarargsFrame) \
    v(operationAOTPrepareTailCall) \
    v(operationAOTLinkCall) \
    v(operationAOTLinkFunction) \
    v(operationAOTConstructByCalling) \
    v(operationAOTNoteFilled) \
    v(operationAOTGiveData) \
    v(operationAOTCallDirectEval) \

// Property names that instructions imply rather than name, so that they are not among the function's identifiers.
enum class WellKnownIdentifier : uint32_t {
    Length,
    Next,
    Done,
    Value,
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

// Allocation.
JSC_DECLARE_JIT_OPERATION(operationAOTNewObject, JSObject*, (JSGlobalObject*, uint32_t inlineCapacity, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateThisWithProperties, JSObject*, (JSGlobalObject*, JSObject* callee, EncodedJSValue* values, uint32_t count, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTNewObjectLiteral, JSObject*, (JSGlobalObject*, EncodedJSValue* values, uint32_t count, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateThis, JSObject*, (JSGlobalObject*, JSObject* callee, uint32_t inlineCapacity));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArray, JSObject*, (JSGlobalObject*, const EncodedJSValue* values, uint32_t count, uint32_t indexingType));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayWithSize, JSObject*, (JSGlobalObject*, EncodedJSValue size));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayBuffer, JSObject*, (JSGlobalObject*, JSCell* immutableButterfly));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayWithSpread, JSObject*, (JSGlobalObject*, const EncodedJSValue* values, uint32_t count));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayWithSpecies, JSObject*, (JSGlobalObject*, EncodedJSValue length, JSObject* array));
JSC_DECLARE_JIT_OPERATION(operationAOTSpread, JSCell*, (JSGlobalObject*, EncodedJSValue iterable));
JSC_DECLARE_JIT_OPERATION(operationAOTNewRegExp, JSObject*, (JSGlobalObject*, JSCell* regExp));
JSC_DECLARE_JIT_OPERATION(operationAOTNewFunction, JSObject*, (JSGlobalObject*, JSScope*, uint32_t index, uint32_t isExpression, uint32_t functionKind, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTSetFunctionName, void, (JSGlobalObject*, JSObject* function, EncodedJSValue name));
JSC_DECLARE_JIT_OPERATION(operationAOTNewInternalFieldObject, JSObject*, (JSGlobalObject*, uint32_t kind));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateInternalFieldObject, JSObject*, (JSGlobalObject*, JSObject* callee, uint32_t kind));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateLexicalEnvironment, JSObject*, (JSGlobalObject*, JSScope*, JSCell* symbolTable, EncodedJSValue initialValue, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTPushWithScope, JSObject*, (JSGlobalObject*, JSScope*, EncodedJSValue object));
JSC_DECLARE_JIT_OPERATION(operationAOTResolveScopeForHoistingFuncDeclInEval, EncodedJSValue, (JSGlobalObject*, JSScope*, uint32_t identifierIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateDirectArguments, JSObject*, (JSGlobalObject*));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateScopedArguments, JSObject*, (JSGlobalObject*, JSObject* scope));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateClonedArguments, JSObject*, (JSGlobalObject*));
JSC_DECLARE_JIT_OPERATION(operationAOTCreateRest, JSObject*, (JSGlobalObject*, uint32_t numParametersToSkip));

// Conversions and tests.
JSC_DECLARE_JIT_OPERATION(operationAOTToThis, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTToObject, JSObject*, (JSGlobalObject*, EncodedJSValue, uint32_t messageIdentifierIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTToPrimitive, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTToPropertyKey, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTTypeof, JSCell*, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTTypeofIsObject, size_t, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTTypeofIsFunction, size_t, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTIsCallable, size_t, (EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTIsConstructor, size_t, (EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTStrcat, EncodedJSValue, (JSGlobalObject*, const EncodedJSValue* values, uint32_t count));
JSC_DECLARE_JIT_OPERATION(operationAOTGetPrototypeOf, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTInstanceof, size_t, (JSGlobalObject*, EncodedJSValue value, EncodedJSValue constructor));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowTDZError, void, (JSGlobalObject*, uint32_t isThis));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowStaticError, void, (JSGlobalObject*, EncodedJSValue message, uint32_t errorType));

// Properties.
JSC_DECLARE_JIT_OPERATION(operationAOTGetByIdWellKnown, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, uint32_t wellKnownIdentifier, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByIdReallocating, void, (VM*, JSObject* base, EncodedJSValue value, const void* megamorphicCacheStoreEntry));
JSC_DECLARE_JIT_OPERATION(operationAOTWriteBarrierAfterPut, void, (VM*, JSCell*));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByIdDirect, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, uint32_t identifierIndex, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByIdWithThis, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue thisValue, uint32_t identifierIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByValWithThis, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByIdWithThis, void, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue value, uint32_t identifierIndex, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByValWithThis, void, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByValDirect, void, (JSGlobalObject*, JSObject* base, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTInById, size_t, (JSGlobalObject*, EncodedJSValue base, uint32_t identifierIndex))
// Map.prototype.set and Set.prototype.add of a key that is not there, for a stub that has found that out (StubIntrinsic). The key
// is one that is its own normal form, and the hash is its hash.
JSC_DECLARE_JIT_OPERATION(operationAOTMapSet, void, (JSGlobalObject*, JSCell* map, EncodedJSValue key, EncodedJSValue value, int32_t hash));
JSC_DECLARE_JIT_OPERATION(operationAOTSetAdd, void, (JSGlobalObject*, JSCell* set, EncodedJSValue key, int32_t hash));
JSC_DECLARE_JIT_OPERATION(operationAOTInByVal, size_t, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTDelById, size_t, (JSGlobalObject*, EncodedJSValue base, uint32_t identifierIndex, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTDelByVal, size_t, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue property, uint32_t isStrict));
// Like everything that is called for a site these are told an identifier, which they have no use for.
JSC_DECLARE_JIT_OPERATION(operationAOTGetPrivateName, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue property, uint32_t, Slot*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTPutPrivateName, void, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value, uint32_t, Slot*, uint32_t isDefine));
JSC_DECLARE_JIT_OPERATION(operationAOTHasPrivateName, size_t, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTHasPrivateBrand, size_t, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue brand));
JSC_DECLARE_JIT_OPERATION(operationAOTCheckPrivateBrand, void, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue brand, uint32_t, Slot*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTSetPrivateBrand, void, (JSGlobalObject*, JSObject* base, EncodedJSValue brand));
JSC_DECLARE_JIT_OPERATION(operationAOTPutAccessorById, void, (JSGlobalObject*, JSObject* base, uint32_t identifierIndex, uint32_t attributes, JSObject* accessor, uint32_t isSetter));
JSC_DECLARE_JIT_OPERATION(operationAOTPutGetterSetterById, void, (JSGlobalObject*, JSObject* base, uint32_t identifierIndex, uint32_t attributes, EncodedJSValue getter, EncodedJSValue setter));
JSC_DECLARE_JIT_OPERATION(operationAOTPutAccessorByVal, void, (JSGlobalObject*, JSObject* base, EncodedJSValue property, uint32_t attributes, JSObject* accessor, uint32_t isSetter));
JSC_DECLARE_JIT_OPERATION(operationAOTDefineDataProperty, void, (JSGlobalObject*, JSObject* base, EncodedJSValue property, EncodedJSValue value, int32_t attributes));
JSC_DECLARE_JIT_OPERATION(operationAOTDefineAccessorProperty, void, (JSGlobalObject*, JSObject* base, EncodedJSValue property, EncodedJSValue getter, EncodedJSValue setter, int32_t attributes));

// for-in. The mode and the index are passed as the numbers the bytecode has them as. modeAndIndex: in and out.
JSC_DECLARE_JIT_OPERATION(operationAOTGetPropertyEnumerator, JSCell*, (JSGlobalObject*, EncodedJSValue base));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorNext, JSCell*, (JSGlobalObject*, EncodedJSValue base, JSCell* enumerator, EncodedJSValue* modeAndIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorGetByVal, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorInByVal, size_t, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorPutByVal, void, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue value, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTEnumeratorHasOwnProperty, size_t, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator));

// for-of. The ones that try something fast hand back the empty value if there is nothing for it but the protocol.
JSC_DECLARE_JIT_OPERATION(operationAOTIteratorOpenTryFast, EncodedJSValue, (JSGlobalObject*, EncodedJSValue iterable, EncodedJSValue symbolIterator, EncodedJSValue* next));
JSC_DECLARE_JIT_OPERATION(operationAOTAsyncIteratorOpenTryFast, EncodedJSValue, (JSGlobalObject*, EncodedJSValue iterable, EncodedJSValue symbolIterator, EncodedJSValue* next));
JSC_DECLARE_JIT_OPERATION(operationAOTIteratorNextTryFast, EncodedJSValue, (JSGlobalObject*, JSObject* iterator));
JSC_DECLARE_JIT_OPERATION(operationAOTIteratorNextWithIndex, EncodedJSValue, (JSGlobalObject*, EncodedJSValue iterable, EncodedJSValue* index));
JSC_DECLARE_JIT_OPERATION(operationAOTAsyncIteratorNextWithDriver, EncodedJSValue, (JSGlobalObject*, JSObject* iterator, JSObject* driver, EncodedJSValue resumeValue));
JSC_DECLARE_JIT_OPERATION(operationAOTMaterializeArrayIterator, JSObject*, (JSGlobalObject*, EncodedJSValue iterable, EncodedJSValue index));
JSC_DECLARE_JIT_OPERATION(operationAOTThrowIteratorResultIsNotObject, void, (JSGlobalObject*));

// Calls.
JSC_DECLARE_JIT_OPERATION(operationAOTSizeFrameForVarargs, size_t, (JSGlobalObject*, EncodedJSValue arguments, uint32_t numUsedStackSlots, uint32_t firstVarArgOffset));
JSC_DECLARE_JIT_OPERATION(operationAOTSetupVarargsFrame, CallFrame*, (JSGlobalObject*, CallFrame* newCallFrame, EncodedJSValue arguments, uint32_t firstVarArgOffset, uint32_t length));
JSC_DECLARE_JIT_OPERATION(operationAOTPrepareTailCall, size_t, (JSGlobalObject*, EncodedJSValue callee));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLinkCall, void, (JSGlobalObject*, EncodedJSValue callee, uint32_t knownCallee, Slot*, uint32_t isConstruct));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTConstructByCalling, UGPRPair, (CallFrame*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLinkFunction, void*, (CallFrame* calleeFrame, uint32_t index, uint32_t distanceOfEnvironment));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTNoteFilled, void, (Data*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTGiveData, void, (Instance*, uint32_t index));
JSC_DECLARE_JIT_OPERATION(operationAOTCallDirectEval, EncodedJSValue, (CallFrame* calleeFrame, JSScope*, EncodedJSValue thisValue, uint32_t bytecodeIndexBits, uint32_t lexicallyScopedFeatures));

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
