/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPredictedValues.h"
#include "AOTThreshold.h"
#include "AOTValueFlow.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowUnobservedDefinitions.h"
#include "AOTVector.h"
#include <array>
#include <memory>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Seconds.h>
#include <atomic>
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/MallocSpan.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CFG;
class ValueSetMaker;
class ValueSets;

// Calls the functor with the path that each `with { type: "file" }` import evaluates to.
void forEachEmbeddedFilePath(const Program&, NOESCAPE const ScopedLambda<void(ModuleID, StringID)>&);

enum class ValueFlowObjectKind : uint8_t {
    Primitive,
    UnknownObject,
    Builtin,
    ModuleBuiltin,
    Namespace,
    Closure,
    CoroutineResult,
    RestArray,
    FunctionPrototype,
    ConstructedObject,
    Site,
    Instance,
    Prototype,
    ResultOfBuiltin,
    Key,
};

struct ValueFlowObjectName {
    ValueFlowObjectKind kind { ValueFlowObjectKind::UnknownObject };
    uint32_t index { 0 };
    ModuleID module;

    friend constexpr bool operator==(const ValueFlowObjectName&, const ValueFlowObjectName&) = default;
    friend constexpr auto operator<=>(const ValueFlowObjectName&, const ValueFlowObjectName&) = default;
};

enum class ValueFlowSiteOrigin : uint8_t {
    Operation,
    DataNode,
    BuiltinResult,
    TypedResult,
};

struct ValueFlowSite {
    ValueFlowSiteOrigin origin { ValueFlowSiteOrigin::Operation };
    ValueID value;
    uint32_t ordinal { 0 };
    LayoutID layout;
    BuiltinClassID builtinClass;
    bool namesPrototype { false };

    friend bool operator==(const ValueFlowSite&, const ValueFlowSite&) = default;
};

