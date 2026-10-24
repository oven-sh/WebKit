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
#include "AOTTaggedLayouts.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include "LineColumn.h"
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

class BuildContext;
class BuiltinTable;

namespace Typed {

class BodyIndex;
class FamilyTables;
class PropertyAccessCache;
class SharedFieldTypes;

class Program final {
    WTF_MAKE_TZONE_ALLOCATED(Program);
    WTF_MAKE_NONCOPYABLE(Program);
public:
    explicit Program(const BuiltinTable&);
    ~Program();

    static std::unique_ptr<Program> read(std::span<const uint8_t> bytes, BuildContext&);

    IRForm form() const { return m_form; }
    void setForm(IRForm form) { m_form = form; }
    // Whether GuardTaintedPositions has put a Check behind every read that may find what a Check elsewhere has turned away.
    bool hasGuardedReads() const { return m_hasGuardedReads; }
    void setHasGuardedReads() { m_hasGuardedReads = true; }
    // While several threads replace functions at once, the record of one of them is in no state to be read: what a call of it returns, what it captures.
    bool isBeingReplaced(FunctionID id) const { return m_functionsBeingReplaced && m_functionsBeingReplaced->get(id.index()); }
    const BuiltinTable& builtins() const { return m_builtins; }
    std::span<const uint8_t> buffer() const { return m_bytes.borrowed; }
    void borrowBuffer(std::span<const uint8_t> bytes) { m_bytes.borrowed = bytes; }
    bool ownsBuffer() const { return !m_bytes.owned.isEmpty(); }
    std::span<const uint8_t> copyBorrowedBytes(std::span<const uint8_t>, const ParallelLoops&);
    void copyBorrowedLineStarts(const ParallelLoops&);
    void adoptBuffer(Vector<uint8_t>&&);

    StringTable strings;
    TypeTable types;
    Vector<TypeSite> typeSites;
    Vector<ExpectedResult> expectedResults;
    Vector<Layout> layouts;
    Vector<TaggedFamily> taggedFamilies;
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
    TypeID provenTypeOf(const DeclaredType& declared) const { return types.provenTypeOf(declared); }
    const LayoutSet& at(LayoutSetID id) const { return types.at(id); }
    const CalleeSet& at(CalleeSetID id) const { return types.at(id); }
    const PropertyKey& at(PropertyKeyID id) const { return types.at(id); }

    const Type& typeOf(const Function& function, ValueID value) const { return types.at(function.at(value).type); }
    const Type& expectedOrTypeOf(const Function& function, ValueID value) const { return types.at(function.at(value).expectedOrType()); }
    const Function& sourceFunctionOf(const Function& function, Origin origin) const { return origin.inlineFrame ? at(function.inlineFrames[origin.inlineFrame].callee) : function; }
    const Function& sourceFunctionOf(const Function& function, const Instruction& instruction) const { return sourceFunctionOf(function, instruction.origin()); }

    void setProvisionalFunctions(const ProvisionalFunctions* functions) { m_provisionalFunctions = functions; }

    // What takes a loop over the members of a family, once per family. indexClasses(), indexNames() and an editor that hands out a layout drop the answers.
    const FamilyTables& familyTables() const { return *m_familyTables; }

    // Null outside a loop that types functions while the layouts stay as they are.
    SharedFieldTypes* sharedFieldTypes() const { return m_caches.fieldTypes; }
    PropertyAccessCache* propertyAccessCache() const { return m_caches.propertyAccesses; }
    void setCaches(SharedFieldTypes* fieldTypes, PropertyAccessCache* propertyAccesses) const
    {
        m_caches.fieldTypes = fieldTypes;
        m_caches.propertyAccesses = propertyAccesses;
    }
    // Null while nobody keeps one.
    BodyIndex* bodyIndex() const { return m_caches.bodyIndex; }
    void setBodyIndex(BodyIndex* index) const { m_caches.bodyIndex = index; }

