/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "CodeBlock.h"
#include "FrameTracers.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

ALWAYS_INLINE FunctionRef caller(Instance* instance, CallFrame* callFrame) { return FunctionRef::at(instance, removeCodePtrTag(callFrame->rawReturnPC())); }
ALWAYS_INLINE FunctionRef callerBytecodeOwner(Instance* instance, CallFrame* callFrame)
{
    FunctionRef function = caller(instance, callFrame);
    if (!function.info().hasInlineFrames) [[likely]]
        return function;
    return function.locationForReturnAddress(removeCodePtrTag(callFrame->rawReturnPC()), callFrame->callerFrame()).function;
}
ALWAYS_INLINE BytecodeIndex callerBytecodeIndex(Instance* instance, CallFrame* callFrame) { return caller(instance, callFrame).bytecodeIndexAt(removeCodePtrTag(callFrame->rawReturnPC()), callFrame->callerFrame()); }

ALWAYS_INLINE void countOperationNamed(Instance* instance, const char* name, const char* detail = nullptr)
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(*instance->vm).countOperation(name, detail);
}

ALWAYS_INLINE void countOperationNamed(VM& vm, const char* name)
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countOperation(name);
}

struct NamedAccess {
    const char* key;
    const char* receiver;
    const char* place;
    ASCIILiteral className;
};

