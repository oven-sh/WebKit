/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBlockFrequencies.h"
#include "AOTBodyIndex.h"
#include "AOTCFG.h"
#include "AOTDiagnostics.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsDeclared.h"
#include "AOTExpandOwnKeysInternal.h"
#include "AOTGenericCopy.h"
#include "AOTHeat.h"
#include "AOTNaturalLoops.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTPropertyAccess.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeTestReport.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

TestPlaceability placeabilityOf(FunctionAnalyses& analyses, const Value& test)
{
    if (provesOf(test.opcode) == Proof::None)
        return TestPlaceability::Unknown;

    Effects bound = declaredBoundOf(test.opcode);
    if (!bound.writes.isEmpty() || bound.mayExitSideways() || bound.mayCollect)
        return TestPlaceability::Unknown;

    std::optional<unsigned> guard = OperandPosition::guard(test.opcode);
    size_t numberOfValues = analyses.function().values.size();
    for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
        ValueID operand = test.operand(operandIndex);
        bool isAsExpected = operandIndex == guard ? !operand : operand && operand.index() < numberOfValues;
        if (!isAsExpected)
            return TestPlaceability::Unknown;
    }
    return TestPlaceability::CanBePlaced;
}

TestConditions::TestConditions(FunctionAnalyses& analyses)
    : m_identities(analyses.identities())
{
}

static uint32_t hashOfCanonicalTest(const Value& test)
{
    Hasher hasher;
    add(hasher, static_cast<uint32_t>(test.opcode));
    test.addPayloadTo(hasher);
    add(hasher, test.numberOfOperands());
    for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex)
        add(hasher, test.operand(operandIndex).index());
    return hasher.hash();
}

TestConditionID TestConditions::add(const Value& test)
{
    Value canonical = canonicalize(test);
    uint32_t hash = hashOfCanonicalTest(canonical);
    if (std::optional<TestConditionID> existing = findCanonical(canonical, hash))
        return *existing;

    Proof proof = provesOf(canonical.opcode);
    RELEASE_ASSERT(proof != Proof::None);
    ValueID subject = canonical.numberOfOperands() ? canonical.operand(testedOperandIndex) : ValueID();
    uint32_t index = static_cast<uint32_t>(m_conditions.size());

    TestCondition condition { WTF::move(canonical), proof, subject, { } };
    if (condition.lastsForStretch()) {
        condition.dependsOn = locationsReadBy(condition.test);
        for (const MemoryLocation& location : condition.dependsOn)
            m_heapsDependedOn.add(location.heap());
        m_stretchConditions.set(index);
    }

    m_conditions.append(WTF::move(condition));
    m_index.add(hash, index);
    return TestConditionID(index);
}

std::optional<TestConditionID> TestConditions::find(const Value& test) const
{
    Value canonical = canonicalize(test);
    return findCanonical(canonical, hashOfCanonicalTest(canonical));
}

Value TestConditions::canonicalize(const Value& test) const
{
    Value canonical = test;
    std::optional<unsigned> guard = OperandPosition::guard(test.opcode);
    for (uint32_t operandIndex = 0; operandIndex < canonical.numberOfOperands(); ++operandIndex) {
        if (operandIndex == guard)
            canonical.setOperand(operandIndex, ValueID());
        else if (ValueID operand = canonical.operand(operandIndex))
            canonical.setOperand(operandIndex, m_identities.identityOf(operand));
    }
    // The origin of a test does not change its result: a test written by the program proves what a test placed before an access proves.
    if (Value::opcodesWithIsFromProgram.contains(canonical.opcode))
        canonical.setIsFromProgram(false);
    canonical.type = TypeID();
    canonical.setExpected({ });
    canonical.representation = Representation::Unchosen;
    canonical.setOrigin({ Instruction::noPosition, 0 });
    return canonical;
}

std::optional<TestConditionID> TestConditions::findCanonical(const Value& test, uint32_t hash) const
{
    std::optional<uint32_t> position = m_index.find(hash, [&](uint32_t candidate) {
        const Value& other = m_conditions[candidate].test;
        if (other.opcode != test.opcode || !other.hasSamePayloadAs(test) || other.numberOfOperands() != test.numberOfOperands())
            return false;
        for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
            if (other.operand(operandIndex) != test.operand(operandIndex))
                return false;
        }
        return true;
    });
    return position ? std::optional(TestConditionID(*position)) : std::nullopt;
}

