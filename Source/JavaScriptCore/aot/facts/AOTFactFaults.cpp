/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFactFaults.h"

#if ENABLE(AOT) && !ENABLE(AOT_FAULT_INJECTION)

#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

std::optional<uint32_t> numberOfMisstatedFact(StringView) { return std::nullopt; }
void listMisstatedFacts(PrintStream&) { }
void misstateSummaries(const Program&, ThinSummaries&, BuildContext&) { }
void misstateLinkTables(Program&, const ThinSummaries&, LinkTables&, LinkTables&, BuildContext&) { }
void misstateFactTables(Program&, LinkTables&, FactTables&, BuildContext&) { }
void misstatePlan(Program&, const Facts&, Plan&, BuildContext&) { }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && !ENABLE(AOT_FAULT_INJECTION)

#if ENABLE(AOT) && ENABLE(AOT_FAULT_INJECTION)

#include "AOTBuildContext.h"
#include "AOTCompileOptions.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTThinSummary.h"
#include "AOTTypeArena.h"
#include <wtf/DataLog.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_FACT_OF_FUNCTION(v) \
    v(FunctionCallersKnown, everyCallerIsKnown, true) \
    v(FunctionCallsHaveOneCallee, everyCallHasOneCallee, true) \
    v(FunctionResultUsersKnown, everyTakerOfResultKnowsCallee, true) \
    v(FunctionTailCallCyclesListed, ringsOfTailCallsAreListed, true) \
    v(FunctionRunsAtMostOnce, runsAtMostOnce, true) \
    v(FunctionUnreached, isReached, false) \
    v(FunctionNeverNamed, liesUnderNameOrIsLost, false) \
    v(FunctionClosuresUnchanged, closuresAreChanged, false) \
    v(FunctionClosureNamesListed, closuresGetOnlyNamesThatAreListed, true) \
    v(FunctionPrototypePropertyUnchanged, prototypePropertyStaysAsMade, true) \
    v(FunctionWithoutInt32Promise, holdsInt32Promise, false)

#define FOR_EACH_FACT_OF_PARAMETER(v) \
    v(ParameterInstancesUnchanged, staysAsItsClassMakesIt, true)

#define FOR_EACH_LATE_FACT_OF_PARAMETER(v) \
    v(ParameterOnlyCalled, isOnlyCalled, true) \
    v(ParameterNeverCaptured, isCapturedWhereOnlyCalled, false) \
    v(ParameterOnlyInspected, isOnlyLookedAt, true)

#define FOR_EACH_FACT_OF_FIELD(v) \
    v(FieldUnwrittenAfterConstruction, isWrittenAfterConstruction, false) \
    v(FieldFilled, isFilled, true) \
    v(FieldAlwaysFilled, isFilledAtEveryRead, true) \
    v(FieldFilledAtAllocation, isOfBirth, true) \
    v(FieldOnlyAccessors, isFilledByAccessorsAlone, true) \
    v(FieldInitialAccessor, holdsAccessorFromBirth, true) \
    v(FieldKeepsAccessor, keepsAccessorOnceLaid, true) \
    v(FieldNotLeftUndefined, isLeftUndefinedByMakers, false) \
    v(FieldNeverGivenNullish, isGivenUndefinedOrNullInSight, false) \
    v(FieldSetByAllocations, isGivenByNoMaker, false) \
    v(FieldHoldsEveryArrival, leavesOutWhatArrives, false) \
    v(FieldWithoutDescriptors, liesBesideWhatDescriptorsDefine, true)

#define FOR_EACH_FACT_OF_APPENDED_FIELD(v) \
    v(AppendedFieldFilled, isFilled, true) \
    v(AppendedFieldAlwaysFilled, isFilledAtEveryRead, true) \
    v(AppendedFieldFilledAtAllocation, isOfBirth, true) \
    v(AppendedFieldOnlyAccessors, isFilledByAccessorsAlone, true) \
    v(AppendedFieldInitialAccessor, holdsAccessorFromBirth, true) \
    v(AppendedFieldKeepsAccessor, keepsAccessorOnceLaid, true)

#define FOR_EACH_FACT_OF_VARIABLE(v) \
    v(VariableNeverStored, isStoredInto, false) \
    v(VariableInstancesUnchanged, staysAsItsClassMakesIt, true) \
    v(VariableFilled, isFilled, true) \
    v(VariableAlwaysFilled, isFilledAtEveryRead, true)

#define FOR_EACH_FACT_OF_LAYOUT(v) \
    v(LayoutNeverAllocated, isAllocated, false) \
    v(LayoutWithoutDirectInstances, hasDirectInstances, false) \
    v(LayoutStaysInCompiledCode, reachesUnseenCode, false) \
    v(LayoutInstancesUnlocked, hasLockableInstances, false) \
    v(LayoutDescriptorsDefineValuesOnly, isDefinedOnByDescriptorsThatLayValuesAlone, true) \
    v(LayoutListsStaticMembers, listsEveryStaticMember, true) \
    v(LayoutWithoutConstructorFunction, hasConstructorFunctionInRow, false) \
    v(LayoutWithoutComputedKeys, isFilledUnderComputedKeys, false) \
    v(LayoutInstancesAreNoPrototypes, instancesAreLaidAsPrototypes, false)

#define FOR_EACH_CHANGE_OF_LAYOUT(v) \
    v(LayoutNeverFrozen, IsFrozen) \
    v(LayoutGainsNoProperty, GainsProperty) \
    v(LayoutKeepsProperties, LosesProperty) \
    v(LayoutPrototypeUnchanged, ChangesPrototype) \
    v(LayoutGetsNoAccessor, GetsAccessor) \
    v(LayoutGetsNoObjectAbove, GetsObjectAbove)

#define FOR_EACH_TRAIT_OF_SUMMARY(v) \
    v(SummaryNeverSuspends, Suspends) \
    v(SummaryWithoutDirectEval, HasDirectEval) \
    v(SummaryWithoutInt32Promise, HoldsInt32Promise)

#define FOR_EACH_KIND_OF_CHANGE_IN_SUMMARY(v) \
    v(FreezeUnseen, Freezes) \
    v(DeleteUnseen, Deletes) \
    v(DescriptorUnseen, DefinesByDescriptor) \
    v(PrototypeChangeUnseen, ChangesPrototype) \
    v(PrototypeUseUnseen, BecomesPrototype) \
    v(CopyUnseen, CopiesInto)

#define FOR_EACH_LIST_OF_SUMMARY(v) \
    v(StoreToFunctionVariableUnseen, functionVariablesStoredInto) \
    v(ClassEvaluationUnseen, classesEvaluated) \
    v(InspectionUnseen, inspections) \
    v(NameReadUnseen, namesRead) \
    v(NameCalledUnseen, namesCalled) \
    v(NameStoredUnseen, namesStored) \
    v(EventNameUnseen, textsThatNameEvents) \
    v(ListStoreInLoopUnseen, constantListsStoredIntoInLoops) \
    v(FieldTestUnseen, keysOfTestedFields) \
    v(FunctionMentionUnseen, functionsNamed) \
    v(ModuleHandedOutUnseen, modulesHandedOut) \
    v(ModuleMentionUnseen, otherModulesNamed) \
    v(MemberCopyUnseen, copiesOfMembers) \
    v(FirstUseOfFunctionUnseen, firstUsesOfFunctions) \
    v(MethodOfClassUnseen, methodsOfClasses) \
    v(ClassInVariableUnseen, classesInVariables) \
    v(StoreOnPrototypeUnseen, storesOnPrototypes) \
    v(FirstUseOfVariableUnseen, firstUsesOfVariables) \
    v(WriteBeforeReturnUnseen, ownWritesOnWaysToReturn) \
    v(MemberUnseen, members)

#define FOR_EACH_SET_OF_LOOKUPS(v) \
    v(LookupNameUnread, namesRead, false) \
    v(LookupNameUncalled, namesCalled, false) \
    v(LookupNameUnspelled, namesSpelledByProgram, false) \
    v(LookupBuiltinUnread, builtinsRead, false) \
    v(LookupBuiltinUncalled, builtinsCalled, false) \
    v(LookupBuiltinNotLetGo, builtinsLetGo, false) \
    v(LookupBuiltinUnusedByFlow, builtinsUsedViaFlow, false) \
    v(BuiltinTakenForConstant, membersTakenForConstants, true) \
    v(BuiltinNotLost, lostBuiltins, false) \
    v(LostBuiltinNameUnread, namesReadThroughLostBuiltins, false) \
    v(UnseenNameUnwrittenByProgram, namesWrittenThroughUnseenByProgram, false) \
    v(UnseenNameUnwrittenByLibrary, namesWrittenThroughUnseenByLibrary, false)

#define FOR_EACH_FLAG_OF_LOOKUPS(v) \
    v(ProgramWritesNoUnknownName, writesUnderAnyNameThroughUnseenByProgram, false) \
    v(LostBuiltinPrototypesUnread, climbsFromLostBuiltins, false) \
    v(LostBuiltinsStayVisible, lostBuiltinsReachUnseen, false) \
    v(NoUnknownKeyRead, readsUnderKeysNobodyKnows, false) \
    v(NativeCodeHoldsNoObjects, nativeCodeMayHoldObjects, false)

#define FOR_EACH_SET_OF_FACT_TABLES(v) \
    v(FunctionNeverReached, functionsNeverReached, true, Functions) \
    v(FunctionRunsOnce, functionsTakenToRunOnce, true, Functions) \
    v(ParsersNeverMakeLayout, layoutsThatParsersMayMake, false, Layouts) \
    v(BuiltinMemberConstant, constantBuiltinMembers, true, Builtins) \
    v(BuiltinMemberLocked, builtinMembersLockedAtStartUp, true, Builtins) \
    v(BuiltinHolderKeysNeverListed, builtinHoldersWhoseKeysAreNeverListed, true, Builtins)

#define FOR_EACH_FLAG_OF_PROGRAM(v) \
    v(ProgramHasOneInstance, hasOneInstance, true) \
    v(ProgramRunsNoUnseenCode, unseenCodeCanRun, false) \
    v(ProgramKeepsMainModulePath, setsMainModulePath, false) \
    v(ProgramMatchesNoRegExps, matchesRegExps, false) \
    v(LegacyRegExpStaticsUnread, mayReadLegacyRegExpStatics, false)

#define FOR_EACH_FACT_OF_SLOT(v) \
    v(SlotFilledAtAllocation, isFilledAtBirth, true) \
    v(SlotFitsEveryStore, everyStoreFits, true) \
    v(SlotStable, isStable, true) \
    v(SlotNeverWithoutStorage, mayExistWithoutStorage, false) \
    v(SlotNotLeftOut, isLeftOutByMakers, false) \
    v(SlotHoldsNoAccessor, holdsAccessor, false) \
    v(SlotClosureCapturesNoEnvironment, deferredClosureCapturesEnvironment, false)

#define FOR_EACH_FACT_OF_PLANNED_CLASS(v) \
    v(ClassMembersLazy, createsMembersLazily, true) \
    v(ClassPrototypeFirst, createsPrototypeBeforeClassObject, true) \
    v(ClassGuardsHoldWhileClear, guardsHoldWhileClear, true) \
    v(PrototypeGuardsHoldWhileClear, guardsOfPrototypeSideHoldWhileClear, true)

