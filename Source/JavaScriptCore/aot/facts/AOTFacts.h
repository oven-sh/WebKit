/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuiltinsInReach.h"
#include "AOTFactRecords.h"
#include "AOTLayoutsByNames.h"
#include "AOTLinkTables.h"
#include "JSExportMacros.h"
#include <memory>
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

enum class FactKind : uint8_t {
    NarrowedTypes,
    ProvenContents,
    OperandSummaries,
    HeapWords,
};
ASCIILiteral nameOf(FactKind);

struct FactsRequest {
    Mechanisms mechanisms { allMechanisms };
    OrderOfWorklist orderOfWorklist { OrderOfWorklist::NewestFirst };
};

class FactSubject {
public:
    enum class Kind : uint8_t { Program, Function, EntryParameter, Result, Thrown, Variable, Field, Layout, Object, Cell, Builtin, Module, InputOfOperation, Operation };

    constexpr FactSubject() = default;

    constexpr Kind kind() const { return m_kind; }
    constexpr uint32_t first() const { return m_first; }
    constexpr uint32_t second() const { return m_second; }

    friend constexpr bool operator==(const FactSubject&, const FactSubject&) = default;
    friend constexpr auto operator<=>(const FactSubject&, const FactSubject&) = default;

private:
    constexpr FactSubject(Kind kind, uint32_t first, uint32_t second)
        : m_kind(kind)
        , m_first(first)
        , m_second(second)
    {
    }

    Kind m_kind { Kind::Program };
    uint32_t m_first { 0 };
    uint32_t m_second { 0 };
};

struct ProgramFactTables {
    Vector<UnseenCodeRecord> unseenCode;
    Vector<UnseenCodeRecord> loadsOfMachineCode;
    bool hasOneInstance { false };
    bool unseenCodeCanRun { true };
    bool unseenScriptsCanRun { true };
    bool setsMainModulePath { true };
    bool matchesRegExps { true };
    bool mayReadLegacyRegExpStatics { true };
    bool mayRecompileRegExps { true };

    Vector<WorkerEntry> workerEntries;
    Vector<Witness> workerStartsWithoutName;
    Vector<Witness> workerStartsFromURLs;
    Vector<RuntimeFunctionID> missingLibraryFunctions;
    Vector<ModuleID> missingLibraryModules;
    Vector<RegExpTextInSight> regExpTextsInSight;
    Vector<RegExpTextWorkedOut> regExpTextsWorkedOut;
    Vector<RegExpCallWithoutText> regExpCallsWithoutText;
};

enum class ReasonBuiltinStaysOpen : uint8_t {
    None,
    NoLockIsLaid,
    IsOpenToDebugger,
    IsLeftOpenForTests,
    IsWrittenUnderItsName,
    HolderIsWrittenUnderAnyName,
    IsWrittenThroughLostBuiltins,
    IsSpelledThroughUnseen,
    HolderIsLetGoByProgram,
};
struct BuiltinLeftOpen {
    BuiltinID member;
    ReasonBuiltinStaysOpen reason { ReasonBuiltinStaysOpen::None };
};

struct AssignedMember {
    FunctionID method;
    FunctionID getter;
    FunctionID setter;
    bool isDeclared { false };
    bool isUnconditional { false };
    bool holdsValue { false };

    explicit operator bool() const { return method || getter || setter; }
};

struct AssignedMemberOfClass {
    std::pair<LayoutID, PropertyKeyID> place() const { return { classLayout, key }; }

    LayoutID classLayout;
    PropertyKeyID key;
    AssignedMember member;
};

class Facts;
AssignedMember findAssignedMember(const Program&, const Facts&, LayoutID classLayout, PropertyKeyID);

struct BeforeEvaluation {
    bool holdsIn(FunctionID id) const { return functions.get(id.index()); }
    TypeID typeOf(VariableID id) const { return id.index() < variables.size() ? variables[id.index()] : TypeID(); }
    TypeID typeOf(FieldReference reference) const
    {
        auto found = std::ranges::lower_bound(fields, reference, { }, &std::pair<FieldReference, TypeID>::first);
        return found != fields.end() && found->first == reference ? found->second : TypeID();
    }
    TypeID resultOf(FunctionID id) const { return id.index() < results.size() ? results[id.index()] : TypeID(); }