AllocationProof allocationProofOf(const Program& program, const Function& function, const Value& allocation, const TestCondition& condition)
{
    return allocationProves(program, function, allocation, condition.test) ? AllocationProof::Proves : AllocationProof::Unknown;
}

static_assert(OperandIndex::TypeTest::value == testedOperandIndex);
static_assert(OperandIndex::ShapeTest::object == testedOperandIndex);
static_assert(OperandIndex::IsPristine::value == testedOperandIndex);
static_assert(OperandIndex::HasArrayStorage::array == testedOperandIndex);
static_assert(OperandIndex::IsClosureOf::value == testedOperandIndex);
static_assert(OperandIndex::IsEnumeratorCurrent::enumerator == testedOperandIndex);

TestSiteReporter::TestSiteReporter(FunctionPassContext& context, TestSiteCandidates wantedCandidates)
    : m_context(context)
    , m_wantedCandidates(wantedCandidates)
    , m_isClaimed(context.function.values.size())
    , m_awaitsExpansion(context.function.values.size())
{
    // A test stays where it was placed, and an operation that ExpandIteration has yet to expand, later in the same round, is unknown code: in a cycle with one, no proof for a stretch lasts a turn.
    auto isUnexpanded = [](const Value& value) {
        return removedByOf(value.opcode) == PassID::ExpandIteration;
    };
    const Function& function = context.function;
    // A list of names that ExpandOwnKeys expands later in the same round is a literal by then, and reads of a literal need no test.
    if (Vector<ValueID, 4> lists = findKeyListsToExpand(context); !lists.isEmpty()) {
        for (BlockID block : analyses().cfg().reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                function.at(id).forEachOperand([&](ValueID operand, uint32_t) {
                    for (ValueID read : CastChain(function, operand)) {
                        if (lists.contains(read))
                            m_awaitsExpansion.set(id.index());
                    }
                });
            }
        }
        m_context.hasUsesAwaitingExpansion = !m_awaitsExpansion.isEmpty();
    }
    if (!function.values.containsIf(isUnexpanded))
        return;

    const CFG& cfg = analyses().cfg();
    const BlockCycles& cycles = analyses().blockCycles();
    BitVector cyclesWithUnexpandedOperation;
    BitVector isReadByUnexpandedOperation(function.values.size());
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (!isUnexpanded(value))
                continue;
            if (std::optional<unsigned> cycle = cycles.outermostCycleOf(block))
                cyclesWithUnexpandedOperation.set(*cycle);
            value.forEachOperand([&](ValueID operand, uint32_t) {
                for (ValueID read : CastChain(function, operand))
                    isReadByUnexpandedOperation.set(read.index());
            });
        }
    }

    for (BlockID block : cfg.reversePostOrder()) {
        std::optional<unsigned> cycle = cycles.outermostCycleOf(block);
        if (!cycle || !cyclesWithUnexpandedOperation.get(*cycle))
            continue;
        for (ValueID id : function.at(block).values) {
            if (!isReadByUnexpandedOperation.get(id.index()))
                m_awaitsExpansion.set(id.index());
        }
    }
    m_context.hasUsesAwaitingExpansion = !m_awaitsExpansion.isEmpty();
}

std::optional<Type> TestSiteReporter::expectedPartOf(ValueID operand, uint16_t droppedPrimitives) const
{
    const Value& value = function().at(operand);
    TypeID expectedType = value.expectedOrType().predicted();
    if (expectedType == value.type)
        return std::nullopt;

    const Type& type = typeOf(operand);
    Type expected = types().at(expectedType).withoutPrimitives(droppedPrimitives).intersectWith(type, types());
    if (expected.isNever() || type.isSubsetOf(expected, program()))
        return std::nullopt;
    return expected;
}