class ValueFlowUniverse {
public:
    static constexpr ValueFlowObjectID primitive(PrimitiveSet kind) { return ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(static_cast<uint16_t>(kind)))); }
    static constexpr uint32_t numberOfPrimitiveKinds = 16;

    static constexpr ValueFlowObjectID unknownObject() { return ValueFlowObjectID(numberOfPrimitiveKinds); }
    static constexpr ValueFlowObjectID untrackedObject() { return ValueFlowObjectID(numberOfPrimitiveKinds + 1); }
    // Goes beside Bun from a read of Bun under its name, along every edge but those through properties of the unknown object. Rules and results never see it in a value.
    static constexpr ValueFlowObjectID markOfNamedBun() { return ValueFlowObjectID(numberOfPrimitiveKinds + 2); }
    // Goes with the answer of a read under a key nobody knows of what a built-in module answers, where the read left out a member that lets in unseen code, along the same edges. A read that names the member finds it where its answer holds the mark, or its receiver: a proxy whose get trap answers with the mark holds it.
    static constexpr ValueFlowObjectID markOfMemberLeftOut() { return ValueFlowObjectID(numberOfPrimitiveKinds + 3); }
    // Goes beside Bun from a load of "bun" under a constant name, and with the answer of a read under a key nobody knows of a Bun that was so loaded or named, along the same edges. So do the values of a copy of such a Bun, and a descriptor of one of them. A read that names a member of Bun finds it where its answer holds the mark, or its receiver: a proxy whose get trap answers with the mark holds it.
    static constexpr ValueFlowObjectID markOfLoadedBun() { return ValueFlowObjectID(numberOfPrimitiveKinds + 4); }
    ValueFlowObjectID objectOf(BuiltinID builtin) const { return ValueFlowObjectID(m_firstBuiltin + m_builtins->canonicalID(builtin).index()); }
    ValueFlowObjectID objectOf(BuiltinID, ModuleID) const;
    void appendObjectsOf(BuiltinID, Vector<ValueFlowObjectID, 32>&) const;
    ValueFlowObjectID namespaceOf(ModuleID module) const { return ValueFlowObjectID(m_firstNamespace + module.index()); }
    ValueFlowObjectID instanceOf(LayoutID layout) const { return ValueFlowObjectID(m_firstLayoutObject + 2 * layout.index()); }
    ValueFlowObjectID prototypeOf(LayoutID layout) const { return ValueFlowObjectID(m_firstLayoutObject + 2 * layout.index() + 1); }
    ValueFlowObjectID resultOf(BuiltinID builtin, uint32_t ordinal) const { return ValueFlowObjectID(m_firstResultOfBuiltin + 2 * m_builtins->canonicalID(builtin).index() + ordinal); }
    ValueFlowObjectID hostObject(uint32_t indexOfRecord) const { return ValueFlowObjectID(m_firstResultOfBuiltin + 2 * m_builtins->builtinCount() + indexOfRecord); }
    ValueFlowObjectID closureOf(FunctionID function) const { return ValueFlowObjectID(m_firstClosure + function.index()); }
    ValueFlowObjectID coroutineResultOf(FunctionID function) const { return ValueFlowObjectID(m_firstCoroutineResult + function.index()); }
    ValueFlowObjectID restArrayOf(FunctionID function) const { return ValueFlowObjectID(m_firstRestArray + function.index()); }
    ValueFlowObjectID prototypeObjectOf(FunctionID function) const { return ValueFlowObjectID(m_firstFunctionPrototype + function.index()); }
    ValueFlowObjectID constructedObjectOf(FunctionID newTarget) const { return ValueFlowObjectID(m_firstConstructedObject + newTarget.index()); }
    ValueFlowObjectID siteObject(const ValueFlowStatement&, uint32_t ordinal = 0) const;
    ValueFlowObjectID siteObject(FunctionID, ValueID, uint32_t ordinal = 0) const;
    bool isSiteObjectOf(const ValueFlowStatement&, ValueFlowObjectID) const;
    ValueFlowObjectID keyConstant(PropertyKeyID key) const { return ValueFlowObjectID(m_firstKey + key.index()); }
    ValueFlowObjectID objectNamed(FunctionID body, const ValueFlowObjectName&) const;

    uint32_t numberOfObjects() const { return m_numberOfObjects; }
    // Objects are numbered kind by kind. A walk that looks for sites, or for sites and instances, goes over these alone, in the order of their numbers.
    uint32_t numberOfSiteObjects() const { return m_firstKey - m_firstSite; }
    ValueFlowObjectID siteObjectAt(uint32_t position) const { return ValueFlowObjectID(m_firstSite + position); }
    uint32_t numberOfSiteAndLayoutObjects() const { return m_firstResultOfBuiltin - m_firstLayoutObject + numberOfSiteObjects(); }
    ValueFlowObjectID siteOrLayoutObjectAt(uint32_t position) const
    {
        uint32_t numberOfLayoutObjects = m_firstResultOfBuiltin - m_firstLayoutObject;
        return position < numberOfLayoutObjects ? ValueFlowObjectID(m_firstLayoutObject + position) : siteObjectAt(position - numberOfLayoutObjects);
    }
    // What has the class of a built-in without being one: what built-ins answer with, the results of coroutines, the arrays of rest parameters, the sites.
    uint32_t numberOfObjectsMadeWithClasses() const { return m_firstClosure - m_firstResultOfBuiltin + numberOfObjectsMadeWithClassesByProgram(); }
    ValueFlowObjectID objectMadeWithClassAt(uint32_t position) const
    {
        uint32_t numberOfResultsOfBuiltins = m_firstClosure - m_firstResultOfBuiltin;
        return position < numberOfResultsOfBuiltins ? ValueFlowObjectID(m_firstResultOfBuiltin + position) : objectMadeWithClassByProgramAt(position - numberOfResultsOfBuiltins);
    }
    uint32_t numberOfObjectsMadeWithClassesByProgram() const { return m_firstFunctionPrototype - m_firstCoroutineResult + numberOfSiteObjects(); }
    ValueFlowObjectID objectMadeWithClassByProgramAt(uint32_t position) const
    {
        uint32_t numberOfObjectsOfFunctions = m_firstFunctionPrototype - m_firstCoroutineResult;
        return position < numberOfObjectsOfFunctions ? ValueFlowObjectID(m_firstCoroutineResult + position) : siteObjectAt(position - numberOfObjectsOfFunctions);
    }
    ValueFlowObjectKind kindOf(ValueFlowObjectID) const;
    bool isObject(ValueFlowObjectID object) const { return object.index() >= numberOfPrimitiveKinds && object.index() < m_firstKey; }

    FunctionID closureFunction(ValueFlowObjectID object) const { return object.index() >= m_firstClosure && object.index() < m_firstCoroutineResult ? FunctionID(object.index() - m_firstClosure) : FunctionID(); }
    BuiltinID builtinOf(ValueFlowObjectID) const;
    ModuleID moduleOf(ValueFlowObjectID) const;
    ModuleID moduleOf(RuntimeModuleID runtimeModule) const { return runtimeModule.index() < m_moduleOfRuntimeModule.size() ? m_moduleOfRuntimeModule[runtimeModule.index()] : ModuleID(); }
    std::atomic<uint32_t>& rememberedClosuresCalledDirectlyBy(FunctionID function) const { return m_closuresCalledDirectlyBy[function.index()]; }
    ModuleID moduleWithBody(FunctionID) const;
    PropertyKeyID keyOf(ValueFlowObjectID object) const { return object.index() >= m_firstKey ? PropertyKeyID(object.index() - m_firstKey) : PropertyKeyID(); }
    FunctionID functionOf(ValueFlowObjectID) const;
    const ValueFlowSite* siteOf(ValueFlowObjectID object) const LIFETIME_BOUND { return object.index() < m_firstSite || object.index() >= m_firstKey ? nullptr : &m_sites[m_siteOfObject[object.index() - m_firstSite]]; }
    LayoutID layoutOf(ValueFlowObjectID) const;
    LayoutID classLayoutOf(ValueFlowObjectID object) const { return kindOf(object) == ValueFlowObjectKind::Prototype ? LayoutID((object.index() - m_firstLayoutObject) / 2) : LayoutID(); }
    BuiltinClassID classOf(ValueFlowObjectID) const;

    ValueSetID primitives() const { return m_primitives; }
    ValueSetID anyPrimitive() const { return m_anyPrimitive; }
    ValueSetID undefinedValue() const { return m_undefinedValue; }
    ValueSetID objects() const { return m_objects; }
    ValueSetID knownObjects() const { return m_knownObjects; }
    ValueSetID builtinObjects() const { return m_builtinObjects; }
    ValueSetID namespaces() const { return m_namespaces; }
    ValueSetID objectsOfRealm() const { return m_objectsOfRealm; }
    ValueSetID lockedObjects() const { return m_lockedObjects; }
    ValueSetID objectsWithLockedPrototypeLink() const { return m_objectsWithLockedPrototypeLink; }
    ValueSetID objectsBornWithoutPrototype() const { return m_objectsBornWithoutPrototype; }
    ValueSetID holdersOfLibraryExports() const { return m_holdersOfLibraryExports; }
    ValueSetID holdersOfExportsOf(ModuleID module) const { return m_holdersOfExportsOfModule[module.index()]; }
    ValueSetID holdersOfLibraryExportsThatRaiseNoEvent() const { return m_holdersOfLibraryExportsThatRaiseNoEvent; }
    ValueSetID eventRaisingLibraryExportHolders() const { return m_eventRaisingLibraryExportHolders; }
    ValueSetID eventRaisingLibraryClosures() const { return m_eventRaisingLibraryClosures; }
    ValueSetID prototypeObjectsOfLibraryClasses() const { return m_prototypeObjectsOfLibraryClasses; }
    ValueSetID instancesBelowLibraryClasses() const { return m_instancesBelowLibraryClasses; }
    ValueSetID instancesBelowBuiltinClasses() const { return m_instancesBelowBuiltinClasses; }
    ValueSetID objectsOfLibraryFoundByNameAlone() const { return m_objectsOfLibraryFoundByNameAlone; }
    ValueSetID callables() const { return m_callables; }
    ValueSetID closures() const { return m_closures; }
    ValueSetID builtinFunctions() const { return m_builtinFunctions; }
    ValueSetID builtinFunctionsOfProtocols() const { return m_builtinFunctionsOfProtocols; }
    ValueSetID builtinFunctionsThatReadSpecies() const { return m_builtinFunctionsThatReadSpecies; }
    // They store into their receiver, change it, or construct what its `constructor` names. None of that happens to an object because it has escaped.
    ValueSetID builtinFunctionsThatActOnReceiver() const { return m_builtinFunctionsThatActOnReceiver; }
    ValueSetID otherCallables() const { return m_otherCallables; }
    std::atomic<uint64_t>& rememberedBoundFunctionsBindingNoArgument() const { return m_boundFunctionsBindingNoArgument[0]; }
    ValueSetID arrays() const { return m_arrays; }
    ValueSetID records() const { return m_records; }
    ValueSetID keys() const { return m_keys; }
    // A set that holds every string still lists these keys, so that collectWorkerEntries() can see where they arrive.
    ValueSetID embeddedFilePaths() const { return m_embeddedFilePaths; }
    ValueSetID strings() const { return m_strings; }
    ValueSetID symbols() const { return m_symbols; }
    ValueSetID instancesOf(BuiltinClassID builtinClass) const { return m_objectsOfClass[builtinClass.index()]; }
    ValueSetID objectsWith(LayoutID layout) const { return m_objectsOfLayout[layout.index()]; }
    ValueSetID sureInstancesOf(BuiltinClassID builtinClass) const { return m_sureInstancesOfClass[builtinClass.index()]; }
    // By what an object was made as, whether or not compiled code has a test for the class.
    ValueSetID objectsThatMayBeMadeAs(BuiltinClassID builtinClass) const { return m_objectsThatMayBeMadeAs[builtinClass.index()]; }
    ValueSetID objectsMadeAs(BuiltinClassID builtinClass) const { return m_objectsMadeAs[builtinClass.index()]; }
    ValueSetID objectsAllocatedAsIterators() const { return m_objectsAllocatedAsIterators; }
    ValueSetID objectsThatMayBeAnything() const { return m_objectsThatMayBeAnything; }
    ValueSetID unknownObjectAlone() const { return m_unknownObjectAlone; }
    ValueSetID untrackedObjectAlone() const { return m_untrackedObjectAlone; }
    ValueSetID placeholdersAlone() const { return m_placeholdersAlone; }
    ValueSetID objectsThatPickTheirLayout() const { return m_objectsThatPickTheirLayout; }
    ValueSetID constructedObjects() const { return m_constructedObjects; }
    ValueSetID instancesOfClasses() const { return m_instancesOfClasses; }
    ValueSetID constructorsWithLayouts() const { return m_constructorsWithLayouts; }
    std::span<const LayoutID> recordsThatTypedCodeTakes() const LIFETIME_BOUND { return m_recordsThatTypedCodeTakes.span(); }
    ValueFlowObjectID objectAllocatedUnder(const Program&, ValueFlowObjectID site, ValueFlowObjectID prototype) const;
    ValueSetID constructorsAnsweringOnlyWhatTheyConstruct() const { return m_constructorsAnsweringOnlyWhatTheyConstruct; }
    ValueFlowPlaceholders placeholders() const { return { untrackedObject(), unknownObject(), m_objectsWithLayoutOfProgram, m_closures }; }
    ValueSetID maskOf(ValueSetMaker&, const Program&, const Type&) const;
    ValueSetID membersSurelyInside(ValueSetMaker&, const Program&, const Type&) const;
    ValueSetID membersSurelyInside(ValueSetMaker&, const Program&, TypeID) const;
    ValueSetID familyObjectsOf(ValueSetMaker&, const Program&, const Type&) const;
    bool namesProgramObjectsAlone(const Program&, const Type&) const;

    const ValueFlowBuiltinTables& builtinTables() const LIFETIME_BOUND { return *m_builtinTables; }