    Vector<VariableID, 2> marks;
    Vector<TypeID> variables;
    Vector<std::pair<FieldReference, TypeID>> fields;
    Vector<TypeID> results;
    BitVector functions;
    LinkedFirstStores firstStores;
};

struct FactTables {
    WTF_MAKE_NONCOPYABLE(FactTables);
public:
    FactTables() = default;
    FactTables(FactTables&&) = default;
    FactTables& operator=(FactTables&&) = default;

    Vector<Summary> summaries;
    BitVector functionsNeverReached;

    FactRows<PositionFacts> entryParameters;
    uint32_t firstLayoutOfNamespace { 0 };
    uint32_t endOfLayoutsOfNamespaces { 0 };

    Vector<VariableFacts> variables;
    Vector<uint32_t> firstFieldOfLayout;
    Vector<FieldFacts> fields;
    BitVector layoutsThatTakeRowsOfBase;

    Vector<LayoutFacts> layouts;
    Vector<ClassFacts> classes;
    BitVector functionsTakenToRunOnce;
    FactRows<ClassMember> membersOfClass;
    Vector<uint32_t> placesOfMembersOfClassByKey;
    BitVector classMemberKeys;
    Vector<AssignedMemberOfClass> assignedMembers;
    BitVector assignedMemberKeys;
    BitVector layoutsThatParsersMayMake;
    LayoutSetID layoutsThatStayWithCompiledCode;

    BitVector modulesWhosePathNoOperationReads;
    struct FunctionGivenToVariable {
        uint32_t variable { 0 };
        FunctionID function;
        FunctionID atStart;
    };
    Vector<FunctionGivenToVariable> functionsGivenToVariables;
    struct FunctionStoredInField {
        FieldReference field;
        FunctionID function;
    };
    Vector<FunctionStoredInField> functionsStoredInFields;
    BitVector functionsWhoseResultIsReadAsKey;

    BitVector constantBuiltinMembers;
    BitVector builtinMembersLockedAtStartUp;
    Vector<BuiltinLeftOpen> builtinMembersLeftOpen;
    Vector<BuiltinID> builtinMembersThatTestsReplace;
    BitVector builtinHoldersWithOpenMembers;
    BitVector builtinMembersBehindWatch;
    MocksOfProgram mocks;
    BitVector builtinHoldersWhoseKeysAreNeverListed;
    Vector<BuiltinUseRecord> builtinUses;
    Vector<FeatureUseRecord> featureUses;
    Vector<LocaleUse> localeUses;

    ProgramFactTables program;

    BeforeEvaluation beforeEvaluation;
};

// "Must be true": the image or the run time is wrong, and not merely slower, where the answer lacks something. An empty list there is a claim.
class Facts {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Facts);
    WTF_MAKE_NONCOPYABLE(Facts);
