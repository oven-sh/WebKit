/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinsInReach.h"
#include "AOTFactRecords.h"
#include "AOTLayoutsByNames.h"
#include "AOTLinkTables.h"
#include "JSExportMacros.h"
#include <memory>
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

// What the walk marks a record with where it leans on a fact: FactUse of walk/AOTCode.h.
enum class FactKind : uint8_t {
    ProvenLayoutSets,
    NarrowedTypes,
    RemovedNullish,
    LiteralValues,
    ExactFunctions,
    ProvenContents,
    ArrayStorageKinds,
    PristineInstances,
    ConstantBuiltinLocations,
    CalleeSets,
    ReceiverSets,
    ClassBases,
    AppendedFields,
    HolderMutations,
    OwnNames,
    ClassEvaluations,
    StoreCounts,
    Initialization,
    ConstructorFieldOrders,
    OperandSummaries,
    ObjectUses,
    ReachedFunctions,
    ExecutionCounts,
    HeapWords,
};
inline constexpr unsigned numberOfFactKinds = 24;
static_assert(static_cast<unsigned>(FactKind::HeapWords) + 1 == numberOfFactKinds);
ASCIILiteral nameOf(FactKind);

struct FactsRequest {
    // Set only by tests and by testaot. What is withheld from Link is claimed by no table.
    Mechanisms mechanisms { allMechanisms };
    // Set only by tests and by testaot.
    OrderOfWorklist orderOfWorklist { OrderOfWorklist::NewestFirst };
    // --explain was asked for.
    bool tracesUnknownValues { false };
};

// What a fact is about.
class FactSubject {
public:
    enum class Kind : uint8_t { Program, Function, EntryParameter, Result, Thrown, Variable, Field, Layout, Object, Cell, Builtin, Module, InputOfOperation, Operation };

    constexpr FactSubject() = default;

    static constexpr FactSubject fromParts(Kind kind, uint32_t first, uint32_t second) { return { kind, first, second }; }

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

struct UnknownOriginOfSubject {
    FactSubject subject;
    UnknownOrigin origin;
};

// The queries of Facts say what each list and flag means.
struct ProgramFactTables {
    Vector<UnseenCodeRecord> unseenCode;
    Vector<UnseenCodeRecord> loadsOfMachineCode;
    bool hasOneInstance { false };
    bool unseenCodeCanRun { true };
    bool unseenScriptsCanRun { true };
    bool findsFilesOfProgramByName { true };
    bool storesPathOfMainModule { true };
    bool reachedFunctionsCountUnreadCode { true };
    bool matchesRegExps { true };
    bool mayReadLegacyRegExpStatics { true };

    // Ascending by path.
    Vector<WorkerEntry> workerEntries;
    std::optional<Witness> workerStartWithoutName;
    // Ascending by place, each once.
    // Must be true: the file of a worker that a URL names comes into the executable by this list alone. From the scan: each `new URL(..)`, URL named by binding, whose value goes on to a construction of Worker or out of the body.
    // Those that go out of the body are listed only where an operation names Worker.
    Vector<Witness> urlsThatStartWorkers;
    // All three ascending.
    Vector<RuntimeFunctionID> libraryFunctionsLacked;
    // Must be true, as libraryFunctionsLacked(). From the records and the scan: a module that an operation or an import evaluates, for which the library has text and the program no body.
    Vector<ModuleID> libraryModulesLacked;
    // Must be true, as libraryFunctionsLacked(). From the scan: what BuiltinTable::moduleLoadsOf() names for a built-in of builtinUses(), where the program has no record of the module.
    Vector<RuntimeModuleID> libraryModulesWithoutRecord;
    // Ascending by pattern and flags, each pair once.
    Vector<RegExpTextInSight> regExpTextsInSight;
    // Ascending by compareProgramStrings() of the pattern, then by flags, each pair once. No pattern is a string of the program.
    Vector<RegExpTextWorkedOut> regExpTextsWorkedOut;
};

// Flat tables. An empty one claims nothing, but for the lists that the queries mark "Must be true".
// Why the program lays no lock on a member of a built-in that it uses and that a lock can be laid on: WritesOnBuiltinsInProgramFacts::reasonToLeaveOpen().
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

struct FactTables {
    WTF_MAKE_NONCOPYABLE(FactTables);
public:
    FactTables() = default;
    FactTables(FactTables&&) = default;
    FactTables& operator=(FactTables&&) = default;