bool FailedConditions::leavesOpen(const Value& testOfMemberConditions, BlockID at, NOESCAPE const ScopedLambda<bool(const MemberConditionRecord&)>& isStateOfProgram) const
{
    if (m_failedAtHead.isEmpty() || !m_failedAtHead[at.index()])
        return false;
    auto asked = testOfMemberConditions.conditions();
    auto isAsked = [&](const MemberConditionRecord& condition) {
        return std::ranges::contains(asked, condition);
    };
    bool isLeftOpen = false;
    for (size_t condition : *m_failedAtHead[at.index()]) {
        const Value& failed = m_conditions.at(TestConditionID(condition)).test;
        if (failed.opcode != Opcode::AreMembersUnchanged)
            continue;
        auto listed = failed.conditions();
        if (std::ranges::all_of(listed, isAsked))
            return false;
        isLeftOpen |= std::ranges::any_of(listed, [&](const MemberConditionRecord& other) {
            return !isAsked(other) && isStateOfProgram(other);
        });
        isLeftOpen |= std::ranges::any_of(asked, [&](const MemberConditionRecord& state) {
            return isStateOfProgram(state) && std::ranges::any_of(listed, [&](const MemberConditionRecord& other) {
                return other.kind == MemberConditionKind::MemberUnchanged && other.layout == state.layout && other.side == state.side && other.key == state.key;
            });
        });
    }
    return isLeftOpen;
}

bool FailedConditions::mayHaveFailedOverStateOfProgram(BlockID at, NOESCAPE const ScopedLambda<bool(const MemberConditionRecord&)>& isStateOfProgram) const
{
    if (m_failedAtHead.isEmpty() || !m_failedAtHead[at.index()])
        return false;
    for (size_t condition : *m_failedAtHead[at.index()]) {
        const Value& failed = m_conditions.at(TestConditionID(condition)).test;
        if (failed.opcode == Opcode::AreMembersUnchanged && std::ranges::any_of(failed.conditions(), [&](const MemberConditionRecord& listed) { return isStateOfProgram(listed); }))
            return true;
    }
    return false;
}

bool TestSiteReporter::isStateOfProgram(const MemberConditionRecord& condition) const
{
    return failsOnceAccessorIsDefined({ program(), &m_context.inputs.flow }, { condition.kind, condition.layout, condition.side, condition.key });
}

bool TestSiteReporter::mayRunEveryTimeBehindFailedTest(BlockID block)
{
    auto isState = [&](const MemberConditionRecord& condition) {
        return isStateOfProgram(condition);
    };
    if (!m_testsStateOfProgram) {
        m_testsStateOfProgram = analyses().opcodesOfValues().contains(Opcode::AreMembersUnchanged) && std::ranges::any_of(analyses().cfg().reversePostOrder(), [&](BlockID candidate) {
            const Terminator& terminator = function().at(candidate).terminator;
            if (!terminator.isConditionalBranch())
                return false;
            const Value& test = function().at(terminator.conditionOperand());
            return test.opcode == Opcode::AreMembersUnchanged && std::ranges::any_of(test.conditions(), isState);
        });
    }
    if (!*m_testsStateOfProgram || function().at(block).frequency.predicted() == BlockFrequency::Rare)
        return false;
    return failedConditions().mayHaveFailedOverStateOfProgram(block, ScopedLambda<bool(const MemberConditionRecord&)>(isState));
}

const FailedConditions& TestSiteReporter::failedConditions() const
{
    if (!m_failedConditions)
        m_failedConditions.emplace(analyses());
    return *m_failedConditions;
}

EarlierTest TestSiteReporter::findEarlierTest(const Value& test) const
{
    if (analyses().findEarlierTest(test, m_point.block) == EarlierTest::Failed) {
        m_hasFailedEarlierTest = true;
        return EarlierTest::Failed;
    }

    // Every failing edge of a chain of tests enters one generic copy, so no single failed edge dominates the copy.
    if (function().at(m_point.block).frequency.predicted() == BlockFrequency::Normal || !failedConditions().mayHaveFailed(test, m_point.block))
        return EarlierTest::Unknown;
    m_hasFailedEarlierTest = true;
    return EarlierTest::Failed;
}

bool TestSiteReporter::mayHaveFailedTestOf(ValueID value) const
{
    return function().at(m_point.block).frequency.predicted() != BlockFrequency::Normal || analyses().hasFailedTestOf(value);
}

bool TestSiteReporter::takesAwaySitesInColdCode()
{
    if (m_blockOfHeatClass != m_point.block) {
        m_blockOfHeatClass = m_point.block;
        m_blockIsCold = heatClassOf(m_context, m_point.block).predicted() == HeatClass::Cold;
    }
    return m_blockIsCold;
}