#define FOR_EACH_SET_OF_SLOT_PLAN(v) \
    v(PlannedClosureNamesListed, functionsWhoseClosuresGetOnlyListedNames, true, Functions) \
    v(PlannedPrototypePropertyUnchanged, functionsWhosePrototypePropertyStaysAsMade, true, Functions) \
    v(PlannedLayoutNeverAllocated, layoutsNeverAllocated, true, Layouts) \
    v(PlannedLayoutWithoutInstances, layoutsWithoutDirectInstances, true, Layouts) \
    v(PlannedKeyWithoutMark, keysOfSlotsThatHoldMarks, false, Keys)

#define FOR_EACH_SET_OF_MODULE_PLAN(v) \
    v(ModuleCalledWhereRequired, modulesCalledWhereRequired, true, Modules) \
    v(ModuleEvaluatedByCodeOnly, modulesEvaluatedByCodeAlone, true, Modules) \
    v(ModuleBodyLeftOut, bodiesThatRowsLeaveOut, true, Functions)

#define FOR_EACH_SET_OF_PLAN(v) \
    v(ModuleWithoutValue, modulesWithValue, false, Modules) \
    v(ModuleImportsNeverMocked, modulesWhoseImportsAreNeverMocked, true, Modules) \
    v(ModuleBodyPassedOver, modulesWhoseBodyIsPassedOver, true, Modules) \
    v(LayoutWithoutClassRecord, layoutsWithClassRecord, false, Layouts) \
    v(FunctionHasClosureCell, functionsWithClosureCell, true, Functions) \
    v(FunctionWithoutClosureCell, functionsWithClosureCell, false, Functions) \
    v(VariableUnfilledAtRuntime, variablesThatRunTimeMayFill, false, Variables) \
    v(FieldNameWithoutFunction, namesOfFieldsWrittenAsOneFunction, false, Keys)

#define FOR_EACH_SET_OF_STACK_PLAN(v) \
    v(BodyStackCheckCovered, bodiesCoveredByCallers, true, Functions) \
    v(SlowBodyStackCheckCovered, slowBodiesCoveredByCallers, true, Functions) \
    v(CoroutineStackCheckCovered, coroutineBodiesCoveredByCallers, true, Functions)

#define FOR_EACH_MUTATION_OF_SHAPE(v) \
    v(ShapeKeepsFields, DeleteField) \
    v(ShapeNeverRedefined, RedefineProperty) \
    v(ShapeGainsNoProperty, AddProperty) \
    v(ShapeIntegrityUnchanged, ChangeIntegrity) \
    v(ShapePrototypeUnchanged, ChangePrototype) \
    v(ShapeBecomesNoPrototype, BecomePrototype) \
    v(ShapeStoresOneType, StoreOfOtherType) \
    v(ShapeNeverConstructedDirectly, ConstructDirectly)

#define FOR_EACH_OTHER_FACT(v) \
    v(EdgeUnseen) v(ArgumentUnseen) v(StoreUnseen) v(ReadUnseen) v(DefinitionUnseen) v(ElementStoreUnseen) v(CallUnseen) v(NewTargetUnseen) \
    v(UnknownValueUnseen) v(AllocationUnseen) v(ClosureUnseen) v(ChangeValueUnseen) v(OperandUnseen) \
    v(ConstructionKeyFilled) v(ConstructionKeyWithoutLookup) v(ConstructionUnseenFromOutside) \
    \
    v(FunctionResultType) v(FunctionResultTypeAsExpected) v(FunctionEffectsNone) v(FunctionEffectsWithoutWrites) \
    v(FunctionEffectsWithoutCalls) v(FunctionEffectsWithoutCollection) v(FunctionCycleNeverAwaits) v(LayoutPrototypeSettledEarly) \
    v(ParameterType) v(ParameterTypeAsExpected) \
    v(FieldType) v(FieldTypeAsExpected) v(AppendedFieldType) v(AppendedFieldTypeAsExpected) \
    v(VariableType) v(VariableTypeAsExpected) v(CaptureType) \
    v(FieldDefinedPlain) v(FieldFactsAsExpected) v(LayoutFactsAsExpected) v(LayoutPrototypeNamesListed) v(LayoutStaticNamesListed) \
    v(MemberConstant) v(MemberAsExpected) v(NameOnPrototypeUnlisted) v(NameOnFunctionUnlisted) \
    v(CallAsExpected) v(CallHasOtherCallee) v(CallOutsideTail) v(KeyAsExpected) v(KeyedReadType) v(KeyedReadTypeAsExpected) \
    v(ContentsType) v(ContentsAsExpected) v(ReceiverUnchanged) v(ReceiverNeverChanged) v(IterableUnchanged) v(ListWithoutHoles) v(PlainList) v(IterableWithoutHoles) v(ViewLengthOverstated) v(GetterProven) v(SetterProven) \
    v(GetterAlwaysLaid) v(CoroutineReturnType) v(ExportStaysIn) \
    v(FunctionUnderNameOfVariable) v(SymbolWithoutPreparation) v(LinkRunsNoUnseenCode) v(MemberKeysUnlisted) v(TagParameterMoved) \
    v(RecordNotFilledBySpread) \
    v(EntryParameterType) v(FieldNeverDeleted) v(FieldNeverRedefined) v(FieldFormPlain) v(FieldIsNoAccessor) v(FieldIsAccessor) \
    v(ClassEvaluatedOnce) \
    v(ClassMemberUnlisted) v(ClassMemberFunction) v(BuiltinUnused) \
    v(RegExpTextUnlisted) v(BuiltinLockedDespiteReplacement) v(BuiltinReplacementByTestUnlisted) \
    v(SlotProvenType) v(SlotType) v(SlotRawType) v(LayoutWithoutMark) v(FieldRawFromAllocation) v(FieldKeptFilled) v(PlannedVariableUndescribed) v(PlannedVariableFunction) \
    v(MemberCreatedByRecord) v(DeferredVariableUnmarked) v(PromotedStoreUnlisted) v(ModuleStepUnlisted) v(ModuleStartErrorNeverCaught) \
    v(ModuleStartOnlyReturns) v(ModuleStartBeginsEarly) v(ModuleLoadTimeFileUnlisted) v(NoRequireEvaluatesModule) v(KeyOrderSwapped) v(KeyOrderNoAllocationStops) \
    v(CoroutineNeverSuspends) v(CallEntryWithoutNewTarget) v(CallEntryBorrowsArguments) v(ParameterGuardType) v(MethodTableRowUnlisted) v(MethodTableRowUnguarded) v(MethodTableMethod) \
    v(MethodSelectorResult) v(TailCallsEnd)

#define FOR_EACH_MISSTATED_FACT(v3, v2, v4, v1) \
    FOR_EACH_FACT_OF_FUNCTION(v3) FOR_EACH_FACT_OF_PARAMETER(v3) FOR_EACH_LATE_FACT_OF_PARAMETER(v3) FOR_EACH_FACT_OF_FIELD(v3) FOR_EACH_FACT_OF_APPENDED_FIELD(v3) FOR_EACH_FACT_OF_VARIABLE(v3) \
    FOR_EACH_FACT_OF_LAYOUT(v3) FOR_EACH_CHANGE_OF_LAYOUT(v2) FOR_EACH_TRAIT_OF_SUMMARY(v2) FOR_EACH_KIND_OF_CHANGE_IN_SUMMARY(v2) FOR_EACH_LIST_OF_SUMMARY(v2) \
    FOR_EACH_SET_OF_LOOKUPS(v3) FOR_EACH_FLAG_OF_LOOKUPS(v3) FOR_EACH_SET_OF_FACT_TABLES(v4) \
    FOR_EACH_FLAG_OF_PROGRAM(v3) FOR_EACH_FACT_OF_SLOT(v3) FOR_EACH_FACT_OF_PLANNED_CLASS(v3) FOR_EACH_SET_OF_SLOT_PLAN(v4) FOR_EACH_SET_OF_MODULE_PLAN(v4) FOR_EACH_SET_OF_PLAN(v4) \
    FOR_EACH_SET_OF_STACK_PLAN(v4) FOR_EACH_MUTATION_OF_SHAPE(v2) FOR_EACH_OTHER_FACT(v1)

namespace {

enum class MisstatedFact : uint32_t {
    None,
    Count,
#define AOT_NAME_OF_FACT(name, ...) name,
    FOR_EACH_MISSTATED_FACT(AOT_NAME_OF_FACT, AOT_NAME_OF_FACT, AOT_NAME_OF_FACT, AOT_NAME_OF_FACT)
#undef AOT_NAME_OF_FACT
};

constexpr ASCIILiteral namesOfMisstatedFacts[] = {
    "None"_s,
    "Count"_s,
#define AOT_NAME_OF_FACT(name, ...) #name ""_s,
    FOR_EACH_MISSTATED_FACT(AOT_NAME_OF_FACT, AOT_NAME_OF_FACT, AOT_NAME_OF_FACT, AOT_NAME_OF_FACT)
#undef AOT_NAME_OF_FACT
};

enum class PlaceKind : uint8_t { Program, Functions, Layouts, Variables, Modules, Builtins, Keys, Classes };

struct BooleanFact {
    MisstatedFact fact;
    uint16_t offset;
    bool claimed;
};

struct RecordTable {
    std::byte* first { nullptr };
    size_t size { 0 };
    size_t stride { 0 };

    template<typename Record>
    static RecordTable of(std::span<Record> records) { return { std::bit_cast<std::byte*>(records.data()), records.size(), sizeof(Record) }; }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    bool& flagAt(size_t index, uint16_t offset) const { return *std::bit_cast<bool*>(first + index * stride + offset); }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
};

using IsDrawn = ScopedLambda<bool(PlaceKind, uint32_t)>;
using ForEachPlace = ScopedLambda<void(const IsDrawn&)>;

} // anonymous namespace

class FactMisstater {
public:
    FactMisstater(const Program&, BuildContext&);

    void misstateSummaries(ThinSummaries&);
    void misstateLinkTables(TypeArena&, const ThinSummaries&, LinkTables& linked, LinkTables& held);
    void misstateFactTables(TypeArena&, LinkTables& linked, FactTables&);
    void misstatePlan(TypeArena&, const Facts&, Plan&);

private:
    bool isAsked(MisstatedFact fact) const { return m_fact == fact || m_fact == MisstatedFact::Count; }
    void misstate(MisstatedFact, const ForEachPlace&);
    void misstateFlags(std::span<const BooleanFact>, RecordTable, RecordTable other, PlaceKind, const ScopedLambda<std::optional<uint32_t>(size_t)>& ownerOf);
    void misstateSet(MisstatedFact, BitVector&, bool claimed, PlaceKind);
    void misstateFlag(MisstatedFact, bool&, bool claimed);
    template<typename Record> void dropEntry(MisstatedFact, Vector<Record>&, PlaceKind, uint32_t (*ownerOf)(const Record&));
    template<typename T, unsigned number> void dropEntryOfSummary(SummaryList<T, number>&, size_t);
    void misstateType(MisstatedFact, TypeArena&, const ScopedLambda<void(const ScopedLambda<void(TypeID&, PlaceKind, uint32_t)>&)>& forEachType);
    void misstateTypesOfCells(TypeArena&, std::span<LinkedCell> linked, std::span<LinkedCell> held, MisstatedFact, MisstatedFact asExpected, PlaceKind, const ScopedLambda<std::optional<uint32_t>(size_t)>& ownerOf);
    void misstateEffects(LinkTables&, MisstatedFact, const ScopedLambda<void(Effects&)>& understate);
    size_t numberOf(PlaceKind) const;
    bool isOwn(PlaceKind, size_t index) const;
    Vector<TypeID, 8> typesWithoutOnePart(TypeArena&, TypeID) const;