    ObjectFacts object(ObjectID id) const { return id ? factRecordAt(objects, id.index()) : ObjectFacts { }; }

    // By function.
    Vector<Summary> summaries;
    BitVector functionsNeverReached;
    Vector<FunctionEffects> effects;
    // By function and entry parameter, captures first, in the order of EntryParameter.
    FactRows<PositionFacts> entryParameters;
    FactCallGraph calls;

    Vector<VariableFacts> variables;
    // One more entry than there are layouts.
    Vector<uint32_t> firstFieldOfLayout;
    Vector<FieldFacts> fields;

    // By layout.
    Vector<LayoutFacts> layouts;
    Vector<ClassFacts> classes;
    // In the order of their definitions.
    FactRows<ClassMember> membersOfClass;
    BitVector layoutsThatParsersMayMake;

    Vector<ObjectFacts> objects;
    // By allocation class, ascending.
    FactRows<PropertyKeyID> addedKeysOfClass;

    BitVector modulesWhosePathNoOperationReads;
    // Ascending by variable. `let hook = () => undefined;  function setHook(f) { hook = f }`: the variables of modules that functions other than the body of a module are seen to give the closures of exactly one function.
    // A guess: it claims nothing of what a variable holds. AOTFacts.cpp.
    struct FunctionGivenToVariable {
        uint32_t variable { 0 };
        FunctionID function;
        // The one function that bodies of modules are seen to store there. Null: none, or several.
        FunctionID atStart;
    };
    Vector<FunctionGivenToVariable> functionsGivenToVariables;
    // By function. AOTFactsStrings.cpp.
    BitVector functionsWhoseResultIsReadAsKey;

    // By built-in.
    BitVector constantBuiltinMembers;
    BitVector builtinMembersLockedAtStartUp;
    // Ascending.
    Vector<BuiltinLeftOpen> builtinMembersLeftOpen;
    // Ascending.
    Vector<BuiltinID> builtinMembersThatTestsReplace;
    // Made once, at the head of computeFacts().
    MocksOfProgram mocks;
    BitVector builtinHoldersWhoseKeysAreNeverListed;
    // Ascending by built-in.
    Vector<BuiltinUseRecord> builtinUses;
    Vector<FeatureUseRecord> featureUses;
    Vector<LocaleUse> localeUses;

    ProgramFactTables program;

    // Both only for --explain. Ascending by object and mutation, ascending by subject.
    Vector<MutationWitness> witnessesOfMutations;
    Vector<UnknownOriginOfSubject> originsOfUnknown;
};

// All that Plan, Compile, Image and the driver may ask about the whole program. Made once, then read by any thread and never changed.
// "Claims nothing": what the query answers out of an empty table. A program compiled with it is slow and right.
// "Must be true": the image or the run time is wrong, and not merely slower, where the answer lacks something. An empty list there is a claim.
class Facts {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Facts);
    WTF_MAKE_NONCOPYABLE(Facts);
public:
    struct Parts {
        std::unique_ptr<BodyIndexTable> bodies;
        std::unique_ptr<ThinSummaries> summaries;
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

    // Behind Compile. Image and the driver ask the tables, and linked() about functions, layouts and fields. Nobody reads a body, a summary or a family any more, or what is held:
    // whoever does stops on a null pointer.
    struct WhatMiddleAloneReads {
        std::unique_ptr<BodyIndexTable> bodies;
        std::unique_ptr<ThinSummaries> summaries;
        std::unique_ptr<Families> families;
        LinkTables held;
        LayoutsByNames layoutsByNames;
    };
    WhatMiddleAloneReads takeWhatMiddleAloneReads() { return { WTF::move(m_bodies), WTF::move(m_summaries), WTF::move(m_families), std::exchange(m_held, { }), std::exchange(m_layoutsByNames, { }) }; }
    // Must be true: an index for each function that has a body.
    const BodyIndexTable& bodies() const { return *m_bodies; }