private:
    friend class ValueFlowBodies;
    friend class ValueFlowLinker;

    struct ModuleBuiltin {
        BuiltinID builtin;
        ModuleID module;

        friend constexpr auto operator<=>(const ModuleBuiltin&, const ModuleBuiltin&) = default;
    };

    void appendFunctionObjectsOf(const Program&, const Type&, Vector<ValueFlowObjectID, 32>& members, Vector<ValueSetID, 8>& parts) const;
    ValueSetID objectsSurelyWithLayoutIn(ValueSetMaker&, const Program&, LayoutSetID) const;
    ValueSetID objectsWithLayoutIn(ValueSetMaker&, const Program&, LayoutSetID) const;

    const BuiltinTable* m_builtins { nullptr };
    uint32_t m_firstBuiltin { 0 };
    uint32_t m_firstModuleBuiltin { 0 };
    uint32_t m_firstNamespace { 0 };
    uint32_t m_firstLayoutObject { 0 };
    uint32_t m_firstResultOfBuiltin { 0 };
    uint32_t m_firstClosure { 0 };
    uint32_t m_firstCoroutineResult { 0 };
    uint32_t m_firstRestArray { 0 };
    uint32_t m_firstFunctionPrototype { 0 };
    uint32_t m_firstConstructedObject { 0 };
    uint32_t m_firstSite { 0 };
    uint32_t m_firstBuiltinResultSite { 0 };
    uint32_t m_firstKey { 0 };
    uint32_t m_numberOfObjects { 0 };
    Vector<ModuleBuiltin> m_moduleBuiltins;
    Vector<std::pair<FunctionID, ModuleID>> m_modulesWithBodies;
    Vector<uint32_t> m_firstSiteOfFunction;
    Vector<ValueFlowSite> m_sites;
    Vector<ValueFlowObjectID> m_objectOfSite;
    Vector<uint32_t> m_siteOfObject;
    Vector<FunctionID> m_functionOfSiteObject;
    Vector<LayoutID> m_layoutOfNamespace;
    Vector<BuiltinClassID> m_classOfCoroutineResult;
    Vector<ValueSetID> m_objectsOfLayout;
    Vector<ValueSetID> m_objectsOfClass;
    Vector<ValueSetID> m_sureInstancesOfClass;
    Vector<ValueSetID> m_objectsThatMayBeMadeAs;
    Vector<ValueSetID> m_objectsMadeAs;
    ValueSetID m_objectsAllocatedAsIterators;
    ValueSetID m_objectsThatMayBeAnything;
    ValueSetID m_objectsWithAnyLayout;
    ValueSetID m_objectsWithLayoutOfProgram;
    ValueSetID m_objectsThatPickTheirLayout;
    ValueSetID m_constructedObjects;
    ValueSetID m_instancesOfClasses;
    BitVector m_classesNamedAboveSeveralClasses;
    ValueSetID m_constructorsWithLayouts;
    ValueSetID m_constructorsAnsweringOnlyWhatTheyConstruct;
    BitVector m_isLayoutOfTable;
    BitVector m_isAllocatedByProgram;
    Vector<LayoutID> m_recordsThatTypedCodeTakes;
    ValueSetID m_closures;
    ValueSetID m_builtinFunctions;
    ValueSetID m_builtinFunctionsOfProtocols;
    ValueSetID m_builtinFunctionsThatReadSpecies;
    ValueSetID m_builtinFunctionsThatActOnReceiver;
    ValueSetID m_builtinsThatMayBeAnyLibraryClosure;
    uint32_t m_numberOfRememberedLayoutSets { 0 };
    std::unique_ptr<std::atomic<uint32_t>[]> m_objectsSurelyWithLayoutIn;
    std::unique_ptr<std::atomic<uint32_t>[]> m_objectsWithLayoutIn;
    std::unique_ptr<std::atomic<uint32_t>[]> m_closuresCalledDirectlyBy;
    Vector<ModuleID> m_moduleOfRuntimeModule;
    std::unique_ptr<std::atomic<uint64_t>[]> m_boundFunctionsBindingNoArgument;
    uint32_t m_numberOfRememberedTypes { 0 };
    std::unique_ptr<std::atomic<uint32_t>[]> m_membersSurelyInsideType;
    BitVector m_isCanonicalEntryOfCallableBuiltin;
    HashMap<uint32_t, Vector<ValueFlowObjectID, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_builtinsThatMayBeClosureOf;
    ValueSetID m_otherCallables;
    ValueSetID m_strings;
    ValueSetID m_symbols;
    ValueSetID m_textKeys;
    ValueSetID m_symbolKeys;
    ValueSetID m_embeddedFilePaths;
    ValueSetID m_knownObjects;
    ValueSetID m_builtinObjects;
    ValueSetID m_namespaces;
    ValueSetID m_objectsOfRealm;
    ValueSetID m_lockedObjects;
    ValueSetID m_objectsWithLockedPrototypeLink;
    ValueSetID m_objectsBornWithoutPrototype;
    ValueSetID m_holdersOfLibraryExports;
    Vector<ValueSetID> m_holdersOfExportsOfModule;
    ValueSetID m_holdersOfLibraryExportsThatRaiseNoEvent;
    ValueSetID m_eventRaisingLibraryExportHolders;
    ValueSetID m_eventRaisingLibraryClosures;
    ValueSetID m_prototypeObjectsOfLibraryClasses;
    ValueSetID m_objectsOfLibraryFoundByNameAlone;
    ValueSetID m_instancesBelowLibraryClasses;
    ValueSetID m_instancesBelowBuiltinClasses;
    ValueSetID m_primitives;
    ValueSetID m_anyPrimitive;
    ValueSetID m_undefinedValue;
    ValueSetID m_objects;
    ValueSetID m_callables;
    ValueSetID m_arrays;
    ValueSetID m_records;
    ValueSetID m_keys;
    ValueSetID m_unknownObjectAlone;
    ValueSetID m_untrackedObjectAlone;
    ValueSetID m_placeholdersAlone;
    std::unique_ptr<ValueFlowBuiltinTables> m_builtinTables;
};