    const Program& m_program;
    BuildContext& m_build;
    MisstatedFact m_fact;
    BitVector m_ownFunctions;
    BitVector m_ownLayouts;
    BitVector m_ownVariables;
    BitVector m_ownModules;
    BitVector m_builtinsInUse;
};

FactMisstater::FactMisstater(const Program& program, BuildContext& build)
    : m_program(program)
    , m_build(build)
    , m_fact(static_cast<MisstatedFact>(build.options().misstatedFact))
{
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (isProgramModuleOf(module.kind) && !module.flags.containsAny({ ModuleFlags::IsLibrary, ModuleFlags::IsBundlerHelper }))
            m_ownModules.set(index);
    }
    auto isInOwnModule = [&](ModuleID module) {
        return module && m_ownModules.get(module.index());
    };
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (function.hasBody() && isInOwnModule(function.module) && !isLibraryCode(program, function))
            m_ownFunctions.set(index);
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (isInOwnModule(program.layouts[index].module))
            m_ownLayouts.set(index);
    }
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        if (isInOwnModule(program.variables[index].module))
            m_ownVariables.set(index);
    }
}

size_t FactMisstater::numberOf(PlaceKind kind) const
{
    switch (kind) {
    case PlaceKind::Program:
        return 1;
    case PlaceKind::Functions:
        return m_program.functions.size();
    case PlaceKind::Layouts:
        return m_program.layouts.size();
    case PlaceKind::Variables:
        return m_program.variables.size();
    case PlaceKind::Modules:
        return m_program.modules.size();
    case PlaceKind::Builtins:
        return m_program.builtins().builtinCount();
    case PlaceKind::Keys:
        return m_program.types.numberOfPropertyKeys();
    case PlaceKind::Classes:
        return m_program.builtins().classCount();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FactMisstater::isOwn(PlaceKind kind, size_t index) const
{
    switch (kind) {
    case PlaceKind::Functions:
        return m_ownFunctions.get(index);
    case PlaceKind::Layouts:
        return m_ownLayouts.get(index);
    case PlaceKind::Variables:
        return m_ownVariables.get(index);
    case PlaceKind::Modules:
        return m_ownModules.get(index);
    case PlaceKind::Builtins:
        return m_builtinsInUse.get(index);
    case PlaceKind::Program:
    case PlaceKind::Keys:
    case PlaceKind::Classes:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FactMisstater::misstate(MisstatedFact fact, const ForEachPlace& forEachPlace)
{
    if (!isAsked(fact))
        return;
    uint32_t numberOfPlaces = 0;
    forEachPlace(scopedLambda<bool(PlaceKind, uint32_t)>([&](PlaceKind, uint32_t) {
        ++numberOfPlaces;
        return false;
    }));
    ASCIILiteral name = namesOfMisstatedFacts[static_cast<size_t>(fact)];
    if (m_fact == MisstatedFact::Count) {
        if (numberOfPlaces)
            dataLogLn("misstated fact ", name, ": ", numberOfPlaces);
        return;
    }
    if (!numberOfPlaces)
        return;
    uint32_t seed = m_build.options().shuffleSeed;
    uint32_t drawn = seed >> 31 ? seed & ~(1u << 31) : seed ? drawBySeed(seed, DrawnFor::MisstatedFactPosition, static_cast<uint32_t>(fact), numberOfPlaces) % numberOfPlaces : numberOfPlaces;
    if (!seed)
        dataLogLn("misstated fact ", name, ": all of ", numberOfPlaces);
    uint32_t place = 0;
    forEachPlace(scopedLambda<bool(PlaceKind, uint32_t)>([&](PlaceKind kind, uint32_t subject) {
        uint32_t current = place++;
        if (seed && current != drawn)
            return false;
        if (seed)
            dataLogLn("misstated fact ", name, ": place ", drawn, " of ", numberOfPlaces, ", kind ", static_cast<unsigned>(kind), ", ", subject);
        return true;
    }));
}

void FactMisstater::misstateFlags(std::span<const BooleanFact> facts, RecordTable table, RecordTable other, PlaceKind kind, const ScopedLambda<std::optional<uint32_t>(size_t)>& ownerOf)
{
    for (const BooleanFact& fact : facts) {
        misstate(fact.fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
            for (size_t index = 0; index < table.size; ++index) {
                std::optional<uint32_t> owner = ownerOf(index);
                bool isClaimed = table.flagAt(index, fact.offset) == fact.claimed && (index >= other.size || other.flagAt(index, fact.offset) == fact.claimed);
                if (!owner || isClaimed || !isDrawn(kind, *owner))
                    continue;
                table.flagAt(index, fact.offset) = fact.claimed;
                if (index < other.size)
                    other.flagAt(index, fact.offset) = fact.claimed;
            }
        }));
    }
}

void FactMisstater::misstateSet(MisstatedFact fact, BitVector& set, bool claimed, PlaceKind kind)
{
    misstate(fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
        for (size_t index = 0, size = numberOf(kind); index < size; ++index) {
            if (isOwn(kind, index) && set.get(index) != claimed && isDrawn(kind, static_cast<uint32_t>(index)))
                set.set(index, claimed);
        }
    }));
}

void FactMisstater::misstateFlag(MisstatedFact fact, bool& flag, bool claimed)
{
    misstate(fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
        if (flag != claimed && isDrawn(PlaceKind::Program, 0))
            flag = claimed;
    }));
}

template<typename Record>
void FactMisstater::dropEntry(MisstatedFact fact, Vector<Record>& list, PlaceKind kind, uint32_t (*ownerOf)(const Record&))
{
    misstate(fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
        list.removeAllMatching([&](const Record& entry) {
            return isOwn(kind, ownerOf(entry)) && isDrawn(kind, ownerOf(entry));
        });
    }));
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
template<typename T, unsigned number>
void FactMisstater::dropEntryOfSummary(SummaryList<T, number>& list, size_t index)
{
    static_assert(number >= SummaryStorage::numberOfListsInRecord);
    std::span<T> entries = list.mutableSpan();
    std::ranges::move(entries.subspan(index + 1), entries.subspan(index).begin());

    SummaryStorage& storage = list.m_storage;
    constexpr uint64_t bit = SummaryStorage::bitOf<number>();
    constexpr bool isKeptForWalk = !!(bit & SummaryStorage::listsKeptForWalk);
    constexpr uint64_t listsOfBlock = isKeptForWalk ? SummaryStorage::listsKeptForWalk : ~SummaryStorage::listsKeptForWalk;
    auto* directory = SummaryStorage::pointerInto<SummaryStorage::Entry>(isKeptForWalk ? storage.blockKeptForWalk : storage.listsInRecord[0], isKeptForWalk ? 0 : storage.offsetOfDirectory);
    size_t place = std::popcount(storage.listsInDirectories & listsOfBlock & (bit - 1));
    if (--directory[place].size)
        return;
    size_t size = std::popcount(storage.listsInDirectories & listsOfBlock);
    for (; place + 1 < size; ++place)
        directory[place] = directory[place + 1];
    storage.listsInDirectories &= ~bit;
}
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

Vector<TypeID, 8> FactMisstater::typesWithoutOnePart(TypeArena& types, TypeID id) const
{
    Vector<TypeID, 8> narrower;
    if (!id || isNeverType(id))
        return narrower;
    Type type = types.at(id);
    if (!type.isJavaScriptValue())
        return narrower;
    auto add = [&](TypeID candidate) {
        if (candidate && candidate != id && !isNeverType(candidate))
            narrower.append(candidate);
    };
    for (uint16_t part : { Primitive::Undefined, Primitive::Null, Primitive::Boolean, Primitive::Integer, Primitive::Double, Primitive::AnyString, Primitive::AnySymbol, Primitive::BigInt, Primitive::Absent }) {
        if (type.primitives & part)
            add(types.withoutPrimitives(id, part));
    }
    if (type.primitives & Primitive::Integer) {
        if (!type.integers.isSubsetOf(Range::int32()))
            add(types.withIntegers(id, Range::int32().intersectWith(type.integers)));
        else if (type.integers.minimum < 0 && type.integers.maximum >= 0)
            add(types.withIntegers(id, Range { 0, type.integers.maximum }));
    }
    if (type.hasObjects())
        add(types.withoutObjects(id));
    if (type.unnamedLayouts && !holdsOneMember(type.unnamedLayouts)) {
        Vector<LayoutID, 8> layouts;
        layouts.append(types.listOf(type.unnamedLayouts));
        for (LayoutID layout : layouts)
            add(types.differenceOf(id, types.forLayout(layout)));
    }
    if (type.objects) {
        Vector<ObjectID, 8> objects;
        types.at(type.objects).forEach([&](ObjectID object) {
            objects.append(object);
        });
        for (size_t index = 0; objects.size() > 1 && index < objects.size(); ++index)
            add(types.differenceOf(id, types.forObject(objects[index])));
    }
    return narrower;
}

void FactMisstater::misstateType(MisstatedFact fact, TypeArena& types, const ScopedLambda<void(const ScopedLambda<void(TypeID&, PlaceKind, uint32_t)>&)>& forEachType)
{
    misstate(fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
        forEachType(scopedLambda<void(TypeID&, PlaceKind, uint32_t)>([&](TypeID& type, PlaceKind kind, uint32_t subject) {
            if (!isOwn(kind, subject))
                return;
            for (TypeID narrower : typesWithoutOnePart(types, type)) {
                if (!isDrawn(kind, subject))
                    continue;
                type = narrower;
                if (!m_build.options().shuffleSeed)
                    return;
            }
        }));
    }));
}

void FactMisstater::misstateTypesOfCells(TypeArena& types, std::span<LinkedCell> linked, std::span<LinkedCell> held, MisstatedFact fact, MisstatedFact asExpected, PlaceKind kind, const ScopedLambda<std::optional<uint32_t>(size_t)>& ownerOf)
{
    misstateType(fact, types, scopedLambda<void(const ScopedLambda<void(TypeID&, PlaceKind, uint32_t)>&)>([&](const ScopedLambda<void(TypeID&, PlaceKind, uint32_t)>& functor) {
        for (size_t index = 0; index < linked.size(); ++index) {
            std::optional<uint32_t> owner = ownerOf(index);
            if (owner && linked[index].everyWriterIsKnown && linked[index].proven)
                functor(linked[index].proven, kind, *owner);
        }
    }));
    misstate(asExpected, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < linked.size() && linked.size() == held.size(); ++index) {
            std::optional<uint32_t> owner = ownerOf(index);
            if (!owner || !held[index].everyWriterIsKnown || !held[index].proven || isNeverType(held[index].proven))
                continue;
            bool isProven = linked[index].everyWriterIsKnown && linked[index].proven;
            if (isProven && types.isSubset(linked[index].proven, held[index].proven))
                continue;
            if (!isDrawn(kind, *owner))
                continue;
            linked[index].proven = held[index].proven;
            linked[index].everyWriterIsKnown = true;
        }
    }));
}

