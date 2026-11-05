/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTable.h"
#include "AOTExpected.h"
#include "AOTIRFunction.h"
#include "AOTLayout.h"
#include "AOTSchemaEnums.h"
#include "AOTSchemaRecords.h"
#include "AOTStringTable.h"
#include "AOTTaggedLayouts.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include "LineColumn.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

class BuildContext;
class BuiltinTable;

namespace Typed {

// Skipped: for testaot alone, which then reads a program file that a binary built at the tag wrote with --aot-out. The wire's half of the schema is the tag's; the hash also covers the compile call, which is not.
enum class SchemaHashCheck : uint8_t { Required, Skipped };

class Program final {
    WTF_MAKE_TZONE_ALLOCATED(Program);
    WTF_MAKE_NONCOPYABLE(Program);

    // In front of `types`, which refers to it.
    BuiltinTableOfBuild m_tableOfBuild;

public:
    explicit Program(const BuiltinTable&);
    ~Program();

    // The bodies of the functions stay where they are: in `bytes`, or where the external bodies point. Both outlive the last use of a Body, which is the end of Compile.
    static std::unique_ptr<Program> read(std::span<const uint8_t> bytes, BuildContext&, SchemaHashCheck = SchemaHashCheck::Required);

    // The executable may hold a function that allocates an error and throws it in place of this built-in's native code.
    bool mayBeTrimmed(BuiltinID builtin) const { return builtin && m_builtinsThatMayBeTrimmed.get(builtin.index()); }
    const BuiltinTable& builtins() const { return m_builtins; }
    // Once, when `replaceableBuiltins` and `openedNames` are whole and nothing has asked the table about a lock.
    void unlockReplaceableBuiltins();
    std::span<const BuiltinID> unlockedBuiltins() const LIFETIME_BOUND { return m_tableOfBuild.unlockedMembers(); }
    bool isUnlocked(BuiltinID member) const { return std::ranges::binary_search(unlockedBuiltins(), member); }
    // The table locks the member, and a test run leaves the lock to the program: BuiltinTableOfBuild::unlockForTestRun(). Ascending. Empty outside a test run.
    std::span<const BuiltinID> builtinsWhoseLockIsLeftToProgram() const LIFETIME_BOUND { return m_tableOfBuild.membersWhoseLockIsLeftToProgram(); }
    bool isLockLeftToProgram(BuiltinID member) const { return std::ranges::binary_search(builtinsWhoseLockIsLeftToProgram(), member); }
    // AOTBuiltinTableOfTestRuns.cpp. Behind Link, on one thread: a name that the flow has brought to the object. The table of a test run has every such object locked by its names.
    void openNameForTestRun(BuiltinID canonicalHolder, StringID name);
    // A test run adds a property of this name to the object, which takes no other new name.
    bool isOpened(BuiltinID canonicalHolder, StringID name) const { return m_namesOpened.contains(static_cast<uint64_t>(canonicalHolder.index()) << 32 | name.index()); }
    bool isOpenedSomewhere(StringID name) const { return m_namesOpenedSomewhere.get(name.index()); }
    // The build of an executable has taken the text for absent on an object that is closed to other names: MembersUnderKey::isClosedKey(). The run time refuses such a name there, and takes every other. From any thread.
    void noteTakenForAbsent(PropertyKeyID) const;
    // Ascending.
    Vector<PropertyKeyID> keysTakenForAbsent() const;
    bool isSomeNameOpenedOn(BuiltinID canonicalHolder) const { return m_holdersOfOpenedNames.get(canonicalHolder.index()); }
    bool testsReplaceBuiltins() const { return m_testsReplaceBuiltins; }
    std::span<const uint8_t> buffer() const { return m_bytes.borrowed; }
    void borrowBuffer(std::span<const uint8_t> bytes) { m_bytes.borrowed = bytes; }
    bool ownsBuffer() const { return !m_bytes.owned.isEmpty(); }
    std::span<const uint8_t> copyBorrowedBytes(std::span<const uint8_t>, const ParallelLoops&);
    void copyBorrowedWordsOfPositions(const ParallelLoops&);
    // Once, behind the Facts scan: BodyIndexTable::takeConstructionStarts().
    void setConstructionStarts(Vector<ConstructionStartInModule>&&);
    void adoptBuffer(Vector<uint8_t>&&);