    // ---- What the middle reads. The queries below are for the ends.
    // PROVEN: it needs no test. It alone says what reaches unseen code (a layout with all five possible changes, a function of which not every caller is known), and so what may be raw, or filled without a look.
    const LinkTables& linked() const LIFETIME_BOUND { return m_linked; }
    // HELD, of the same form: WhatUnseenCodeDoes::OnlyReadsAndCalls. No proof: an invariant that is enforced. Whatever is not compiled code of the program comes through the doors of the compiled object, and a store of compiled code
    // is proven on the spot to fit or is a checked store. A value that does not fit takes the field out of its slot. So lean on it ONLY behind the test that sees the taking-out, or behind the guard of an entry.
    const LinkTables& held() const LIFETIME_BOUND { return m_held; }
    const LayoutsByNames& layoutsByNames() const LIFETIME_BOUND { return m_layoutsByNames; }
    const Families& families() const LIFETIME_BOUND { return *m_families; }
    const ThinSummaries& summaries() const LIFETIME_BOUND { return *m_summaries; }
    // Those of the request, without what leans on a mechanism that was withheld. Plan has no other word of them.
    Mechanisms mechanisms() const { return m_mechanisms; }

    // ---- Functions
    // Claims nothing: null.
    const Summary* summaryOf(FunctionID id) const LIFETIME_BOUND { return id.index() < m_tables.summaries.size() ? &m_tables.summaries[id.index()] : nullptr; }
    // Claims nothing: true.
    bool hasLoops(FunctionID id) const
    {
        const Summary* summary = summaryOf(id);
        return !summary || !summary->hasNoLoops;
    }
    // Claims nothing: true.
    bool mayBeReached(FunctionID id) const { return !m_tables.functionsNeverReached.get(id.index()); }
    // Claims nothing: false.
    bool everyCallerIsKnown(FunctionID id) const { return id.index() < m_linked.functions.size() && m_linked.function(id).everyCallerIsKnown; }
    // Claims nothing: Unknown. Link has no table that says more.
    ClosureUse closureUse(FunctionID) const { return ClosureUse::Unknown; }
    // Claims nothing: false. In one realm: LinkedFunction::runsAtMostOnce.
    bool runsAtMostOnce(FunctionID id) const { return id.index() < m_linked.functions.size() && m_linked.function(id).runsAtMostOnce; }
    // Claims nothing: Effects::unknownCode().
    Effects effectsOnReturn(FunctionID id) const { return factRecordAt(m_tables.effects, id.index()).onReturn; }
    Effects effectsOnThrow(FunctionID id) const { return factRecordAt(m_tables.effects, id.index()).onThrow; }
    // Claims nothing: no type.
    PositionFacts typeOfEntryParameter(FunctionID id, uint32_t entryParameter) const
    {
        std::span<const PositionFacts> rows = m_tables.entryParameters.of(id);
        return entryParameter < rows.size() ? rows[entryParameter] : PositionFacts { };
    }
    // Claims nothing: no site has a row and there is no list. Then CallingConventions::indirectCallersAreAllListed() holds of no function.
    const FactCallGraph& calls() const { return m_tables.calls; }

    // ---- Variables and fields
    // Claims nothing: VariableFacts { }.
    VariableFacts variable(VariableID id) const { return factRecordAt(m_tables.variables, id.index()); }
    // Claims nothing: 0, which is fewer than the layout declares.
    uint32_t numberOfFieldsOf(LayoutID id) const
    {
        const Vector<uint32_t>& first = m_tables.firstFieldOfLayout;
        size_t numberOfLayouts = first.isEmpty() ? 0 : first.size() - 1;
        return id.index() < numberOfLayouts ? first[id.index() + 1] - first[id.index()] : 0;
    }
    // Claims nothing: FieldFacts { }.
    FieldFacts field(FieldReference reference) const
    {
        if (reference.field >= numberOfFieldsOf(reference.layout))
            return { };
        return m_tables.fields[m_tables.firstFieldOfLayout[reference.layout.index()] + reference.field];
    }