void FactMisstater::misstateEffects(LinkTables& tables, MisstatedFact fact, const ScopedLambda<void(Effects&)>& understate)
{
    misstate(fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
        for (size_t index : m_ownFunctions) {
            LinkedFunction& function = tables.functions[index];
            if (!function.isReached)
                continue;
            Effects onReturn = function.effectsOnReturn();
            Effects onThrow = function.effectsOnThrow();
            understate(onReturn);
            understate(onThrow);
            if ((onReturn == function.effectsOnReturn() && onThrow == function.effectsOnThrow()) || !isDrawn(PlaceKind::Functions, static_cast<uint32_t>(index)))
                continue;
            Vector<Effects> kept;
            kept.append(onReturn);
            kept.append(onThrow);
            tables.known.blocksOfDistinctEffects.append(WTF::move(kept));
            function.keptEffectsOnReturn = &tables.known.blocksOfDistinctEffects.last()[0];
            function.keptEffectsOnThrow = &tables.known.blocksOfDistinctEffects.last()[1];
            for (LinkedEffectsGivenArguments& given : tables.known.givenArguments) {
                if (given.function.index() != index)
                    continue;
                understate(given.onReturn);
                understate(given.onThrow);
                if (given.onReturn.writes.isEmpty())
                    given.layoutsWritten = 0;
            }
            if (onReturn.writes.isEmpty() && onThrow.writes.isEmpty() && index < tables.known.layoutsWrittenByFunction.size())
                tables.known.layoutsWrittenByFunction[index] = 0;
        }
    }));
}

void FactMisstater::misstateSummaries(ThinSummaries& summaries)
{
    using Fact = MisstatedFact;
    auto primitiveConstantOf = [](const ThinSummary& summary) {
        for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
            const SummaryNode& node = summary.nodes[index];
            if (node.kind == SummaryNode::Kind::Constant && holdsOnlyPrimitives(TypeID::fromBits(node.first)))
                return index;
        }
        return noSummaryNode;
    };
    auto forEachSummary = [&](Fact fact, const auto& functor) {
        misstate(fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
            for (size_t index : m_ownFunctions) {
                ThinSummary& summary = summaries.at(FunctionID(static_cast<uint32_t>(index)));
                auto isDrawnHere = [&] {
                    return isDrawn(PlaceKind::Functions, static_cast<uint32_t>(index));
                };
                functor(summary, isDrawnHere);
            }
        }));
    };
    auto redirect = [&](const ThinSummary& summary, uint32_t& node, const auto& isDrawn) {
        uint32_t constant = node == noSummaryNode ? noSummaryNode : primitiveConstantOf(summary);
        if (constant != noSummaryNode && constant != node && isDrawn())
            node = constant;
    };

    forEachSummary(Fact::EdgeUnseen, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.edges.size(); ++index) {
            SummaryEdge& edge = summary.edges[index];
            if (edge.from != edge.to && isDrawn())
                edge.to = edge.from;
        }
    });
    forEachSummary(Fact::ArgumentUnseen, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.arguments.size(); ++index)
            redirect(summary, summary.arguments[index], isDrawn);
    });
    auto accessUnseen = [&](Fact fact, SummaryAccess::Kind kind) {
        forEachSummary(fact, [&](ThinSummary& summary, const auto& isDrawn) {
            for (size_t index = 0; index < summary.accesses.size(); ++index) {
                SummaryAccess& access = summary.accesses[index];
                if (access.kind == kind && !access.isToContents() && isDrawn())
                    access.kind = SummaryAccess::Kind::Has;
            }
        });
    };
    accessUnseen(Fact::StoreUnseen, SummaryAccess::Kind::Store);
    accessUnseen(Fact::ReadUnseen, SummaryAccess::Kind::Read);
    accessUnseen(Fact::DefinitionUnseen, SummaryAccess::Kind::Define);
    forEachSummary(Fact::ElementStoreUnseen, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.accesses.size(); ++index) {
            SummaryAccess& access = summary.accesses[index];
            if (access.kind == SummaryAccess::Kind::Store && access.isToContents())
                redirect(summary, access.value, isDrawn);
        }
    });
    forEachSummary(Fact::CallUnseen, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.calls.size(); ++index) {
            if (summary.calls[index].callee == SummaryCall::Callee::Value)
                redirect(summary, summary.calls[index].subject, isDrawn);
        }
    });
    forEachSummary(Fact::NewTargetUnseen, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.calls.size(); ++index)
            redirect(summary, summary.calls[index].newTarget, isDrawn);
    });
    forEachSummary(Fact::ChangeValueUnseen, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.changes.size(); ++index)
            redirect(summary, summary.changes[index].other, isDrawn);
    });
    forEachSummary(Fact::OperandUnseen, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.operators.size(); ++index) {
            for (uint32_t& operand : summary.operators[index].operands)
                redirect(summary, operand, isDrawn);
        }
    });
    auto nodeUnseen = [&](Fact fact, SummaryNode::Kind kind) {
        forEachSummary(fact, [&](ThinSummary& summary, const auto& isDrawn) {
            for (size_t index = 0; index < summary.nodes.size(); ++index) {
                if (summary.nodes[index].kind == kind && isDrawn())
                    summary.nodes[index] = SummaryNode { SummaryNode::Kind::Constant, TypeID(TypeTable::undefined).bits(), 0 };
            }
        });
    };
    nodeUnseen(Fact::UnknownValueUnseen, SummaryNode::Kind::Unknown);
    nodeUnseen(Fact::AllocationUnseen, SummaryNode::Kind::Allocation);
    nodeUnseen(Fact::ClosureUnseen, SummaryNode::Kind::Closure);

#define AOT_CHANGE_UNSEEN(name, changeKind) \
    forEachSummary(Fact::name, [&](ThinSummary& summary, const auto& isDrawn) { \
        for (size_t index = 0; index < summary.changes.size(); ++index) { \
            if (summary.changes[index].kind == SummaryChange::Kind::changeKind) \
                redirect(summary, summary.changes[index].subject, isDrawn); \
        } \
    });
    FOR_EACH_KIND_OF_CHANGE_IN_SUMMARY(AOT_CHANGE_UNSEEN)
#undef AOT_CHANGE_UNSEEN

#define AOT_TRAIT_UNSEEN(name, trait) \
    forEachSummary(Fact::name, [&](ThinSummary& summary, const auto& isDrawn) { \
        if (summary.traits.contains(SummaryTrait::trait) && isDrawn()) \
            summary.traits.remove(SummaryTrait::trait); \
    });
    FOR_EACH_TRAIT_OF_SUMMARY(AOT_TRAIT_UNSEEN)
#undef AOT_TRAIT_UNSEEN

#define AOT_ENTRY_UNSEEN(name, list) \
    forEachSummary(Fact::name, [&](ThinSummary& summary, const auto& isDrawn) { \
        for (size_t index = 0; index < summary.list.size();) { \
            if (isDrawn()) \
                dropEntryOfSummary(summary.list, index); \
            else \
                ++index; \
        } \
    });
    FOR_EACH_LIST_OF_SUMMARY(AOT_ENTRY_UNSEEN)
#undef AOT_ENTRY_UNSEEN

    forEachSummary(Fact::ConstructionKeyFilled, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.keysOfConstructions.size(); ++index) {
            if (!summary.keysOfConstructions[index].isFilled && isDrawn())
                summary.keysOfConstructions[index].isFilled = true;
        }
    });
    forEachSummary(Fact::ConstructionKeyWithoutLookup, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.keysOfConstructions.size(); ++index) {
            if (summary.keysOfConstructions[index].isLookedUp && isDrawn())
                summary.keysOfConstructions[index].isLookedUp = false;
        }
    });
    forEachSummary(Fact::ConstructionUnseenFromOutside, [&](ThinSummary& summary, const auto& isDrawn) {
        for (size_t index = 0; index < summary.constructions.size(); ++index) {
            if (summary.constructions[index].isSeenFromOutside && isDrawn())
                summary.constructions[index].isSeenFromOutside = false;
        }
    });
}

void FactMisstater::misstateLinkTables(TypeArena& types, const ThinSummaries& summaries, LinkTables& linked, LinkTables& held)
{
    using Fact = MisstatedFact;
    auto drawn = [&](Fact fact, const auto& functor) {
        misstate(fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
            functor(isDrawn);
        }));
    };
    auto isOwnFunction = [&](FunctionID function) {
        return function && m_ownFunctions.get(function.index());
    };

    Vector<uint32_t> functionOfParameter;
    functionOfParameter.fill(UINT32_MAX, linked.parameters.size());
    for (size_t index : m_ownFunctions) {
        const LinkedFunction& function = linked.functions[index];
        for (uint32_t parameter = 0; function.isReached && parameter < function.numberOfParameters; ++parameter)
            functionOfParameter[function.firstParameter + parameter] = static_cast<uint32_t>(index);
    }
    Vector<uint32_t> functionOfCapture;
    functionOfCapture.fill(UINT32_MAX, linked.captures.size());
    Vector<uint32_t> layoutOfField;
    layoutOfField.fill(UINT32_MAX, linked.fields.size());
    Vector<uint32_t> layoutOfAppendedField;
    layoutOfAppendedField.fill(UINT32_MAX, linked.appendedFields.size());
    auto noteOwners = [](Vector<uint32_t>& owners, const Vector<uint32_t>& first, size_t owner) {
        if (owner + 1 >= first.size())
            return;
        for (uint32_t place = first[owner]; place < first[owner + 1]; ++place)
            owners[place] = static_cast<uint32_t>(owner);
    };
    for (size_t index : m_ownFunctions) {
        if (linked.functions[index].isReached)
            noteOwners(functionOfCapture, linked.firstCaptureOfFunction, index);
    }
    for (size_t index : m_ownLayouts) {
        if (index >= linked.layouts.size() || !linked.layouts[index].isAllocated)
            continue;
        noteOwners(layoutOfField, linked.firstFieldOfLayout, index);
        noteOwners(layoutOfAppendedField, linked.firstAppendedFieldOfLayout, index);
    }
    auto ownerIn = [](const Vector<uint32_t>& owners) {
        return [&owners](size_t index) -> std::optional<uint32_t> {
            return index < owners.size() && owners[index] != UINT32_MAX ? std::optional(owners[index]) : std::nullopt;
        };
    };
    auto findFunctionOfParameter = ownerIn(functionOfParameter);
    auto findFunctionOfCapture = ownerIn(functionOfCapture);
    auto findLayoutOfField = ownerIn(layoutOfField);
    auto findLayoutOfAppendedField = ownerIn(layoutOfAppendedField);
    auto findOwnFunction = [&](size_t index) -> std::optional<uint32_t> {
        return m_ownFunctions.get(index) && linked.functions[index].isReached ? std::optional(static_cast<uint32_t>(index)) : std::nullopt;
    };
    auto findOwnVariable = [&](size_t index) -> std::optional<uint32_t> {
        return m_ownVariables.get(index) ? std::optional(static_cast<uint32_t>(index)) : std::nullopt;
    };
    auto findOwnLayout = [&](size_t index) -> std::optional<uint32_t> {
        return m_ownLayouts.get(index) && linked.layouts[index].isAllocated ? std::optional(static_cast<uint32_t>(index)) : std::nullopt;
    };
    using OwnerOf = std::optional<uint32_t>(size_t);
    auto ownerOfParameter = scopedLambda<OwnerOf>(findFunctionOfParameter);
    auto ownerOfCapture = scopedLambda<OwnerOf>(findFunctionOfCapture);
    auto ownerOfField = scopedLambda<OwnerOf>(findLayoutOfField);
    auto ownerOfAppendedField = scopedLambda<OwnerOf>(findLayoutOfAppendedField);
    auto ownFunction = scopedLambda<OwnerOf>(findOwnFunction);
    auto ownVariable = scopedLambda<OwnerOf>(findOwnVariable);
    auto ownLayout = scopedLambda<OwnerOf>(findOwnLayout);