public:
    struct Parts {
        std::unique_ptr<BodyIndexTable> bodies;
        std::unique_ptr<ThinSummaries> summaries;
        std::unique_ptr<Families> familiesOfFlow;
        std::unique_ptr<Families> families;
        LinkTables linked;
        LinkTables held;
        LayoutsByNames layoutsByNames;
        Mechanisms mechanisms;
        FactTables tables;
    };

    Facts(const Program& program, Parts&& parts)
        : m_program(program)
        , m_bodies(WTF::move(parts.bodies))
        , m_summaries(WTF::move(parts.summaries))
        , m_families(WTF::move(parts.families))
        , m_linked(WTF::move(parts.linked))
        , m_held(WTF::move(parts.held))
        , m_layoutsByNames(WTF::move(parts.layoutsByNames))
        , m_mechanisms(parts.mechanisms)
        , m_tables(WTF::move(parts.tables))
    {
        RELEASE_ASSERT(m_bodies && m_summaries && m_families);
    }

    const Program& program() const { return m_program; }
    const FactTables& tables() const { return m_tables; }

    struct WhatMiddleAloneReads {
        std::unique_ptr<BodyIndexTable> bodies;
        std::unique_ptr<ThinSummaries> summaries;
        std::unique_ptr<Families> families;
        LinkTables held;
        LayoutsByNames layoutsByNames;
    };
    WhatMiddleAloneReads takeWhatMiddleAloneReads() { return { WTF::move(m_bodies), WTF::move(m_summaries), WTF::move(m_families), std::exchange(m_held, { }), std::exchange(m_layoutsByNames, { }) }; }
    void trimSummariesForWalk(const ParallelLoops& loops) { m_summaries->trimForWalk(loops); }
    const BodyIndexTable& bodies() const { return *m_bodies; }
    const BeforeEvaluation& beforeEvaluation() const LIFETIME_BOUND { return m_tables.beforeEvaluation; }
    FunctionID functionThatCallEntersByBinding(const BodyIndex&, const BodyValue& call, ValueID* checkThatPasses = nullptr) const;
    bool testMentionsWhatIsOutOfReach(TypeReader, TypeID tested, NOESCAPE const ScopedLambda<bool(FunctionID)>& isInReach) const;
    TypeID typeBelievedOfEntryParameter(TypeArena&, FunctionID, unsigned entryParameter, TypeID knownOfCallers) const;

    const LinkTables& linked() const LIFETIME_BOUND { return m_linked; }
    const LinkTables& held() const LIFETIME_BOUND { return m_held; }
    bool saysOfInheritedFieldsWhatBaseSays(LayoutID layout) const { return m_linked.layouts[layout.index()].takesCellsOfBase && m_held.layouts[layout.index()].takesCellsOfBase && m_tables.layoutsThatTakeRowsOfBase.get(layout.index()); }
    const LayoutsByNames& layoutsByNames() const LIFETIME_BOUND { return m_layoutsByNames; }
    const Families& families() const LIFETIME_BOUND { return *m_families; }
    const ThinSummaries& summaries() const LIFETIME_BOUND { return *m_summaries; }
    Mechanisms mechanisms() const { return m_mechanisms; }

    const Summary* summaryOf(FunctionID id) const LIFETIME_BOUND { return id.index() < m_tables.summaries.size() ? &m_tables.summaries[id.index()] : nullptr; }
    bool hasLoops(FunctionID id) const
    {
        const Summary* summary = summaryOf(id);
        return !summary || !summary->hasNoLoops;
    }
    bool mayBeReached(FunctionID id) const { return !m_tables.functionsNeverReached.get(id.index()); }
    bool everyCallerIsKnown(FunctionID id) const { return id.index() < m_linked.functions.size() && m_linked.function(id).everyCallerIsKnown; }
    bool runsAtMostOnce(FunctionID id) const { return id.index() < m_linked.functions.size() && m_linked.function(id).runsAtMostOnce; }
    bool isTakenToRunOnce(FunctionID id) const { return m_tables.functionsTakenToRunOnce.get(id.index()); }
    Effects effectsOnReturn(FunctionID id) const { return effectsSeenByCaller(id.index() < m_linked.functions.size() ? m_linked.function(id).effectsOnReturn() : effectsOfUnknownCode); }
    Effects effectsOnThrow(FunctionID id) const { return effectsSeenByCaller(id.index() < m_linked.functions.size() ? m_linked.function(id).effectsOnThrow() : effectsOfUnknownCode); }
    PositionFacts typeOfEntryParameter(FunctionID id, uint32_t entryParameter) const
    {
        std::span<const PositionFacts> rows = m_tables.entryParameters.of(id);
        return entryParameter < rows.size() ? rows[entryParameter] : PositionFacts { };
    }

    VariableFacts variable(VariableID id) const { return factRecordAt(m_tables.variables, id.index()); }
    uint32_t numberOfFieldsOf(LayoutID id) const
    {
        const Vector<uint32_t>& first = m_tables.firstFieldOfLayout;
        size_t numberOfLayouts = first.isEmpty() ? 0 : first.size() - 1;
        return id.index() < numberOfLayouts ? first[id.index() + 1] - first[id.index()] : 0;
    }
    FieldFacts field(FieldReference reference) const
    {
        if (reference.field >= numberOfFieldsOf(reference.layout))
            return { };
        return m_tables.fields[m_tables.firstFieldOfLayout[reference.layout.index()] + reference.field];
    }

    LayoutFacts layout(LayoutID id) const { return factRecordAt(m_tables.layouts, id.index()); }
    bool has(LayoutID id, LayoutFact fact) const { return layout(id).facts.contains(fact); }
    bool parsersMayMake(LayoutID id) const { return m_tables.layoutsThatParsersMayMake.get(id.index()); }
    ClassFacts classOf(LayoutID id) const { return factRecordAt(m_tables.classes, id.index()); }
    bool isEvaluatedOncePerInstance(LayoutID classLayout) const { return classOf(classLayout).isEvaluatedOncePerInstance; }
    bool isEvaluatedAtMostOnce(LayoutID classLayout) const { return classOf(classLayout).evaluations == ExecutionCount::AtMostOnce; }
    std::span<const ClassMember> membersOf(LayoutID classLayout) const LIFETIME_BOUND { return m_tables.membersOfClass.of(classLayout); }
    JS_EXPORT_PRIVATE std::span<const uint32_t> placesOfMembersUnder(LayoutID classLayout, ClassSide, PropertyKeyID) const LIFETIME_BOUND;
    bool hasMemberUnder(LayoutID classLayout, ClassSide side, PropertyKeyID key) const { return !placesOfMembersUnder(classLayout, side, key).empty(); }

    BuiltinLocationState builtinLocation(BuiltinID member) const { return member && m_tables.constantBuiltinMembers.get(member.index()) ? BuiltinLocationState::Constant : BuiltinLocationState::Unknown; }
    const BitVector& builtinMembersLockedAtStartUp() const LIFETIME_BOUND { return m_tables.builtinMembersLockedAtStartUp; }
    std::span<const BuiltinLeftOpen> builtinMembersLeftOpen() const LIFETIME_BOUND { return m_tables.builtinMembersLeftOpen.span(); }
    std::span<const BuiltinID> builtinMembersThatTestsReplace() const LIFETIME_BOUND { return m_tables.builtinMembersThatTestsReplace.span(); }
    bool hasOpenMember(BuiltinID canonicalHolder) const { return m_tables.builtinHoldersWithOpenMembers.get(canonicalHolder.index()); }
    bool isBehindWatch(BuiltinID member) const { return member && m_tables.builtinMembersBehindWatch.get(member.index()); }
    const MocksOfProgram& mocks() const LIFETIME_BOUND { return m_tables.mocks; }
    bool listsKeysOf(BuiltinID canonicalHolder) const { return !m_tables.builtinHoldersWhoseKeysAreNeverListed.get(canonicalHolder.index()); }
    std::span<const BuiltinUseRecord> builtinUses() const LIFETIME_BOUND { return m_tables.builtinUses.span(); }
    bool isUsedByOperation(BuiltinID id) const
    {
        const BuiltinUseRecord* use = findFactRow(builtinUses(), id.index(), &BuiltinUseRecord::builtin);
        return use && hasFunction(use->witness);
    }
    bool isLetGo(BuiltinID id) const
    {
        const BuiltinUseRecord* use = findFactRow(builtinUses(), id.index(), &BuiltinUseRecord::builtin);
        return use && (static_cast<uint32_t>(use->uses) & static_cast<uint32_t>(BuiltinUse::Escaped));
    }
    bool isUsedWithoutNamesFromRunTime(BuiltinID id) const
    {
        const BuiltinUseRecord* use = findFactRow(builtinUses(), id.index(), &BuiltinUseRecord::builtin);
        return use && !(static_cast<uint32_t>(use->uses) & static_cast<uint32_t>(BuiltinUse::NamedAtRunTime));
    }
    std::span<const FeatureUseRecord> featureUses() const LIFETIME_BOUND { return m_tables.featureUses.span(); }
    std::span<const LocaleUse> localeUses() const LIFETIME_BOUND { return m_tables.localeUses.span(); }
    bool mayReadLegacyRegExpStatics() const { return m_tables.program.mayReadLegacyRegExpStatics; }
    bool mayRecompileRegExps() const { return m_tables.program.mayRecompileRegExps; }

    bool isPathReadByOperation(ModuleID id) const { return !m_tables.modulesWhosePathNoOperationReads.get(id.index()); }
    const FactTables::FunctionGivenToVariable* findFunctionGivenTo(VariableID id) const { return findFactRow(m_tables.functionsGivenToVariables.span(), id.index(), &FactTables::FunctionGivenToVariable::variable); }
    const FactTables::FunctionStoredInField* findFunctionGivenTo(FieldReference field) const { return findFactRow(m_tables.functionsStoredInFields.span(), field, &FactTables::FunctionStoredInField::field); }
    bool resultIsReadAsKey(FunctionID id) const { return m_tables.functionsWhoseResultIsReadAsKey.get(id.index()); }

    bool unseenCodeCanRun() const { return m_tables.program.unseenCodeCanRun; }
    bool unseenScriptsCanRun() const { return m_tables.program.unseenScriptsCanRun; }
    std::span<const UnseenCodeRecord> unseenCode() const LIFETIME_BOUND { return m_tables.program.unseenCode.span(); }
    std::span<const UnseenCodeRecord> loadsOfMachineCode() const LIFETIME_BOUND { return m_tables.program.loadsOfMachineCode.span(); }
    bool hasOneInstance() const { return m_tables.program.hasOneInstance; }
    bool matchesRegExps() const { return m_tables.program.matchesRegExps; }
    bool setsMainModulePath() const { return m_tables.program.setsMainModulePath; }
    std::span<const RegExpTextInSight> regExpTextsInSight() const LIFETIME_BOUND { return m_tables.program.regExpTextsInSight.span(); }
    std::span<const RegExpTextWorkedOut> regExpTextsWorkedOut() const LIFETIME_BOUND { return m_tables.program.regExpTextsWorkedOut.span(); }
    std::span<const WorkerEntry> workerEntries() const LIFETIME_BOUND { return m_tables.program.workerEntries.span(); }
    const Vector<Witness>& workerStartsWithoutName() const { return m_tables.program.workerStartsWithoutName; }
    std::span<const RuntimeFunctionID> missingLibraryFunctions() const LIFETIME_BOUND { return m_tables.program.missingLibraryFunctions.span(); }

