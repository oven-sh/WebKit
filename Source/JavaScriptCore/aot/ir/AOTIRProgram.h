/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
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
    bool someClassExtendsMixin { false };
    Vector<TaggedFamily> taggedFamilies;
    Vector<Variable> variables;
    Vector<Module> modules;
    Vector<ModuleInstance> moduleInstances;
    Vector<ModuleLink> moduleLinks;
    Vector<ModuleFailure> moduleFailures;
    Vector<AmbiguousExport> ambiguousExports;
    Vector<SourceStamp> sourceStamps;
    Vector<DeclaredRecord> declaredRecords;
    Vector<EvalScope> evalScopes;
    Vector<Function> functions;
    UnitIDTables unitsOfBodies;
    Vector<Data> data;
    Vector<Entry> entries;
    Vector<RuntimeFunctionEntry> runtimeFunctions;
    Vector<WebAssemblyModule> webAssemblyModules;
    Vector<ReplaceableBuiltin> replaceableBuiltins;
    Vector<OpenedName> openedNames;
    Vector<BuiltinUsedAtStart> builtinsUsedAtStart;
    Wire::ExcludedCode excludedCode { };
    bool admitsReplacedBuiltins { false };
    bool shouldAssumeNullishHostResults { false };
    uint16_t builtinInjectedAsMinimumInteger { 0 };
    uint16_t builtinInjectedAsNull { 0 };
    uint16_t opcodeInjectedAsOtherKind { 0 };
    uint16_t builtinInjectedAsHoldingNull { 0 };
    uint16_t builtinInjectedAsFresh { 0 };
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
    PropertyKeyID keyOfWordOnWhatSeesAccesses() const { return m_derivedTables.keyOfWordOnWhatSeesAccesses; }
    ALWAYS_INLINE BuiltinName builtinNameOf(PropertyKeyID id) const
    {
        const PropertyKey& key = at(id);
        if (key.kind == PropertyKeyKind::Text) [[likely]]
            return strings.builtinName(key.text);
        return builtinNameOfKeyThatIsNoText(id, key);
    }
    BuiltinName builtinNameOfKeyThatIsNoText(PropertyKeyID, const PropertyKey&) const;
    inline NameClass nameClassOf(PropertyKeyID) const;
    bool isIndexLike(PropertyKeyID key) const { return mayBeElementKey(nameClassOf(key)); }

    LayoutID layoutOf(BuiltinLayoutID id) const { return m_derivedTables.builtinLayouts[id.index()]; }
    LayoutSetID declaredMembersOf(LayoutID id) const { return m_derivedTables.declaredMembers[id.index()]; }
    bool hasDeclaredMembers(LayoutID id) const { return id.index() < m_derivedTables.declaredMembers.size() && m_derivedTables.declaredMembers[id.index()]; }
    std::span<const LayoutSetID> membersOfParentsOfOneClass() const LIFETIME_BOUND { return m_derivedTables.membersOfParentsOfOneClass.span(); }
    void setMembersOfParentsOfOneClass(Vector<LayoutSetID>&& sets) { m_derivedTables.membersOfParentsOfOneClass = WTF::move(sets); }
    void setDeclaredMembers(LayoutID, LayoutSetID);
    std::span<const LayoutID> layoutsExpectedBelow(LayoutID) const LIFETIME_BOUND;
    std::span<const LayoutID> layoutsWithName(PropertyKeyID) const LIFETIME_BOUND;
    std::span<const LayoutID> viewsAbove(LayoutID) const LIFETIME_BOUND;
    struct FieldOfView {
        LayoutID view;
        uint32_t field { 0 };
    };
    static constexpr size_t fewestViewsIndexedByKey = 8;
    std::span<const FieldOfView> fieldsOfViews(PropertyKeyID) const LIFETIME_BOUND;
    template<typename Functor>
    bool visitInheritedFields(LayoutID id, PropertyKeyID key, const Functor& functor) const
    {
        std::span<const FieldOfView> fields = fieldsOfViews(key);
        std::span<const LayoutID> views = viewsAbove(id);
        auto nextField = fields.begin();
        auto nextView = views.begin();
        while (nextField != fields.end() && nextView != views.end()) {
            if (nextField->view < *nextView)
                nextField = std::ranges::lower_bound(nextField, fields.end(), *nextView, { }, &FieldOfView::view);
            else if (*nextView < nextField->view)
                nextView = std::ranges::lower_bound(nextView, views.end(), nextField->view);
            else {
                if (!functor(*nextField))
                    return false;
                ++nextField;
                ++nextView;
            }
        }
        return true;
    }
    bool prototypeMaySelectOtherClass(LayoutID) const;
    void indexClasses();
    void indexNames();
    void indexNames(const ParallelLoops&);
    void indexViews(const ParallelLoops&);

    void appendBuiltinLayouts();
    void appendNamespaceLayouts(const ParallelLoops&);
    LayoutID layoutOfWholeNamespace(ModuleID) const;
    LayoutID layoutOfNamespaceThatListsNoName(ModuleID) const;
    VariableID variableBehindExport(ModuleID, uint32_t index) const;
    std::span<const FieldReference> exportedFields() const LIFETIME_BOUND { return m_derivedTables.exportedFields.span(); }
    struct InstanceField {
        FieldReference field;
        ModuleID linkTarget;
    };
    std::span<const InstanceField> instanceFields() const LIFETIME_BOUND { return m_derivedTables.instanceFields.span(); }
    const ModuleInstance* findModuleInstance(ModuleID) const;
    VariableID instanceVariable(ModuleID id) const { return id.index() < m_derivedTables.instanceVariables.size() ? m_derivedTables.instanceVariables[id.index()].first : VariableID(); }
    VariableID severalInstancesVariable(ModuleID id) const { return id.index() < m_derivedTables.instanceVariables.size() ? m_derivedTables.instanceVariables[id.index()].second : VariableID(); }
    bool isModuleInstance(LayoutID id) const { return moduleScopeKind(id) == ModuleScopeKind::IsInstance; }
    bool isInstanceVariable(FieldReference) const;
    bool isInstanceLink(FieldReference) const;
    bool isInstanceConstant(FieldReference) const;
    std::span<const ModuleLink> linksOfModule(ModuleID) const LIFETIME_BOUND;
    void indexInstanceFields();
    void indexModuleScopes();
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
    static LineColumn lineColumnAfter(const SourcePositions&, size_t numberOfLines, size_t numberOfAdjustments, uint32_t position);
    const ConstructionStart* findConstructionReportedAt(ModuleID, uint32_t position) const;