    // ---- Layouts and classes
    // Claims nothing: LayoutFacts { }.
    LayoutFacts layout(LayoutID id) const { return factRecordAt(m_tables.layouts, id.index()); }
    bool has(LayoutID id, LayoutFact fact) const { return layout(id).facts.contains(fact); }
    // Claims nothing: false. The run time's parsers then make ordinary objects.
    bool parsersMayMake(LayoutID id) const { return m_tables.layoutsThatParsersMayMake.get(id.index()); }
    // Claims nothing: ClassFacts { }. The record of the class then names the written base as expected.
    // BaseState::Layout tells the run time that one and the same class object stands at the heritage each time (ClassTable::baseOf()), which takes a count of evaluations. Link counts none, so no table says more.
    ClassFacts classOf(LayoutID id) const { return factRecordAt(m_tables.classes, id.index()); }
    // Claims nothing: false. Of a class that `class` defines: AOTFacts.cpp, fillClassTablesForFacts(). Of a constructor function nothing is claimed.
    bool isEvaluatedAtMostOnce(LayoutID classLayout) const { return classOf(classLayout).evaluations == ExecutionCount::AtMostOnce; }
    // What the DefineMethods of the program give the class, in the order of the definitions, reached or not: AOTFacts.cpp, fillMembersOfClassesForFacts(). A member's number (DefineMethod::index) is its place.
    // THE RECORD OF THE CLASS IS WRITTEN FROM IT, AND A GUARD OF THE CLASS MEANS "AS THE RECORD DECLARES": whoever asks what stands under a name above an instance asks this list. ClassCreationPlan::members stands row for row beside it.
    std::span<const ClassMember> membersOf(LayoutID classLayout) const LIFETIME_BOUND { return m_tables.membersOfClass.of(classLayout); }

    // ---- Objects
    // Claims nothing: ObjectFacts { }.
    ObjectFacts object(ObjectID id) const { return m_tables.object(id); }
    // Holds of every named object of the type, and the type has no `unknown`. Claims nothing: false.
    bool everyObjectHas(TypeReader, TypeID, ObjectFact) const;
    // Holds of some named object of the type. Claims nothing: false.
    bool someObjectHas(TypeReader, TypeID, ObjectFact) const;
    // Claims nothing: empty. Only what is made ready for goes by it.
    std::span<const PropertyKeyID> addedKeysOf(AllocationClassID id) const LIFETIME_BOUND { return m_tables.addedKeysOfClass.of(id); }

