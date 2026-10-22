/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "CollectionScope.h"
#include "JSCJSValue.h"
#include "VM.h"
#include <algorithm>

namespace JSC { namespace AOT {

class Object;
enum class Entry : uint16_t;
struct Instance;
struct RuntimeType;

// Append only: these numbers are immediates in the code of a validated image.
enum class ValidationFailureKind : uint8_t {
    ValueType,
    UnreachableValue,
    FrameCellEscaped,
    SavedFrameRootMissing,
    VisitedCellsDiffer,
    OperationEnteredVM,
    OperationCollected,
    CacheHitDiffers,
    CacheEntryDropped,
    ImpossibleMutation,
    TypeTestsDiffer,
    BarrierWasNeeded,
    BuiltinNotImmutable,
    ArrayNotPristine,
    ShapeWordInvalid,
    StackTraceDiffers,
    FastPathResultDiffers,
    OperationPreconditionBroken,
    NativeCallChangedOperand,
    NativeCallWroteHeap,
    NativeCallRanCode,
    NativeCallThrew,
    NativeCallCollected,
    NativeCallKeptOperand,
    AssumedType,
    UnlistedCallee,
    SecondRun,
    ReadsDiffer,
    ContainerContents,
    ArrayStorageKind,
    RemovedFieldNamed,
    ReceiverShape,
    CallChangedKeptRegister,
    UnwindingChangedKeptRegister,
    ArrayElementsNotOrdinary,
    KeysListed,
    LegacyRegExpStaticRead,
    GuardNoLongerHolds,
    BindingReadBeforeStore,
    SlotEmpty,
    LookupDiffers,
    HeapSlotType,
    MaskCutKnownValue,
    FamilyNotClosed,
    FunctionCellInFrameGotProperties,
    OperationConvertedCell,
    OperationWroteHeap,
};

static constexpr unsigned numberOfValidationFailureKinds = 47;

ASCIILiteral nameOf(ValidationFailureKind);

struct ValidationFailure {
    ValidationFailureKind kind;
    FunctionID function;
    Typed::ValueID value;
    JSValue found;
};

// What ValidationFailure::found holds for CallChangedKeptRegister and UnwindingChangedKeptRegister: an int32 with the register and, if a call names it, the callee.
struct ChangedRegister {
    static constexpr unsigned calleeShift = 8;
    static constexpr uint32_t unknownCallee = (1u << (32 - calleeShift)) - 1;
    static constexpr uint32_t calleeIsStub = 1u << (31 - calleeShift);
    static constexpr unsigned unknownRegister = (1u << calleeShift) - 1;

    static constexpr int32_t encode(unsigned registerIndex, uint32_t callee)
    {
        return static_cast<int32_t>(registerIndex | std::min(callee, unknownCallee) << calleeShift);
    }

    template<typename Family>
    static constexpr int32_t encodeForStub(Family family)
    {
        return encode(unknownRegister, calleeIsStub | static_cast<uint32_t>(family));
    }
};

// The third argument of operationAOTCompareOperandWithPhotograph: which photograph, and who was called between it and the comparison: an entry of the run-time table or a family of stubs.
struct PhotographedCall {
    static constexpr unsigned maxOperands = 8;
    static constexpr unsigned calleeShift = 8;
    static constexpr uint32_t calleeIsStub = 1u << (31 - calleeShift);

    static constexpr uint32_t encode(unsigned position, uint32_t callee) { return position | callee << calleeShift; }
};

[[noreturn]] JS_EXPORT_PRIVATE void crashOnValidationFailure(VM&, const ValidationFailure&);

// Looks at no more than a fixed number of the values a large container holds.
bool contentsHaveType(Instance&, JSCell* container, Typed::ContentsSlot, const RuntimeType&);
bool hasOrdinaryElements(VM&, JSValue array);
// RegExp.$1, RegExp.lastMatch and their kin. A match of a program that never reads them leaves nothing for them.
JS_EXPORT_PRIVATE void willReadLegacyRegExpStatics(VM&);

// A slot is empty, holds undefined or null as a marker, or holds a value of the type of its field. Anything else was stored by a writer that tests no type.
// Allocates nothing and runs no code, so a collector thread may call it while the mutators are stopped.
JS_EXPORT_PRIVATE void validateSlotTypes(VM&, const Object&);
// Also looks at what died since the last collection. Not for a thread that collects.
JS_EXPORT_PRIVATE void validateSlotTypesOfLiveObjects(VM&);

ALWAYS_INLINE bool validatesRuntime(VM& vm) { return vm.aotState().validatesRuntime; }

struct OperationPermissions {
    bool mayEnterVM { true };
    bool mayCollect { true };
};

OperationPermissions permissionsOf(Entry);

struct CollectionValidator {
    void didFinishCollection(VM&, CollectionScope);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