inline bool isWrittenInProgram(const Function& function)
{
    return function.role == FunctionRole::Source || function.role == FunctionRole::EntryChecks || function.role == FunctionRole::GenericVersion;
}

enum class ValueFlowHostNode : uint8_t {
    UsedBuiltins,
    WrittenBuiltins,
    RunBuiltins,
    ModulesOfSeenCode,
    FunctionsOfSeenCode,
    UnseenCodeRuns,
    UnknownCodeWrites,
    HostChoosesOperands,
    ModulesAreRequiredByName,
    ModulesAreImportedByName,
    RequiredValuesLoadedByName,
    NamespacesLoadedByName,
    ModuleObjectsHandedOut,
    CreatedNamespaces,
    HeldByNativeCode,
    LibraryModulesAreRequiredByName,
    LibraryValuesLoadedByName,
    ObjectNobodyKnowsIsWritten,
    NewTargetsFromOperands,
    UnseenCodeChangesRealm,
    ConstructorsAboveEscapedInstances,
    ObjectsAboveEscapedInstances,
    HoldersOfExportsInEscapedObjects,
    ValueNobodyKnowsIsCalled,
    BuiltinsThatStatementsName,
    PrototypeObjectsOfInstances,
    PairsOfEntries,
    MapsFilledAsRecordsSay,
    BuiltinsHeldWithoutBeingListed,
    UnseenCodeHasLoader,
    StoredUnderNamesOfMethodsCallingStartupLoads,
    RecordsMadeByParsers,
    BuiltinObjectsThatStatementsWrite,
    FunctionsOfHostTakenFromKnownHolders,
    NamespacesTiedByStatementsOfBodies,
    ChildrenOfModuleObjects,
    ParentsOfModuleObjects,
    PairsOfSteps,
    CodeOutsideImageIsLoaded,
    NamespacesCarriedOutAsValues,
    ValuesCarriedOut,
    CopiesOfObjectsNobodyKnows,
    ObjectsHoldingSymbolsUsedOnlyAsKeys,
    ClosuresThatStatementsMayRename,
    ClosuresThatStatementsName,
    ClosuresThatStatementsGiveDisplayName,
    ValuesUnderUnknownKeyInHeldHoldersOfExports,
    GettersUnderUnknownKeyInHeldHoldersOfExports,
    SettersUnderUnknownKeyInHeldHoldersOfExports,
    MembersUnderUnknownKeyInHeldPrototypeObjectsOfLibraryClasses,
    SourcesOfCopiesIntoUnknownObject,
    SourcesOfCopiesIntoUntrackedObject,
    ClosuresThatDefinitionsCompare,
    PrototypeObjectsThatWritesUnderUnknownKeysReach,
    NamesReadOffValuesNobodyKnows,
    NamesFoundBelowValuesNobodyKnows,
    NamesOfReadsThatHandTheirValueOn,
    LoadersAndEvaluatorsThatEvaluatedTextNames,
    LoadersAndEvaluatorsThatProgramNames,
    PrototypeObjectsOfLibraryClassesWhoseOwnKeysAreListed,
    AllElseIsAtRest,
    ProxiesWhoseHandlerIsFollowed,
    ProxiesThatAreOpen,
    ProxiesThatHandOnWhatComesFromBelow,
    ProxiesWithTrapsForWhatComesFromBelow,
    TargetsOfProxiesThatAreListed,
    LibraryModulesRequestedByProgram,
    LibraryModulesHandedOut,
    ObjectsNobodyFollowsThatDefinitionsName,
    StoredUnderSymbolsNobodyKnows,
    StoredByLibraryUnderSymbolsNobodyKnows,
    SymbolsThatHostLooksForMayBeInHand,
    HoldersOfLibraryExportsThatProgramWrites,
    HoldersOfClassesThatAppliedRowsName,
};