private:
    enum class ModuleScopeKind : uint8_t { IsNone, IsInstance, IsOtherEnvironment };
    ModuleScopeKind moduleScopeKind(LayoutID id) const { return id.index() < m_derivedTables.moduleScopeKinds.size() ? m_derivedTables.moduleScopeKinds[id.index()] : ModuleScopeKind::IsNone; }

    struct Bytes {
        std::span<const uint8_t> borrowed;
        OwnedBytes owned;
        MallocSpan<uint32_t> wordsOfPositions;
    };

    struct DerivedTables {
        Vector<LayoutID> builtinLayouts;
        Vector<LayoutSetID> declaredMembers;
        Vector<LayoutSetID> membersOfParentsOfOneClass;
        Vector<LayoutID> layoutsInOrderOfBases;
        Vector<uint32_t> placeInOrderOfBases;
        Vector<uint32_t> numberOfLayoutsBelow;
        Vector<uint32_t> firstLayoutWithName;
        Vector<LayoutID> layoutsWithName;
        Vector<uint32_t> firstViewAbove;
        Vector<LayoutID> viewsAbove;
        Vector<uint32_t> firstFieldsByKey;
        Vector<FieldOfView> fieldsOfViewsByKey;
        Vector<PropertyKeyID> builtinNameKeys;
        PropertyKeyID keyOfWordOnWhatSeesAccesses;
        Vector<BuiltinName> registeredSymbolNames;
        Vector<std::pair<BuiltinID, ModuleID>> compiledModulesOfNamespaceObjects;
        Vector<uint8_t> locationBitsOfVariables;
        Vector<uint8_t> locationBitsOfLayouts;
        Vector<FieldReference> exportedFields;
        Vector<InstanceField> instanceFields;
        Vector<ModuleScopeKind> moduleScopeKinds;
        Vector<std::pair<VariableID, VariableID>> instanceVariables;
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

ALWAYS_INLINE LayoutID baseWhoseFieldListIsCopiedWhole(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (!layout.copiesWholeFieldListOfBase)
        return { };
    LayoutID base = writtenBaseOf(layout);
    return !program.at(base).fieldsChangedAfterLayoutsBelowTookThem && layout.fields.size() - layout.ownFieldCount == program.at(base).fields.size() ? base : LayoutID();
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

inline void forEachOfSeveralParents(const Program& program, LayoutID id, NOESCAPE const Invocable<void(LayoutID)> auto& functor)
{
    const Layout& layout = program.at(id);
    if (layout.base || !layout.has(LayoutFlags::HasHeritage) || !isDefinedByClass(layout))
        return;
    for (LayoutID parent : layout.supertypes) {
        if (parent != id && isDefinedByClass(program.at(parent)))
            functor(parent);
    }
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
FunctionID findNakedConstructorOf(const Program&, BuiltinID);
FunctionID functionThatSorts(const Program&);
bool isLibraryCode(const Program&, const Function&);
bool isBundlerHelperModule(const Program&, const Module&);
bool isCommonJSFacade(const Program&, const Module&);
bool parsersCouldMakeRecordsOf(const Program&, LayoutID);
bool isBundlerHelperNamedByProgram(const Program&, ModuleID);
BitVector modulesThatHostAsksFor(const Program&, bool beginsModulesOfData = false);
bool answersToPath(const Program&, const Module&);
ModuleID compiledModuleWithObject(const Program&, BuiltinID);
bool objectOfLibraryFileIsResultOfBody(const Program&, ModuleID);

constexpr bool hasNamespaceLayout(ModuleKind kind)
{
    return kind == ModuleKind::SourceText || kind == ModuleKind::RuntimeSource;
}
ModuleID commonJsModuleCapturedBy(const Program&, const Module& facade);
Vector<ModuleID, 2> formsInImage(const Program&, ModuleID named);
bool reachesModuleObjects(const Program&);
BuiltinID hostRequireFunctionFor(const Module&);
bool loaderEntersBodyAtMostOnce(const Module&);
bool isBodyWrittenInAnotherFile(const Program&, FunctionID);
bool importMayAnswerWithMock(const Module&);
bool requireMayAnswerWithMock(const Program&, const Module&);
bool requireReadsModuleValue(const Program&, const Module&);
bool requireAddsToRequireCache(const Program&, ModuleID referrer, ModuleID target);
bool importStatementListsInRequireCache(const Program&, ModuleID target);
bool loaderFillsVariablesOffValueOf(const Module&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