    unsigned numberOfFunctionsAsRead() const { return m_countsAsRead.functions; }
    unsigned numberOfLayoutsAsRead() const { return m_countsAsRead.layouts; }
    void recordCountsAsRead();
    // The reader has forgotten the long expectations of the functions and has added the checks at their entries, where it had each body at hand.
    bool readerPreparedFunctionsForTyping() const { return m_readerPreparedFunctionsForTyping; }
    void setReaderPreparedFunctionsForTyping() { m_readerPreparedFunctionsForTyping = true; }
    // What the reader saw in the bodies while it decoded them, for the steps in front of the typing that would walk over all bodies to find it. It holds for the functions as read.
    struct BodiesAsRead {
        // Ascending.
        Vector<FunctionID> functionsThatNameObjectCreate;
        BitVector functionsCalledDirectly;
    };
    const BodiesAsRead* bodiesAsRead() const { return m_bodiesAsRead ? &*m_bodiesAsRead : nullptr; }
    void setBodiesAsRead(BodiesAsRead&& bodies) { m_bodiesAsRead = WTF::move(bodies); }
    void forgetBodiesAsRead() { m_bodiesAsRead = std::nullopt; }
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
    const Vector<PropertyKeyID>* keysStoredThroughUnknownReceivers() const { return m_keysStoredThroughUnknownReceivers ? &*m_keysStoredThroughUnknownReceivers : nullptr; }
    void setKeysStoredThroughUnknownReceivers(Vector<PropertyKeyID>&& ascendingKeys) { m_keysStoredThroughUnknownReceivers = WTF::move(ascendingKeys); }
    bool mayBeStoredThroughUnknownReceiver(PropertyKeyID) const;
    // The layouts that have this one on their chain of proven bases, ascending, without those that are never allocated.
    std::span<const LayoutID> layoutsBelow(LayoutID) const LIFETIME_BOUND;
    // By the parents that are proven and, where none is, by those that the program text names.
    std::span<const LayoutID> layoutsExpectedBelow(LayoutID) const LIFETIME_BOUND;
    // The layouts whose objects have the name, as a field or as a member of a class above them, ascending, without those that are never allocated.
    std::span<const LayoutID> layoutsWithName(PropertyKeyID) const LIFETIME_BOUND;
    // For each of them, in the same order, the layout that declares the field or the class that defines the member.
    std::span<const LayoutID> declarersOfName(PropertyKeyID) const LIFETIME_BOUND;
    // Whether an allocation of the layout under a prototype may answer with another object.
    bool prototypeMaySelectOtherClass(LayoutID) const;
    void indexClasses();
    void indexNames();
    void indexNames(const ParallelLoops&);

    void appendBuiltinLayouts();
    void appendNamespaceLayouts();
    // None for a module whose names the loader finds at run time.
    LayoutID layoutOfWholeNamespace(ModuleID) const;
    void internBuiltinNameKeys();

    bool isEntry(FunctionID) const;
    LineColumn lineColumnFor(ModuleID, uint32_t position) const;
    StringID pathForLineColumns(ModuleID) const;

private:
    friend class ProgramEditor;

    struct Bytes {
        std::span<const uint8_t> borrowed;
        Vector<uint8_t> owned;
        Vector<uint32_t> lineStarts;
    };

    struct CountsAsRead {
        unsigned functions { 0 };
        unsigned layouts { 0 };
    };

    struct Caches {
        SharedFieldTypes* fieldTypes { nullptr };
        PropertyAccessCache* propertyAccesses { nullptr };
        BodyIndex* bodyIndex { nullptr };
    };

    struct DerivedTables {
        Vector<LayoutID> builtinLayouts;
        Vector<LayoutSetID> declaredMembers;
        Vector<CalleeSetID> functionFamilies;
        Vector<Vector<LayoutID>> layoutsBelow;
        Vector<Vector<LayoutID>> layoutsExpectedBelow;
        Vector<uint32_t> firstLayoutWithName;
        Vector<LayoutID> layoutsWithName;
        Vector<LayoutID> declarersOfName;
        unsigned numberOfClasses { 0 };
        Vector<PropertyKeyID> builtinNameKeys;
        Vector<BuiltinName> registeredSymbolNames;
    };

