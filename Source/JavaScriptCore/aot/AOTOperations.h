/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTRuntime.h"
#include "OperationResult.h"

namespace JSC {

class Exception;
class JSCell;
class JSObject;
class JSScope;

namespace AOT {

JSC_DECLARE_JIT_OPERATION(operationAOTValueAdd, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueSub, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueMul, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueDiv, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueMod, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValuePow, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueBitAnd, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueBitOr, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueBitXor, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueLShift, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueRShift, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueURShift, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueNegate, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueBitNot, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueInc, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueDec, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTToNumber, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTToNumeric, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTToString, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareLess, size_t, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareLessEq, size_t, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareGreater, size_t, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareGreaterEq, size_t, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareEq, size_t, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareStrictEq, size_t, (JSGlobalObject*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTGetById, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, uint32_t identifierIndex, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTPutById, void, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue value, uint32_t identifierIndex, Slot*, uint32_t flags));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTGetElementOrEmpty, EncodedJSValue, (JSGlobalObject*, EncodedJSValue array, EncodedJSValue index));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByVal, void, (JSGlobalObject*, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTResolveScope, JSObject*, (JSGlobalObject*, JSScope*, uint32_t identifierIndex, Slot*, uint32_t localScopeDepth));
JSC_DECLARE_JIT_OPERATION(operationAOTGetFromScope, EncodedJSValue, (JSGlobalObject*, JSObject* scope, uint32_t identifierIndex, Slot*, uint32_t getPutInfo));
JSC_DECLARE_JIT_OPERATION(operationAOTReadLazyClosureVar, EncodedJSValue, (JSGlobalObject*, JSObject* scope, uint32_t offset));
JSC_DECLARE_JIT_OPERATION(operationAOTFillImportSlot, JSObject*, (JSGlobalObject*, JSObject* importer, uint32_t slot));
JSC_DECLARE_JIT_OPERATION(operationAOTPutToScope, void, (JSGlobalObject*, JSObject* scope, EncodedJSValue value, uint32_t identifierIndex, Slot*, uint32_t how));
JSC_DECLARE_JIT_OPERATION(operationAOTThrow, void, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCheckType, void, (JSGlobalObject*, EncodedJSValue, uint32_t mask));
JSC_DECLARE_JIT_OPERATION(operationAOTGetLengthTheLongWay, EncodedJSValue, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCheckTypedLayout, void, (JSGlobalObject*, EncodedJSValue, uint32_t layoutID)); // Lowering::checkTypedLayout()
JSC_DECLARE_JIT_OPERATION(operationAOTCoerceToTypedLayout, EncodedJSValue, (JSGlobalObject*, EncodedJSValue, uint32_t layoutID)); // Lowering::coerceToTypedLayout()
// which: the number of the name | family << 32 | slot << 48 | whether undefined will do << 56.
JSC_DECLARE_JIT_OPERATION(operationAOTReadField, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, uint32_t which)); // Stub::ReadSlot0: id | slot << 16 | undefined will do << 24
JSC_DECLARE_JIT_OPERATION(operationAOTGetFieldTheLongWay, EncodedJSValue, (JSGlobalObject*, EncodedJSValue base, uint64_t which));
JSC_DECLARE_JIT_OPERATION(operationAOTValidateTypedObject, void, (JSGlobalObject*, JSObject*)); // Lowering::validateNewObject()
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTVerifyFact, size_t, (JSGlobalObject*, EncodedJSValue, uint64_t lowHalfOfType, uint64_t highHalfOfType, uint32_t which, uint32_t identifierIndexPlusOne, uint64_t scopeWhenCompiled, uint32_t scopeOffset));
JSC_DECLARE_JIT_OPERATION(operationAOTHandleTraps, void, (JSGlobalObject*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTWriteBarrier, void, (VM*, JSCell*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTCatch, Exception*, (VM*));
extern "C" UGPRPair SYSV_ABI findCallTarget(CallFrame* calleeFrame, CallLinkInfo*);
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTThrowStackOverflowError, void, (Instance*));
JSC_DECLARE_JIT_OPERATION(operationAOTFindEqualAtom, StringImpl*, (JSGlobalObject*, JSString*)); // Null: there is none.
JSC_DECLARE_JIT_OPERATION(operationAOTSwitchString, int32_t, (JSGlobalObject*, EncodedJSValue, uint32_t tableIndex, uint32_t whose));
JSC_DECLARE_JIT_OPERATION(operationAOTSwitchChar, int32_t, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTFMod, double, (double, double));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTPow, double, (double, double));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTDoubleToInt32, int32_t, (double));

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
