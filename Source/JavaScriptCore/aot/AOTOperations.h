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
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (Instance*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTGetPlainly, EncodedJSValue, (JSCell* base, UniquedStringImpl*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTHasPlainly, size_t, (JSCell* base, UniquedStringImpl*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTGetPlainlyAtSite, EncodedJSValue, (JSCell* base, UniquedStringImpl*, PolymorphicSlots*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTGetPlainlyAndFill, EncodedJSValue, (JSCell* base, UniquedStringImpl*, Instance*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTGetByKeyPlainly, EncodedJSValue, (JSCell* base, JSCell* key, Instance*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTPutByKeyPlainly, size_t, (JSCell* base, JSCell* key, EncodedJSValue, Instance*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTHasByKeyPlainly, size_t, (JSCell* base, JSCell* key, Instance*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTPrepareGetOfProxy, uintptr_t, (Instance*, JSCell* proxy, uint32_t name, EncodedJSValue* results));
JSC_DECLARE_JIT_OPERATION(operationAOTValidateGetOfProxy, EncodedJSValue, (Instance*, JSCell* target, uint32_t name, EncodedJSValue answer));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareLess, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareLessEq, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareGreater, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareGreaterEq, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareEq, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareEqToText, size_t, (Instance*, EncodedJSValue, uint64_t text));
JSC_DECLARE_JIT_OPERATION(operationAOTCompareStrictEq, size_t, (Instance*, EncodedJSValue, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTGetById, EncodedJSValue, (Instance*, EncodedJSValue base, uint32_t identifierIndex, Slot*));
JSC_DECLARE_JIT_OPERATION(operationAOTPutById, void, (Instance*, EncodedJSValue base, EncodedJSValue value, uint32_t identifierIndex, Slot*, uint32_t flags));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByNameAndFillTable, EncodedJSValue, (Instance*, EncodedJSValue base, uint32_t name));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByNameAndFillTable, void, (Instance*, EncodedJSValue base, EncodedJSValue value, uint32_t name, uint32_t flagBits));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (Instance*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByVal, void, (Instance*, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict));
JSC_DECLARE_JIT_OPERATION(operationAOTGetByValAndCache, EncodedJSValue, (Instance*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByValAndCache, void, (Instance*, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value));
JSC_DECLARE_JIT_OPERATION(operationAOTPutByValAndCacheSloppy, void, (Instance*, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value));
JSC_DECLARE_JIT_OPERATION(operationAOTInByValAndCache, size_t, (Instance*, EncodedJSValue base, EncodedJSValue property));
JSC_DECLARE_JIT_OPERATION(operationAOTDeleteByValAndCache, void, (Instance*, EncodedJSValue base, EncodedJSValue property, uint32_t, Slot*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTDeleteByValAndCacheSloppy, size_t, (Instance*, EncodedJSValue base, EncodedJSValue property, uint32_t, Slot*, uint32_t));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTWriteBarrier, void, (VM*, JSCell*));
#if AOT_TESTS_SHADOW_IN_COMPILED_CODE
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTReportPoisonedAccess, void, (void*, uint32_t, Instance*));
#endif
extern "C" UGPRPair SYSV_ABI findCallTarget(CallFrame* calleeFrame, CallLinkInfo*);
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTThrowStackOverflowError, void, (Instance*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandler, UGPRPair, (VM*, Instance*));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