#define AOT_FLAG_OF_RECORD(name, member, claimed) { Fact::name, __builtin_offsetof(Record, member), claimed },
    {
        using Record = LinkedFunction;
        static constexpr BooleanFact facts[] = { FOR_EACH_FACT_OF_FUNCTION(AOT_FLAG_OF_RECORD) };
        misstateFlags(facts, RecordTable::of(linked.functions.mutableSpan()), RecordTable::of(held.functions.mutableSpan()), PlaceKind::Functions, ownFunction);
    }
    {
        using Record = LinkedParameter;
        static constexpr BooleanFact facts[] = { FOR_EACH_FACT_OF_PARAMETER(AOT_FLAG_OF_RECORD) };
        misstateFlags(facts, RecordTable::of(linked.parameters.mutableSpan()), RecordTable::of(held.parameters.mutableSpan()), PlaceKind::Functions, ownerOfParameter);
    }
    {
        using Record = LinkedCell;
        static constexpr BooleanFact factsOfFields[] = { FOR_EACH_FACT_OF_FIELD(AOT_FLAG_OF_RECORD) };
        static constexpr BooleanFact factsOfAppendedFields[] = { FOR_EACH_FACT_OF_APPENDED_FIELD(AOT_FLAG_OF_RECORD) };
        static constexpr BooleanFact factsOfVariables[] = { FOR_EACH_FACT_OF_VARIABLE(AOT_FLAG_OF_RECORD) };
        misstateFlags(factsOfFields, RecordTable::of(linked.fields.mutableSpan()), RecordTable::of(held.fields.mutableSpan()), PlaceKind::Layouts, ownerOfField);
        misstateFlags(factsOfAppendedFields, RecordTable::of(linked.appendedFields.mutableSpan()), { }, PlaceKind::Layouts, ownerOfAppendedField);
        misstateFlags(factsOfVariables, RecordTable::of(linked.variables.mutableSpan()), RecordTable::of(held.variables.mutableSpan()), PlaceKind::Variables, ownVariable);
    }
    {
        using Record = LinkedLayout;
        static constexpr BooleanFact facts[] = { FOR_EACH_FACT_OF_LAYOUT(AOT_FLAG_OF_RECORD) };
        misstateFlags(facts, RecordTable::of(linked.layouts.mutableSpan()), RecordTable::of(held.layouts.mutableSpan()), PlaceKind::Layouts, ownLayout);
    }

    misstateTypesOfCells(types, linked.fields.mutableSpan(), held.fields.mutableSpan(), Fact::FieldType, Fact::FieldTypeAsExpected, PlaceKind::Layouts, ownerOfField);
    misstateTypesOfCells(types, linked.appendedFields.mutableSpan(), held.appendedFields.mutableSpan(), Fact::AppendedFieldType, Fact::AppendedFieldTypeAsExpected, PlaceKind::Layouts, ownerOfAppendedField);
    misstateTypesOfCells(types, linked.variables.mutableSpan(), held.variables.mutableSpan(), Fact::VariableType, Fact::VariableTypeAsExpected, PlaceKind::Variables, ownVariable);

    using TypeFunctor = ScopedLambda<void(TypeID&, PlaceKind, uint32_t)>;
    auto typesIn = [&](Fact fact, const auto& forEachType) {
        misstateType(fact, types, scopedLambda<void(const TypeFunctor&)>([&](const TypeFunctor& functor) {
            forEachType(functor);
        }));
    };
    typesIn(Fact::CaptureType, [&](const TypeFunctor& functor) {
        for (size_t index = 0; index < linked.captures.size(); ++index) {
            std::optional<uint32_t> owner = ownerOfCapture(index);
            if (owner && linked.captures[index].everyWriterIsKnown && linked.captures[index].proven)
                functor(linked.captures[index].proven, PlaceKind::Functions, *owner);
        }
    });
    auto resultsOf = [&](LinkTables& tables) {
        return [&](const TypeFunctor& functor) {
            for (size_t index : m_ownFunctions) {
                if (tables.functions[index].isReached)
                    functor(tables.functions[index].result, PlaceKind::Functions, static_cast<uint32_t>(index));
            }
        };
    };
    typesIn(Fact::FunctionResultType, resultsOf(linked));
    drawn(Fact::FunctionResultTypeAsExpected, [&](const IsDrawn& isDrawn) {
        for (size_t index : m_ownFunctions) {
            TypeID expected = held.functions[index].result;
            TypeID proven = linked.functions[index].result;
            if (linked.functions[index].isReached && expected && !isNeverType(expected) && (!proven || !types.isSubset(proven, expected)) && isDrawn(PlaceKind::Functions, static_cast<uint32_t>(index)))
                linked.functions[index].result = expected;
        }
    });
    auto parametersOf = [&](LinkTables& tables) {
        return [&](const TypeFunctor& functor) {
            for (size_t index = 0; index < tables.parameters.size(); ++index) {
                if (std::optional<uint32_t> owner = ownerOfParameter(index))
                    functor(tables.parameters[index].proven, PlaceKind::Functions, *owner);
            }
        };
    };
    typesIn(Fact::ParameterType, parametersOf(linked));
    drawn(Fact::ParameterTypeAsExpected, [&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < linked.parameters.size(); ++index) {
            std::optional<uint32_t> owner = ownerOfParameter(index);
            TypeID expected = held.parameters[index].proven;
            TypeID proven = linked.parameters[index].proven;
            if (!owner || !expected || isNeverType(expected) || (proven && types.isSubset(proven, expected)) || !isDrawn(PlaceKind::Functions, *owner))
                continue;
            linked.parameters[index].proven = expected;
            linked.functions[*owner].everyCallerIsKnown = true;
        }
    });

    misstateEffects(linked, Fact::FunctionEffectsNone, scopedLambda<void(Effects&)>([](Effects& effects) {
        effects = Effects::none();
    }));
    misstateEffects(linked, Fact::FunctionEffectsWithoutWrites, scopedLambda<void(Effects&)>([](Effects& effects) {
        effects.writes = { };
    }));
    misstateEffects(linked, Fact::FunctionEffectsWithoutCalls, scopedLambda<void(Effects&)>([](Effects& effects) {
        effects.mayCallUnknownCode = false;
    }));
    misstateEffects(linked, Fact::FunctionEffectsWithoutCollection, scopedLambda<void(Effects&)>([](Effects& effects) {
        effects.mayCollect = false;
    }));

#define AOT_CHANGE_OF_LAYOUT(name, change) \
    drawn(Fact::name, [&](const IsDrawn& isDrawn) { \
        for (size_t index : m_ownLayouts) { \
            if (!linked.layouts[index].isAllocated || !linked.layouts[index].possibleChanges.contains(LayoutChange::change) || !isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index))) \
                continue; \
            linked.layouts[index].possibleChanges.remove(LayoutChange::change); \
            held.layouts[index].possibleChanges.remove(LayoutChange::change); \
        } \
    });
    FOR_EACH_CHANGE_OF_LAYOUT(AOT_CHANGE_OF_LAYOUT)
