/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionTables.h"
#include "JSObject.h"
#include <span>
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct State;
class ReifiedSlimClosure;

template<typename Visitor>
ALWAYS_INLINE void visitSlimClosureCaptures(const JSCell* cell, uint32_t closureNumber, Visitor& visitor)
{
    const EncodedJSValue* captures = reinterpret_cast<const EncodedJSValue*>(reinterpret_cast<const uint8_t*>(cell) + offsetOfSlimClosureCaptures);
    unsigned numberOfReferences = captureCountsOfClosureNumber(closureNumber).references;
    for (unsigned i = 0; i < numberOfReferences; ++i)
        visitor.appendUnbarriered(JSValue::decode(captures[i]));
}

class SlimClosure final : public JSObject {
public:
    using Base = JSObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | GetOwnPropertySlotIsImpureForPropertyAbsence | OverridesGetOwnPropertyNames | OverridesPut | OverridesIsExtensible | OverridesGetCallData | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | ImplementsHasInstance | ImplementsDefaultHasInstance;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess mode>
    static CompleteSubspace* subspaceFor(VM& vm) { return vm.heap.aotClosureSpace<mode>(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static SlimClosure* create(VM&, Structure*, FunctionID, std::span<const uint64_t> captures = { }, unsigned numberOfReferences = 0);
    static SlimClosure* createForCompiledCode(VM&, Structure*, size_t allocationSize);

    static constexpr ptrdiff_t offsetOfWord() { return OBJECT_OFFSETOF(SlimClosure, m_word); }
    static constexpr ptrdiff_t offsetOfCaptures() { return sizeof(SlimClosure); }
    static constexpr size_t allocationSize(unsigned numberOfCaptures) { return sizeof(SlimClosure) + numberOfCaptures * sizeof(uint64_t); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    template<typename Visitor> ALWAYS_INLINE static bool tryVisitFields(SlimClosure*, Visitor&);
    static void analyzeHeap(JSCell*, HeapAnalyzer&);
    static size_t estimatedSize(JSCell*, VM&);
    static CallData getCallData(JSCell*);
    static CallData getConstructData(JSCell*);

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&) { return false; }
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned) { return true; }
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);
    static bool isExtensible(JSObject*, JSGlobalObject*) { return true; }
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);
    static bool customHasInstance(JSObject*, JSGlobalObject*, JSValue);

    uint32_t closureNumber() const { return ClosureWord::closureNumberOf(m_word); }
    FunctionID function() const { return functionOfClosureWord(m_word); }
    FunctionKindData kind() const { return kindOfClosureNumber(closureNumber()); }
    bool canConstruct() const { return FunctionTables::canConstruct(kind()); }
    bool isBound() const { return kind().isBound(); }
    std::span<const uint64_t> captures() const { return { reinterpret_cast<const uint64_t*>(this + 1), captureCountsOfClosureNumber(closureNumber()).captures }; }
    unsigned numberOfReferences() const { return captureCountsOfClosureNumber(closureNumber()).references; }
    State& state() const;
    bool nameWasLookedFor() const { return TypeInfo::perCellBit(inlineTypeFlags()); }
    void noteLookForName() { setPerCellBit(true); }
    enum class PrototypeObject : bool { MadeAtFirstRead, MadeNow };
    ReifiedSlimClosure* reify(VM&, PrototypeObject = PrototypeObject::MadeAtFirstRead);
    ReifiedSlimClosure* reifyWithAddedProperty(VM&, Structure* structureWithProperty, PropertyOffset, JSValue);

private:
    static constexpr unsigned numberOfInitialProperties = 4;
    Structure* fillInitialProperties(VM&, std::span<JSValue, numberOfInitialProperties>);
    ReifiedSlimClosure* becomeReified(VM&, Structure*, std::span<const JSValue>);

    SlimClosure(VM& vm, Structure* structure, uint64_t word)
        : Base(vm, structure)
        , m_word(word)
    {
    }

    uint64_t m_word;
};

template<typename Visitor>
ALWAYS_INLINE bool SlimClosure::tryVisitFields(SlimClosure* closure, Visitor& visitor)
{
    StructureID structureID = closure->structureID();
    if (structureID.isNuked())
        return false;
    WTF::loadLoadFence();
    if (structureID.decode()->typeInfo().type() != SlimClosureType)
        return false;
    uint64_t word = closure->m_word;
    WTF::loadLoadFence();
    if (closure->structureID() != structureID)
        return false;
    if (word)
        visitSlimClosureCaptures(closure, ClosureWord::closureNumberOf(word), visitor);
    return true;
}

class ReifiedSlimClosure final : public JSObjectWithButterfly {
public:
    using Base = JSObjectWithButterfly;
    enum class Variant : uint8_t { Ordinary, Bound };
    // A stack frame shows the property `name` of a function object from the first look for it on, in front of a displayName.
    enum class NameProperty : bool { HeldFromTheStart, TakenAtFirstLook };
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData | OverridesPut | ImplementsHasInstance | ImplementsDefaultHasInstance;
    static constexpr unsigned flagsThatSeeLooksForName = OverridesGetOwnPropertySlot | OverridesGetOwnSpecialPropertyNames;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess mode>
    static CompleteSubspace* subspaceFor(VM& vm) { return vm.heap.aotClosureSpace<mode>(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype, Variant, NameProperty);

    static constexpr ptrdiff_t offsetOfFunctionIndexInButterfly() { return offsetInButterfly(firstOutOfLineOffset) * static_cast<ptrdiff_t>(sizeof(EncodedJSValue)); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    template<typename Visitor> ALWAYS_INLINE static void visitFields(ReifiedSlimClosure* closure, Visitor& visitor) { visitSlimClosureCaptures(closure, closure->closureNumber(), visitor); }
    static void analyzeHeap(JSCell*, HeapAnalyzer&);
    static CallData getCallData(JSCell*);
    static CallData getConstructData(JSCell*);

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static void getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool customHasInstance(JSObject*, JSGlobalObject*, JSValue);
    // Under every other name defineOwnProperty() is that of an ordinary object.
    static bool hasOrdinaryDefineOwnProperty(VM&, PropertyName);

    uint32_t closureNumber() const { return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(butterfly()) + offsetOfFunctionIndexInButterfly()); }
    FunctionID function() const { return closureFunctionWithNumber(closureNumber()); }
    FunctionKindData kind() const { return kindOfClosureNumber(closureNumber()); }
    bool canConstruct() const { return FunctionTables::canConstruct(kind()); }
    bool isBound() const { return kind().isBound(); }
    std::span<const uint64_t> captures() const { return { reinterpret_cast<const uint64_t*>(this + 1), captureCountsOfClosureNumber(closureNumber()).captures }; }
    unsigned numberOfReferences() const { return captureCountsOfClosureNumber(closureNumber()).references; }
    State& state() const;
    JSValue lookForNameProperty(VM&);
    // ClosureWord::isRenamedBit.
    void markRenamed() { setPerCellBit(true); }

private:
    ReifiedSlimClosure() = delete;
};

JSC_DECLARE_CUSTOM_GETTER(createPrototypeProperty);
JSC_DECLARE_CUSTOM_GETTER(takeNameProperty);

static_assert(SlimClosure::offsetOfWord() == offsetOfSlimClosureWord);
static_assert(sizeof(SlimClosure) == offsetOfSlimClosureCaptures);
static_assert(sizeof(ReifiedSlimClosure) == sizeof(SlimClosure));

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