// A catch clause counts as rare because a throw leads to it. It is code of the program all the same, no arm of a failed test, and a program that throws to leave a loop runs it as often as the loop.
auto TestSiteReporter::candidates() -> std::span<const Candidate>
{
    if (m_candidates)
        return m_candidates->span();
    m_candidates.emplace();
    const Function& function = m_context.function;
    const BlockFrequencies& frequencies = m_context.analyses.blockFrequencies();
    for (BlockID block : m_context.analyses.cfg().reversePostOrder()) {
        if (frequencies.classOf(block).predicted() == BlockFrequency::Rare && !isWrittenInCatchClause(block) && !mayRunEveryTimeBehindFailedTest(block))
            continue;
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            Opcode operation = function.at(values[index]).opcode;
            if (!testSiteOpcodes.contains(operation) || m_awaitsExpansion.quickGet(values[index].index()))
                continue;
            if (m_wantedCandidates == TestSiteCandidates::AccessesToObjectsMadeHere && !isAccessToObjectMadeHere(function.at(values[index])))
                continue;
            m_candidates->append(Candidate { values[index], { block, index }, operation });
        }
    }
    return m_candidates->span();
}

bool TestSiteReporter::isAccessToObjectMadeHere(const Value& access) const
{
    if (!isNamedAccess(access.opcode))
        return false;
    const Value& receiver = function().at(analyses().identityOf(access.operand(*receiverOf(access.opcode))));
    return receiver.opcode == Opcode::NewObject && receiver.layout() && allocatesOwnLayout(program(), receiver);
}

bool TestSiteReporter::isWrittenInCatchClause(BlockID block)
{
    if (!m_blocksOfCatchClauses) {
        const CFG& cfg = analyses().cfg();
        m_blocksOfCatchClauses.emplace(function().blocks.size());
        for (BlockID candidate : cfg.hasHandlers() ? cfg.reversePostOrder() : std::span<const BlockID> { }) {
            if (function().at(candidate).frequency.predicted() != BlockFrequency::Normal)
                continue;
            auto comesOutOfClause = [&](Edge edge) {
                return !edge.isHandlerEdge() && (m_blocksOfCatchClauses->quickGet(edge.from.index()) || cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(candidate));
            };
            if (function().at(candidate).role == BlockRole::CatchHandler || (candidate != entryBlock && std::ranges::all_of(cfg.incomingEdges(candidate), comesOutOfClause)))
                m_blocksOfCatchClauses->quickSet(candidate.index());
        }
    }
    return m_blocksOfCatchClauses->quickGet(block.index());
}

void TestSiteReporter::keepPlanOfUse(PlannedAccess&& planned)
{
    if (m_plannedAccessOfValue.isEmpty())
        m_plannedAccessOfValue.fill(UINT32_MAX, function().values.size());
    m_plannedAccessOfValue[m_use.index()] = m_plannedAccesses.size();
    m_plannedAccesses.append(WTF::move(planned));
}

std::optional<PlannedAccess> TestSiteReporter::takePlanOfUse(bool isOnlyCalled)
{
    if (m_plannedAccessOfValue.isEmpty())
        return std::nullopt;
    uint32_t position = std::exchange(m_plannedAccessOfValue[m_use.index()], UINT32_MAX);
    if (position == UINT32_MAX || m_plannedAccesses[position].isOnlyCalled.value_or(isOnlyCalled) != isOnlyCalled)
        return std::nullopt;
    return WTF::move(m_plannedAccesses[position]);
}

// The tests of a site are made in order.
bool TestSiteReporter::hasFailedTest(std::span<const Value> tests) const
{
    Vector<ValueID, 2> subjectsTestedForOtherType;
    for (const Value& test : tests) {
        Proof proof = provesOf(test.opcode);
        ValueID subject = test.numberOfOperands() ? analyses().identityOf(test.operand(testedOperandIndex)) : ValueID();
        bool isMadeOnOtherObject = readsOnlySubjectOf(proof) && subjectsTestedForOtherType.contains(subject);
        EarlierTest earlier = isMadeOnOtherObject ? analyses().findEarlierTest(test, m_point.block) : findEarlierTest(test);
        if (earlier == EarlierTest::Failed)
            return true;
        if (proof == Proof::TestedType)
            subjectsTestedForOtherType.append(subject);
    }
    return false;
}

AOT_THRESHOLD(size_t, minFieldsOfBagOfOptions, 24);

