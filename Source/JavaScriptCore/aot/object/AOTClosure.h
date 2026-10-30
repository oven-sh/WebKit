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

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype, bool isBound = false);
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
    static bool customHasInstance(JSObject*, JSGlobalObject*, JSValue);

    uint32_t closureNumber() const { return ClosureWord::closureNumberOf(m_entry); }
    FunctionKindData kind() const { return kindOfClosureNumber(closureNumber()); }
    std::span<const uint64_t> captures() const { return { reinterpret_cast<const uint64_t*>(this + 1), captureCountsOfClosureNumber(closureNumber()).captures }; }
    unsigned numberOfReferences() const { return captureCountsOfClosureNumber(closureNumber()).references; }
    JSCell* environment() const { return numberOfReferences() ? std::bit_cast<JSCell*>(captures()[0]) : nullptr; }
    FunctionID function() const { return functionOfClosureWord(m_entry); }
    uint64_t entry() const { return m_entry; }
    void markRenamed() { m_entry |= 1ULL << ClosureWord::isRenamedBit; }
    State& state() const { return State::of(structure()->realm()); }
    const LayoutData* classData() const;
    bool isLazyClassObject() const;
    // Nothing was ever stored in, defined on or deleted from a closure that has the Structure it was made with, so its name and its length are those of its function.
    bool hasStructureOfNewClosure(VM&) const;
    // A look that found the Structure of a new closure left it there. The note stays when the Structure changes.
    bool nameWasLookedFor() const { return TypeInfo::perCellBit(inlineTypeFlags()); }
    void noteLookForName() { setPerCellBit(true); }
    bool hasOrdinaryDefineOwnProperty(VM&, PropertyName) const;
    static bool isNameThatBuiltinFunctionsRefuse(VM&, UniquedStringImpl*);
    bool refusesOwnProperty(VM&, PropertyName) const;
    bool isBuiltinFunction() const;
    void ensureClassObject(VM&);
    // Leaves the class object with `prototype` as its only property: what it owned before is the caller's to put back.
    void initializeClassObject(VM&, JSObject* parent, JSObject* prototype);
    void adoptBoundFunctionStructure(VM&);
    JSValue prototypeProperty(VM&);
    void setPrototypeProperty(VM&, JSObject* prototype, unsigned attributes);
    void setNameProperty(VM&, JSString*, unsigned attributes);

private:
    Closure(VM& vm, Structure* structure, NativeExecutable* executable, uint64_t entry)
        : Base(vm, executable, structure->realm(), structure)
        , m_entry(entry)
    {
    }

    void prepareForGenericAccess(VM&);
    bool ensurePrototypeProperty(VM&);

    uint64_t m_entry;
};
static_assert(sizeof(JSFunction) == offsetOfClosureEntry && Closure::offsetOfEntry() == offsetOfClosureEntry && sizeof(Closure) == offsetOfClosureCaptures);

ALWAYS_INLINE bool isCompiledClosure(const JSCell* cell) { return cell->type() == CompiledClosureType; }

template<typename Visitor>
ALWAYS_INLINE void Closure::visitFields(Closure* closure, Visitor& visitor)
{
    if (!closure->m_entry)
        return;
    for (uint64_t reference : closure->captures().first(closure->numberOfReferences()))
        visitor.appendUnbarriered(JSValue::decode(reference));
}

JSC_DECLARE_HOST_FUNCTION(callClassConstructor);
JSC_DECLARE_HOST_FUNCTION(callCompiledClosure);
JSC_DECLARE_HOST_FUNCTION(constructCompiledClosure);
JSC_DECLARE_HOST_FUNCTION(constructBoundClosure);

enum class LookupPosition : uint8_t { Receiver, PrototypeChain };

// For DefineMethod. Until its definition ends nothing else has seen the class object or the prototype object of a class, and those of a class that is evaluated once have Structures of their own.
bool tryDefineClassMemberWithoutTransition(JSGlobalObject*, Closure& classObject, const LayoutData& classLayout, JSObject* holder, PropertyName, JSObject* function, MethodKind);
bool mayHaveLazyProperty(VM&, JSCell*, UniquedStringImpl*, LookupPosition);
bool closureHandlesPrototypeProperty(JSCell*);
GetValueFunc closurePrototypePropertyGetter();

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