    // ---- Built-ins
    // Claims nothing: Unknown. Constant only of a member that builtinMembersLockedAtStartUp() lists: the lock makes the claim true, whoever writes. What the table locks is asked of the table.
    BuiltinLocationState builtinLocation(BuiltinID member) const { return member && m_tables.constantBuiltinMembers.get(member.index()) ? BuiltinLocationState::Constant : BuiltinLocationState::Unknown; }
    // Claims nothing: empty but for LookupsPastProgramObjects::membersTakenForConstants. A member that a program may replace (BuiltinMutability::ConstantUnlessWritten), that reached code names and that no write was followed to: WritesOnBuiltinsInProgramFacts::reasonToLeaveOpen().
    // A lock too many refuses a write of the program's own, so whatever is in doubt stays open. What nobody followed is refused when it runs: text that is evaluated, code from disk, native code.
    const BitVector& builtinMembersLockedAtStartUp() const LIFETIME_BOUND { return m_tables.builtinMembersLockedAtStartUp; }
    // Claims nothing. For whoever asks why a write is let through.
    std::span<const BuiltinLeftOpen> builtinMembersLeftOpen() const LIFETIME_BOUND { return m_tables.builtinMembersLeftOpen.span(); }
    // Must be true: the run time refuses a write on a member that its table locks and this list lacks. Claims nothing of any value. Empty outside a test run.
    std::span<const BuiltinID> builtinMembersThatTestsReplace() const LIFETIME_BOUND { return m_tables.builtinMembersThatTestsReplace.span(); }
    // For the plan and the walk: mayComeThroughMock(program, facts.mocks(), exporter, index). A default one: no module may be mocked.
    const MocksOfProgram& mocks() const LIFETIME_BOUND { return m_tables.mocks; }
    // Claims nothing: true. False: nobody ever has the object but operations that name it and read, test, call or write a member under a name that is written there. So nobody can tell when it got each member.
    // Whoever lists the keys of an object holds it: an object that was let go, that a read past the program's own objects has found, or on which an operation or a row in sight runs a protocol, or an accessor that the program may have laid above it, may be listed.
    bool listsKeysOf(BuiltinID canonicalHolder) const { return !m_tables.builtinHoldersWhoseKeysAreNeverListed.get(canonicalHolder.index()); }
    // Must be true: a built-in that is not listed may be trimmed out of the executable.
    // From the scan: each built-in that an operation names by binding, and each member that a read under a constant key finds on one.
    // A built-in object that goes anywhere else has BuiltinUse::Escaped, and each built-in below it (Builtin::holder leads up to it) has a row of its own with the same use and the witness of the escape: no reader goes up the holders.
    // So has the object of a module of native code (BuiltinTable::namespaceOf()) wherever the loader fills variables out of it or an operation gives its namespace object. What `require()` answers is followed as any built-in object.
    // Where unseenCodeCanRun() the global object has escaped so, without a witness. A row keeps its lowest witness: isLowerWitness().
    // From the summaries: each member of a prototype object, of what has escaped and of what stands above either, whose name reached code reads or calls on any value. Where reached code reads under a computed key, each such member.
    // Those rows have no witness. A computed key, the name of a protocol or a name that the text of the library alone spells finds no function of which callOrReadOfBuiltinIsEvent() holds. A name that text of the program reads or calls does: `f.constructor`.
    std::span<const BuiltinUseRecord> builtinUses() const LIFETIME_BOUND { return m_tables.builtinUses.span(); }
    // Must be true, as builtinUses(): the stubs of foreign calls are left out on its word. A row whose witness stands in a body, be it the escape of a holder.
    bool isUsedByOperation(BuiltinID id) const
    {
        const BuiltinUseRecord* use = findFactRow(builtinUses(), id.index(), &BuiltinUseRecord::builtin);
        return use && hasFunction(use->witness);
    }
    // Must be true: a feature that is not listed is removed from the executable. The features (AOTBuiltinFeatures.h) of the built-ins of builtinUses().
    std::span<const FeatureUseRecord> featureUses() const LIFETIME_BOUND { return m_tables.featureUses.span(); }
    // Must be true: the data of a locale that no record asks for is left out. One record of LocaleUseKind::Unknown claims nothing.
    // From the scan: each call of a built-in of builtinUses() that goes by a locale: Constant where the tag is a constant text, HostDefault where none is passed, else Unknown. Unknown too for one that has BuiltinUse::Escaped, whose calls nobody sees.
    std::span<const LocaleUse> localeUses() const LIFETIME_BOUND { return m_tables.localeUses.span(); }
    // Claims nothing: true. No: no unseen code can run, no body uses a member of RegExp that changes on its own (`RegExp.$1`, `lastMatch`), and none reads the name of one off a value that the look does not follow.
    bool mayReadLegacyRegExpStatics() const { return m_tables.program.mayReadLegacyRegExpStatics; }