Expected<bool> asksForMoreThanLayoutsTell(const Program& program, const Function& function, const TypeArena& types, const Value& test)
{
    if (test.opcode != Opcode::TypeTest)
        return false;
    const Type& tested = types.at(test.testedType());
    if (!tested.layouts)
        return false;
    // `handle.flags`, `handle.end()`, where the handle is an object of the host or a record that stands in for one: no test of layouts lets the object of the host pass, and it is as expected as the record.
    if (tested.classes.isEmpty() && !program.expectedOrTypeOf(function, test.operand(testedOperandIndex)).predicted().classes.isEmpty())
        return true;
    // `compilerOptions.strict`: a record type of dozens of fields, every one of them optional, describes a bag of options.
    return std::ranges::all_of(program.at(tested.layouts).completeMembers(), [&](LayoutID id) {
        const Layout& layout = program.at(id);
        return layout.kind == LayoutKind::Record && layout.fields.size() >= minFieldsOfBagOfOptions && std::ranges::none_of(layout.fields, [](const Field& field) {
            return field.flags.containsAny({ FieldFlags::IsRequired, FieldFlags::IsRequiredUntilDeleted });
        });
    });
}

// `node.self = node`: a use may name one value twice, a branch may have narrowed only one of the names, and each name may get a test of its own. What passes the tests takes the place of every name, so it has the types of all names and passes all tests of the value.
static bool canPassWithOtherTests(const TestSiteReporter& reporter, std::span<const Value> tests, const Value& test)
{
    if (!provesForever(provesOf(test.opcode)))
        return true;
    const Value& use = reporter.useValue();
    ValueID subject = reporter.analyses().identityOf(test.operand(testedOperandIndex));
    auto isAboutSubject = [&](const Value& other) {
        return provesForever(provesOf(other.opcode)) && reporter.analyses().identityOf(other.operand(testedOperandIndex)) == subject;
    };
    // The first test of a value answers for all of them.
    if (&*std::ranges::find_if(tests, isAboutSubject) != &test)
        return true;

    ValueID tested = test.operand(testedOperandIndex);
    Type type = reporter.typeOf(tested);
    bool hasOtherName = false;
    use.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!operand || operand == tested || use.operandKind(operandIndex) == OperandKind::Guard || reporter.analyses().identityOf(operand) != subject)
            return;
        hasOtherName = true;
        type = type.intersectWith(reporter.typeOf(operand), reporter.types());
    });
    if (!hasOtherName && std::ranges::count_if(tests, isAboutSubject) == 1)
        return true;
    for (const Value& other : tests) {
        if (isAboutSubject(other))
            type = narrowTypeByTest({ reporter.program(), reporter.function(), reporter.types() }, type, other, testedOperandIndex, EdgeSide::Taken);
    }
    return !type.isNever();
}

void TestSiteReporter::report(std::span<const Value> tests, const Effects& effectsWhenPassed, TestOutcome outcome)
{
    RELEASE_ASSERT(m_use && !m_didReport && !tests.empty());
    for (const Value& test : tests)
        RELEASE_ASSERT(placeabilityOf(analyses(), test) == TestPlaceability::CanBePlaced);
    if (std::ranges::any_of(tests, [&](const Value& test) { return asksForMoreThanLayoutsTell(program(), function(), types(), test).predicted() || !canPassWithOtherTests(*this, tests, test); }))
        return;

    // A block that is marked is a slow path: a generic copy, the arm behind a failed test. A block that only the estimate of a branch calls unlikely is typed code like the rest, and the estimate may be wrong.
    bool isInTypedCode = function().at(m_point.block).frequency.predicted() == BlockFrequency::Normal;
    if (!isInTypedCode && !m_hasFailedEarlierTest) {
        const FailedConditions& failed = failedConditions();
        auto isState = [&](const MemberConditionRecord& condition) {
            return isStateOfProgram(condition);
        };
        bool isAboutSubjectOfFailedTest = std::ranges::any_of(tests, [&](const Value& test) {
            if (test.opcode == Opcode::AreMembersUnchanged)
                return failed.leavesOpen(test, m_point.block, ScopedLambda<bool(const MemberConditionRecord&)>(isState));
            return test.numberOfOperands() && failed.mayHaveFailedTestOf(analyses().identityOf(test.operand(testedOperandIndex)), m_point.block);
        });
        if (!isAboutSubjectOfFailedTest)
            return;
    }
    if (hasFailedTest(tests))
        return;

    m_sites.sites.append(TestSite { m_rule, m_use, m_point, static_cast<uint32_t>(m_sites.tests.size()), static_cast<uint32_t>(tests.size()), effectsWhenPassed, outcome });
    m_sites.tests.append(tests);
    claimUse();
    m_didReport = true;
}