inline constexpr unsigned numberOfValueFlowHostNodes = 74;

class ValueFlowProgramNodes {
public:
    static constexpr ValueFlowNodeID escaped() { return ValueFlowNodeID(0u); }
    static constexpr ValueFlowNodeID lost() { return ValueFlowNodeID(1u); }
    ValueFlowNodeID variable(VariableID variable) const { return ValueFlowNodeID(m_firstVariable + variable.index()); }
    ValueFlowNodeID field(LayoutID layout, unsigned index) const { return ValueFlowNodeID(m_firstFieldOfLayout[layout.index()] + index); }
    ValueFlowNodeID environmentField(LayoutID layout, unsigned index) const { return field(layout, index); }
    ValueFlowNodeID privateField(LayoutID layout, unsigned index) const { return field(layout, index); }

    ValueFlowNodeID entryParameter(FunctionID function, unsigned index) const { return ValueFlowNodeID(m_firstNodeOfFunction[function.index()] + index); }
    ValueFlowNodeID capture(FunctionID function, unsigned index) const { return entryParameter(function, index); }
    ValueFlowNodeID result(FunctionID function, unsigned index) const { return ValueFlowNodeID(m_firstResultOfFunction[function.index()] + index); }
    ValueFlowNodeID thrown(FunctionID function) const { return ValueFlowNodeID(m_thrownOfFunction[function.index()]); }
    ValueFlowNodeID yielded(FunctionID function) const { return ValueFlowNodeID(m_thrownOfFunction[function.index()] + 1); }
    ValueFlowNodeID sent(FunctionID function) const { return ValueFlowNodeID(m_thrownOfFunction[function.index()] + 2); }
    ValueFlowNodeID ofHost(ValueFlowHostNode node) const { return ValueFlowNodeID(m_thrownOfFunction.last() + 1 + static_cast<uint32_t>(node)); }
    ValueFlowNodeID hub(ValueFlowBehaviorID behavior) const { return ValueFlowNodeID(m_thrownOfFunction.last() + 1 + numberOfValueFlowHostNodes + behavior.index()); }
    ValueFlowBehaviorID behaviorOf(ValueFlowNodeID hub) const { return ValueFlowBehaviorID(hub.index() - m_thrownOfFunction.last() - 1 - numberOfValueFlowHostNodes); }

    // What the unobserved definitions of an initializer define. It holds nothing until super() may call a constructor that the class does not name,
    // or, where the definitions ask for it, may answer an object that refuses a definition.
    struct UnobservedDefinitions {
        LayoutID classCallingSuper;
        FunctionID initializer;
        ValueFlowNodeID value;
        bool areObservedWhereObjectRefusesDefinition { false };
    };
    ValueFlowNodeID valueOf(const UnobservedFieldDefinition&) const;
    bool isValueOfUnobservedDefinitions(FunctionID initializer, ValueFlowNodeID) const;
    std::span<const UnobservedDefinitions> unobservedDefinitionsBehindSuperOf(LayoutID classCallingSuper) const LIFETIME_BOUND;

    struct PassedParameter {
        unsigned entryParameter { 0 };
        ValueSetID mask;
        ValueFlowMaskKind maskKind { ValueFlowMaskKind::Plain };
    };
    struct ResultSummary {
        ValueFlowNodeID ownResult;
        Vector<PassedParameter, 2> passedParameters;
    };
    const ResultSummary* resultSummaryOf(FunctionID function) const
    {
        const ResultSummary& summary = m_resultSummaries[function.index()];
        return summary.ownResult ? &summary : nullptr;
    }

    uint32_t numberOfNodes() const { return m_numberOfNodes; }
    uint32_t firstNodeOf(FunctionID function) const { return m_firstNodeOfFunction[function.index()]; }
    uint32_t endOfNodesOf(FunctionID function) const { return m_firstNodeOfFunction[function.index() + 1]; }
    FunctionID functionOf(ValueFlowNodeID) const;

private:
    friend class ValueFlowBodies;
    friend class ValueFlowLinker;

    uint32_t m_firstVariable { 2 };
    uint32_t m_numberOfNodes { 0 };
    Vector<uint32_t> m_firstFieldOfLayout;
    Vector<uint32_t> m_firstNodeOfFunction;
    Vector<uint32_t> m_firstResultOfFunction;
    Vector<uint32_t> m_thrownOfFunction;
    Vector<ResultSummary> m_resultSummaries;
    Vector<UnobservedDefinitions> m_unobservedDefinitionsByClass;
    Vector<UnobservedDefinitions> m_unobservedDefinitionsByInitializer;
};

struct ValueFlowNodeName {
    enum class Kind : uint8_t { None, OfBody, Escaped, Variable, Field, EntryParameter, Result, Thrown, OfHost, Hub };

    Kind kind { Kind::None };
    uint32_t owner { 0 };
    uint32_t index { 0 };