#undef AOT_CHANGE_OF_LAYOUT

    auto namesAreListed = [&](Fact fact, size_t side) {
        drawn(fact, [&, side](const IsDrawn& isDrawn) {
            for (size_t index : m_ownLayouts) {
                if (!linked.layouts[index].isAllocated || !linked.layouts[index].namesAreDefinedPastLists[side] || !isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index)))
                    continue;
                linked.layouts[index].namesAreDefinedPastLists[side] = false;
                held.layouts[index].namesAreDefinedPastLists[side] = false;
            }
        });
    };
    namesAreListed(Fact::LayoutPrototypeNamesListed, 0);
    namesAreListed(Fact::LayoutStaticNamesListed, 1);
    drawn(Fact::LayoutFactsAsExpected, [&](const IsDrawn& isDrawn) {
        for (size_t index : m_ownLayouts) {
            LinkedLayout& layout = linked.layouts[index];
            const LinkedLayout& expected = held.layouts[index];
            if (!layout.isAllocated || (layout.reachesUnseenCode == expected.reachesUnseenCode && layout.possibleChanges == expected.possibleChanges && layout.hasLockableInstances == expected.hasLockableInstances) || !isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index)))
                continue;
            layout.reachesUnseenCode = expected.reachesUnseenCode;
            layout.possibleChanges = expected.possibleChanges;
            layout.hasLockableInstances = expected.hasLockableInstances;
        }
    });
    drawn(Fact::FieldDefinedPlain, [&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < linked.fields.size(); ++index) {
            std::optional<uint32_t> owner = ownerOfField(index);
            if (owner && !linked.fields[index].formThatDefinitionsCreate.isEmpty() && isDrawn(PlaceKind::Layouts, *owner))
                linked.fields[index].formThatDefinitionsCreate = { };
        }
    });
    drawn(Fact::FieldFactsAsExpected, [&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < linked.fields.size() && linked.fields.size() == held.fields.size(); ++index) {
            std::optional<uint32_t> owner = ownerOfField(index);
            if (owner && linked.fields[index] != held.fields[index] && isDrawn(PlaceKind::Layouts, *owner))
                linked.fields[index] = held.fields[index];
        }
    });

    auto memberConstant = [&](Fact fact, Vector<LinkedMember>& members) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (LinkedMember& member : members) {
                if (member.holdsOneFunction() || !member.written || !isOwnFunction(member.written) || !isDrawn(PlaceKind::Layouts, member.classLayout.index()))
                    continue;
                member.everyWriterIsKnown = true;
                member.function = member.written;
            }
        });
    };
    memberConstant(Fact::MemberConstant, linked.members);
    drawn(Fact::MemberAsExpected, [&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < linked.members.size() && linked.members.size() == held.members.size(); ++index) {
            LinkedMember& member = linked.members[index];
            const LinkedMember& expected = held.members[index];
            if (member.classLayout != expected.classLayout || member.key != expected.key || (member.everyWriterIsKnown == expected.everyWriterIsKnown && member.function == expected.function) || !isDrawn(PlaceKind::Layouts, member.classLayout.index()))
                continue;
            member = expected;
        }
    });
    dropEntry<LinkedNameOnPrototype>(Fact::NameOnPrototypeUnlisted, linked.namesOnPrototypes, PlaceKind::Layouts, [](const LinkedNameOnPrototype& name) { return name.layout.index(); });
    dropEntry<LinkedNameOnFunction>(Fact::NameOnFunctionUnlisted, linked.namesOnFunctions, PlaceKind::Functions, [](const LinkedNameOnFunction& name) { return name.holder.index(); });

    auto functionOf = [](const auto& record) {
        return record.function;
    };
    auto addExpectedEntries = [&](Fact fact, auto& proven, const auto& expected, const auto& isSamePlace, const auto& isBefore, const auto& functionOf) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            size_t size = proven.size();
            for (const auto& entry : expected) {
                bool isListed = std::ranges::any_of(proven.span().first(size), [&](const auto& listed) {
                    return isSamePlace(listed, entry);
                });
                if (!isListed && isOwnFunction(functionOf(entry)) && isDrawn(PlaceKind::Functions, functionOf(entry).index()))
                    proven.append(entry);
            }
            if (proven.size() != size)
                std::ranges::sort(proven, isBefore);
        });
    };
    auto isSameSite = [](const auto& first, const auto& second) {
        return first.function == second.function && first.site == second.site;
    };
    auto isEarlierSite = [](const auto& first, const auto& second) {
        return LinkTables::placeOfSite(first.function, first.site) < LinkTables::placeOfSite(second.function, second.site);
    };
    auto isSameCall = [](const auto& first, const auto& second) {
        return first.caller == second.caller && first.site == second.site;
    };
    auto isEarlierCall = [](const auto& first, const auto& second) {
        return LinkTables::placeOfSite(first.caller, first.site) < LinkTables::placeOfSite(second.caller, second.site);
    };
    auto callerOf = [](const auto& record) {
        return record.caller;
    };
    auto numberCalls = [&] {
        if (linked.firstCallOfCaller.isEmpty())
            return;
        std::ranges::fill(linked.firstCallOfCaller, 0);
        for (const LinkedCall& call : linked.calls)
            ++linked.firstCallOfCaller[call.caller.index() + 1];
        for (size_t index = 1; index < linked.firstCallOfCaller.size(); ++index)
            linked.firstCallOfCaller[index] += linked.firstCallOfCaller[index - 1];
    };
    addExpectedEntries(Fact::CallAsExpected, linked.calls, held.calls, isSameCall, isEarlierCall, callerOf);
    if (m_fact == Fact::CallAsExpected)
        numberCalls();
    drawn(Fact::CallHasOtherCallee, [&](const IsDrawn& isDrawn) {
        for (LinkedCall& call : linked.calls) {
            if (!isOwnFunction(call.caller) || !isOwnFunction(call.callee))
                continue;
            const Function& callee = m_program.at(call.callee);
            for (size_t other : m_ownFunctions) {
                const Function& function = m_program.functions[other];
                bool isAlike = other != call.callee.index() && linked.functions[other].isReached && function.kind == callee.kind && function.parameters.size() == callee.parameters.size() && function.captures.size() == callee.captures.size() && function.flags == callee.flags;
                if (!isAlike || !isDrawn(PlaceKind::Functions, call.caller.index()))
                    continue;
                call.callee = FunctionID(static_cast<uint32_t>(other));
                break;
            }
        }
    });
    drawn(Fact::CallOutsideTail, [&](const IsDrawn& isDrawn) {
        for (LinkedCall& call : linked.calls) {
            if (isOwnFunction(call.caller) && call.isInTailPosition && isDrawn(PlaceKind::Functions, call.caller.index()))
                call.isInTailPosition = false;
        }
    });
    addExpectedEntries(Fact::KeyAsExpected, linked.accessesUnderOneKey, held.accessesUnderOneKey, isSameSite, isEarlierSite, functionOf);
    addExpectedEntries(Fact::KeyedReadTypeAsExpected, linked.resultsOfReadsByKey, held.resultsOfReadsByKey, isSameSite, isEarlierSite, functionOf);
    addExpectedEntries(Fact::ContentsAsExpected, linked.contentsAtOperations, held.contentsAtOperations, isSameSite, isEarlierSite, functionOf);
    typesIn(Fact::KeyedReadType, [&](const TypeFunctor& functor) {
        for (LinkedResultOfRead& read : linked.resultsOfReadsByKey)
            functor(read.proven, PlaceKind::Functions, read.function.index());
    });
    typesIn(Fact::ContentsType, [&](const TypeFunctor& functor) {
        for (LinkedContentsAtOperation& contents : linked.contentsAtOperations) {
            functor(contents.key, PlaceKind::Functions, contents.function.index());
            functor(contents.value, PlaceKind::Functions, contents.function.index());
        }
    });
    auto addSites = [&](Fact fact, Vector<LinkedReceiverAsMade>& proven) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            size_t size = proven.size();
            for (size_t index : m_ownFunctions) {
                FunctionID function(static_cast<uint32_t>(index));
                if (!linked.functions[index].isReached)
                    continue;
                auto addSite = [&](ValueID site) {
                    LinkedReceiverAsMade entry { function, site };
                    if (site && !std::ranges::binary_search(proven.span().first(size), entry) && isDrawn(PlaceKind::Functions, function.index()))
                        proven.append(entry);
                };
                for (const SummaryCall& call : summaries.at(function).calls) {
                    if (call.callee != SummaryCall::Callee::Binding)
                        addSite(call.site);
                }
                for (const SummaryAccess& access : summaries.at(function).accesses)
                    addSite(access.site);
            }
            std::ranges::sort(proven);
        });
    };
    addSites(Fact::ReceiverUnchanged, linked.receiversThatStayAsMade);
    addSites(Fact::ReceiverNeverChanged, linked.receiversThatNobodyChanges);
    addSites(Fact::IterableUnchanged, linked.iterablesThatStayAsMade);
    addSites(Fact::ListWithoutHoles, linked.listsWithoutHoles);
    addSites(Fact::PlainList, linked.plainListsRead);
    addSites(Fact::IterableWithoutHoles, linked.iterablesWithoutHoles);
    drawn(Fact::ViewLengthOverstated, [&](const IsDrawn& isDrawn) {
        for (LinkedReadOffViews& read : linked.readsOffViewsOfWrittenLengths) {
            if (isOwnFunction(read.function) && isDrawn(PlaceKind::Functions, read.function.index()))
                ++read.leastLength;
        }
    });
    auto accessorFlag = [&](Fact fact, bool LinkedAccessorsOfField::*flag, bool claimed) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (LinkTables* tables : { &linked, &held }) {
                for (LinkedAccessorsOfField& accessors : tables->accessorsExpectedInFields) {
                    if (m_ownLayouts.get(accessors.field.layout.index()) && accessors.*flag != claimed && isDrawn(PlaceKind::Layouts, accessors.field.layout.index()))
                        accessors.*flag = claimed;
                }
            }
        });
    };
    accessorFlag(Fact::GetterProven, &LinkedAccessorsOfField::getterIsProven, true);
    accessorFlag(Fact::SetterProven, &LinkedAccessorsOfField::setterIsProven, true);
    accessorFlag(Fact::GetterAlwaysLaid, &LinkedAccessorsOfField::getterMayNotBeLaidYet, false);
    typesIn(Fact::CoroutineReturnType, [&](const TypeFunctor& functor) {
        for (LinkedCoroutine& coroutine : linked.coroutines)
            functor(coroutine.returned, PlaceKind::Functions, coroutine.function.index());
    });
    misstateSet(Fact::ExportStaysIn, linked.exportsThatStayIn, true, PlaceKind::Variables);
    dropEntry<LinkedFunctionUnderName>(Fact::FunctionUnderNameOfVariable, linked.functionsLaidUnderNamesOfVariables, PlaceKind::Variables, [](const LinkedFunctionUnderName& laid) { return laid.variable; });
    dropEntry<PropertyKeyID>(Fact::SymbolWithoutPreparation, linked.symbolsWithPreparation, PlaceKind::Keys, [](const PropertyKeyID& key) { return key.index(); });
    misstateFlag(Fact::LinkRunsNoUnseenCode, linked.unseenCodeCanRun, false);
    misstateSet(Fact::MemberKeysUnlisted, linked.keysOfMembers[0], false, PlaceKind::Keys);
    drawn(Fact::TagParameterMoved, [&](const IsDrawn& isDrawn) {
        for (TaggedMaker& allocator : linked.taggedMakers) {
            if (isDrawn(PlaceKind::Functions, allocator.constructor.index()))
                ++allocator.tagParameter;
        }
    });
    drawn(Fact::RecordNotFilledBySpread, [&](const IsDrawn& isDrawn) {
        for (size_t index : m_ownLayouts) {
            if ((linked.recordsThatSpreadsFill.get(index) || held.recordsThatSpreadsFill.get(index)) && isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index))) {
                linked.recordsThatSpreadsFill.clear(index);
                held.recordsThatSpreadsFill.clear(index);
            }
        }
    });

    LookupsPastProgramObjects& lookups = linked.lookups;
    for (const BitVector* builtins : { &lookups.builtinsRead, &lookups.builtinsCalled, &lookups.builtinsConstructed, &lookups.builtinsLetGo, &lookups.builtinsUsedViaFlow, &lookups.builtinsCalledOutOfSight, &lookups.membersTakenForConstants, &lookups.membersWritten, &lookups.lostBuiltins, &lookups.builtinsAtUnseen })
        m_builtinsInUse.merge(*builtins);
#define AOT_SET_OF_LOOKUPS(name, set, claimed) \
    misstateSet(Fact::name, lookups.set, claimed, std::string_view(#set).starts_with("names") ? PlaceKind::Keys : std::string_view(#set).starts_with("classes") || std::string_view(#set).starts_with("staticallyInstantiated") ? PlaceKind::Classes : PlaceKind::Builtins);
    FOR_EACH_SET_OF_LOOKUPS(AOT_SET_OF_LOOKUPS)
#undef AOT_SET_OF_LOOKUPS
#define AOT_FLAG_OF_LOOKUPS(name, flag, claimed) misstateFlag(Fact::name, lookups.flag, claimed);
    FOR_EACH_FLAG_OF_LOOKUPS(AOT_FLAG_OF_LOOKUPS)
#undef AOT_FLAG_OF_LOOKUPS
}

void FactMisstater::misstateFactTables(TypeArena& types, LinkTables& linked, FactTables& tables)
{
    using Fact = MisstatedFact;
    {
        using Record = LinkedParameter;
        static constexpr BooleanFact facts[] = { FOR_EACH_LATE_FACT_OF_PARAMETER(AOT_FLAG_OF_RECORD) };
        Vector<uint32_t> functionOfParameter;
        functionOfParameter.fill(UINT32_MAX, linked.parameters.size());
        for (size_t index : m_ownFunctions) {
            const LinkedFunction& function = linked.functions[index];
            for (uint32_t parameter = 0; function.isReached && parameter < function.numberOfParameters; ++parameter)
                functionOfParameter[function.firstParameter + parameter] = static_cast<uint32_t>(index);
        }
        misstateFlags(facts, RecordTable::of(linked.parameters.mutableSpan()), { }, PlaceKind::Functions, [&](size_t index) -> std::optional<uint32_t> {
            return functionOfParameter[index] != UINT32_MAX ? std::optional(functionOfParameter[index]) : std::nullopt;
        });
    }
    auto drawn = [&](Fact fact, const auto& functor) {
        misstate(fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
            functor(isDrawn);
        }));
    };

    for (const BuiltinUseRecord& use : tables.builtinUses)
        m_builtinsInUse.set(use.builtin);
    for (const BuiltinLeftOpen& open : tables.builtinMembersLeftOpen)
        m_builtinsInUse.set(open.member.index());
    m_builtinsInUse.merge(linked.lookups.membersWritten);
    m_builtinsInUse.merge(linked.lookups.builtinsCalled);
    m_builtinsInUse.merge(linked.lookups.builtinsRead);

#define AOT_SET_OF_FACT_TABLES(name, set, claimed, kind) misstateSet(Fact::name, tables.set, claimed, PlaceKind::kind);
    FOR_EACH_SET_OF_FACT_TABLES(AOT_SET_OF_FACT_TABLES)
