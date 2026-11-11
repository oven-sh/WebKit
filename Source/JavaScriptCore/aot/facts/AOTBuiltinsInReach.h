/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFactRecords.h"
#include <memory>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>


namespace JSC { namespace AOT {

class BuildContext;
struct BuiltinCondition;

namespace Typed {

class BuilderOfProgramFacts;
class Program;
class ThinSummaries;
struct FactTables;
struct InputsOfProgramFacts;
struct LinkTables;
struct LookupsPastProgramObjects;

class LooksAtBodies;
struct DeleterOfLooksAtBodies {
    void operator()(LooksAtBodies*) const;
};
using OwnedLooksAtBodies = std::unique_ptr<LooksAtBodies, DeleterOfLooksAtBodies>;
OwnedLooksAtBodies makeEmptyLooksAtBodies(const BodyIndexTable&);
void lookAtBodies(LooksAtBodies&, const Program&, const ThinSummaries&, BuildContext&);
const BodyIndexTable& bodiesLookedAt(const LooksAtBodies&);
class FieldsOfNumbers;
struct LinkTables;
const FieldsOfNumbers& fieldsReadAsIntegersIn(const LooksAtBodies&);
const FieldsOfNumbers& fieldsReadForArithmeticIn(const LooksAtBodies&);
const FieldsOfNumbers& fieldsStoredWhatIsNoInt32In(const LooksAtBodies&);
FieldsOfNumbers fieldsStoredCountsIn(const LooksAtBodies&, const Program&, const LinkTables& linked);
const BitVector& keysStoredWhatIsNoInt32In(const LooksAtBodies&);

class BuiltinsInReach {
    WTF_MAKE_NONCOPYABLE(BuiltinsInReach);
public:
    BuiltinsInReach(const Program&, const LooksAtBodies&, const ThinSummaries&, BuildContext&);
    ~BuiltinsInReach();

    void noteFindingsIn(FunctionID);
    void noteFindingsIn(std::span<const FunctionID>);
    void noteWhatBodyMakes(FunctionID);
    bool hasNoFindingsIn(FunctionID) const;
    bool knowsWhatBodyMakes(FunctionID) const;
    void noteEvaluated(ModuleID);
    void noteNativeCodeMayHoldObjects();
    void noteNameRead(PropertyKeyID);
    void noteNameCalled(PropertyKeyID, bool mayPassOperands);
    void noteNameCalledByNativeCodeOffUnseenObjects(PropertyKeyID);
    void noteNameSpelledByProgram(PropertyKeyID);
    void noteReadUnderKeyNobodyKnows();
    void noteReadUnderKeyNobodyKnowsOff(BuiltinID canonicalHolder);
    void noteReadUnderNameThatLoopGave();
    void noteReadOfProgramUnderKeyNobodyKnows(FunctionID, ValueID);
    void noteRead(BuiltinID);
    void noteCalled(BuiltinID);
    void noteConstructed(BuiltinID);
    void noteCalledByCode(BuiltinID);
    void noteLetGo(BuiltinID);
    void noteCalledOutOfSight(BuiltinID);
    void noteCalledWithoutRow(BuiltinID);
    void noteCalledWithOperandsNobodyRead(BuiltinID);
    void noteRowApplied(BuiltinID canonical, BuiltinOverloadID);
    Vector<uint64_t> takeCallsWhoseRowsLinkTells();
    void noteCallLeftToUnseen(FunctionID caller, ValueID site);
    void noteCallMayRunCode(BuiltinID);
    void noteOperandMayMeetCondition(BuiltinID, BuiltinName toldUnder);
    void noteCalledAtSite(FunctionID caller, ValueID site, bool isConstruction, BuiltinID row);

    void settle(Vector<BuiltinID>& cameIntoReach, Vector<BuiltinID>& cameOutOfSight);
    Vector<BuiltinID> takeMembersWrittenAfterAll() { return std::exchange(m_membersWrittenAfterAll, { }); }
    struct NameOnHolder {
        BuiltinID holder;
        PropertyKeyID key;
        friend bool operator==(const NameOnHolder&, const NameOnHolder&) = default;
    };
    Vector<NameOnHolder> takeNamesLaidAfterAll() { return std::exchange(m_namesLaidAfterAll, { }); }

