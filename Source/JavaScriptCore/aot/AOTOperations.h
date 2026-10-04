/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "OperationResult.h"

namespace JSC {

class Exception;
class JSCell;
class JSObject;
class JSScope;

namespace AOT {

JSC_DECLARE_JIT_OPERATION(operationAOTValueAdd, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueSub, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueMul, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueDiv, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueMod, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValuePow, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueBitAnd, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueBitOr, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueBitXor, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueLShift, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueRShift, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueURShift, EncodedJSValue, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueNegate, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueBitNot, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueInc, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTValueDec, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTToNumber, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTToNumeric, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTToString, EncodedJSValue, (Instance*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (Instance*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTProgramConstant, EncodedJSValue, (Instance*, uint32_t number));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTTemplateObject, EncodedJSValue, (Instance*, uint32_t number));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTCreateTransientConstant, EncodedJSValue, (Instance*, uint32_t number));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareLess, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareLessEq, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareGreater, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareGreaterEq, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareEq, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareStrictEq, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTGetById, EncodedJSValue, (Instance*, EncodedJSValue base, uint32_t identifierIndex, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTPutById, void, (Instance*, EncodedJSValue base, EncodedJSValue value, uint32_t identifierIndex, Slot*, uint32_t flags));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (Instance*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTGetElementOrEmpty, EncodedJSValue, (Instance*, EncodedJSValue array, EncodedJSValue index));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByVal, void, (Instance*, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTResolveScope, JSObject*, (Instance*, JSScope*, uint32_t identifierIndex, Slot*, uint32_t localScopeDepth));
JSC_DECLARE_JIT_OPERATION(operationAOTGetFromScope, EncodedJSValue, (Instance*, JSObject* scope, uint32_t identifierIndex, Slot*, uint32_t getPutInfo));
JSC_DECLARE_JIT_OPERATION(operationAOTReadLazyClosureVar, EncodedJSValue, (Instance*, JSObject* scope, uint32_t offset));
JSC_DECLARE_JIT_OPERATION(operationAOTFillImportSlot, JSObject*, (Instance*, JSObject* importer, uint32_t slot));
JSC_DECLARE_JIT_OPERATION(operationAOTPutToScope, void, (Instance*, JSObject* scope, EncodedJSValue value, uint32_t identifierIndex, Slot*, uint32_t how));
JSC_DECLARE_JIT_OPERATION(operationAOTThrow, void, (Instance*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCheckType, void, (Instance*, EncodedJSValue, uint32_t mask));
JSC_DECLARE_JIT_OPERATION(operationAOTCheckTypeAheadOfCreateThis, void, (Instance*, EncodedJSValue, uint32_t mask, JSObject* newTarget));
JSC_DECLARE_JIT_OPERATION(operationAOTGetLengthSlow, size_t, (Instance*, JSObject* array));
JSC_DECLARE_JIT_OPERATION(operationAOTCheckTypedLayout, void, (Instance*, EncodedJSValue, uint32_t layoutID));
JSC_DECLARE_JIT_OPERATION(operationAOTCoerceToTypedLayout, EncodedJSValue, (Instance*, EncodedJSValue, uint32_t layoutID));
JSC_DECLARE_JIT_OPERATION(operationAOTReadField, EncodedJSValue, (Instance*, EncodedJSValue base, uint32_t which));
JSC_DECLARE_JIT_OPERATION(operationAOTCountGuessedPlace, void, (Instance*, EncodedJSValue base, uint32_t which, uint32_t identifierIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTToFieldValue, EncodedJSValue, (Instance*, EncodedJSValue, uint64_t packedFieldType, uint32_t identifierIndex));
JSC_DECLARE_JIT_OPERATION(operationAOTGetFieldSlow, EncodedJSValue, (Instance*, EncodedJSValue base, uint64_t which));
JSC_DECLARE_JIT_OPERATION(operationAOTValidateTypedObject, void, (Instance*, JSObject*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTVerifyInferredType, size_t, (Instance*, EncodedJSValue, uint64_t typeLowHalf, uint64_t typeHighHalf, uint32_t which, uint32_t identifierIndexPlusOne, uint64_t scopeWhenCompiled, uint32_t scopeOffset));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTReportBranchFoldedWrongly, void, (Instance*, uint32_t branch, uint32_t tested, uint32_t isTaken));
JSC_DECLARE_JIT_OPERATION(operationAOTHandleTraps, void, (Instance*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTWriteBarrier, void, (VM*, JSCell*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTCatch, Exception*, (VM*));
extern "C" UGPRPair SYSV_ABI findCallTarget(CallFrame* calleeFrame, CallLinkInfo*);
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTThrowStackOverflowError, void, (Instance*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTThrowCalledIndirectlyError, void, (Instance*, EncodedJSValue callee));
JSC_DECLARE_JIT_OPERATION(operationAOTLatin1StringEqualTo, StringImpl*, (Instance*, JSString*));
JSC_DECLARE_JIT_OPERATION(operationAOTSwitchString, int32_t, (Instance*, EncodedJSValue, uint32_t tableIndex, uint32_t whose));
JSC_DECLARE_JIT_OPERATION(operationAOTSwitchChar, int32_t, (Instance*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTFMod, double, (double, double));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTPow, double, (double, double));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTDoubleToInt32, int32_t, (double));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