#undef AOT_SET_OF_FACT_TABLES
#define AOT_FLAG_OF_PROGRAM(name, flag, claimed) misstateFlag(Fact::name, tables.program.flag, claimed);
    FOR_EACH_FLAG_OF_PROGRAM(AOT_FLAG_OF_PROGRAM)
#undef AOT_FLAG_OF_PROGRAM

    using TypeFunctor = ScopedLambda<void(TypeID&, PlaceKind, uint32_t)>;
    misstateType(Fact::EntryParameterType, types, scopedLambda<void(const TypeFunctor&)>([&](const TypeFunctor& functor) {
        for (size_t index : m_ownFunctions) {
            if (index >= tables.entryParameters.numberOfOwners() || !linked.functions[index].isReached)
                continue;
            for (uint32_t row = tables.entryParameters.firstRowOfOwner[index]; row < tables.entryParameters.firstRowOfOwner[index + 1]; ++row)
                functor(tables.entryParameters.rows[row].proven, PlaceKind::Functions, static_cast<uint32_t>(index));
        }
    }));
    drawn(Fact::FunctionCycleNeverAwaits, [&](const IsDrawn& isDrawn) {
        for (size_t index : m_ownFunctions) {
            uint32_t cycle = linked.functions[index].cycle;
            if (cycle + 1 >= linked.firstCycleAwaited.size() || linked.firstCycleAwaited[cycle] == linked.firstCycleAwaited[cycle + 1] || !isDrawn(PlaceKind::Functions, static_cast<uint32_t>(index)))
                continue;
            linked.cyclesAwaited.removeAt(linked.firstCycleAwaited[cycle]);
            for (size_t later = cycle + 1; later < linked.firstCycleAwaited.size(); ++later)
                --linked.firstCycleAwaited[later];
        }
    });
    drawn(Fact::LayoutPrototypeSettledEarly, [&](const IsDrawn& isDrawn) {
        for (size_t index : m_ownLayouts) {
            if (linked.layouts[index].isAllocated && !linked.layouts[index].prototypeObjectIsSettledAheadOfEveryObject && isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index)))
                linked.layouts[index].prototypeObjectIsSettledAheadOfEveryObject = true;
        }
    });
    auto fields = [&](Fact fact, const auto& functor) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (size_t layout : m_ownLayouts) {
                if (layout + 1 >= tables.firstFieldOfLayout.size() || !linked.layouts[layout].isAllocated)
                    continue;
                for (uint32_t field = tables.firstFieldOfLayout[layout]; field < tables.firstFieldOfLayout[layout + 1]; ++field) {
                    functor(tables.fields[field], [&] {
                        return isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(layout));
                    });
                }
            }
        });
    };
    fields(Fact::FieldNeverDeleted, [](FieldFacts& field, const auto& isDrawn) {
        if (!field.stability.contains(FieldStability::IsNeverDeleted) && isDrawn())
            field.stability.add(FieldStability::IsNeverDeleted);
    });
    fields(Fact::FieldNeverRedefined, [](FieldFacts& field, const auto& isDrawn) {
        if (!field.stability.contains(FieldStability::IsNeverRedefined) && isDrawn())
            field.stability.add(FieldStability::IsNeverRedefined);
    });
    fields(Fact::FieldFormPlain, [](FieldFacts& field, const auto& isDrawn) {
        if (!field.formOfValue.isEmpty() && isDrawn())
            field.formOfValue = { };
    });
    auto fieldFact = [&](Fact fact, FieldFact bit, bool claimed) {
        fields(fact, [&](FieldFacts& field, const auto& isDrawn) {
            if (field.facts.contains(bit) != claimed && isDrawn())
                field.facts.set(bit, claimed);
        });
    };
    fieldFact(Fact::FieldIsNoAccessor, FieldFact::IsAccessor, false);
    fieldFact(Fact::FieldIsAccessor, FieldFact::IsAccessor, true);
    auto classes = [&](Fact fact, const auto& functor) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (size_t index : m_ownLayouts) {
                if (index < tables.classes.size() && m_program.layouts[index].kind == LayoutKind::Instance && m_program.layouts[index].constructor) {
                    functor(LayoutID(static_cast<uint32_t>(index)), tables.classes[index], [&] {
                        return isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index));
                    });
                }
            }
        });
    };
    classes(Fact::ClassEvaluatedOnce, [](LayoutID, ClassFacts& facts, const auto& isDrawn) {
        if (facts.evaluations == ExecutionCount::Unknown && isDrawn())
            facts.evaluations = ExecutionCount::AtMostOnce;
    });
    auto members = [&](Fact fact, const auto& functor) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (size_t index : m_ownLayouts) {
                if (index >= tables.membersOfClass.numberOfOwners())
                    continue;
                for (uint32_t row = tables.membersOfClass.firstRowOfOwner[index]; row < tables.membersOfClass.firstRowOfOwner[index + 1]; ++row) {
                    functor(tables.membersOfClass.rows[row], [&] {
                        return isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index));
                    });
                }
            }
        });
    };
    members(Fact::ClassMemberUnlisted, [](ClassMember& member, const auto& isDrawn) {
        if (member.isAssignedByProgram && member.function && isDrawn()) {
            member.function = { };
            member.isAssignedByProgram = false;
        }
    });
    members(Fact::ClassMemberFunction, [&](ClassMember& member, const auto& isDrawn) {
        for (size_t other : m_ownFunctions) {
            FunctionID function(static_cast<uint32_t>(other));
            if (member.function && function != member.function && m_program.at(function).kind == m_program.at(member.function).kind && m_program.at(function).parameters.size() == m_program.at(member.function).parameters.size() && isDrawn()) {
                member.function = function;
                break;
            }
        }
    });
    dropEntry<BuiltinUseRecord>(Fact::BuiltinUnused, tables.builtinUses, PlaceKind::Builtins, [](const BuiltinUseRecord& use) { return use.builtin; });
    dropEntry<RegExpTextInSight>(Fact::RegExpTextUnlisted, tables.program.regExpTextsInSight, PlaceKind::Program, [](const RegExpTextInSight&) { return 0u; });
    drawn(Fact::BuiltinLockedDespiteReplacement, [&](const IsDrawn& isDrawn) {
        for (const BuiltinLeftOpen& open : tables.builtinMembersLeftOpen) {
            if (open.reason == ReasonBuiltinStaysOpen::IsWrittenUnderItsName && isDrawn(PlaceKind::Builtins, open.member.index())) {
                tables.constantBuiltinMembers.set(open.member.index());
                tables.builtinMembersLockedAtStartUp.set(open.member.index());
            }
        }
    });
    dropEntry<BuiltinID>(Fact::BuiltinReplacementByTestUnlisted, tables.builtinMembersThatTestsReplace, PlaceKind::Builtins, [](const BuiltinID& builtin) { return builtin.index(); });
}

void FactMisstater::misstatePlan(TypeArena& types, const Facts& facts, Plan& plan)
{
    using Fact = MisstatedFact;
    auto drawn = [&](Fact fact, const auto& functor) {
        misstate(fact, scopedLambda<void(const IsDrawn&)>([&](const IsDrawn& isDrawn) {
            functor(isDrawn);
        }));
    };
    const LinkTables& linked = facts.linked();
    PlanParts& parts = plan.m_parts;
    SlotPlanParts& slots = parts.slots.m_parts;

    auto isOwnLayoutInUse = [&](size_t index) {
        return m_ownLayouts.get(index) && slots.layouts[index].hasSlots() && !slots.layoutsNeverAllocated.get(index);
    };
#define AOT_FACT_OF_SLOT(name, flag, claimed) \
    drawn(Fact::name, [&](const IsDrawn& isDrawn) { \
        for (size_t index = 0; index < slots.layouts.size(); ++index) { \
            if (!isOwnLayoutInUse(index)) \
                continue; \
            for (FieldIndex field = 0; field < slots.layouts[index].numberOfFields(); ++field) { \
                PlacedSlot& slot = slots.layouts[index].slots[field]; \
                if (slot.hasStorage() && slot.flag != claimed && isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index))) \
                    slot.flag = claimed; \
            } \
        } \
    });
    FOR_EACH_FACT_OF_SLOT(AOT_FACT_OF_SLOT)
#undef AOT_FACT_OF_SLOT

    using TypeFunctor = ScopedLambda<void(TypeID&, PlaceKind, uint32_t)>;
    auto typesOfSlots = [&](Fact fact, TypeID PlacedSlot::*type) {
        misstateType(fact, types, scopedLambda<void(const TypeFunctor&)>([&](const TypeFunctor& functor) {
            for (size_t index = 0; index < slots.layouts.size(); ++index) {
                for (FieldIndex field = 0; isOwnLayoutInUse(index) && field < slots.layouts[index].numberOfFields(); ++field)
                    functor(slots.layouts[index].slots[field].*type, PlaceKind::Layouts, static_cast<uint32_t>(index));
            }
        }));
    };
    typesOfSlots(Fact::SlotProvenType, &PlacedSlot::provenType);
    typesOfSlots(Fact::SlotType, &PlacedSlot::type);
    typesOfSlots(Fact::SlotRawType, &PlacedSlot::typeOfRawValue);
    drawn(Fact::LayoutWithoutMark, [&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < slots.layouts.size(); ++index) {
            if (isOwnLayoutInUse(index) && slots.layouts[index].mayBearMark && isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index)))
                slots.layouts[index].mayBearMark = false;
        }
    });
    drawn(Fact::FieldRawFromAllocation, [&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < slots.layouts.size(); ++index) {
            for (FieldIndex field = 0; isOwnLayoutInUse(index) && field < slots.layouts[index].numberOfFields(); ++field) {
                if (slots.layouts[index].slots[field].isFilledAtBirth && !slots.layouts[index].holdsRawValueFromBirth(field) && isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index)))
                    slots.layouts[index].fieldsThatHoldRawValuesFromBirth.set(field);
            }
        }
    });
    drawn(Fact::FieldKeptFilled, [&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < slots.layouts.size(); ++index) {
            for (FieldIndex field = 0; isOwnLayoutInUse(index) && field < slots.layouts[index].numberOfFields(); ++field) {
                Vector<FieldIndex>& filled = slots.layouts[index].fieldsHeldFilled;
                if (!slots.layouts[index].slots[field].hasStorage() || filled.contains(field) || !isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index)))
                    continue;
                filled.append(field);
                std::ranges::sort(filled);
            }
        }
    });
    drawn(Fact::PlannedVariableUndescribed, [&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < slots.variables.size(); ++index) {
            if (m_ownVariables.get(index) && slots.variables[index].isDescribedToRunTime && isDrawn(PlaceKind::Variables, static_cast<uint32_t>(index)))
                slots.variables[index].isDescribedToRunTime = false;
        }
    });
    drawn(Fact::PlannedVariableFunction, [&](const IsDrawn& isDrawn) {
        for (size_t index = 0; index < slots.variables.size(); ++index) {
            if (m_ownVariables.get(index) && slots.variables[index].function && isDrawn(PlaceKind::Variables, static_cast<uint32_t>(index)))
                slots.variables[index].function = { };
        }
    });
#define AOT_FACT_OF_PLANNED_CLASS(name, flag, claimed) \
    drawn(Fact::name, [&](const IsDrawn& isDrawn) { \
        for (size_t index = 0; index < slots.classCreation.size(); ++index) { \
            if (m_ownLayouts.get(index) && m_program.layouts[index].kind == LayoutKind::Instance && m_program.layouts[index].constructor && slots.classCreation[index].flag != claimed && isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index))) \
                slots.classCreation[index].flag = claimed; \
        } \
    });
    FOR_EACH_FACT_OF_PLANNED_CLASS(AOT_FACT_OF_PLANNED_CLASS)
