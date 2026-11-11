/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionTables.h"
#include "JSCJSValue.h"
#include "RememberedSlot.h"
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/text/SymbolImpl.h>

namespace JSC { namespace AOT {

class Closure;
struct LayoutData;
struct State;

class ClosureTable {
    WTF_MAKE_NONCOPYABLE(ClosureTable);
public:
    explicit ClosureTable(State&);

    static Structure* realmClosureStructure(VM&, JSGlobalObject*, unsigned index);
    Structure* closureStructure(VM&, unsigned index);
    Structure* closureStructureFor(VM& vm, FunctionID function) { return closureStructure(vm, m_tables.kindOfFunction(function.index()).closureStructureOfKind()); }
    Structure* slimClosureStructure(FunctionKindData kind) { return m_slimClosureStructures[kind.slimClosureKind()]; }
    Structure* slimClosureStructure(VM&, FunctionID function) { return slimClosureStructure(m_tables.kindOfFunction(function.index())); }
    Structure* reifiedSlimClosureStructure(VM&, FunctionID);
    JSCell* uncreatedPrototypeProperty() const { return m_uncreatedPrototypeProperty; }
    Structure* asyncBoundFunctionStructure() const { return m_asyncBoundFunctionStructure; }
    void setAsyncBoundFunctionStructure(Structure*);
    void didGivePrototypePropertyTo(VM&, JSObject* closure);

    static constexpr unsigned regionShift = 8;

    JSCell* closure(VM&, FunctionID);
    JSCell* closureIfCreated(FunctionID) const;
    bool hasCellFor(FunctionID) const;
    bool hasSingletonClosure(FunctionID) const;
    void registerClassObject(Closure*);
    void registerFirstClassObject(Closure*, FunctionID constructor, const LayoutData& classLayout);
    void registerClosureOfMember(FunctionID, JSCell*);
    void forgetClosures(uint32_t firstCell, uint32_t numberOfCells);
    JSObject* createClosure(VM&, FunctionID, std::span<const uint64_t> captures, unsigned numberOfReferences);
    JSObject* createClosureOver(VM&, FunctionID, JSValue captured);
    JSObject* createClosureForOneCall(VM&, FunctionID);
    JSObject* calleeOfModuleBodies(VM&, FunctionID body);
    template<typename Visitor> void visitRoots(Visitor&);

private:
    template<typename Appender> void visitRegion(Appender&, size_t begin, size_t end);
    void createStructures(VM&);
    void didCreateClosureFor(FunctionID, JSCell*);

    State& m_state;
    const ProgramTables& m_tables;
    RememberedSlotSet m_writtenRegions;
    Structure* m_closureStructures[numberOfClosureStructures] { };
    Structure* m_slimClosureStructures[numberOfSlimClosureKinds] { };
    Structure* m_reifiedSlimClosureStructures[2][2] { };
    Structure* m_asyncBoundFunctionStructure { nullptr };
    RefPtr<PrivateSymbolImpl> m_functionIndexPrivateName;
    JSCell* m_uncreatedPrototypeProperty { nullptr };
    JSObject* m_calleeOfModuleBodies { nullptr };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
