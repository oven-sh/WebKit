/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTIRForm.h"
#include "AOTIRFunction.h"
#include "AOTLayout.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSchemaRecords.h"
#include "AOTStringTable.h"
#include "AOTTypeTable.h"
#include "LineColumn.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
class BuiltinTable;

namespace Typed {

class Program final {
    WTF_MAKE_TZONE_ALLOCATED(Program);
    WTF_MAKE_NONCOPYABLE(Program);
public:
    explicit Program(const BuiltinTable&);

    static std::unique_ptr<Program> read(std::span<const uint8_t> bytes, BuildContext&);

    IRForm form() const { return m_form; }
    void setForm(IRForm form) { m_form = form; }
    const BuiltinTable& builtins() const { return m_builtins; }
    std::span<const uint8_t> buffer() const { return m_bytes.borrowed; }
    void borrowBuffer(std::span<const uint8_t> bytes) { m_bytes.borrowed = bytes; }
    void adoptBuffer(Vector<uint8_t>&&);

    StringTable strings;
    TypeTable types;
    Vector<TypeSite> typeSites;
    Vector<ExpectedResult> expectedResults;
    Vector<Layout> layouts;
    Vector<Variable> variables;
    Vector<Module> modules;
    Vector<ModuleFailure> moduleFailures;
    Vector<EvalScope> evalScopes;
    Vector<Function> functions;
    Vector<Data> data;
    Vector<Entry> entries;
    Vector<RuntimeFunctionEntry> runtimeFunctions;
    Vector<WebAssemblyModule> webAssemblyModules;

    Function& at(FunctionID id) { return functions[id.index()]; }
    const Function& at(FunctionID id) const { return ProvisionalFunctions::contains(id) ? m_provisionalFunctions->at(id) : functions[id.index()]; }
    Layout& at(LayoutID id) { return layouts[id.index()]; }
    const Layout& at(LayoutID id) const { return layouts[id.index()]; }
    Variable& at(VariableID id) { return variables[id.index()]; }
    const Variable& at(VariableID id) const { return variables[id.index()]; }
    Module& at(ModuleID id) { return modules[id.index()]; }
    const Module& at(ModuleID id) const { return modules[id.index()]; }
    const ModuleFailure& at(ModuleFailureID id) const { return moduleFailures[id.index()]; }
    const EvalScope& at(EvalScopeID id) const { return evalScopes[id.index()]; }
    const TypeSite& at(TypeSiteID id) const { return typeSites[id.index()]; }
    const ExpectedResult& at(ExpectedResultID id) const { return expectedResults[id.index()]; }
    const Data& at(DataID id) const { return data[id.index()]; }
    const Type& at(TypeID id) const { return types.at(id); }
    const LayoutSet& at(LayoutSetID id) const { return types.at(id); }
    const CalleeSet& at(CalleeSetID id) const { return types.at(id); }
    const PropertyKey& at(PropertyKeyID id) const { return types.at(id); }

    const Type& typeOf(const Function& function, ValueID value) const { return types.at(function.at(value).type); }
    const Type& predictionOrTypeOf(const Function& function, ValueID value) const { return types.at(function.at(value).predictionOrType()); }
    const Function& sourceFunctionOf(const Function& function, Origin origin) const { return origin.inlineFrame ? at(function.inlineFrames[origin.inlineFrame].callee) : function; }
    const Function& sourceFunctionOf(const Function& function, const Instruction& instruction) const { return sourceFunctionOf(function, instruction.origin()); }

    void setProvisionalFunctions(const ProvisionalFunctions* functions) { m_provisionalFunctions = functions; }

    unsigned numberOfFunctionsAsRead() const { return m_countsAsRead.functions; }
    unsigned numberOfLayoutsAsRead() const { return m_countsAsRead.layouts; }
    void recordCountsAsRead();
    void setNumberOfFunctionsAsRead(unsigned);

    std::span<const uint8_t> string(StringID id) const { return strings[id]; }
    NameClass nameClass(StringID id) const { return strings.nameClass(id); }
    BuiltinName builtinName(StringID id) const { return strings.builtinName(id); }
    StringID stringOf(BuiltinName textName) const { return strings.stringOf(textName); }

    PropertyKeyID keyOf(BuiltinName name) const { return m_derivedTables.builtinNameKeys[name.index()]; }
    BuiltinName builtinNameOf(PropertyKeyID) const;
    NameClass nameClassOf(PropertyKeyID) const;
    bool isIndexLike(PropertyKeyID key) const { return mayBeElementKey(nameClassOf(key)); }