void TestSiteReporter::moveTo(TestSiteRuleID rule, ValueID use, ProgramPoint point)
{
    m_rule = rule;
    m_use = use;
    m_point = point;
    m_didReport = false;
    m_hasFailedEarlierTest = false;
}

ValueID TestSiteRewriter::insertBeforeUse(Value&& value)
{
    builder().setOrigin(useValue().origin());
    ValueID id = builder().insertBefore(point(), WTF::move(value));
    m_inputs.changedValues.append(id);
    return id;
}

std::span<const TestSiteRule> testSiteRules()
{
    static constexpr TestSiteRule rules[] {
#define AOT_DEFINE_TEST_SITE_RULE(id, report, rewrite) { TestSiteRuleID::id, #id ""_s, report, rewrite },
        FOR_EACH_AOT_TEST_SITE_RULE(AOT_DEFINE_TEST_SITE_RULE)
#undef AOT_DEFINE_TEST_SITE_RULE
    };
    return rules;
}

const TestSiteRule& ruleOf(TestSiteRuleID id)
{
    return testSiteRules()[static_cast<unsigned>(id)];
}

using SubjectAndType = std::pair<ValueID, TypeID>;

static bool canBeDecidedEarlier(TestSiteReporter& reporter, const Value& test)
{
    if (test.opcode != Opcode::TypeTest)
        return false;
    const Type& type = reporter.typeOf(test.valueOperand());
    const Type& tested = reporter.types().at(test.testedType());
    return type.overlaps(tested, reporter.program()) && !type.isSubsetOf(tested, reporter.program())
        && placeabilityOf(reporter.analyses(), makeTypeTest(test.valueOperand(), test.testedType())) == TestPlaceability::CanBePlaced;
}

static SubjectAndType subjectAndTypeOf(TestSiteReporter& reporter, const Value& test)
{
    return { reporter.analyses().identityOf(test.valueOperand()), test.testedType() };
}

static const NaturalLoop* findLoopAround(TestSiteReporter& reporter, BlockID block)
{
    if (!reporter.analyses().cfg().hasCycle())
        return nullptr;
    const NaturalLoops& loops = reporter.analyses().naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(block);
    while (loop && isClosedOnlyByWaysBack(reporter.analyses(), *loop).predicted())
        loop = loops.innerMostOuterLoop(*loop);
    return loop;
}

static bool isDefinedOutsideLoop(TestSiteReporter& reporter, ValueID value, const NaturalLoop& loop)
{
    ValueLocation location = reporter.analyses().useLists().locationOf(value);
    return location.placement != ValuePlacement::Detached && !reporter.analyses().naturalLoops().belongsTo(location.block, loop);
}

// The blocks of a loop that hold a use that is tested. A loop is walked once, however many tests ask about it, and a claim is looked at once, however many loops there are:
// what is claimed behind the walk goes to the loops around it.
class TestedUsesInLoops {
public:
    explicit TestedUsesInLoops(TestSiteReporter& reporter)
        : m_reporter(reporter)
    {
    }

    // What stands behind the passing edge of the test does not run in the copy, where the test has failed.
    bool holdsTestedUseWhereTestFails(const NaturalLoop& loop)
    {
        FunctionAnalyses& analyses = m_reporter.analyses();
        const Terminator& terminator = m_reporter.function().at(m_reporter.point().block).terminator;
        BlockID wherePassed;
        if (terminator.isConditionalBranch() && terminator.conditionOperand() == m_reporter.use() && analyses.hasSingleIncomingEdge(terminator.taken().block))
            wherePassed = terminator.taken().block;

        const BlocksOfLoop& blocks = blocksOf(loop);
        auto runsWhereTestFails = [&](BlockID block) {
            return !wherePassed || !analyses.dominators().dominates(wherePassed, block);
        };
        return std::ranges::any_of(blocks.withAccess, runsWhereTestFails) || std::ranges::any_of(blocks.withClaimedUse, runsWhereTestFails);
    }

private:
    struct BlocksOfLoop {
        Vector<BlockID> withAccess;
        Vector<BlockID> withClaimedUse;
    };