    // ---- Modules
    // Claims nothing: true.
    bool isPathReadByOperation(ModuleID id) const { return !m_tables.modulesWhosePathNoOperationReads.get(id.index()); }
    // Null: stores behind the start of the program are seen to bring no function, or several. No proof: whoever goes by it tests for what it names.
    const FactTables::FunctionGivenToVariable* findFunctionGivenTo(VariableID id) const { return findFactRow(m_tables.functionsGivenToVariables.span(), id.index(), &FactTables::FunctionGivenToVariable::variable); }
    // Somewhere what a call of the function gives is looked up in a Map or a Set. A guess at what pays: a text that the function joins and returns is made flat and hashed.
    bool resultIsReadAsKey(FunctionID id) const { return m_tables.functionsWhoseResultIsReadAsKey.get(id.index()); }

    // ---- The program
    // Claims nothing: true. False only where no record of unseenCode() admits JavaScript.
    bool unseenCodeCanRun() const { return m_tables.program.unseenCodeCanRun; }
    // Claims nothing: true.
    bool unseenScriptsCanRun() const { return m_tables.program.unseenScriptsCanRun; }
    // Must be true: the run time turns away the texts and the loads that no record lets in: imageFlagsAdmittedBy() of image/AOTImageBuilder.cpp.
    // From the scan: DirectEval, each load of a module by a name that is no constant, and each built-in of builtinUses() of which callOrReadOfBuiltinIsEvent() holds and that is called, constructed or escaped.
    // From the records: a module that is imported and not compiled, the option that admits code.
    // Beside a record that admits JavaScript stands one of a load by name: such code has import().
    std::span<const UnseenCodeRecord> unseenCode() const LIFETIME_BOUND { return m_tables.program.unseenCode.span(); }
    // Must be true: the front end hears of native code that the program loads from this list alone. From the scan: each built-in of builtinUses() that BuiltinTable::loadersOfMachineCode() lists, as callee or as named, and that is called, constructed or escaped.
    std::span<const UnseenCodeRecord> loadsOfMachineCode() const LIFETIME_BOUND { return m_tables.program.loadsOfMachineCode.span(); }
    // Claims nothing: false.
    bool hasOneInstance() const { return m_tables.program.hasOneInstance; }
    // Claims nothing: true, all three.
    bool findsFilesOfProgramByName() const { return m_tables.program.findsFilesOfProgramByName; }
    bool reachedFunctionsCountUnreadCode() const { return m_tables.program.reachedFunctionsCountUnreadCode; }
    bool matchesRegExps() const { return m_tables.program.matchesRegExps; }
    // Must be true, both ways: the run time takes a store into Bun.main on a yes and refuses it on a no.
    // From the records: yes only where Program::replaceableBuiltins lists the member. Everywhere else the front end has decided in the code which file is the main module.
    bool storesPathOfMainModule() const { return m_tables.program.storesPathOfMainModule; }
    // Both claim nothing: empty. A text that is not listed is compiled when the program runs, and one that never arrives costs its bytes. From workOutRegExpTexts(). Empty with BuiltinCalls withheld.
    std::span<const RegExpTextInSight> regExpTextsInSight() const LIFETIME_BOUND { return m_tables.program.regExpTextsInSight.span(); }
    std::span<const RegExpTextWorkedOut> regExpTextsWorkedOut() const LIFETIME_BOUND { return m_tables.program.regExpTextsWorkedOut.span(); }
    // Must be true: the entry file of a worker that is not listed is not in the executable.
    // From the scan: each construction of Worker, named by binding, whose first argument is a constant text.
    std::span<const WorkerEntry> workerEntries() const LIFETIME_BOUND { return m_tables.program.workerEntries.span(); }
    // Must be true: the lowest place that starts a worker with anything else, and Worker wherever it has BuiltinUse::Escaped. Nothing says that workerEntries() and urlsThatStartWorkers are all there is.
    // An escape without a place counts only where an operation names Worker or unseenCodeCanRun(): `value.constructor` is Worker only once somebody has made one.
    std::optional<Witness> workerStartWithoutName() const { return m_tables.program.workerStartWithoutName; }
    // Must be true: code of the library that is lacked and not listed is not in the executable.
    // From the scan: what BuiltinTable::calledFunctionsOf() names for a built-in of builtinUses(), where Program::runtimeFunctions has no entry for it.
    // A record stops the build in the front end, so the three lists leave out what hangs on a condition, and for a built-in that no operation names all but what every program holds.
    std::span<const RuntimeFunctionID> libraryFunctionsLacked() const LIFETIME_BOUND { return m_tables.program.libraryFunctionsLacked.span(); }