    StringTable strings;
    TypeTable types;
    Vector<TypeSite> typeSites;
    Vector<ExpectedResult> expectedResults;
    Vector<Layout> layouts;
    // Ascending by maker.
    Vector<TaggedFamily> taggedFamilies;
    Vector<Variable> variables;
    Vector<Module> modules;
    Vector<ModuleFailure> moduleFailures;
    Vector<AmbiguousExport> ambiguousExports;
    Vector<SourceStamp> sourceStamps;
    Vector<EvalScope> evalScopes;
    Vector<Function> functions;
    // What the bodies of `functions` read their ids through.
    UnitIDTables unitsOfBodies;
    Vector<Data> data;
    Vector<Entry> entries;
    Vector<RuntimeFunctionEntry> runtimeFunctions;
    Vector<WebAssemblyModule> webAssemblyModules;
    // What the front end says of members of built-in objects, each once. staysLocked: a test run could open the member, but the front end has folded by it: it has left a lookup out because no file spells a store to it.
    Vector<ReplaceableBuiltin> replaceableBuiltins;
    Vector<OpenedName> openedNames;
    // CompileOptions::excludedCode of the build that read the program.
    Wire::ExcludedCode excludedCode { };
    // CompileOptions::admitsReplacedBuiltins() of the build that read the program: a test run.
    bool admitsReplacedBuiltins { false };
    // testing=NativeCodeAnswersNullish of the build that read the program, wrong on purpose: every row of native code is taken to write undefined or null for its result.
    bool rowsOfNativeCodeWriteNullish { false };
    // CompileOptions::admitsUnseenCode() of the build that read the program.
    bool admitsInspector { false };

    Function& at(FunctionID id) { return functions[id.index()]; }
    const Function& at(FunctionID id) const { return functions[id.index()]; }
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
    Type at(TypeID id) const { return types.at(id); }
    ProvenInEveryVersion provenTypeOf(const DeclaredType& declared) const { return types.provenTypeOf(declared); }
    SetView<LayoutID, LayoutSetID> at(LayoutSetID id) const { return types.at(id); }
    const PropertyKey& at(PropertyKeyID id) const { return types.at(id); }

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
    // The layouts whose written bases lead up to this one, without itself. In preorder of the tree of bases, siblings ascending: whoever needs them ascending sorts them.
    // By written bases, so an expectation and never a proof: `new.target` may also select a class whose heritage nobody followed, or one that is not below at all. Facts::layoutsBelow() is the proof.
    // It stops the build in front of indexClasses(), and for a layout added behind it.
    std::span<const LayoutID> layoutsExpectedBelow(LayoutID) const LIFETIME_BOUND;
    std::span<const LayoutID> layoutsWithName(PropertyKeyID) const LIFETIME_BOUND;
    std::span<const LayoutID> declarersOfName(PropertyKeyID) const LIFETIME_BOUND;
    // The layouts that an object of this layout is written as: its supertypes, its written bases and theirs, ascending, without itself. `interface Type { id: number }` above what `function Type(this: Type) { }` makes.
    // By what is written, so an expectation and never a proof. None for a layout that is never allocated, and for one added behind indexViews().
    // None either where the written bases lead to nothing but classes with objects of their own: findField() goes up that row, so nobody asks those classes for a name that the layout lacks. A row of classes is as long as the
    // program makes it, and each of its classes would list all above it.
    std::span<const LayoutID> viewsAbove(LayoutID) const LIFETIME_BOUND;
    // The fields under the key that those layouts declare, in their order, of each the one that Layout::findField() answers with. The objects of one constructor may be written as hundreds of types.
    // What a layout has from the layout above it is listed there: that one is a view too, and the record is a copy.
    // Kept where there are at least fewestViewsIndexedByKey of them: whoever has fewer asks each.
    struct FieldWrittenAbove {
        PropertyKeyID key;
        LayoutID view;
        uint32_t field { 0 };
    };
    static constexpr size_t fewestViewsIndexedByKey = 8;
    std::span<const FieldWrittenAbove> fieldsWrittenAbove(LayoutID, PropertyKeyID) const LIFETIME_BOUND;
    // By the layout alone: Compile asks this.
    bool prototypeMaySelectOtherClass(LayoutID) const;
    void indexClasses();
    void indexNames();
    void indexNames(const ParallelLoops&);
    void indexViews(const ParallelLoops&);

    void appendBuiltinLayouts();
    void appendNamespaceLayouts(const ParallelLoops&);
    LayoutID layoutOfWholeNamespace(ModuleID) const;
    void internBuiltinNameKeys();
    // `L<n>` in every text. The layouts of built-ins and namespaces stand between the wire's and the derived ones and have no number: `L(<token>)`, `L(m<n>)`.
    // A derived layout is counted on from the wire's, so its number does not move when the table of built-ins gains a layout.
    uint32_t numberInTextOf(LayoutID) const;
    // Null for a number that no layout has.
    LayoutID layoutNumberedInText(uint32_t) const;