    static constexpr ValueFlowNodeName ofBody(uint32_t index) { return { Kind::OfBody, index, 0 }; }
    static constexpr ValueFlowNodeName variable(VariableID variable) { return { Kind::Variable, variable.index(), 0 }; }
    static constexpr ValueFlowNodeName field(LayoutID layout, uint32_t index) { return { Kind::Field, layout.index(), index }; }
    static constexpr ValueFlowNodeName entryParameter(FunctionID function, uint32_t index) { return { Kind::EntryParameter, function.index(), index }; }
    static constexpr ValueFlowNodeName result(FunctionID function, uint32_t index) { return { Kind::Result, function.index(), index }; }
    static constexpr ValueFlowNodeName thrown(FunctionID function) { return { Kind::Thrown, function.index(), 0 }; }
    static constexpr ValueFlowNodeName ofHost(ValueFlowHostNode node) { return { Kind::OfHost, static_cast<uint32_t>(node), 0 }; }
    static constexpr ValueFlowNodeName hub(ValueFlowBehaviorID behavior) { return { Kind::Hub, behavior.index(), 0 }; }
    static constexpr ValueFlowNodeName escaped() { return { Kind::Escaped, 0, 0 }; }

    constexpr explicit operator bool() const { return kind != Kind::None; }
    friend constexpr bool operator==(const ValueFlowNodeName&, const ValueFlowNodeName&) = default;
    friend constexpr auto operator<=>(const ValueFlowNodeName&, const ValueFlowNodeName&) = default;
};

// The table of built-ins says how many behaviors there are. The host nodes are counted here, and their enumeration has 8 bits.
static_assert(numberOfValueFlowHostNodes <= std::numeric_limits<std::underlying_type_t<ValueFlowHostNode>>::max());
static_assert(ValueFlowNodeName::hub(ValueFlowBehaviorID(100000u)).owner == 100000 && ValueFlowNodeName::hub(ValueFlowBehaviorID(0u)) != ValueFlowNodeName::ofHost(static_cast<ValueFlowHostNode>(0)));

enum class ValueFlowBodyNodeKind : uint8_t {
    EntryParameter,
    Result,
    Thrown,
    Yielded,
    Sent,
    Caught,
    OperationResult,
    Filter,
    Merge,
};

struct ValueFlowBody {
    static constexpr uint32_t none = UINT32_MAX;

    struct Constant {
        uint16_t primitives { 0 };
        Vector<ValueFlowObjectName, 1> objects;

        bool isEmpty() const { return !primitives && objects.isEmpty(); }
        friend bool operator==(const Constant&, const Constant&) = default;
    };

    struct Mask {
        Vector<TypeID, 1> inside;
        Vector<TypeID, 1> outside;
        ValueFlowMaskKind kind { ValueFlowMaskKind::Checked };
        Vector<std::pair<BuiltinID, ModuleID>, 0> outsideBuiltinsOfModules { };
        // What may be an instance of one of these, and what is not surely an instance of any of those.
        Vector<BuiltinClassID, 0> insideOneOfClasses { };
        Vector<BuiltinClassID, 0> outsideClasses { };

        bool admitsEverything() const { return inside.isEmpty() && outside.isEmpty() && outsideBuiltinsOfModules.isEmpty() && insideOneOfClasses.isEmpty() && outsideClasses.isEmpty(); }
        friend bool operator==(const Mask&, const Mask&) = default;
    };

    struct Node {
        ValueFlowBodyNodeKind kind { ValueFlowBodyNodeKind::Merge };
        ValueID value;
        uint32_t mask { 0 };
    };

    struct Edge {
        ValueFlowNodeName from;
        ValueFlowNodeName to;

        friend constexpr bool operator==(const Edge&, const Edge&) = default;
        friend constexpr auto operator<=>(const Edge&, const Edge&) = default;
    };

    struct Seed {
        ValueFlowNodeName node;
        uint32_t constant { 0 };
        uint32_t mask { 0 };

        friend constexpr bool operator==(const Seed&, const Seed&) = default;
        friend constexpr auto operator<=>(const Seed&, const Seed&) = default;
    };

    struct Operand {
        ValueFlowNodeName node;
        uint32_t constant { 0 };
        uint32_t mask { 0 };

        friend constexpr bool operator==(const Operand&, const Operand&) = default;
    };

    struct Statement {
        Opcode opcode { };
        ValueID site;
        uint32_t firstOperand { 0 };
        uint32_t operandCount { 0 };
        ValueFlowNodeName result;
        ValueFlowNodeName thrown;
        ValueFlowStatementKind kind { ValueFlowStatementKind::Operation };
        uint32_t firstSite { none };
        bool isOnlyCalledOnItsHolder { false };
        uint8_t guardCount { 0 };
        uint32_t firstGuard { 0 };
    };

    struct ObservedValue {
        ValueID test;
        Operand operand;
    };

    struct PassedParameter {
        uint32_t entryParameter { 0 };
        uint32_t mask { 0 };
    };

    FunctionID function;
    uint32_t numberOfEntryParameters { 0 };
    uint32_t numberOfResults { 0 };
    bool hasYieldedAndSent { false };
    uint32_t ownResult { none };
    std::array<uint32_t, 2> valueOfUnobservedDefinitions { none, none };
    Vector<PassedParameter> passedParameters;
    bool answersOnlyWhatItConstructs { false };
    Vector<Node> nodes;
    Vector<ValueFlowSite> sites;
    Vector<Constant> constants;
    Vector<Mask> masks;
    Vector<Edge> edges;
    Vector<Edge> edgesOnceReached;
    Vector<Seed> seedsOnceReached;
    Vector<FunctionID> functionsEnteredOnceReached;
    Vector<Statement> statements;
    uint32_t firstStatementLeavingBody { 0 };
    Vector<Operand> operands;
    Vector<ObservedValue> observedValues;
    struct ComparedValues {
        Operand left;
        Operand right;
    };
    Vector<ComparedValues> comparedValues;
    Vector<ValueID> valuesAliveForComparisonsOnly;
    // As many. Where such a value has a node of its own.
    Vector<ValueFlowNodeName> nodesOfValuesAliveForComparisonsOnly;
    BitVector valuesBehindChecks;
    bool knowsValuesBehindChecks { false };

    uint32_t result(uint32_t index) const { return numberOfEntryParameters + index; }
    uint32_t thrown() const { return numberOfEntryParameters + numberOfResults; }
    uint32_t yielded() const { return thrown() + 1; }
    uint32_t sent() const { return thrown() + 2; }
    std::span<const Operand> operandsOf(const Statement& statement) const LIFETIME_BOUND { return operands.subspan(statement.firstOperand, statement.operandCount); }
};