    const BuiltinTable& m_builtins;
    IRForm m_form { IRForm::AsRead };
    bool m_hasGuardedReads { false };
    const BitVector* m_functionsBeingReplaced { nullptr };
    const ProvisionalFunctions* m_provisionalFunctions { nullptr };
    Bytes m_bytes;
    CountsAsRead m_countsAsRead;
    bool m_readerPreparedFunctionsForTyping { false };
    std::optional<BodiesAsRead> m_bodiesAsRead;
    DerivedTables m_derivedTables;
    std::optional<Vector<PropertyKeyID>> m_keysStoredThroughUnknownReceivers;
    mutable Caches m_caches;
    std::unique_ptr<FamilyTables> m_familyTables;
};

// The functions that take long wherever each function is looked at, the largest first.
Vector<uint32_t> largeFunctions(const Program&, const ParallelLoops&);
void sortLargestFirst(const Program&, const ParallelLoops&, Vector<FunctionID>&);

BitVector functionsCalledByHost(const Program&);
BitVector functionsCalledByHost(const Program&, const ParallelLoops&);

void forEachFunctionCalledByHost(const Program& program, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    for (size_t index : functionsCalledByHost(program))
        functor(FunctionID(index));
}

const RuntimeFunctionEntry* findRuntimeFunction(const Program&, RuntimeFunctionID);
bool isLibraryCode(const Program&, const Function&);
// Bun's own code that is loaded like a file of the program: what the bundler imports for the constructs it lowers. The program's source does not name it.
bool isBundlerHelperModule(const Program&, const Module&);
// A file of the program imports it under its own name, so it answers to that name at run time like every module of the library that is in the image. What it imports in turn has no name that a loader is asked for.
bool isBundlerHelperNamedByProgram(const Program&, ModuleID);
// The table lists the object that a module of the host answers as a built-in. Where the program holds that module as compiled code, the object is what its body makes. None for any other built-in.
ModuleID compiledModuleWithObject(const Program&, BuiltinID);
// ES modules import a CommonJS file through a module of its own, the face, whose body evaluates the file.
ModuleID commonJsModuleCapturedBy(const Program&, const Module& face);
// Code of some file names, by binding, a way from one module object to another: `children`, `parent`, `require.main`, process.mainModule.
bool namesWaysToModuleObjects(const Program&);

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
    // Stands around a loop in which several threads replace these functions, by replaceFunction() or by assigning to what at() gives.
    class ReplacedFunctions {
        WTF_MAKE_NONCOPYABLE(ReplacedFunctions);
    public:
        ReplacedFunctions(ProgramEditor&, std::initializer_list<std::span<const FunctionID>>);
        ~ReplacedFunctions();

    private:
        Program& m_program;
        BitVector m_functions;
    };
    void removeBody(FunctionID);

    LayoutID addLayout(Layout&&);
    Layout& at(LayoutID);
    // For changing what the record of a layout says of one field, but for the key of the field.
    Field& at(FieldReference);
    // For appending hidden fields, which only operations that are added with them read, and for saying in the flow of the layout where they are and in which order they are filled.
    Layout& atForHiddenFields(LayoutID);
    // Which objects lie in the cells of the objects of a layout. No analysis of a body or of a layout asks.
    void setInnerObjects(LayoutID, Vector<InnerObject>&&);

    Variable& at(VariableID);
    VariableID addVariable(Variable&&);
    Module& at(ModuleID);
    DataID addData(Data&&);

    // For writing the words of an operation that only placement fills in. What an analysis says of the body stays as it is.
    Function& atForPlacedWords(FunctionID);

    // For replacing operations by others that do no more, or moving them into a function that this one then calls. What a caller sees the function do stays as it is.
    Function& atForLowering(FunctionID);

    // For writing where the cells that the function allocates lie, and whether a closure of the frame is lent. Nothing that is kept of a body from one pass to the next reads that.
    Function& atForStorage(FunctionID);

    Vector<FunctionID> changedFunctions() const;
    // Without those that were only lowered.
    Vector<FunctionID> functionsWithChangedOperations() const;
    Vector<FunctionID> functionsThatWereOnlyLowered() const;
    // Of a function with changed operations: atForStorage() alone gave it out.
    bool wasGivenOutOnlyForStorage(FunctionID) const;
    // The layouts and variables that were there before and were given out.
    Vector<LayoutID> changedLayouts() const;
    Vector<VariableID> changedVariables() const;
    // The fields that were given out by themselves. Ascending, each once.
    Vector<FieldReference> changedFields() const;

private:
    static constexpr uint8_t hasChangedOperations = 1;
    static constexpr uint8_t hasChangedPlacedWords = 2;
    static constexpr uint8_t wasLowered = 4;
    static constexpr uint8_t hasChangedStorage = 8;

    static constexpr uint8_t isChangedLayout = 1;
    static constexpr uint8_t mayHaveNewHiddenFields = 2;
    static constexpr uint8_t keptAllFieldsInline = 4;

    void didChange(FunctionID, uint8_t change);
    void inheritFrom(FunctionID original, Function&) const;

    Program& m_program;
    Vector<uint8_t> m_changes;
    Vector<uint8_t> m_isChangedLayout;
    mutable Lock m_changedFieldsLock;
    Vector<FieldReference> m_changedFields WTF_GUARDED_BY_LOCK(m_changedFieldsLock);
    Vector<uint8_t> m_isChangedVariable;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