#undef AOT_FACT_OF_PLANNED_CLASS
    auto memberCreation = [&](Fact fact, MemberCreation claimed) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (size_t index = 0; index < slots.classCreation.size(); ++index) {
                for (MemberCreation& creation : slots.classCreation[index].members) {
                    if (m_ownLayouts.get(index) && creation != claimed && isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(index)))
                        creation = claimed;
                }
            }
        });
    };
    memberCreation(Fact::MemberCreatedByRecord, MemberCreation::ByClassRecord);
#define AOT_SET_OF_SLOT_PLAN(name, set, claimed, kind) misstateSet(Fact::name, slots.set, claimed, PlaceKind::kind);
    FOR_EACH_SET_OF_SLOT_PLAN(AOT_SET_OF_SLOT_PLAN)
#undef AOT_SET_OF_SLOT_PLAN

    drawn(Fact::DeferredVariableUnmarked, [&](const IsDrawn& isDrawn) {
        for (DeferredVariable& variable : parts.deferredDefinitions.variables) {
            if (variable.mark && isDrawn(PlaceKind::Variables, variable.variable.index()))
                variable.mark = 0;
        }
    });
    dropEntry<StoreOfCapturedValue>(Fact::PromotedStoreUnlisted, parts.promotedEnvironments.stores, PlaceKind::Functions, [](const StoreOfCapturedValue& store) { return store.function.index(); });

    ModuleEvaluationParts& modules = parts.moduleEvaluation.m_parts;
#define AOT_SET_OF_MODULE_PLAN(name, set, claimed, kind) misstateSet(Fact::name, modules.set, claimed, PlaceKind::kind);
    FOR_EACH_SET_OF_MODULE_PLAN(AOT_SET_OF_MODULE_PLAN)
#undef AOT_SET_OF_MODULE_PLAN
    drawn(Fact::ModuleStepUnlisted, [&](const IsDrawn& isDrawn) {
        for (ModuleStart& start : modules.starts) {
            if (!start.steps.empty() && m_ownModules.get(start.module.index()) && isDrawn(PlaceKind::Modules, start.module.index()))
                start.steps = start.steps.first(start.steps.size() - 1);
        }
    });
    auto starts = [&](Fact fact, bool ModuleStart::*flag, bool claimed) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (ModuleStart& start : modules.starts) {
                if (m_ownModules.get(start.module.index()) && start.*flag != claimed && isDrawn(PlaceKind::Modules, start.module.index()))
                    start.*flag = claimed;
            }
        });
    };
    starts(Fact::ModuleStartErrorNeverCaught, &ModuleStart::mayBeLeftByCaughtError, false);
    starts(Fact::ModuleStartOnlyReturns, &ModuleStart::bodyOnlyReturns, true);
    starts(Fact::ModuleStartBeginsEarly, &ModuleStart::beginsInBody, false);
    drawn(Fact::ModuleLoadTimeFileUnlisted, [&](const IsDrawn& isDrawn) {
        for (ModuleStart& start : modules.starts) {
            if (!start.filesRunWhileLoaded.empty() && isDrawn(PlaceKind::Modules, start.module.index()))
                start.filesRunWhileLoaded = start.filesRunWhileLoaded.first(start.filesRunWhileLoaded.size() - 1);
        }
    });
    misstateFlag(Fact::NoRequireEvaluatesModule, modules.noRequireEvaluatesESModule, true);

    KeyOrderParts& orders = parts.keyOrders.m_parts;
    auto keyOrders = [&](Fact fact, const auto& functor) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (size_t layout = 0; layout + 1 < orders.firstOrderOfLayout.size(); ++layout) {
                if (!m_ownLayouts.get(layout))
                    continue;
                for (uint32_t order = orders.firstOrderOfLayout[layout]; order < orders.firstOrderOfLayout[layout + 1]; ++order) {
                    for (uint32_t place = orders.firstFieldOfOrder[order]; place < orders.firstFieldOfOrder[order + 1]; ++place) {
                        functor(place, place + 1 == orders.firstFieldOfOrder[order + 1], [&] {
                            return isDrawn(PlaceKind::Layouts, static_cast<uint32_t>(layout));
                        });
                    }
                }
            }
        });
    };
    keyOrders(Fact::KeyOrderSwapped, [&](uint32_t place, bool isLast, const auto& isDrawn) {
        if (!isLast && isDrawn())
            std::swap(orders.fields[place], orders.fields[place + 1]);
    });
    keyOrders(Fact::KeyOrderNoAllocationStops, [&](uint32_t place, bool, const auto& isDrawn) {
        if (orders.someMakerStopsBehindField[place] && isDrawn())
            orders.someMakerStopsBehindField[place] = false;
    });

#define AOT_SET_OF_PLAN(name, set, claimed, kind) misstateSet(Fact::name, parts.set, claimed, PlaceKind::kind);
    FOR_EACH_SET_OF_PLAN(AOT_SET_OF_PLAN)
#undef AOT_SET_OF_PLAN
    misstateSet(Fact::TailCallsEnd, parts.functionsThatEndTheirTailCalls.whateverArrives, true, PlaceKind::Functions);

    CallingConventions& conventions = parts.conventions;
    auto functions = [&](Fact fact, const auto& functor) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (size_t index : m_ownFunctions) {
                if (index < conventions.m_parts.conventions.size() && linked.functions[index].isReached) {
                    functor(conventions.m_parts.conventions[index], [&] {
                        return isDrawn(PlaceKind::Functions, static_cast<uint32_t>(index));
                    });
                }
            }
        });
    };
    functions(Fact::CoroutineNeverSuspends, [](FunctionConvention& convention, const auto& isDrawn) {
        if (convention.has(CodeRole::CoroutineBody) && !convention.coroutineNeverSuspends && isDrawn())
            convention.coroutineNeverSuspends = true;
    });
    functions(Fact::CallEntryWithoutNewTarget, [](FunctionConvention& convention, const auto& isDrawn) {
        if (convention.callEntryTakesNewTarget && isDrawn())
            convention.callEntryTakesNewTarget = false;
    });
    functions(Fact::CallEntryBorrowsArguments, [](FunctionConvention& convention, const auto& isDrawn) {
        if (!convention.borrowedArguments && isDrawn())
            convention.borrowedArguments = 1;
    });
    misstateType(Fact::ParameterGuardType, types, scopedLambda<void(const TypeFunctor&)>([&](const TypeFunctor& functor) {
        for (size_t index : m_ownFunctions) {
            if (index >= conventions.m_parts.conventions.size() || !conventions.m_parts.conventions[index].has(CodeRole::ParameterGuard))
                continue;
            const FunctionConvention& convention = conventions.m_parts.conventions[index];
            for (uint32_t parameter = 0; parameter < convention.numberOfEntryParameters; ++parameter)
                functor(conventions.m_parts.typesBehindGuards[convention.firstTypeBehindGuard + parameter], PlaceKind::Functions, static_cast<uint32_t>(index));
        }
    }));
    auto rowsOfMethodTable = [&](Fact fact, const auto& functor) {
        drawn(fact, [&](const IsDrawn& isDrawn) {
            for (MethodFamily& family : conventions.m_parts.methodTable.families) {
                for (size_t row = 0; row < family.rows.size(); ++row) {
                    if (!m_ownLayouts.get(family.rows[row].layout.index()))
                        continue;
                    functor(family, row, [&] {
                        return isDrawn(PlaceKind::Layouts, family.rows[row].layout.index());
                    });
                }
            }
        });
    };
    rowsOfMethodTable(Fact::MethodTableRowUnlisted, [](MethodFamily& family, size_t row, const auto& isDrawn) {
        if (family.rows.size() > 1 && isDrawn())
            family.rows.removeAt(row);
    });
    rowsOfMethodTable(Fact::MethodTableRowUnguarded, [](MethodFamily& family, size_t row, const auto& isDrawn) {
        if (family.rows[row].guardedClass && isDrawn())
            family.rows[row].guardedClass = { };
    });
    rowsOfMethodTable(Fact::MethodTableMethod, [](MethodFamily& family, size_t row, const auto& isDrawn) {
        for (FunctionID method : family.methods) {
            if (method != family.rows[row].method && isDrawn()) {
                family.rows[row].method = method;
                break;
            }
        }
    });
    misstateType(Fact::MethodSelectorResult, types, scopedLambda<void(const TypeFunctor&)>([&](const TypeFunctor& functor) {
        for (MethodSelector& selector : conventions.m_parts.methodTable.selectors)
            functor(selector.typeOfResult, PlaceKind::Program, 0);
    }));
#define AOT_SET_OF_STACK_PLAN(name, set, claimed, kind) misstateSet(Fact::name, conventions.m_stackTests.set, claimed, PlaceKind::kind);
    FOR_EACH_SET_OF_STACK_PLAN(AOT_SET_OF_STACK_PLAN)
#undef AOT_SET_OF_STACK_PLAN

    ShapeTableParts& shapes = parts.shapes.m_parts;
#define AOT_MUTATION_OF_SHAPE(name, mutation) \
    drawn(Fact::name, [&](const IsDrawn& isDrawn) { \
        for (PlacedShape& shape : shapes.shapes) { \
            if (shape.layout && m_ownLayouts.get(shape.layout.index()) && !shape.impossibleMutations.contains(ObjectMutation::mutation) && isDrawn(PlaceKind::Layouts, shape.layout.index())) \
                shape.impossibleMutations.add(ObjectMutation::mutation); \
        } \
    });
    FOR_EACH_MUTATION_OF_SHAPE(AOT_MUTATION_OF_SHAPE)
#undef AOT_MUTATION_OF_SHAPE
}

#undef AOT_FLAG_OF_RECORD

std::optional<uint32_t> numberOfMisstatedFact(StringView name)
{
    for (size_t number = 1; number < std::size(namesOfMisstatedFacts); ++number) {
        if (name == namesOfMisstatedFacts[number])
            return static_cast<uint32_t>(number);
    }
    return std::nullopt;
}

void listMisstatedFacts(PrintStream& out)
{
    for (size_t number = 1; number < std::size(namesOfMisstatedFacts); ++number)
        out.println("  MisstateFact:"_s, namesOfMisstatedFacts[number]);
}

void misstateSummaries(const Program& program, ThinSummaries& summaries, BuildContext& build)
{
    FactMisstater(program, build).misstateSummaries(summaries);
}

void misstateLinkTables(Program& program, const ThinSummaries& summaries, LinkTables& linked, LinkTables& held, BuildContext& build)
{
    TypeArena types(program.types, ArenaKeeps::InProgram);
    FactMisstater(program, build).misstateLinkTables(types, summaries, linked, held);
}

void misstateFactTables(Program& program, LinkTables& linked, FactTables& tables, BuildContext& build)
{
    TypeArena types(program.types, ArenaKeeps::InProgram);
    FactMisstater(program, build).misstateFactTables(types, linked, tables);
}

void misstatePlan(Program& program, const Facts& facts, Plan& plan, BuildContext& build)
{
    TypeArena types(program.types, ArenaKeeps::InProgram);
    FactMisstater(program, build).misstatePlan(types, facts, plan);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && ENABLE(AOT_FAULT_INJECTION)