    void takeInNewClaims()
    {
        const NaturalLoops& loops = m_reporter.analyses().naturalLoops();
        std::span<const ValueID> claimed = m_reporter.claimedUses();
        for (; m_numberOfClaimsSeen < claimed.size(); ++m_numberOfClaimsSeen) {
            ValueLocation location = m_reporter.analyses().useLists().locationOf(claimed[m_numberOfClaimsSeen]);
            if (location.placement != ValuePlacement::InBlock || m_holdsClaimedUse.set(location.block.index()))
                continue;
            for (const NaturalLoop* loop = loops.innerMostLoopOf(location.block); loop; loop = loops.innerMostOuterLoop(*loop)) {
                if (auto found = m_blocksOfLoops.find(loop); found != m_blocksOfLoops.end())
                    found->value.withClaimedUse.append(location.block);
            }
        }
    }

    const BlocksOfLoop& blocksOf(const NaturalLoop& loop)
    {
        takeInNewClaims();
        const Function& function = m_reporter.function();
        auto result = m_blocksOfLoops.add(&loop, BlocksOfLoop { });
        BlocksOfLoop& blocks = result.iterator->value;
        if (!result.isNewEntry)
            return blocks;
        for (unsigned index = 0; index < loop.size(); ++index) {
            const Block& block = function.at(loop.at(index));
            bool hasAccess = accessClassOf(block.terminator.kind) != AccessClass::None || std::ranges::any_of(block.values, [&](ValueID value) {
                return accessClassOf(function.at(value).opcode) != AccessClass::None;
            });
            if (hasAccess)
                blocks.withAccess.append(loop.at(index));
            if (m_holdsClaimedUse.get(loop.at(index).index()))
                blocks.withClaimedUse.append(loop.at(index));
        }
        return blocks;
    }

    TestSiteReporter& m_reporter;
    HashMap<const NaturalLoop*, BlocksOfLoop> m_blocksOfLoops;
    BitVector m_holdsClaimedUse;
    size_t m_numberOfClaimsSeen { 0 };
};

enum class RarerSide : uint8_t { Neither, WherePassed, WhereFailed };

static RarerSide rarerSideOf(TestSiteReporter& reporter)
{
    const Terminator& terminator = reporter.function().at(reporter.point().block).terminator;
    if (!terminator.isConditionalBranch() || terminator.conditionOperand() != reporter.use())
        return RarerSide::Neither;
    Expected<BlockFrequency> whenPassed = reporter.function().at(terminator.taken().block).frequency;
    Expected<BlockFrequency> whenFailed = reporter.function().at(terminator.notTaken().block).frequency;
    if (isRarerThan(whenPassed, whenFailed).predicted())
        return RarerSide::WherePassed;
    return isRarerThan(whenFailed, whenPassed).predicted() ? RarerSide::WhereFailed : RarerSide::Neither;
}

static std::optional<Value> testInFrontOf(TestSiteReporter& reporter, const Value& test, RarerSide rarerSide)
{
    if (rarerSide != RarerSide::WherePassed)
        return makeTypeTest(test.valueOperand(), test.testedType());
    Type rest = reporter.typeOf(test.valueOperand()).without(reporter.types().at(test.testedType()), reporter.types()).withoutClassProofs().withUntestableBuiltinsWidened();
    if (rest.overlaps(reporter.types().at(test.testedType()), reporter.program()))
        return std::nullopt;
    Value testForRest = makeTypeTest(test.valueOperand(), reporter.types().intern(WTF::move(rest)));
    if (placeabilityOf(reporter.analyses(), testForRest) != TestPlaceability::CanBePlaced)
        return std::nullopt;
    return testForRest;
}

// Behind a test that failed stands the copy of the failed tests, where nothing else is tested either.
static bool isSurpriseWhereItFails(TestSiteReporter& reporter, const Value& testInFront)
{
    Type expected = reporter.types().at(reporter.function().at(testInFront.valueOperand()).expectedOrType().predicted()).withoutClassProofs();
    return expected.isSubsetOf(reporter.types().at(testInFront.testedType()), reporter.program());
}