    // ---- --explain
    // Claims nothing: false.
    bool tracesUnknownValues() const { return !m_tables.originsOfUnknown.isEmpty(); }
    // Claims nothing: UnprovenCause::Unknown.
    UnknownOrigin originOfUnknownAt(const FactSubject& subject) const
    {
        const UnknownOriginOfSubject* row = findFactRow(m_tables.originsOfUnknown.span(), subject, &UnknownOriginOfSubject::subject);
        return row ? row->origin : UnknownOrigin { };
    }
    // Claims nothing: noWitness.
    Witness witnessOf(ObjectID object, ObjectMutation mutation) const
    {
        for (const MutationWitness& row : std::ranges::equal_range(m_tables.witnessesOfMutations, object, { }, &MutationWitness::object)) {
            if (row.mutation == mutation)
                return row.witness;
        }
        return noWitness;
    }

private:
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

// ClassData::builtinBase: the constructor of ClassFacts::baseClass. Claims nothing: null.
BuiltinID builtinParentNamedByClassRecord(const Program&, const Facts&, LayoutID);

// AOTFacts.cpp also defines raisesEventWhenCalled() of AOTFactRecords.h and isMadeWithCodeOfLibrary() of builtins/AOTBuiltinMembers.h, which kept code calls.
// AOTFactsOfProgram.cpp defines hasFeatureGuard() of builtins/AOTBuiltinMembers.h.

// AOTFactsOfProgram.cpp. Every list that a query marks "Must be true", and unseenCodeCanRun() and unseenScriptsCanRun(), which follow from unseenCode().
// Out of the same look: the members that are locked at the start, and the report of the writes that a lock turns away (TS100014), as far as one body shows them.
// What lookAtBodies() has found in the body of each function that Link has reached, the names that its summary stores, and LinkTables::lookups. It goes by no mechanism.
struct InputsOfProgramFacts {
    const Program& program;
    const BodyIndexTable& bodies;
    const LooksAtBodies& looks;
    const ThinSummaries& summaries;
    const LinkTables& linked;
    BuildContext& build;
};
void computeFactsOfProgram(const InputsOfProgramFacts&, FactTables&);
// AOTFactsRegExpTexts.cpp. regExpTextsInSight() and regExpTextsWorkedOut(), out of the bodies and their summaries alone, on the thread that asks.
void workOutRegExpTexts(const Program&, const BodyIndexTable&, const ThinSummaries&, ProgramFactTables&);

// The indexes of bodies > scanBodies() > Families > link() > the tables. Null once the stage has reported an error.
// It owes the program's own tables: TypeTable::endStableNumbersOfPropertyKeys() in front of the indexes, and between them and the scan ObjectTable::reserveSites(), with no site: no object stands for an allocation site,
// Program::setConstructionStarts(BodyIndexTable::takeConstructionStarts()) and noteSubclassesOfBuiltinClasses().
JS_EXPORT_PRIVATE std::unique_ptr<Facts> computeFacts(Program&, BuildContext&, const FactsRequest& = { });
// For the tests: BreachOfClosure in AOTLinkFlow.h. A line for each, of the flow in each order of its worklist.
JS_EXPORT_PRIVATE void printBreachesOfClosureOfLink(PrintStream&, Program&, BuildContext&, Mechanisms);
// With the table that verifyBodies() has filled for this program and these loops: the scan builds only the indexes that it lacks. Null: the scan builds them all.
JS_EXPORT_PRIVATE std::unique_ptr<Facts> computeFacts(Program&, BuildContext&, const FactsRequest&, std::unique_ptr<BodyIndexTable>&&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
