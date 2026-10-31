/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTable.h"
#include "AOTExpected.h"
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

    // In front of `types`, which refers to it.
    BuiltinTableOfBuild m_tableOfBuild;

public:
    explicit Program(const BuiltinTable&);
    ~Program();

    static std::unique_ptr<Program> read(std::span<const uint8_t> bytes, BuildContext&);

    IRForm form() const { return m_form; }
    void setForm(IRForm form) { m_form = form; }
    bool hasGuardedReads() const { return m_hasGuardedReads; }
    void setHasGuardedReads() { m_hasGuardedReads = true; }
    // The executable may hold a function that allocates an error and throws it in place of this built-in's native code.
    bool mayBeTrimmed(BuiltinID builtin) const { return builtin && m_builtinsThatMayBeTrimmed.get(builtin.index()); }
    void setBuiltinsThatMayBeTrimmed(BitVector&& builtins) { m_builtinsThatMayBeTrimmed = WTF::move(builtins); }
    bool isBeingReplaced(FunctionID id) const { return m_functionsBeingReplaced && m_functionsBeingReplaced->get(id.index()); }
    const BuiltinTable& builtins() const { return m_builtins; }
    // Once, when `replaceableBuiltins` and `openedNames` are whole and nothing has asked the table about a lock.
    void unlockReplaceableBuiltins();
    std::span<const BuiltinID> unlockedBuiltins() const LIFETIME_BOUND { return m_tableOfBuild.unlockedMembers(); }
    bool isUnlocked(BuiltinID member) const { return std::ranges::binary_search(unlockedBuiltins(), member); }
    // A test run adds a property of this name to the object, which takes no other new name.
    bool isOpened(BuiltinID canonicalHolder, StringID name) const { return std::ranges::contains(openedNames, OpenedName { canonicalHolder, name }); }
    bool isOpenedSomewhere(StringID name) const { return m_namesOpenedSomewhere.get(name.index()); }
    bool testsReplaceBuiltins() const { return m_testsReplaceBuiltins; }
    // A row for a pristine operand leaves out the members that the general row of the function looks up on it: Array.prototype.toString joins a pristine array by itself.
    // Where one of those names is unlocked only the general row says what the function does.
    bool looksUpUnlockedName(BuiltinID function) const { return m_functionsLookingUpUnlockedNames.get(m_builtins.canonicalID(function).index()); }
    // No row of the function says that it calls the member: `new Map(pairs)` and Map.prototype.set. So no row says what the function does.
    bool callsUnlockedMemberThatNoRowNames(BuiltinID function) const { return m_functionsCallingUnlockedMembersThatNoRowNames.get(m_builtins.canonicalID(function).index()); }
    // A lookup on a pristine instance goes along the chain of its class: on an array `toString` finds Array.prototype.toString, whatever a test has put under Object.prototype.toString.
    bool passesByRowFor(BuiltinID function, BuiltinTypeID tested) const;
    std::span<const uint8_t> buffer() const { return m_bytes.borrowed; }
    void borrowBuffer(std::span<const uint8_t> bytes) { m_bytes.borrowed = bytes; }
    bool ownsBuffer() const { return !m_bytes.owned.isEmpty(); }
    std::span<const uint8_t> copyBorrowedBytes(std::span<const uint8_t>, const ParallelLoops&);
    void copyBorrowedWordsOfPositions(const ParallelLoops&);
    void collectConstructionStarts();
    void adoptBuffer(Vector<uint8_t>&&);

    StringTable strings;
    TypeTable types;
    Vector<TypeSite> typeSites;
    Vector<ExpectedResult> expectedResults;
    Vector<Layout> layouts;
    Vector<TaggedFamily> taggedFamilies;
    // A parameter whose declared type is a function type with known parameter types. Sorted.
    struct DeclaredCallback {
        FunctionID function;
        uint32_t parameter;
        FunctionID functionType;

        friend auto operator<=>(const DeclaredCallback&, const DeclaredCallback&) = default;
    };
    Vector<DeclaredCallback> declaredCallbacks;
    Vector<Variable> variables;
    Vector<Module> modules;
    Vector<ModuleFailure> moduleFailures;
    Vector<AmbiguousExport> ambiguousExports;
    Vector<SourceStamp> sourceStamps;
    Vector<EvalScope> evalScopes;
    Vector<Function> functions;
    Vector<Data> data;
    Vector<Entry> entries;
    Vector<RuntimeFunctionEntry> runtimeFunctions;
    Vector<WebAssemblyModule> webAssemblyModules;
    Vector<ReplaceableBuiltin> replaceableBuiltins;
    Vector<OpenedName> openedNames;
    // CompileOptions::holdsLibraryAlone(): a run that nobody knows yet adds whatever names a test run can add.
    bool takesAnyNameFromTests { false };

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
    ProvenInEveryVersion provenTypeOf(const DeclaredType& declared) const { return types.provenTypeOf(declared); }
    const LayoutSet& at(LayoutSetID id) const { return types.at(id); }
    const CalleeSet& at(CalleeSetID id) const { return types.at(id); }
    const PropertyKey& at(PropertyKeyID id) const { return types.at(id); }

    const Type& typeOf(const Function& function, ValueID value) const { return types.at(function.at(value).type); }
    Expected<const Type&> expectedOrTypeOf(const Function& function, ValueID value) const { return types.at(function.at(value).expectedOrType().predicted()); }
    const Function& sourceFunctionOf(const Function& function, Origin origin) const { return origin.inlineFrame ? at(function.inlineFrames[origin.inlineFrame].callee) : function; }
    const Function& sourceFunctionOf(const Function& function, const Instruction& instruction) const { return sourceFunctionOf(function, instruction.origin()); }

    void setProvisionalFunctions(const ProvisionalFunctions* functions) { m_provisionalFunctions = functions; }

    const FamilyTables& familyTables() const { return *m_familyTables; }

    SharedFieldTypes* sharedFieldTypes() const { return m_caches.fieldTypes; }
    PropertyAccessCache* propertyAccessCache() const { return m_caches.propertyAccesses; }
    void setCaches(SharedFieldTypes* fieldTypes, PropertyAccessCache* propertyAccesses) const
    {
        m_caches.fieldTypes = fieldTypes;
        m_caches.propertyAccesses = propertyAccesses;
    }
    BodyIndex* bodyIndex() const { return m_caches.bodyIndex; }
    void setBodyIndex(BodyIndex* index) const { m_caches.bodyIndex = index; }

    unsigned numberOfFunctionsAsRead() const { return m_countsAsRead.functions; }
    unsigned numberOfLayoutsAsRead() const { return m_countsAsRead.layouts; }
    void recordCountsAsRead();
    bool readerPreparedFunctionsForTyping() const { return m_readerPreparedFunctionsForTyping; }
    void setReaderPreparedFunctionsForTyping() { m_readerPreparedFunctionsForTyping = true; }
    struct BodiesAsRead {
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
    bool hasDeclaredMembers(LayoutID id) const { return id.index() < m_derivedTables.declaredMembers.size() && m_derivedTables.declaredMembers[id.index()]; }
    void setDeclaredMembers(LayoutID, LayoutSetID);
    CalleeSetID familyOf(FunctionID asRead) const { return m_derivedTables.functionFamilies[asRead.index()]; }
    void setFunctionFamilies(Vector<CalleeSetID>&& families) { m_derivedTables.functionFamilies = WTF::move(families); }
    const Vector<PropertyKeyID>* keysStoredThroughUnknownReceivers() const { return m_keysStoredThroughUnknownReceivers ? &*m_keysStoredThroughUnknownReceivers : nullptr; }
    void setKeysStoredThroughUnknownReceivers(Vector<PropertyKeyID>&& ascendingKeys) { m_keysStoredThroughUnknownReceivers = WTF::move(ascendingKeys); }
    bool mayBeStoredThroughUnknownReceiver(PropertyKeyID) const;
    void setKeysDefinedOnObjectsWithoutKnownLayout(Vector<PropertyKeyID>&& ascendingKeys) { m_keysDefinedOnObjectsWithoutKnownLayout = WTF::move(ascendingKeys); }
    bool isDefinedOnObjectsWithoutKnownLayout(PropertyKeyID key) const { return std::ranges::binary_search(m_keysDefinedOnObjectsWithoutKnownLayout, key); }
    std::span<const LayoutID> layoutsBelow(LayoutID) const LIFETIME_BOUND;
    std::span<const LayoutID> layoutsExpectedBelow(LayoutID) const LIFETIME_BOUND;
    std::span<const LayoutID> layoutsWithName(PropertyKeyID) const LIFETIME_BOUND;
    std::span<const LayoutID> declarersOfName(PropertyKeyID) const LIFETIME_BOUND;
    bool prototypeMaySelectOtherClass(LayoutID) const;
    void indexClasses();
    void indexNames();
    void indexNames(const ParallelLoops&);

    void appendBuiltinLayouts();
    void appendNamespaceLayouts();
    LayoutID layoutOfWholeNamespace(ModuleID) const;
    void internBuiltinNameKeys();

    bool isEntry(FunctionID) const;
    LineColumn lineColumnFor(ModuleID, uint32_t position) const;
    // A stack trace reports a construction at its callee. Call sites and the report of an uncaught error show where the expression starts.
    const ConstructionStart* findConstructionReportedAt(ModuleID, uint32_t position) const;
    StringID pathForLineColumns(ModuleID) const;

private:
    friend class ProgramEditor;

    struct Bytes {
        std::span<const uint8_t> borrowed;
        Vector<uint8_t> owned;
        Vector<uint32_t> wordsOfPositions;
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
    BitVector m_namesOpenedSomewhere;
    bool m_testsReplaceBuiltins { false };
    BitVector m_functionsLookingUpUnlockedNames;
    BitVector m_functionsCallingUnlockedMembersThatNoRowNames;
    // By the class of a pristine part: the functions that look up a name which, on an instance of the class or of one below it, finds what a test may have put there.
    // A class that is not among the first goes by looksUpUnlockedName() alone.
    BitVector m_classesWithKnownChains;
    Vector<BitVector> m_functionsLookingUpUnlockedNamesOnInstances;
    IRForm m_form { IRForm::AsRead };
    bool m_hasGuardedReads { false };
    BitVector m_builtinsThatMayBeTrimmed;
    const BitVector* m_functionsBeingReplaced { nullptr };
    const ProvisionalFunctions* m_provisionalFunctions { nullptr };
    Bytes m_bytes;
    CountsAsRead m_countsAsRead;
    Vector<Vector<ConstructionStart>> m_constructionStartsOfModules;
    bool m_readerPreparedFunctionsForTyping { false };
    std::optional<BodiesAsRead> m_bodiesAsRead;
    DerivedTables m_derivedTables;
    std::optional<Vector<PropertyKeyID>> m_keysStoredThroughUnknownReceivers;
    Vector<PropertyKeyID> m_keysDefinedOnObjectsWithoutKnownLayout;
    mutable Caches m_caches;
    std::unique_ptr<FamilyTables> m_familyTables;
};

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
bool isBundlerHelperModule(const Program&, const Module&);
bool isBundlerHelperNamedByProgram(const Program&, ModuleID);
// The host asks the loader for an entry point. Where the program loads files under names that the build does not follow, it also asks for the files marked IsFoundByName, and for those that a copy from disk may import: MayBeEvicted.
BitVector modulesThatHostAsksFor(const Program&, bool findsFilesByName, bool beginsModulesOfData = false);
// A name made at run time finds the module in the image. The image holds no code of a module of the host that has a source and this kind, and what a program writes of a module with an open namespace is the object: the loader answers for both.
bool answersToPath(const Program&, const Module&);
ModuleID compiledModuleWithObject(const Program&, BuiltinID);
ModuleID commonJsModuleCapturedBy(const Program&, const Module& face);
// The attributes of an import() say as what a file is loaded, and its text need not say them. These are the modules that hold the file of `named` as something that only an attribute asks for.
Vector<ModuleID, 2> formsInImage(const Program&, ModuleID named);
bool namesWaysToModuleObjects(const Program&);
// `require` hands the module to this function of the host, so a stack trace lists it between what the module runs and the code that called `require`.
BuiltinID functionOfHostThatRequireCallsFor(const Module&);
// Modules::hostAnswersInPlaceOf(): the loader of the host answers with what the factory of a mock made, and not with an object of the image.
bool importMayAnswerWithMock(const Module&);
// `require` takes a name that starts with `node:` to the registry, where no mock stands, and `require("bun")` is the global `Bun`.
bool requireMayAnswerWithMock(const Program&, const Module&);
// From SplitCoroutines on.
bool isBodyOfModuleEvaluatedByRequire(const Program&, const Function&);

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
    // The tables by class and by name, after classes were added or have another base.
    void indexClassesAndNames(const ParallelLoops&);
    void setDeclaredMembers(LayoutID, LayoutSetID);
    Field& at(FieldReference);
    Layout& atForHiddenFields(LayoutID);

    Variable& at(VariableID);
    VariableID addVariable(Variable&&);
    Module& at(ModuleID);
    DataID addData(Data&&);

    Function& atForPlacedWords(FunctionID);

    Function& atForLowering(FunctionID);

    Function& atForStorage(FunctionID);

    Vector<FunctionID> changedFunctions() const;
    Vector<FunctionID> functionsWithChangedOperations() const;
    Vector<FunctionID> functionsThatWereOnlyLowered() const;
    bool wasGivenOutOnlyForStorage(FunctionID) const;
    Vector<LayoutID> changedLayouts() const;
    Vector<VariableID> changedVariables() const;
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