// A pass that splits a value by its type tests it at each use.
void reportRepeatedTests(TestSiteReporter& reporter)
{
    HashMap<SubjectAndType, unsigned> numberOfTests;
    BitVector canBeDecided;
    reporter.forEachCandidate({ Opcode::TypeTest }, [&](const Value& test) {
        if (!canBeDecidedEarlier(reporter, test))
            return;
        canBeDecided.set(reporter.use().index());
        ++numberOfTests.add(subjectAndTypeOf(reporter, test), 0).iterator->value;
    });
    if (canBeDecided.isEmpty())
        return;

    TestedUsesInLoops testedUses(reporter);
    reporter.forEachCandidate({ Opcode::TypeTest }, [&](const Value& test) {
        if (!canBeDecided.get(reporter.use().index()))
            return;
        SubjectAndType tested = subjectAndTypeOf(reporter, test);
        const NaturalLoop* loop = findLoopAround(reporter, reporter.point().block);
        bool isInvariantInLoop = loop && isDefinedOutsideLoop(reporter, tested.first, *loop);
        if (numberOfTests.get(tested) < 2 && !isInvariantInLoop)
            return;
        RarerSide rarerSide = rarerSideOf(reporter);
        std::optional<Value> testInFront = testInFrontOf(reporter, test, rarerSide);
        if (!testInFront)
            return;
        if (rarerSide != RarerSide::Neither || isSurpriseWhereItFails(reporter, *testInFront))
            reporter.report({ *testInFront }, Effects::none());
        else if (isInvariantInLoop && !testedUses.holdsTestedUseWhereTestFails(*loop))
            reporter.report({ *testInFront }, Effects::none(), TestOutcome::IsUnpredictable);
    });
}

void rewriteRepeatedTest(TestSiteRewriter&)
{
}

static void appendTypeTestRecord(const Program& program, FunctionID id, const Instruction& test, Vector<Wire::TypeTestRecord>& records)
{
    const Function& function = program.at(id);
    Origin origin = test.origin();
    Wire::SourceLocation location = function.locationOf(origin);
    if (!hasModule(location))
        return;

    FunctionID source = origin.inlineFrame ? function.inlineFrames[origin.inlineFrame].callee : id;
    records.append(Wire::TypeTestRecord {
        .location = location,
        .function = findFunctionAsRead(&program, source).index(),
        .failure = Wire::TypeTestFailure::TakesGenericPath,
        .copies = 1,
    });
}

Vector<Wire::TypeTestRecord> collectTypeTestRecords(const Program& program)
{
    return collectTypeTestRecords(program, ParallelLoops(1));
}

Vector<Wire::TypeTestRecord> collectTypeTestRecords(const Program& program, const ParallelLoops& loops)
{
    OpcodeSet typeTestOpcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (opcode == Opcode::Check || std::ranges::contains(replacedOperationsOf(opcode), Opcode::Check) || opcode == Opcode::TypeTest || familyOf(opcode) == OperationFamily::Guard)
            typeTestOpcodes.add(opcode);
    }

    Vector<Wire::TypeTestRecord> records = collectInParallel<Wire::TypeTestRecord>(loops, program.functions.size(), [&](unsigned index, Vector<Wire::TypeTestRecord>& found) {
        FunctionID id(index);
        forEachListedValue(program, id, typeTestOpcodes, [&](BlockID, uint32_t, ValueID, const Value& value) {
            appendTypeTestRecord(program, id, value, found);
        });
        if (!mayEndBlockWith(program, id, { TerminatorKind::SwitchOnShape }))
            return;
        for (const Block& block : program.at(id).blocks) {
            if (block.terminator.kind == TerminatorKind::SwitchOnShape)
                appendTypeTestRecord(program, id, block.terminator, found);
        }
    });

    auto orderKey = [](const Wire::TypeTestRecord& record) {
        return std::tuple { record.location.module, record.location.position, record.function };
    };
    parallelSort(loops, records.mutableSpan(), [&](const Wire::TypeTestRecord& a, const Wire::TypeTestRecord& b) {
        return orderKey(a) < orderKey(b);
    });

    size_t recordCount = 0;
    for (Wire::TypeTestRecord record : records) {
        if (recordCount && records[recordCount - 1].location == record.location) {
            uint32_t& copies = records[recordCount - 1].copies;
            copies = saturatingSum<uint32_t>(copies, record.copies);
            continue;
        }
        records[recordCount++] = record;
    }
    records.shrink(recordCount);
    return records;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