    bool mayBeHandedWhatIsNoTextNumberOrBoolean(BuiltinID canonical) const;
    bool hasCallThatMayRunCode(BuiltinID canonical) const;
    bool hasCallWhoseOperandMayMeet(const BuiltinCondition&, BuiltinID canonical) const;
    bool mayRun(BuiltinID canonical) const;
    bool mayRunOrHasObjects(BuiltinID canonical) const;
    bool isAskedForOtherNames(BuiltinID canonical) const;
    bool isOutOfSight(BuiltinID canonical) const;
    const BuiltinCondition* lightFormOf(BuiltinID canonical) const;
    bool loaderOfHostMayRun() const;
    bool hostIsAskedForValueOfModule() const;
    bool unseenCodeCanRun() const;
    bool loaderOfMachineCodeIsInUse() const;
    bool defersRuntimeNames() const;
    Vector<ModuleID> takeModulesSpelledInTexts();
    bool allowRuntimeNames();
    bool objectsMayHaveMethodCalledWithStartupLoads() const;

    bool takeForConstant(BuiltinID member);
    void takeForUnlaid(BuiltinID canonicalHolder, PropertyKeyID);

    void noteWrite(BuiltinID holder, PropertyKeyID, bool onlyDeletes = false);
    void noteHandedWhatIsNoTextNumberOrBoolean(BuiltinID holder);
    void noteWriteThroughLostBuiltins(PropertyKeyID, bool isByFileOfProduct);
    void noteWriteThroughUnseenByProgram(PropertyKeyID, bool isByFileOfProduct);
    void noteNameWrittenThroughUnseenByLibrary(PropertyKeyID);
    void noteReadThroughLostBuiltins(PropertyKeyID);
    void noteClimbFromLostBuiltins();
    void noteClimbFromUnseenObjects();
    void noteLost(BuiltinID);
    void noteAtUnseen(BuiltinID);
    void noteOptionThatRunsCodePassed(BuiltinID);
    void noteLostBuiltinsReachUnseen();
    void noteInstanceAtUnseen(BuiltinClassID);
    void noteInstanceHasEscaped(BuiltinClassID);
    void noteInstanceHandedToFormatter(BuiltinClassID);
    void noteObjectHandedToFormatter(bool isWalked);
    void noteInstancesLoseTheirConstructor();
    void noteTypeHandedToHandler(BuiltinTypeID);
    void recordUsedViaFlow(BuiltinID);
    void noteFoundWhereFlowBroughtHolder(BuiltinID, FunctionID reader);
    void noteStandsAboveClass(BuiltinID, FunctionID constructor);
    void noteEntryHandsHostNothing(uint32_t entry);
    void noteHostMayFindSomethingAt(uint32_t entry);
    void noteHostStartsByCallInSight(uint32_t entry);
    std::span<const FunctionID> functionsThatAnswerEveryNameOnInstances() const;
    void noteInstancesAnswerEveryNameWithObjects();
    bool isWorthFollowingToWrites(BuiltinID canonical) const { return m_builtinsWorthFollowingToWrites.quickGet(canonical.index()); }

    LookupsPastProgramObjects takeLookups();
    void putBackLookups(const LookupsPastProgramObjects&);

    bool mayHaveNameLaid(BuiltinID canonicalHolder, PropertyKeyID) const;
    bool mayStillBeTakenForConstant(BuiltinID member) const;

private:
    std::unique_ptr<FactTables> m_tables;
    std::unique_ptr<LinkTables> m_linked;
    std::unique_ptr<InputsOfProgramFacts> m_inputs;
    std::unique_ptr<BuilderOfProgramFacts> m_builder;
    BitVector m_builtinsWorthFollowingToWrites;
    Vector<BuiltinID> m_membersWrittenAfterAll;
    Vector<NameOnHolder> m_namesTakenForUnlaid;
    Vector<NameOnHolder> m_namesLaidAfterAll;
    bool m_wasToldSomethingNew { true };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