    // 0 to 63, from what the variable or the layout is and never from its id. Null for an id past the lists as they stood at computeLocationBits().
    std::optional<uint8_t> locationBitOf(VariableID id) const { return locationBitAt(m_derivedTables.locationBitsOfVariables, id.index()); }
    std::optional<uint8_t> locationBitOf(LayoutID id) const { return locationBitAt(m_derivedTables.locationBitsOfLayouts, id.index()); }
    // Once, on one thread, when `strings`, `modules`, `layouts` and `variables` are whole, and in front of TypeTable::endStableNumbersOfPropertyKeys().
    void computeLocationBits();
    void computeLocationBits(const ParallelLoops&);

    // The first module of kind RuntimeSource whose runtime module has this namespace object. Null: no compiled code makes the object.
    ModuleID compiledModuleOfNamespaceObject(BuiltinID canonical) const;
    bool isEntry(FunctionID) const;
    LineColumn lineColumnFor(ModuleID, uint32_t position) const;
    // A stack trace reports a construction at its callee. Call sites and the report of an uncaught error show where the expression starts.
    const ConstructionStart* findConstructionReportedAt(ModuleID, uint32_t position) const;

private:
    struct Bytes {
        std::span<const uint8_t> borrowed;
        Vector<uint8_t> owned;
        Vector<uint32_t> wordsOfPositions;
    };

    struct DerivedTables {
        Vector<LayoutID> builtinLayouts;
        Vector<LayoutSetID> declaredMembers;
        // Every layout in preorder of the tree of written bases. What lies below a layout stands right behind it.
        Vector<LayoutID> layoutsInOrderOfBases;
        Vector<uint32_t> placeInOrderOfBases;
        Vector<uint32_t> numberOfLayoutsBelow;
        Vector<uint32_t> firstLayoutWithName;
        Vector<LayoutID> layoutsWithName;
        Vector<LayoutID> declarersOfName;
        Vector<uint32_t> firstViewAbove;
        Vector<LayoutID> viewsAbove;
        // By layout, sorted by key.
        Vector<uint32_t> firstFieldWrittenAbove;
        Vector<FieldWrittenAbove> fieldsWrittenAbove;
        Vector<PropertyKeyID> builtinNameKeys;
        Vector<BuiltinName> registeredSymbolNames;
        // Sorted by built-in, one entry a built-in.
        Vector<std::pair<BuiltinID, ModuleID>> compiledModulesOfNamespaceObjects;
        Vector<uint8_t> locationBitsOfVariables;
        Vector<uint8_t> locationBitsOfLayouts;
    };

    static std::optional<uint8_t> locationBitAt(const Vector<uint8_t>& bits, uint32_t index)
    {
        if (index >= bits.size())
            return std::nullopt;
        return bits[index];
    }

    const BuiltinTable& m_builtins;
    BitVector m_namesOpenedSomewhere;
    mutable Lock m_keysTakenForAbsentLock;
    mutable BitVector m_keysTakenForAbsent WTF_GUARDED_BY_LOCK(m_keysTakenForAbsentLock);
    BitVector m_holdersOfOpenedNames;
    // `openedNames`, for isOpened(): a large test run opens names by the hundred thousand.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesOpened;
    bool m_testsReplaceBuiltins { false };
    BitVector m_builtinsThatMayBeTrimmed;
    Bytes m_bytes;
    Vector<Vector<ConstructionStart>> m_constructionStartsOfModules;
    DerivedTables m_derivedTables;
};

BitVector functionsCalledByHost(const Program&);
BitVector functionsCalledByHost(const Program&, const ParallelLoops&);

const RuntimeFunctionEntry* findRuntimeFunction(const Program&, RuntimeFunctionID);
bool isLibraryCode(const Program&, const Function&);
bool isBundlerHelperModule(const Program&, const Module&);
// The ES module through which importers see a CommonJS file. Its first request requires the file.
bool isFaceOfCommonJSFile(const Program&, const Module&);
bool isBundlerHelperNamedByProgram(const Program&, ModuleID);
// The host asks the loader for an entry point. Where the program loads files under names that the build does not follow, it also asks for the files marked IsFoundByName, and for those that a copy from disk may import: MayBeEvicted.
// Where the program admits the inspector it asks for any file with code.
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
bool requireReadsModuleValue(const Program&, const Module&);
// overridableRequire() in CommonJS.ts.
bool requireAddsToRequireCache(const Program&, ModuleID referrer, ModuleID target);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
