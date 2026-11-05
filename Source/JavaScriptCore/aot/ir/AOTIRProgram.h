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
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

class BuildContext;
class BuiltinTable;

namespace Typed {

enum class SchemaHashCheck : uint8_t { Required, Skipped };

class Program final {
    WTF_MAKE_TZONE_ALLOCATED(Program);
    WTF_MAKE_NONCOPYABLE(Program);

    BuiltinTableOfBuild m_tableOfBuild;

public:
    explicit Program(const BuiltinTable&);
    ~Program();

    static std::unique_ptr<Program> read(std::span<const uint8_t> bytes, BuildContext&, SchemaHashCheck = SchemaHashCheck::Required);

    bool mayBeTrimmed(BuiltinID builtin) const { return builtin && m_builtinsThatMayBeTrimmed.get(builtin.index()); }
    const BuiltinTable& builtins() const { return m_builtins; }
    void unlockReplaceableBuiltins();
    std::span<const BuiltinID> unlockedBuiltins() const LIFETIME_BOUND { return m_tableOfBuild.unlockedMembers(); }
    bool isUnlocked(BuiltinID member) const { return std::ranges::binary_search(unlockedBuiltins(), member); }
    std::span<const BuiltinID> builtinsWhoseLockIsLeftToProgram() const LIFETIME_BOUND { return m_tableOfBuild.membersWhoseLockIsLeftToProgram(); }
    bool isLockLeftToProgram(BuiltinID member) const { return std::ranges::binary_search(builtinsWhoseLockIsLeftToProgram(), member); }
    void openNameForTestRun(BuiltinID canonicalHolder, StringID name);
    bool isOpened(BuiltinID canonicalHolder, StringID name) const { return m_namesOpened.contains(static_cast<uint64_t>(canonicalHolder.index()) << 32 | name.index()); }
    bool isOpenedSomewhere(StringID name) const { return m_namesOpenedSomewhere.get(name.index()); }
    void noteTakenForAbsent(PropertyKeyID) const;
    Vector<PropertyKeyID> keysTakenForAbsent() const;
    bool isSomeNameOpenedOn(BuiltinID canonicalHolder) const { return m_holdersOfOpenedNames.get(canonicalHolder.index()); }
    bool testsReplaceBuiltins() const { return m_testsReplaceBuiltins; }
    std::span<const uint8_t> buffer() const { return m_bytes.borrowed; }
    void borrowBuffer(std::span<const uint8_t> bytes) { m_bytes.borrowed = bytes; }
    bool ownsBuffer() const { return !!m_bytes.owned.sizeInBytes(); }
    std::span<const uint8_t> copyBorrowedBytes(std::span<const uint8_t>, const ParallelLoops&);
    void copyBorrowedWordsOfPositions(const ParallelLoops&);
    void setConstructionStarts(Vector<ConstructionStartInModule>&&);
    using OwnedBytes = MallocSpan<uint8_t, WTF::VectorBufferMalloc>;
    void adoptBuffer(OwnedBytes&&);

    StringTable strings;
    TypeTable types;
    Vector<TypeSite> typeSites;
    Vector<ExpectedResult> expectedResults;
    Vector<Layout> layouts;
    Vector<TaggedFamily> taggedFamilies;
    Vector<Variable> variables;
    Vector<Module> modules;
    Vector<ModuleFailure> moduleFailures;
    Vector<AmbiguousExport> ambiguousExports;
    Vector<SourceStamp> sourceStamps;
    Vector<EvalScope> evalScopes;
    Vector<Function> functions;
    UnitIDTables unitsOfBodies;
    Vector<Data> data;
    Vector<Entry> entries;
    Vector<RuntimeFunctionEntry> runtimeFunctions;
    Vector<WebAssemblyModule> webAssemblyModules;
    Vector<ReplaceableBuiltin> replaceableBuiltins;
    Vector<OpenedName> openedNames;
    Wire::ExcludedCode excludedCode { };
    bool admitsReplacedBuiltins { false };
    bool rowsOfNativeCodeWriteNullish { false };
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
    inline NameClass nameClassOf(PropertyKeyID) const;
    bool isIndexLike(PropertyKeyID key) const { return mayBeElementKey(nameClassOf(key)); }

    LayoutID layoutOf(BuiltinLayoutID id) const { return m_derivedTables.builtinLayouts[id.index()]; }
    LayoutSetID declaredMembersOf(LayoutID id) const { return m_derivedTables.declaredMembers[id.index()]; }
    bool hasDeclaredMembers(LayoutID id) const { return id.index() < m_derivedTables.declaredMembers.size() && m_derivedTables.declaredMembers[id.index()]; }
    void setDeclaredMembers(LayoutID, LayoutSetID);
    std::span<const LayoutID> layoutsExpectedBelow(LayoutID) const LIFETIME_BOUND;
    std::span<const LayoutID> layoutsWithName(PropertyKeyID) const LIFETIME_BOUND;
    std::span<const LayoutID> declarersOfName(PropertyKeyID) const LIFETIME_BOUND;
    std::span<const LayoutID> viewsAbove(LayoutID) const LIFETIME_BOUND;
    struct FieldWrittenAbove {
        PropertyKeyID key;
        LayoutID view;
        uint32_t field { 0 };
    };
    static constexpr size_t fewestViewsIndexedByKey = 8;
    std::span<const FieldWrittenAbove> fieldsWrittenAbove(LayoutID, PropertyKeyID) const LIFETIME_BOUND;
    bool prototypeMaySelectOtherClass(LayoutID) const;
    void indexClasses();
    void indexNames();
    void indexNames(const ParallelLoops&);
    void indexViews(const ParallelLoops&);