ValueFlowBody reduceBody(const Program&, const ValueFlowBuiltinTables&, FunctionID, const UnobservedFieldDefinitions* = nullptr);
ValueFlowBody makeHostBody(const Program&, const ValueFlowBuiltinTables&, std::span<const uint64_t> notedOperations = { });
bool startsWithoutValueInSolve(const Program&, const Variable&);

// In the order of the sites of a conversion to an object.
inline constexpr std::pair<uint16_t, NamedBuiltinClass> wrappersOfPrimitives[] {
    { Primitive::AnyString, BuiltinClasses::String },
    { Primitive::Number, BuiltinClasses::Number },
    { Primitive::Boolean, BuiltinClasses::Boolean },
    { Primitive::AnySymbol, BuiltinClasses::Symbol },
    { Primitive::BigInt, BuiltinClasses::BigInt },
};
bool isUsedOnlyAsInstanceOfOwnFunction(const Program&, FunctionID, ValueID thisValue);

struct ValueFlowObservedValue {
    FunctionID function;
    ValueID test;
    ValueFlowOperand operand;
};

struct ValueFlowComparedValues {
    FunctionID function;
    ValueFlowOperand left;
    ValueFlowOperand right;
};

struct ValueFlowValueOfFunction {
    FunctionID function;
    ValueID value;
    ValueFlowNodeID node;
};

struct ValueFlowBodiesTimes {
    Seconds reduce;
    Seconds number;
    Seconds makeMasks;
    Seconds link;
};

enum class ValueFlowNotedOperation : uint8_t {
    NewObject,
    NewClosure,
    CallDirect,
    DirectCall,
    Data,
    CreateIteratorResult,
    AllocationSite,
    PieceOutsideSlots,
    FinishObject,
    LoopHint,
    ClassDefinition,
    StoreToLocation,
    Builtin,
    ModuleOperation,
};

using ValueFlowNotedOperations = uint64_t;

constexpr ValueFlowNotedOperations notedOperations(std::initializer_list<ValueFlowNotedOperation> operations)
{
    ValueFlowNotedOperations bits = 0;
    for (ValueFlowNotedOperation operation : operations)
        bits |= 1ull << static_cast<unsigned>(operation);
    return bits;
}

constexpr ValueFlowNotedOperations notedOperationsOf(Opcode opcode)
{
    ValueFlowNotedOperations bits = 0;
    auto note = [&](ValueFlowNotedOperation operation, bool holds) {
        if (holds)
            bits |= notedOperations({ operation });
    };
    note(ValueFlowNotedOperation::NewObject, opcode == Opcode::NewObject);
    note(ValueFlowNotedOperation::NewClosure, opcode == Opcode::NewClosure);
    note(ValueFlowNotedOperation::CallDirect, opcode == Opcode::CallDirect);
    note(ValueFlowNotedOperation::DirectCall, accessClassOf(opcode) == AccessClass::DirectCall);
    note(ValueFlowNotedOperation::Data, opcode == Opcode::Data);
    note(ValueFlowNotedOperation::CreateIteratorResult, opcode == Opcode::CreateIteratorResult);
    note(ValueFlowNotedOperation::AllocationSite, PayloadWord::allocationSite(opcode).has_value());
    note(ValueFlowNotedOperation::PieceOutsideSlots, opcode == Opcode::DefineGetter || opcode == Opcode::DefineSetter || opcode == Opcode::ElemDefine || opcode == Opcode::CopyDataProperties);
    note(ValueFlowNotedOperation::FinishObject, opcode == Opcode::FinishObject);
    note(ValueFlowNotedOperation::LoopHint, opcode == Opcode::LoopHint);
    note(ValueFlowNotedOperation::ClassDefinition, opcode == Opcode::NewClass || opcode == Opcode::DefineMethod);
    note(ValueFlowNotedOperation::StoreToLocation, opcode == Opcode::PutModuleVariable || opcode == Opcode::PutEnvironmentField);
    note(ValueFlowNotedOperation::Builtin, opcode == Opcode::Builtin);
    note(ValueFlowNotedOperation::ModuleOperation, PayloadWord::module(opcode).has_value() || opcode == Opcode::ImportDynamic || opcode == Opcode::RequireDynamic || opcode == Opcode::RequireBuiltinModule);
    return bits;
}

ValueFlowNotedOperations notedOperationsOf(const Function&);

class ValueFlowBodies {
    WTF_MAKE_NONCOPYABLE(ValueFlowBodies);
public:
    ValueFlowBodies();
    ValueFlowBodies(ValueFlowBodies&&);
    ValueFlowBodies& operator=(ValueFlowBodies&&);
    ~ValueFlowBodies();

    ValueSets& sets() const { return *m_sets; }
    const ValueFlowUniverse& universe() const LIFETIME_BOUND { return m_universe; }
    const ValueFlowProgramNodes& nodes() const LIFETIME_BOUND { return m_nodes; }