private:
    static Effects effectsSeenByCaller(Effects effects)
    {
        effects.maySuspend = false;
        return effects;
    }

    const Program& m_program;
    std::unique_ptr<BodyIndexTable> m_bodies;
    std::unique_ptr<ThinSummaries> m_summaries;
    std::unique_ptr<Families> m_families;
    LinkTables m_linked;
    LinkTables m_held;
    LayoutsByNames m_layoutsByNames;
    Mechanisms m_mechanisms;
    FactTables m_tables;
};


struct InputsOfProgramFacts {
    const Program& program;
    const BodyIndexTable& bodies;
    const LooksAtBodies& looks;
    const ThinSummaries& summaries;
    const LinkTables& linked;
    BuildContext& build;
};
void computeFactsOfProgram(const InputsOfProgramFacts&, FactTables&);
void workOutRegExpTexts(const Program&, const BodyIndexTable&, const ThinSummaries&, ProgramFactTables&);

BlockID blockWhereListsOfArgumentsAreFirstNamed(const Program&, const BodyIndexTable&, FunctionID);
JS_EXPORT_PRIVATE std::unique_ptr<Facts> computeFacts(Program&, BuildContext&, const FactsRequest& = { });
JS_EXPORT_PRIVATE void printBreachesOfClosureOfLink(PrintStream&, Program&, BuildContext&, Mechanisms);
JS_EXPORT_PRIVATE std::unique_ptr<Facts> computeFacts(Program&, BuildContext&, const FactsRequest&, std::unique_ptr<BodyIndexTable>&&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
