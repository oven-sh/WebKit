/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTSlotRepresentation.h"
#include "CollectionScope.h"
#include "JSCJSValue.h"
#include "MarkedBlock.h"
#include "VM.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <tuple>
#include <wtf/BitSet.h>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class Object;
enum class Entry : uint16_t;
struct FieldData;
struct LayoutData;
struct RuntimeType;

using Typed::ValidationFailureKind;

struct ValidationFailure {
    ValidationFailureKind kind;
    FunctionID function;
    Typed::ValueID value;
    JSValue found;
    // How compiled code held a value that has no encoding to read its class from, or Void if the check looked at no value. `found` is empty in both cases.
    Typed::Representation unboxedAs { Typed::Representation::Unchosen };
};

// The third argument of operationAOTReportValidationFailure.
struct ReportedFailure {
    static constexpr unsigned unboxedAsShift = 16;

    static constexpr uint32_t encode(ValidationFailureKind kind, Typed::Representation unboxedAs) { return static_cast<uint32_t>(kind) | static_cast<uint32_t>(unboxedAs) << unboxedAsShift; }
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

// What ValidationFailure::found holds for CallCollected and CallThrew: an int32 that says who was called.
struct CalledCode {
    enum class Kind : uint32_t { Operation, Stub, Function, Address };
    static constexpr unsigned kindShift = 28;
    static constexpr uint32_t maxNumber = (1u << kindShift) - 1;

    static constexpr int32_t encode(Kind kind, uint32_t number) { return static_cast<int32_t>(static_cast<uint32_t>(kind) << kindShift | std::min(number, maxNumber)); }
};

// In a validated image two moves stand behind every call that has no site: into a scratch register on x86-64, into the zero register on ARM64. They say which value of the function calls, and whom.
struct MarkOfCallWithoutSite {
    static constexpr uint64_t tag = 0xa07c;
    static constexpr unsigned tagShift = 32;

    static constexpr uint64_t wordFor(uint32_t payload) { return tag << tagShift | payload; }
    static std::optional<MarkOfCallWithoutSite> read(const void* returnAddress);

    Typed::ValueID value;
    int32_t callee;
};

// The third argument of operationAOTCompareOperandWithPhotograph: which photograph, and who was called between it and the comparison: an entry of the run-time table or a family of stubs.
struct PhotographedCall {
    static constexpr unsigned maxOperands = 8;
    static constexpr unsigned calleeShift = 8;
    static constexpr uint32_t calleeIsStub = 1u << (31 - calleeShift);

    static constexpr uint32_t encode(unsigned position, uint32_t callee) { return position | callee << calleeShift; }
};

[[noreturn]] JS_EXPORT_PRIVATE void crashOnValidationFailure(VM&, const ValidationFailure&);

// RegExp.$1, RegExp.lastMatch and their kin. A match of a program that never reads them leaves nothing for them.
JS_EXPORT_PRIVATE void willReadLegacyRegExpStatics(VM&);

struct SlotOutsideType {
    const FieldData* field;
    uint64_t bits;
    // Otherwise nothing was made of the bits.
    bool isFormOfValue;
    JSValue found;
};

// A slot is empty, holds undefined or null as a marker, or holds a value of the type of its field. A reference is followed only to a cell of the heap.
JS_EXPORT_PRIVATE std::optional<SlotOutsideType> findSlotOutsideType(VM&, const Object&);
JS_EXPORT_PRIVATE void validateSlotTypes(VM&, const Object&);

ALWAYS_INLINE bool validatesRuntime(VM& vm) { return vm.aotState().validatesRuntime; }

// True once an image built with --aot-testing=CheckProofsAtRunTime is attached. Its code is identical to a production build's.
JS_EXPORT_PRIVATE extern std::atomic<bool> imageChecksProofs;
// The compiler's claims about the objects of a layout (which changes never happen to them, which names they ever get, what their slots hold)
// are rechecked whenever the runtime changes an object, and for every object that survives a garbage collection.
// The check changes nothing that the program or compiled code can observe, so it also runs against production code.
ALWAYS_INLINE bool checksClaimsAboutObjects(VM& vm) { return vm.aotState().validatesRuntime || imageChecksProofs.load(std::memory_order_relaxed); }

struct OperationPermissions {
    bool mayEnterVM { true };
    bool mayCollect { true };
};

OperationPermissions permissionsOf(Entry);

using CollectionVersions = std::tuple<HeapVersion, HeapVersion, HeapVersion>;
CollectionVersions collectionVersionsOf(VM&);

// What is asked of a slot is the same for every object of a layout: whether a type describes it at all, where it lies, whether the type admits the markers for undefined and null,
// and between which integers a number lies. It is worked out once, at the first object of the layout that is looked at.
struct SlotCheck {
    const FieldData* field;
    const RuntimeType* type;
    const SlotTraits* traits;
    // Of a slot for integers. Empty where the type admits none.
    int32_t leastInteger;
    int32_t greatestInteger;
    uint32_t offset;
    SlotRepresentation representation;
    bool admitsUndefined;
    bool admitsNull;
};

struct SlotPlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(SlotPlan);
    struct Range {
        uint16_t begin { 0 };
        uint16_t end { 0 };
    };
    // Those of the cell, then those of the Extension, by granule.
    Vector<SlotCheck> checks;
    uint32_t numberInCell { 0 };
    uint64_t granules { 0 };
    std::array<Range, 64> checksOfGranule { };
};

struct RecheckedCells {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RecheckedCells);
    struct OfBlock {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(OfBlock);
        WTF::BitSet<MarkedBlock::atomsPerBlock> cells;
        uint64_t lastCollection { 0 };
    };
    UncheckedKeyHashMap<MarkedBlock*, std::unique_ptr<OfBlock>> blocks;
    uint64_t numberOfCollections { 0 };
    // A layout is data of an image, and so are the types of its fields: a plan never goes stale.
    UncheckedKeyHashMap<const LayoutData*, std::unique_ptr<SlotPlan>> plans;
    const LayoutData* layoutAskedLast { nullptr };
    const SlotPlan* planAskedLast { nullptr };
};

struct CollectionValidator {
    void didFinishCollection(VM&, CollectionScope);
};
inline CollectionValidator collectionValidator;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