    LayoutID layoutOf(BuiltinLayoutID id) const { return m_derivedTables.builtinLayouts[id.index()]; }
    LayoutSetID declaredMembersOf(LayoutID id) const { return m_derivedTables.declaredMembers[id.index()]; }
    void setDeclaredMembers(LayoutID, LayoutSetID);
    CalleeSetID familyOf(FunctionID asRead) const { return m_derivedTables.functionFamilies[asRead.index()]; }
    void setFunctionFamilies(Vector<CalleeSetID>&& families) { m_derivedTables.functionFamilies = WTF::move(families); }
    // The layouts that have this one on their chain of proven bases, ascending, without those that are never allocated.
    std::span<const LayoutID> layoutsBelow(LayoutID) const LIFETIME_BOUND;
    // The layouts whose objects have the name, as a field or as a member of a class above them, ascending, without those that are never allocated.
    std::span<const LayoutID> layoutsWithName(PropertyKeyID) const LIFETIME_BOUND;
    // For each of them, in the same order, the layout that declares the field or the class that defines the member.
    std::span<const LayoutID> declarersOfName(PropertyKeyID) const LIFETIME_BOUND;
    // Whether an allocation of the layout under a prototype may answer with another object. The definition of a class ties its prototype object to its layout,
    // so the prototype object of any other class selects something else: an instance of that class, or an ordinary object where the class has no shape.
    // The layout of a constructor function and of a literal stays under any prototype.
    bool prototypeMaySelectOtherClass(LayoutID) const;
    void indexClasses();
    void indexNames();

    void appendBuiltinLayouts();
    void appendNamespaceLayouts();
    void internBuiltinNameKeys();

    bool isEntry(FunctionID) const;
    LineColumn lineColumnFor(ModuleID, uint32_t position) const;

private:
    struct Bytes {
        std::span<const uint8_t> borrowed;
        Vector<uint8_t> owned;
    };

    struct CountsAsRead {
        unsigned functions { 0 };
        unsigned layouts { 0 };
    };

    struct DerivedTables {
        Vector<LayoutID> builtinLayouts;
        Vector<LayoutSetID> declaredMembers;
        Vector<CalleeSetID> functionFamilies;
        Vector<Vector<LayoutID>> layoutsBelow;
        Vector<uint32_t> firstLayoutWithName;
        Vector<LayoutID> layoutsWithName;
        Vector<LayoutID> declarersOfName;
        unsigned numberOfClasses { 0 };
        Vector<PropertyKeyID> builtinNameKeys;
        Vector<BuiltinName> registeredSymbolNames;
    };

    const BuiltinTable& m_builtins;
    IRForm m_form { IRForm::AsRead };
    const ProvisionalFunctions* m_provisionalFunctions { nullptr };
    Bytes m_bytes;
    CountsAsRead m_countsAsRead;
    DerivedTables m_derivedTables;
};

// The functions that take long wherever each function is looked at, the largest first.
Vector<uint32_t> largeFunctions(const Program&, const ParallelLoops&);

BitVector functionsCalledByHost(const Program&);
BitVector functionsCalledByHost(const Program&, const ParallelLoops&);

void forEachFunctionCalledByHost(const Program& program, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    for (size_t index : functionsCalledByHost(program))
        functor(FunctionID(index));
}

const RuntimeFunctionEntry* findRuntimeFunction(const Program&, RuntimeFunctionID);
bool isLibraryCode(const Program&, const Function&);

class ProgramEditor {
    WTF_MAKE_NONCOPYABLE(ProgramEditor);
public:
    explicit ProgramEditor(Program&);

    const Program& program() const { return m_program; }
    TypeTable& types() { return m_program.types; }
    StringTable& strings() { return m_program.strings; }

    Function& at(FunctionID);
    FunctionID addFunction(Function&&);
    FunctionID addFunctionLike(FunctionID original, Function&&);
    FunctionID reserveFunctions(unsigned count);
    void replaceFunction(FunctionID, Function&&);
    void replaceFunctionLike(FunctionID, FunctionID original, Function&&);
    void removeBody(FunctionID);

    LayoutID addLayout(Layout&&);
    LayoutID reserveLayouts(unsigned count);
    Layout& at(LayoutID);

    Variable& at(VariableID);
    VariableID addVariable(Variable&&);
    Module& at(ModuleID);
    DataID addData(Data&&);

    Vector<FunctionID> changedFunctions() const;
    // A layout, a variable or a module that was there before may have changed, or the layouts below a layout have.
    bool mayHaveChangedSharedRecords() const { return m_mayHaveChangedSharedRecords; }

private:
    void didChange(FunctionID);
    void didGiveOutSharedRecord();
    void inheritFrom(FunctionID original, Function&) const;

    Program& m_program;
    Vector<uint8_t> m_isChanged;
    uint8_t m_mayHaveChangedSharedRecords { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
