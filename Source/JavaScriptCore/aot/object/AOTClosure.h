/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionTables.h"
#include "AOTState.h"
#include "JSFunction.h"
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct LayoutData;
struct State;

class Closure final : public JSFunction {
public:
    using Base = JSFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess mode>
    static CompleteSubspace* subspaceFor(VM& vm) { return vm.heap.aotClosureSpace<mode>(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static Closure* create(VM&, Structure*, FunctionID, std::span<const uint64_t> captures = { }, unsigned numberOfReferences = 0);
    static Closure* createForCompiledCode(VM&, Structure*, size_t allocationSize);

    static constexpr unsigned maxNumberOfCaptures = UINT8_MAX;
    static constexpr ptrdiff_t offsetOfEntry() { return OBJECT_OFFSETOF(Closure, m_entry); }
    static constexpr ptrdiff_t offsetOfCaptures() { return sizeof(Closure); }
    static constexpr size_t allocationSize(unsigned numberOfCaptures) { return sizeof(Closure) + numberOfCaptures * sizeof(uint64_t); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    template<typename Visitor> ALWAYS_INLINE static void visitFields(Closure*, Visitor&);
    static void analyzeHeap(JSCell*, HeapAnalyzer&);
    static CallData getCallData(JSCell*);
    static CallData getConstructData(JSCell*);

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static void getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);

    uint32_t closureNumber() const { return ClosureWord::closureNumberOf(m_entry); }
    FunctionKindData kind() const { return kindOfClosureNumber(closureNumber()); }
    std::span<const uint64_t> captures() const { return { reinterpret_cast<const uint64_t*>(this + 1), captureCountsOfClosureNumber(closureNumber()).captures }; }
    unsigned numberOfReferences() const { return captureCountsOfClosureNumber(closureNumber()).references; }
    JSCell* environment() const { return numberOfReferences() ? std::bit_cast<JSCell*>(captures()[0]) : nullptr; }
    FunctionID function() const { return functionOfClosureWord(m_entry); }
    uint64_t entry() const { return m_entry; }
    State& state() const;
    const LayoutData* classData() const;
    bool isLazyClassObject() const;
    bool mayHoldOwnProperties(VM&, std::span<UniquedStringImpl* const> names) const;
    bool isLazyClassObjectOf(const LayoutData&) const;
    bool hasStructureOfNewClosure(VM&) const;
    bool hasOrdinaryDefineOwnProperty(VM&, PropertyName) const;
    static bool isNameThatBuiltinFunctionsRefuse(VM&, UniquedStringImpl*);
    bool refusesOwnProperty(VM&, PropertyName) const;
    bool holdsStaticFieldUnder(UniquedStringImpl*) const;
    bool valueUnderIsHeardByNobody(UniquedStringImpl*) const;
    bool isBuiltinFunction() const;
    bool hasStaticFieldsLeft() const;
    bool canHoldStaticFieldInVariable(UniquedStringImpl*) const;
    void ensureClassObject(VM&);
    void ensureStaticMethods(VM&);
    void initializeClassObject(VM&, JSObject* parent, JSObject* prototype);
    JSValue prototypeProperty(VM&);
    void setPrototypeProperty(VM&, JSObject* prototype, unsigned attributes);
    void setNameProperty(VM&, JSString*, unsigned attributes);

private:
    Closure(VM& vm, Structure* structure, NativeExecutable* executable, uint64_t entry)
        : Base(vm, executable, structure->realm(), structure)
        , m_entry(entry)
    {
    }

    bool ensurePrototypeProperty(VM&);
    bool tryCreatePrototypeProperty(VM&, JSValue);

    uint64_t m_entry;
};
static_assert(sizeof(JSFunction) == offsetOfClosureEntry && Closure::offsetOfEntry() == offsetOfClosureEntry && sizeof(Closure) == offsetOfClosureCaptures);
static_assert(offsetOfCopyInHeapOfClosureInFrame(Closure::offsetOfCaptures(), 3) == static_cast<ptrdiff_t>(Closure::allocationSize(3)));

ALWAYS_INLINE bool isCompiledClosure(const JSCell* cell) { return cell->type() == CompiledClosureType; }

JSValue findOnChainAbove(VM&, const JSObject*, UniquedStringImpl*, unsigned& attributes);
bool holdsWhatIsFoundAbove(VM&, const JSObject*, UniquedStringImpl*, JSValue, unsigned attributes);
void holdClosureToLackingNameSlowCase(VM&, const JSCell* closure, UniquedStringImpl*);
bool closureAdmitsName(VM&, const JSCell* closure, UniquedStringImpl*);
ALWAYS_INLINE void holdClosureToLackingName(VM& vm, const JSCell* cell, UniquedStringImpl* uid)
{
    if (isCompiledCalleeType(cell->type())) [[unlikely]]
        holdClosureToLackingNameSlowCase(vm, cell, uid);
}
void holdClosureToItsPrototype(VM&, const JSCell* closure);

Structure* createStructureOfScopesInHeapSnapshots(VM&, JSGlobalObject*);
JSCell* createScopeInHeapSnapshot(VM&, Structure*, JSCell* closure);
bool hasScopeInHeapSnapshots(JSCell*);

template<typename Visitor>
ALWAYS_INLINE void Closure::visitFields(Closure* closure, Visitor& visitor)
{
    uint64_t entry = closure->m_entry;
    if (!entry)
        return;
    unsigned numberOfReferences = captureCountsOfClosureNumber(ClosureWord::closureNumberOf(entry)).references;
    for (uint64_t reference : std::span { reinterpret_cast<const uint64_t*>(closure + 1), numberOfReferences })
        visitor.appendUnbarriered(JSValue::decode(reference));
}

JSC_DECLARE_HOST_FUNCTION(callClassConstructor);
JSC_DECLARE_HOST_FUNCTION(callCompiledClosure);
JSC_DECLARE_HOST_FUNCTION(constructCompiledClosure);

enum class LookupPosition : uint8_t { Receiver, PrototypeChain };

bool tryDefineClassMemberWithoutTransition(JSGlobalObject*, Closure& classObject, const LayoutData& classLayout, JSObject* holder, PropertyName, JSObject* function, MethodKind);
bool mayHaveLazyProperty(VM&, JSCell*, UniquedStringImpl*, LookupPosition);
bool closureHandlesPrototypeProperty(JSCell*);
GetValueFunc closurePrototypePropertyGetter();

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