    std::span<const ValueFlowEdge> edges() const LIFETIME_BOUND { return m_edges.span(); }
    std::span<const ValueFlowNodeMask> masks() const LIFETIME_BOUND { return m_masks.span(); }
    std::span<const ValueFlowStatement> statements() const LIFETIME_BOUND { return m_statements.span(); }
    std::span<const ValueFlowOperand> guardsOf(const ValueFlowStatement& statement) const LIFETIME_BOUND { return m_operands.span().subspan(statement.firstGuard, statement.guardCount); }
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement& statement) const LIFETIME_BOUND { return m_operands.span().subspan(statement.firstOperand, statement.operandCount); }
    std::span<const ValueFlowOperand> operands() const LIFETIME_BOUND { return m_operands.span(); }
    std::span<const ValueFlowEdge> edgesOnceReached() const LIFETIME_BOUND { return m_edgesOnceReached.span(); }
    std::span<const ValueFlowNodeSet> seedsOnceReached() const LIFETIME_BOUND { return m_seedsOnceReached.span(); }
    std::span<const FunctionID> functionsEnteredOnceReached() const LIFETIME_BOUND { return m_functionsEnteredOnceReached.span(); }
    std::span<const ValueFlowBodyRange> ranges() const LIFETIME_BOUND { return m_ranges.span(); }
    std::span<const ValueFlowObservedValue> observedValues() const LIFETIME_BOUND { return m_observedValues.span(); }
    std::span<const ValueFlowComparedValues> comparedValues() const LIFETIME_BOUND { return m_comparedValues.span(); }
    std::span<const ValueFlowValueOfFunction> valuesAliveForComparisonsOnly() const LIFETIME_BOUND { return m_valuesAliveForComparisonsOnly.span(); }
    const UnobservedFieldDefinitions& unobservedDefinitions() const LIFETIME_BOUND { return m_unobservedDefinitions; }
    const ValueFlowBodiesTimes& times() const { return m_times; }
    const BitVector* valuesBehindChecks(FunctionID function) const LIFETIME_BOUND { return m_knowsValuesBehindChecks[function.index()] ? &m_valuesBehindChecks[function.index()] : nullptr; }
    bool holdsAnyOf(FunctionID function, ValueFlowNotedOperations operations) const { return m_notedOperations[function.index()] & operations; }
    void addBytesTo(ValueFlowBytesByTable&) const;

    uint32_t numberOfBodies() const { return m_ranges.size() - 1; }
    uint32_t hostBody() const { return numberOfBodies() - 1; }
    std::span<const ValueFlowStatement> statementsOf(uint32_t body) const LIFETIME_BOUND { return m_statements.span().subspan(m_ranges[body].statement, m_ranges[body + 1].statement - m_ranges[body].statement); }
    std::span<const ValueFlowEdge> edgesOnceReached(uint32_t body) const LIFETIME_BOUND { return m_edgesOnceReached.subspan(m_ranges[body].edgeOnceReached, m_ranges[body + 1].edgeOnceReached - m_ranges[body].edgeOnceReached); }
    std::span<const ValueFlowNodeSet> seedsOnceReached(uint32_t body) const LIFETIME_BOUND { return m_seedsOnceReached.subspan(m_ranges[body].seedOnceReached, m_ranges[body + 1].seedOnceReached - m_ranges[body].seedOnceReached); }
    std::span<const FunctionID> functionsEnteredOnceReached(uint32_t body) const LIFETIME_BOUND { return m_functionsEnteredOnceReached.subspan(m_ranges[body].functionEntered, m_ranges[body + 1].functionEntered - m_ranges[body].functionEntered); }

private:
    friend class ValueFlowLinker;

    std::unique_ptr<ValueSets> m_sets;
    ValueFlowUniverse m_universe;
    ValueFlowProgramNodes m_nodes;
    Vector<ValueFlowEdge> m_edges;
    Vector<ValueFlowNodeMask> m_masks;
    MallocSpan<ValueFlowStatement> m_statements;
    MallocSpan<ValueFlowOperand> m_operands;
    Vector<ValueFlowEdge> m_edgesOnceReached;
    Vector<ValueFlowNodeSet> m_seedsOnceReached;
    Vector<FunctionID> m_functionsEnteredOnceReached;
    Vector<ValueFlowObservedValue> m_observedValues;
    Vector<ValueFlowComparedValues> m_comparedValues;
    Vector<ValueFlowValueOfFunction> m_valuesAliveForComparisonsOnly;
    UnobservedFieldDefinitions m_unobservedDefinitions;
    ValueFlowBodiesTimes m_times;
    Vector<ValueFlowNotedOperations> m_notedOperations;
    Vector<BitVector> m_valuesBehindChecks;
    Vector<uint8_t> m_knowsValuesBehindChecks;
    Vector<ValueFlowBodyRange> m_ranges;
};

// The values whose types as read may be owed to a check.
inline BitVector valuesTypedBehindChecks(const Program& program, const Function& function, const CFG& cfg, const BitVector* checksThatTestNothing = nullptr)
{
    return PredictedValues(function, cfg, { .programToReadComparisonsWith = &program, .checksToLeaveOut = checksThatTestNothing }).valuesBehindChecks();
}

struct ValueFlowInstancedFunction {
    FunctionID function;
    unsigned positionOfKey { 0 };
    Vector<ValueFlowEdge> edges;
    Vector<ValueFlowNodeSet> seeds;
};

class ValueFlowInstancedFunctions {
    WTF_MAKE_NONCOPYABLE(ValueFlowInstancedFunctions);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowInstancedFunctions);
public:
    static constexpr uint32_t noKey = UINT32_MAX;
    struct TextPassed {
        uint32_t positionOfArgument;
        StringID text;
    };
    static constexpr uint32_t mostArgumentsLookedAtForTexts = 64;
    static Vector<uint32_t> findParametersThatAreKeys(const Program&, const ParallelLoops&, Vector<TextPassed>& textsPassed, Vector<ValueFlowNotedOperations>& notedOperations);

    ValueFlowInstancedFunctions(const Program&, const ValueFlowBodies&, std::span<const uint32_t> positionOfKey, ValueSetMaker&, const ParallelLoops&);

    bool isEmpty() const { return m_functions.isEmpty(); }
    const ValueFlowInstancedFunction* find(FunctionID function) const { return function && function.index() < m_indexOfFunction.size() && m_indexOfFunction[function.index()] ? &m_functions[m_indexOfFunction[function.index()] - 1] : nullptr; }
    bool holdsNode(ValueFlowNodeID node) const { return node.index() < m_isNodeOfFunction.size() && m_isNodeOfFunction.quickGet(node.index()); }
    ValueSetID closures() const { return m_closures; }

    static constexpr ValueFlowObjectID markerOfOtherCalls() { return ValueFlowUniverse::primitive(PrimitiveSet::Undefined); }
    AOT_CLASS_THRESHOLD(unsigned, mostMarkersOfCall, 8);
    AOT_CLASS_THRESHOLD(unsigned, mostInstances, 64);
    AOT_CLASS_THRESHOLD(uint32_t, mostNodesOfBody, 1024);

private:
    Vector<ValueFlowInstancedFunction> m_functions;
    Vector<uint32_t> m_indexOfFunction;
    BitVector m_isNodeOfFunction;
    ValueSetID m_closures;
};

ValueFlowBodies reduceBodies(const Program&, BuildContext&, Vector<ValueFlowNotedOperations>&& = { });

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