inline NamedAccess classifyNamedAccess(VM& vm, JSValue base, JSValue key, UniquedStringImpl* name)
{
    const char* keyKind = "key-is-constant";
    if (!name) {
        if (key.isSymbol()) {
            keyKind = "key-is-symbol";
            name = &asSymbol(key)->privateName().uid();
        } else if (key.isString()) {
            const StringImpl* impl = asString(key)->tryGetValueImpl();
            keyKind = !impl ? "key-is-rope" : impl->isAtom() ? "key-is-atom" : "key-is-string-but-no-atom";
            if (impl && impl->isAtom())
                name = static_cast<UniquedStringImpl*>(const_cast<StringImpl*>(impl));
        } else
            keyKind = key.isNumber() ? "key-is-number" : "key-is-other";
    }
    if (name && !name->isSymbol() && parseIndex(*name)) {
        keyKind = "key-is-index-string";
        name = nullptr;
    }
    const char* receiver = "receiver-is-not-object";
    const char* place = "property-is-unknown";
    ASCIILiteral className = ""_s;
    if (base.isObject() && asObject(base)->type() != FinalObjectType) {
        receiver = "receiver-is-not-final";
        className = asObject(base)->classInfo()->className;
    } else if (base.isObject()) {
        constexpr unsigned namedSlots = Structure::numberOfSlotsWithPropertyNameIDs;
        Structure* structure = asObject(base)->structure();
        receiver = structure->isDictionary() ? "receiver-is-dictionary" : !structure->recordsPropertyNames() ? "receiver-has-field-ids" : structure->outOfLineSize() ? "receiver-has-out-of-line-properties"
            : structure->inlineSize() > namedSlots ? "receiver-has-properties-beyond-named-slots" : "receiver-is-all-in-named-slots";
        if (name) {
            unsigned attributes = 0;
            PropertyOffset offset = structure->get(vm, name, attributes);
            auto inSlot = [&] {
                uint16_t id = structure->fieldIDInSlot(offset);
                return !id ? "property-is-in-slot-not-known-yet" : id == Structure::noPropertyNameID ? "property-is-in-slot-marked-nameless" : id == Structure::ambiguousFieldID ? "property-is-in-slot-of-structure-that-records-nothing" : "property-has-id-in-slot";
            };
            auto notOwn = [&]() -> const char* {
                unsigned depth = 0;
                for (JSValue next = asObject(base)->getPrototypeDirect(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
                    JSObject* holder = asObject(next);
                    Structure* holderStructure = holder->structure();
                    ++depth;
                    if (holder->type() == ProxyObjectType || holderStructure->typeInfo().overridesGetOwnPropertySlot() || (holderStructure->typeInfo().hasStaticPropertyTable() && !holderStructure->staticPropertiesReified()))
                        return "property-may-be-on-prototype-that-cannot-be-asked";
                    unsigned holderAttributes = 0;
                    if (holderStructure->get(vm, name, holderAttributes) == invalidOffset)
                        continue;
                    if (holderAttributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
                        return depth == 1 ? "property-is-inherited-accessor-at-depth-1" : depth == 2 ? "property-is-inherited-accessor-at-depth-2" : "property-is-inherited-accessor-deeper";
                    return depth == 1 ? "property-is-inherited-value-at-depth-1" : depth == 2 ? "property-is-inherited-value-at-depth-2" : "property-is-inherited-value-deeper";
                }
                bool isPlain = asObject(base)->getPrototypeDirect() == structure->realm()->objectPrototype();
                return isPlain ? "property-is-absent-under-object-prototype" : "property-is-absent-under-another-chain";
            };
            place = offset == invalidOffset ? notOwn() : attributes ? "property-has-attributes" : !isInlineOffset(offset) ? "property-is-out-of-line"
                : static_cast<unsigned>(offset) >= namedSlots ? "property-is-beyond-named-slots" : inSlot();
        }
    }
    return { keyKind, receiver, place, className };
}

inline void noteNamedAccess(Instance* instance, const char* operation, JSValue base, JSValue key, UniquedStringImpl* name = nullptr)
{
    if (!Options::useAOTOperationCounters()) [[likely]]
        return;
    VM& vm = *instance->vm;
    NamedAccess access = classifyNamedAccess(vm, base, key, name);
    auto& table = runtimeTable(vm);
    for (const char* detail : { access.key, access.receiver, access.place })
        table.countOperation(operation, detail);
    table.noteGuest("named-access", makeString(StringView::fromLatin1(operation), ' ', StringView::fromLatin1(access.key), ' ', StringView::fromLatin1(access.receiver), access.className.isEmpty() ? ""_s : ":"_s, access.className, ' ', StringView::fromLatin1(access.place)), { });
}

ALWAYS_INLINE void countOperationBySlotState(Instance* instance, const char* name, const Slot* slot)
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(*instance->vm).countOperationBySlotState(name, slot);
}

ALWAYS_INLINE void countOperationAtSite(Instance* instance, CallFrame* callFrame, const char* name)
{
    if (!Options::useAOTOperationCounters()) [[likely]]
        return;
    FunctionRef function = callerBytecodeOwner(instance, callFrame);
    BytecodeIndex bytecodeIndex = callerBytecodeIndex(instance, callFrame);
    LineColumn position = function.lineColumnFor(bytecodeIndex);
    runtimeTable(*instance->vm).countOperationAtSite(name, function.index, bytecodeIndex.offset(), position.line, position.column);
}

ALWAYS_INLINE void countOperationFor(Instance* instance, CallFrame* callFrame)
{
    constexpr uint32_t samplingInterval = 8;
    instance->operationSamplingState = instance->operationSamplingState * 1664525u + 1013904223u;
    static_assert(samplingInterval == 1u << 3);
    if (instance->operationSamplingState >> (32 - 3)) [[likely]]
        return;
    FunctionRef function = caller(instance, callFrame);
    if (!function.instance->dataIfExists(function.index)) [[unlikely]]
        function.instance->countMisses(function.index, samplingInterval);
}

#define AOT_OPERATION_BEGIN(instance) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    UNUSED_VARIABLE(globalObject); \
    VM& vm = *(instance)->vm; \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    ++(instance)->effectEpoch; \
    countOperationFor(instance, callFrame); \
    countOperationNamed(instance, __func__); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

#define AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    UNUSED_VARIABLE(globalObject); \
    VM& vm = *(instance)->vm; \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    ++(instance)->effectEpoch; \
    countOperationNamed(instance, __func__); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

ALWAYS_INLINE Data* callerData(Instance* instance, CallFrame* callFrame)
{
    FunctionRef function = caller(instance, callFrame);
    Data* data = function.instance->dataIfExists(function.index);
    return data ? data : function.instance->sharedData;
}
ALWAYS_INLINE UnlinkedCodeBlock* callerCode(Instance* instance, CallFrame* callFrame) { return callerBytecodeOwner(instance, callFrame).ensureUnlinkedCodeBlock(); }

ALWAYS_INLINE void didFillSlot(VM&, Data* data)
{
    if (data == SharedData::get())
        return;
    data->slotEpoch++;
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
}
ALWAYS_INLINE const Identifier& identifierAt(Instance* instance, CallFrame* callFrame, unsigned index) { UNUSED_PARAM(callFrame); return instance->program->identifierAsIdentifier(index); }
ALWAYS_INLINE FunctionRef callerBytecodeOwner(Instance* instance, CallFrame* callFrame, uint32_t whose)
{
    FunctionRef function = caller(instance, callFrame);
    if (!whose) [[likely]]
        return function;
    return { function.instance, function.info().function()->knownCallees()[whose - 1] };
}
ALWAYS_INLINE PutPropertySlot::Context putByIdContextOf(Instance* instance, CallFrame* callFrame) { return caller(instance, callFrame).codeType() == EvalCode ? PutPropertySlot::PutByIdEval : PutPropertySlot::PutById; }

template<typename CellType>
ALWAYS_INLINE void prepareInlineAllocation(Instance* instance, Instance::InlineAllocation kind, Structure* structure)
{
    auto& data = instance->inlineAllocations[static_cast<unsigned>(kind)];
    if (data.allocator) [[likely]]
        return;
    static_assert(!JSCell::structureIDOffset() && JSCell::indexingTypeAndMiscOffset() == sizeof(uint32_t));
    data.header = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
    data.allocator = subspaceFor<CellType>(*instance->vm)->allocatorFor(sizeof(CellType), AllocatorForMode::EnsureAllocator).localAllocator();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
