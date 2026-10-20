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
    Structure* slimClosureStructure(VM&, FunctionID);
    Structure* reifiedSlimClosureStructure(VM&, FunctionID);
    // What the `prototype` property of a slim closure with properties holds until something reads it.
    JSCell* uncreatedPrototypeProperty() const { return m_uncreatedPrototypeProperty; }
    Structure* boundClosureStructure(VM&, FunctionID);
    Structure* asyncBoundFunctionStructure() const { return m_asyncBoundFunctionStructure; }
    void setAsyncBoundFunctionStructure(Structure*);

    JSCell* closure(VM&, FunctionID);
    JSCell* closureIfCreated(FunctionID) const;
    // True for a class or constructor function that is evaluated once and captures nothing: closure() creates its class object for whoever asks first.
    bool hasSingletonClosure(FunctionID) const;
    void registerClassObject(Closure*);
    void forgetClosure(FunctionID);
    JSObject* createClosure(VM&, FunctionID, std::span<const uint64_t> captures, unsigned numberOfReferences);
    JSObject* createClosureOver(VM&, FunctionID, JSValue captured);
    // For a function that captures nothing and that no value of the program names, as the body of a module: the table does not keep the closure.
    JSObject* createClosureForOneCall(VM&, FunctionID);
    template<typename Visitor> void visitRoots(Visitor&);

private:
    template<typename Appender> void visitRegion(Appender&, size_t begin, size_t end);
    void ensureStructures(VM&);
    void createSlimClosureStructures(VM&);
    void didCreateClosureFor(FunctionID, JSCell*);

    static constexpr unsigned regionShift = 12;

    State& m_state;
    const ProgramTables& m_tables;
    RememberedSlotSet m_writtenRegions;
    Structure* m_closureStructures[numberOfClosureStructures] { };
    Structure* m_slimClosureStructures[numberOfSlimClosureKinds] { };
    Structure* m_reifiedSlimClosureStructures[2][3] { };
    Structure* m_boundClosureStructures[2] { };
    Structure* m_asyncBoundFunctionStructure { nullptr };
    RefPtr<PrivateSymbolImpl> m_functionIndexPrivateName;
    JSCell* m_uncreatedPrototypeProperty { nullptr };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
