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
struct State;

class ClosureTable {
    WTF_MAKE_NONCOPYABLE(ClosureTable);
public:
    explicit ClosureTable(State&);

    static Structure* realmClosureStructure(VM&, JSGlobalObject*, unsigned index);
    Structure* closureStructure(VM&, unsigned index);
    Structure* closureStructureFor(VM& vm, FunctionID function) { return closureStructure(vm, functionKindData(function).closureStructure); }
    Structure* slimClosureStructure(VM&, FunctionID function) { return m_slimClosureStructures[functionKindData(function).slimClosureKind()]; }
    Structure* reifiedSlimClosureStructure(VM&, FunctionID);
    JSCell* uncreatedPrototypeProperty() const { return m_uncreatedPrototypeProperty; }
    JSCell* untakenNameProperty() const { return m_untakenNameProperty; }
    Structure* boundClosureStructure(VM&, FunctionID);
    Structure* asyncBoundFunctionStructure() const { return m_asyncBoundFunctionStructure; }
    void setAsyncBoundFunctionStructure(Structure*);
    // The first lookup of `prototype`, the first construction and the guards of a constructor function each give the closure another Structure. None of them changes what any other name finds.
    void didGivePrototypePropertyTo(VM&, JSObject* closure);

    JSCell* closure(VM&, FunctionID);
    JSCell* closureIfCreated(FunctionID) const;
    bool hasCellFor(FunctionID) const;
    // True for a class or constructor function that is evaluated once and captures nothing: closure() creates its class object for whoever asks first.
    bool hasSingletonClosure(FunctionID) const;
    void registerClassObject(Closure*);
    void registerClosureOfMember(FunctionID, JSCell*);
    void forgetClosure(FunctionID);
    JSObject* createClosure(VM&, FunctionID, std::span<const uint64_t> captures, unsigned numberOfReferences);
    JSObject* createClosureOver(VM&, FunctionID, JSValue captured);
    // For a function that captures nothing and that no value of the program names, as the body of a module: the table does not keep the closure.
    JSObject* createClosureForOneCall(VM&, FunctionID);
    // What the frame that enters the body of an ES module has as its callee. Such a body never looks at it, so the bodies of the instance share the closure of the first one.
    JSObject* calleeOfModuleBodies(VM&, FunctionID body);
    template<typename Visitor> void visitRoots(Visitor&);

private:
    template<typename Appender> void visitRegion(Appender&, size_t begin, size_t end);
    void createStructures(VM&);
    void didCreateClosureFor(FunctionID, JSCell*);

    static constexpr unsigned regionShift = 8;

    State& m_state;
    const ProgramTables& m_tables;
    RememberedSlotSet m_writtenRegions;
    Structure* m_closureStructures[numberOfClosureStructures] { };
    Structure* m_slimClosureStructures[numberOfSlimClosureKinds] { };
    Structure* m_reifiedSlimClosureStructures[2][3][2] { };
    Structure* m_boundClosureStructures[2] { };
    Structure* m_asyncBoundFunctionStructure { nullptr };
    RefPtr<PrivateSymbolImpl> m_functionIndexPrivateName;
    JSCell* m_uncreatedPrototypeProperty { nullptr };
    JSCell* m_untakenNameProperty { nullptr };
    JSObject* m_calleeOfModuleBodies { nullptr };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