    void appendBuiltinLayouts();
    void appendNamespaceLayouts(const ParallelLoops&);
    LayoutID layoutOfWholeNamespace(ModuleID) const;
    VariableID variableBehindExport(ModuleID, uint32_t index) const;
    void internBuiltinNameKeys();
    uint32_t numberInTextOf(LayoutID) const;
    LayoutID layoutNumberedInText(uint32_t) const;

    static constexpr uint8_t locationBitOfVariablesThatPlanAdds = 0;
    std::optional<uint8_t> locationBitOf(VariableID id) const { return id && id.index() >= variables.size() ? std::optional(locationBitOfVariablesThatPlanAdds) : locationBitAt(m_derivedTables.locationBitsOfVariables, id.index()); }
    std::optional<uint8_t> locationBitOf(LayoutID id) const { return locationBitAt(m_derivedTables.locationBitsOfLayouts, id.index()); }
    void computeLocationBits();
    void computeLocationBits(const ParallelLoops&);

    ModuleID compiledModuleOfNamespaceObject(BuiltinID canonical) const;
    bool isEntry(FunctionID) const;
    LineColumn lineColumnFor(ModuleID, uint32_t position) const;
    const ConstructionStart* findConstructionReportedAt(ModuleID, uint32_t position) const;

private:
    struct Bytes {
        std::span<const uint8_t> borrowed;
        OwnedBytes owned;
        MallocSpan<uint32_t> wordsOfPositions;
    };

    struct DerivedTables {
        Vector<LayoutID> builtinLayouts;
        Vector<LayoutSetID> declaredMembers;
        Vector<LayoutID> layoutsInOrderOfBases;
        Vector<uint32_t> placeInOrderOfBases;
        Vector<uint32_t> numberOfLayoutsBelow;
        Vector<uint32_t> firstLayoutWithName;
        Vector<LayoutID> layoutsWithName;
        Vector<LayoutID> declarersOfName;
        Vector<uint32_t> firstViewAbove;
        Vector<LayoutID> viewsAbove;
        Vector<uint32_t> firstFieldWrittenAbove;
        Vector<FieldWrittenAbove> fieldsWrittenAbove;
        Vector<PropertyKeyID> builtinNameKeys;
        Vector<BuiltinName> registeredSymbolNames;
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
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesOpened;
    bool m_testsReplaceBuiltins { false };
    BitVector m_builtinsThatMayBeTrimmed;
    Bytes m_bytes;
    Vector<Vector<ConstructionStart>> m_constructionStartsOfModules;
    DerivedTables m_derivedTables;
};

ALWAYS_INLINE NameClass Program::nameClassOf(PropertyKeyID id) const
{
    const PropertyKey& key = at(id);
    switch (key.kind) {
    case PropertyKeyKind::Text:
        return strings.nameClass(key.text);
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return NameClass::Ordinary;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE const Field& fieldAt(const Program& program, FieldReference reference)
{
    return program.at(reference.layout).fields[reference.field];
}

ALWAYS_INLINE FieldReference declarationOf(const Program& program, FieldReference reference)
{
    for (size_t steps = 0; fieldAt(program, reference).flags.contains(FieldFlags::IsInherited); ++steps) {
        RELEASE_ASSERT(steps < program.layouts.size());
        const Field& field = fieldAt(program, reference);
        reference = { field.originLayout, *field.originField };
    }
    return reference;
}

ALWAYS_INLINE std::optional<FieldReference> findField(const Program& program, LayoutID id, PropertyKeyID key)
{
    if (!id)
        return std::nullopt;
    const Layout& layout = program.at(id);
    if (auto field = layout.findField(key))
        return FieldReference { id, *field };
    if (layout.inheritedFields == InheritedFields::AllOfRow)
        return std::nullopt;
    return findFieldAboveInRow(program, id, key);
}

inline LayoutID declaredLayoutOf(const Program& program, LayoutID id)
{
    LayoutID current = id;
    while (current && program.at(current).isDerived())
        current = writtenBaseOf(program.at(current));
    return current;
}

BitVector functionsCalledByHost(const Program&);
BitVector functionsCalledByHost(const Program&, const ParallelLoops&);

const RuntimeFunctionEntry* findRuntimeFunction(const Program&, RuntimeFunctionID);
bool isLibraryCode(const Program&, const Function&);
bool isBundlerHelperModule(const Program&, const Module&);
bool isFaceOfCommonJSFile(const Program&, const Module&);
bool isBundlerHelperNamedByProgram(const Program&, ModuleID);
BitVector modulesThatHostAsksFor(const Program&, bool beginsModulesOfData = false);
bool answersToPath(const Program&, const Module&);
ModuleID compiledModuleWithObject(const Program&, BuiltinID);
ModuleID commonJsModuleCapturedBy(const Program&, const Module& face);
Vector<ModuleID, 2> formsInImage(const Program&, ModuleID named);
bool namesWaysToModuleObjects(const Program&);
BuiltinID functionOfHostThatRequireCallsFor(const Module&);
bool importMayAnswerWithMock(const Module&);
bool requireMayAnswerWithMock(const Program&, const Module&);
bool requireReadsModuleValue(const Program&, const Module&);
bool requireAddsToRequireCache(const Program&, ModuleID referrer, ModuleID target);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
