/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowReportInternal.h"

#if ENABLE(AOT)

#include "AOTBindGlobalNames.h"
#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTCallGraph.h"
#include "AOTCheckCause.h"
#include "AOTDiagnostics.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTResultRules.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::tuple_size_v<decltype(Wire::AccessCountRecord::accesses)> == numberOfEnumerators<KnowledgeLevel>);
static_assert(std::tuple_size_v<decltype(Wire::AccessCountRecord::calls)> == numberOfEnumerators<KnowledgeLevel>);
static_assert(std::tuple_size_v<decltype(Wire::AccessCountRecord::accessCosts)> == numberOfEnumerators<AccessCost>);

namespace {

struct AccessDescription {
    AccessClass accessClass { AccessClass::None };
    std::optional<KnowledgeLevel> level;
    PropertyKeyID key;
    std::span<const LayoutID> receivers;
    uint32_t numberOfCandidates { 0 };
    // The code of the operation starts with a test that is not an operation of its own.
    bool testsInOwnCode { false };
};

// Which calls are inlined depends on heat, so a copy in a caller speaks for the site only where the function has no code of its own.
enum class CopyPlace : uint8_t { OwnBody, Caller };

struct FlowSiteObservation {
    AccessSiteID site;
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    AccessCost cost { AccessCost::ByName };
    BlockFrequency frequency { BlockFrequency::Normal };
    CopyPlace place { CopyPlace::OwnBody };
    bool isInGenericVersion { false };
    LayoutID layout;
    PropertyKeyID key;
    uint32_t candidatesOfLeftOperation { 0 };
    double heat { 0 };
};

// An operator, a test or a protocol that is left in the form that takes any value.
struct GeneralOperation {
    Wire::SourceLocation location;
    AccessKind kind { AccessKind::Unknown };
    FunctionID function;
    // The access or the call that has made the operand nobody knows the type of.
    AccessSiteID madeBy;
    bool isParameter { false };
    double heat { 0 };
};

struct ObservationsOfFunction {
    Vector<FlowSiteObservation> sites;
    Vector<GeneralOperation> generalOperations;
};

// What a branch proves on an edge is a parameter of the block that the edge enters.
struct PassedTest {
    ValueID proof;
    // What a test that narrows a type looked at. A guard stands for no value.
    ValueID narrowedValue;
    BlockID enteredBlock;
    Origin origin;
    Proof proves { Proof::None };
    bool isCharged { false };
};

struct PlaceOfAccess {
    BlockID block;
    Wire::SourceLocation location;
};

// A pass that puts a test in front of an access gives the test the source position of that access.
class TestsMadeForAccesses {
    WTF_MAKE_NONCOPYABLE(TestsMadeForAccesses);
public:
    TestsMadeForAccesses(const Program& program, const Function& function, const CFG& cfg)
        : m_program(program)
        , m_function(function)
        , m_cfg(cfg)
    {
    }

    // A test is charged once: a read and a store can have the same position.
    AccessCost charge(const Instruction& access, const PlaceOfAccess&);

private:
    void collectTests();
    PassedTest* find(ValueID proof);
    bool isCounted(const PassedTest&) const;
    bool standsInFrontOf(const PlaceOfAccess&, const PassedTest&) const;
    void chargeTestsBehind(ValueID operand, const PlaceOfAccess&, AccessCost&);

    const Program& m_program;
    const Function& m_function;
    const CFG& m_cfg;
    Vector<PassedTest> m_tests;
    bool m_didCollectTests { false };
};

void TestsMadeForAccesses::collectTests()
{
    m_didCollectTests = true;
    for (BlockID block : m_cfg.reversePostOrder()) {
        const Terminator& terminator = m_function.at(block).terminator;
        forEachEdgeResult(m_function, block, [&](const EdgeResult& result, ValueID parameter) {
            if (result.kind == EdgeResultKind::DeclaredResult)
                return;
            uint32_t targetIndex = result.caseIndex.value_or(result.side == EdgeSide::Taken ? 0 : 1);
            m_tests.append(PassedTest {
                .proof = parameter,
                .narrowedValue = result.kind == EdgeResultKind::NarrowedOperand ? result.subject : ValueID(),
                .enteredBlock = terminator.targets[targetIndex].block,
                .origin = result.test ? m_function.at(result.test).origin() : terminator.origin(),
                .proves = result.proof,
            });
        });
    }
    AOT::stableSort(m_tests, { }, [](const PassedTest& test) {
        return test.proof.index();
    });
}

PassedTest* TestsMadeForAccesses::find(ValueID proof)
{
    auto found = std::ranges::lower_bound(m_tests, proof.index(), { }, [](const PassedTest& test) {
        return test.proof.index();
    });
    return found != m_tests.end() && found->proof == proof ? &*found : nullptr;
}

// Every language tests for null and for the bounds of an array.
bool TestsMadeForAccesses::isCounted(const PassedTest& test) const
{
    if (test.proves == Proof::NotNullish || test.proves == Proof::InBounds)
        return false;
    if (!test.narrowedValue)
        return true;
    const Type& passed = m_program.typeOf(m_function, test.proof);
    return !m_program.typeOf(m_function, test.narrowedValue).withoutNullish().isSubsetOf(passed, m_program);
}

// The tests of one access are in a row: passing one leads to the next, and passing the last leads to the access.
bool TestsMadeForAccesses::standsInFrontOf(const PlaceOfAccess& place, const PassedTest& test) const
{
    if (m_function.locationOf(test.origin) != place.location)
        return false;

    AOT_THRESHOLD(unsigned, maxBlocksInRow, 16);
    BlockID block = test.enteredBlock;
    for (unsigned count = 0; block != place.block; ++count) {
        const Terminator& terminator = m_function.at(block).terminator;
        bool isTestOfAccess = terminator.isConditionalBranch() && m_function.locationOf(terminator.origin()) == place.location;
        if (count == maxBlocksInRow || !(isTestOfAccess || terminator.kind == TerminatorKind::Jump))
            return false;
        block = terminator.targets[0].block;
    }
    return true;
}

void TestsMadeForAccesses::chargeTestsBehind(ValueID operand, const PlaceOfAccess& place, AccessCost& cost)
{
    while (operand) {
        const Value& value = m_function.at(operand);
        if (value.opcode == Opcode::AllGuards) {
            value.forEachChild([&](ValueID guard) {
                chargeTestsBehind(guard, place, cost);
            });
            return;
        }
        if (isCastOrConversion(value.opcode)) {
            operand = value.operand(0);
            continue;
        }

        PassedTest* test = value.opcode == Opcode::Parameter ? find(operand) : nullptr;
        if (!test)
            return;
        if (!test->isCharged && standsInFrontOf(place, *test) && isCounted(*test)) {
            test->isCharged = true;
            AccessCost costOfTest = test->proves == Proof::SlotFilled ? AccessCost::TestsSlot : AccessCost::TestsReceiver;
            if (rankOf(costOfTest) < rankOf(cost))
                cost = costOfTest;
        }
        operand = test->narrowedValue;
    }
}

AccessCost TestsMadeForAccesses::charge(const Instruction& access, const PlaceOfAccess& place)
{
    if (!m_didCollectTests)
        collectTests();
    AccessCost cost = AccessCost::Free;
    if (m_tests.isEmpty() || !hasModule(place.location))
        return cost;
    access.forEachChild([&](ValueID operand) {
        chargeTestsBehind(operand, place, cost);
    });
    return cost;
}

} // anonymous namespace

static AccessDescription accessDescriptionOf(const Program& program, const Value& value)
{
    AccessDescription access { .accessClass = accessClassOf(value.opcode) };
    if (access.accessClass == AccessClass::None)
        return access;
    access.testsInOwnCode = globalBindingAccesses.contains(value.opcode);
    if (PayloadWord::level(value.opcode))
        access.level = value.level();
    if (PayloadWord::key(value.opcode))
        access.key = value.key();
    if (PayloadWord::receivers(value.opcode)) {
        if (LayoutSetID receivers = value.receivers()) {
            access.receivers = program.at(receivers).members.span();
            if (program.at(receivers).isComplete)
                access.numberOfCandidates = access.receivers.size();
        }
    }
    if (PayloadWord::callees(value.opcode)) {
        if (CalleeSetID callees = value.callees(); callees && program.at(callees).isComplete)
            access.numberOfCandidates = program.at(callees).functions.size() + program.at(callees).builtins.size();
    }
    return access;
}

static AccessDescription accessDescriptionOf(const Program&, const Terminator& terminator)
{
    return { .accessClass = accessClassOf(terminator.kind) };
}

// An array, a function or a built-in object has no layout to lose: flow knows every receiver of such a site.
static KnowledgeLevel levelFromSiteLoss(const FlowResults& flow, AccessSiteID site)
{
    UnprovenCause cause = flow.lossOf(site).cause;
    bool knowsReceivers = cause == UnprovenCause::Unknown || cause == UnprovenCause::NonLayoutObject;
    return knowsReceivers ? KnowledgeLevel::Proven : KnowledgeLevel::Expected;
}

static bool hasProvenField(const Program& program, LayoutID layout, PropertyKeyID key)
{
    std::optional<FieldReference> field = findField(program, layout, key, BaseChain::Proven);
    return field && fieldTypeStateOf(program, *field) == FieldTypeState::Proven;
}

static LayoutID findReportedReceiver(const Program& program, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    auto unproven = std::ranges::find_if(receivers, [&](LayoutID layout) {
        return !hasProvenField(program, layout, key);
    });
    if (unproven != receivers.end())
        return *unproven;
    return receivers.empty() ? LayoutID() : receivers.front();
}

static FlowSiteObservation observationOf(const Program& program, const FlowResults& flow, const AccessDescription& access, AccessSiteID site)
{
    FlowSiteObservation observation { .site = site, .key = access.key };
    switch (access.accessClass) {
    case AccessClass::Slot:
        observation.level = access.level ? *access.level : levelFromSiteLoss(flow, site);
        if (observation.level != KnowledgeLevel::Proven)
            observation.layout = findReportedReceiver(program, access.receivers, access.key);
        return observation;
    case AccessClass::ByName:
    case AccessClass::IndirectCall:
        observation.layout = access.receivers.empty() ? LayoutID() : access.receivers.front();
        observation.candidatesOfLeftOperation = access.numberOfCandidates;
        return observation;
    case AccessClass::DirectCall:
        observation.level = levelFromSiteLoss(flow, site);
        return observation;
    case AccessClass::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static AccessCost costOf(const FlowResults& flow, const AccessDescription& access, const FlowSiteObservation& observation, const Instruction& instruction, BlockID block, TestsMadeForAccesses& tests)
{
    switch (access.accessClass) {
    case AccessClass::ByName:
    case AccessClass::IndirectCall:
        return AccessCost::ByName;
    case AccessClass::DirectCall:
        // A call has no operand for the tests it relies on.
        return observation.level == KnowledgeLevel::Proven ? AccessCost::Free : AccessCost::TestsReceiver;
    case AccessClass::Slot:
        if (access.testsInOwnCode)
            return AccessCost::TestsReceiver;
        if (observation.site.index() >= flow.numberOfAccessSites())
            return AccessCost::Free;
        return tests.charge(instruction, { block, flow.at(observation.site).location });
    case AccessClass::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An entry that stands for several source accesses describes the one its own stamp came from, and each store of a run by its placed key.
static void forEachDescribedSite(const Program& program, const Value& value, const auto& functor)
{
    AccessDescription access = accessDescriptionOf(program, value);
    if (access.accessClass == AccessClass::None)
        return;
    if (Value::opcodesWithPlacedKeys.contains(value.opcode)) {
        for (PlacedKey placedKey : value.placedKeys()) {
            if (placedKey.accessSite)
                functor(placedKey.accessSite, AccessDescription { access.accessClass, placedKey.level, placedKey.key, access.receivers });
        }
        return;
    }
    AccessSiteID stampedSite = PayloadWord::accessSite(value.opcode) ? value.accessSite() : AccessSiteID();
    forEachAccessSite(value, [&](AccessSiteID site) {
        functor(site, site == stampedSite ? access : AccessDescription { .accessClass = access.accessClass });
    });
}

static void forEachDescribedSite(const Program& program, const Terminator& terminator, const auto& functor)
{
    AccessDescription access = accessDescriptionOf(program, terminator);
    if (access.accessClass == AccessClass::None)
        return;
    forEachAccessSite(terminator, [&](AccessSiteID site) {
        functor(site, access);
    });
}

template<typename InstructionType>
static void collectObservations(const Program& program, const FlowResults& flow, const Function& function, const InstructionType& instruction, BlockID block, double heat, TestsMadeForAccesses& tests, Vector<FlowSiteObservation>& observations)
{
    forEachDescribedSite(program, instruction, [&](AccessSiteID site, const AccessDescription& access) {
        FlowSiteObservation observation = observationOf(program, flow, access, site);
        observation.cost = costOf(flow, access, observation, instruction, block, tests);
        observation.heat = heat;
        observation.frequency = function.at(block).frequency;
        observation.place = instruction.inlineFrame ? CopyPlace::Caller : CopyPlace::OwnBody;
        observation.isInGenericVersion = program.sourceFunctionOf(function, instruction.origin()).role == FunctionRole::GenericVersion;
        observations.append(observation);
    });
}

static uint8_t rarityOf(BlockFrequency frequency)
{
    switch (frequency) {
    case BlockFrequency::Normal:
        return 0;
    case BlockFrequency::Unlikely:
        return 1;
    case BlockFrequency::Rare:
        return 2;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr OpcodeSet constants {
    Opcode::Undefined, Opcode::Null, Opcode::Absent, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::KeyConstant
};

static bool isByName(AccessClass accessClass)
{
    return accessClass == AccessClass::ByName || accessClass == AccessClass::IndirectCall;
}

namespace {

struct SlowHalf {
    Wire::SourceLocation location;
    uint8_t rarityOfOtherHalf { UINT8_MAX };
};

} // anonymous namespace

static auto orderKey(Wire::SourceLocation location)
{
    return std::tuple { location.module, location.position };
}

// The fast half of an access may be an operation that has no stamp: the length of a string, one of its code units, a built-in function that is named and no longer read.
static void gradeSlowHalvesOfUnstampedOperations(const FlowResults& flow, const Function& function, const CFG& cfg, Vector<FlowSiteObservation>& observations)
{
    auto isCandidate = [&](const FlowSiteObservation& observation) {
        return observation.cost == AccessCost::ByName && observation.frequency != BlockFrequency::Normal && !observation.isInGenericVersion
            && observation.site.index() < flow.numberOfAccessSites() && hasModule(flow.at(observation.site).location);
    };

    Vector<SlowHalf, 8> slowHalves;
    for (const FlowSiteObservation& observation : observations) {
        if (isCandidate(observation))
            slowHalves.append(SlowHalf { flow.at(observation.site).location });
    }
    if (slowHalves.isEmpty())
        return;
    AOT::sort(slowHalves, { }, [](const SlowHalf& half) {
        return orderKey(half.location);
    });

    auto find = [&](Wire::SourceLocation location) -> SlowHalf* {
        auto found = std::ranges::lower_bound(slowHalves, orderKey(location), { }, [](const SlowHalf& half) {
            return orderKey(half.location);
        });
        return found != slowHalves.end() && found->location == location ? &*found : nullptr;
    };

    for (BlockID blockID : cfg.reversePostOrder()) {
        const Block& block = function.at(blockID);
        uint8_t rarity = rarityOf(block.frequency);
        if (rarity == rarityOf(BlockFrequency::Rare))
            continue;
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (isByName(accessClassOf(value.opcode)) || isCastOrConversion(value.opcode) || constants.contains(value.opcode))
                continue;
            if (SlowHalf* half = find(function.locationOf(value.origin())))
                half->rarityOfOtherHalf = std::min(half->rarityOfOtherHalf, rarity);
        }
    }

    for (FlowSiteObservation& observation : observations) {
        if (isCandidate(observation) && find(flow.at(observation.site).location)->rarityOfOtherHalf < rarityOf(observation.frequency))
            observation.cost = AccessCost::TestsReceiver;
    }
}

static AccessKind kindOfOperator(Opcode opcode)
{
    switch (opcode) {
    case Opcode::ToBoolean:
        return AccessKind::Truthiness;
    case Opcode::StrictEq:
    case Opcode::Eq:
        return AccessKind::Equality;
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        return AccessKind::Comparison;
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::ToString:
        return AccessKind::Conversion;
    default:
        return AccessKind::Arithmetic;
    }
}

// Unknown for an operation that has a form for the types of its operands.
static AccessKind kindOfGeneralOperation(const Value& value)
{
    if (PayloadWord::form(value.opcode))
        return value.form() == OperatorForm::Generic ? kindOfOperator(value.opcode) : AccessKind::Unknown;
    switch (value.opcode) {
    case Opcode::ValueEqualsLiteral:
    case Opcode::ValueLooselyEqualsLiteral:
        return AccessKind::Equality;
    case Opcode::TypeOf:
        return AccessKind::TypeOf;
    case Opcode::InstanceOf:
        return AccessKind::InstanceOf;
    case Opcode::IteratorOpen:
    case Opcode::IteratorResume:
        return AccessKind::Iteration;
    case Opcode::CopyDataProperties:
    case Opcode::CopyObject:
        return AccessKind::Spread;
    default:
        return AccessKind::Unknown;
    }
}

// The operand whose type keeps the operation general comes out of a read, a call or a parameter. Where two ways meet, the first that leads to one of these speaks for the operand.
static bool findMakerOfOperand(const Function& function, ValueID operand, GeneralOperation& operation)
{
    const Value* definition = &function.at(sameObjectThroughCasts(function, operand));
    if (definition->opcode == Opcode::ResultAt)
        definition = &function.at(sameObjectThroughCasts(function, definition->resultsOperand()));
    if (definition->opcode == Opcode::Parameter) {
        operation.isParameter = true;
        return true;
    }
    if (!PayloadWord::accessSite(definition->opcode) || !definition->accessSite())
        return false;
    operation.madeBy = definition->accessSite();
    return true;
}

static void collectGeneralOperation(const Program& program, const Function& function, const Value& value, double heat, Vector<GeneralOperation>& operations)
{
    AccessKind kind = kindOfGeneralOperation(value);
    if (kind == AccessKind::Unknown)
        return;
    const Function& source = program.sourceFunctionOf(function, value.origin());
    Wire::SourceLocation location = function.locationOf(value.origin());
    if (!hasModule(location) || source.role == FunctionRole::GenericVersion)
        return;
    GeneralOperation operation { .location = location, .kind = kind, .function = value.inlineFrame ? function.inlineFrames[value.inlineFrame].callee : FunctionID(), .heat = heat };
    bool hasMaker = false;
    value.forEachChild([&](ValueID operand) {
        if (!hasMaker && !constants.contains(function.at(operand).opcode) && program.typeOf(function, operand).hasObjects())
            hasMaker = findMakerOfOperand(function, operand, operation);
    });
    operations.append(operation);
}

static ObservationsOfFunction observeAccessSites(const Program& program, const FlowResults& flow, BuildContext& build, const Heat* heat, FunctionID id)
{
    ObservationsOfFunction observations;
    const Function& function = program.at(id);
    CFG cfg(function);
    TestsMadeForAccesses tests(program, function, cfg);
    // How often a function is entered is a product of guesses, one for every loop and call on the way, and is off by more the larger it is. Its rank among the functions holds better: the hottest count a hundred times the coldest.
    std::optional<FunctionAnalyses> analyses;
    double weightOfFunction = 0;
    if (heat) {
        analyses.emplace(AnalysisContext { build, program, &flow, nullptr, id }, function);
        weightOfFunction = 100.0 / (101 - std::min<unsigned>(heat->percentileOf(program, id), 100));
    }
    for (BlockID blockID : cfg.reversePostOrder()) {
        const Block& block = function.at(blockID);
        double heatOfBlock = analyses ? weightOfFunction * analyses->blockFrequencies().frequencyOf(blockID) : 0;
        for (ValueID value : block.values) {
            collectObservations(program, flow, function, function.at(value), blockID, heatOfBlock, tests, observations.sites);
            if (heat)
                collectGeneralOperation(program, function, function.at(value), heatOfBlock, observations.generalOperations);
        }
        collectObservations(program, flow, function, block.terminator, blockID, heatOfBlock, tests, observations.sites);
    }
    for (GeneralOperation& operation : observations.generalOperations) {
        if (!operation.function)
            operation.function = id;
    }
    gradeSlowHalvesOfUnstampedOperations(flow, function, cfg, observations.sites);
    return observations;
}

static bool isCheck(Opcode opcode)
{
    return opcode == Opcode::Check || std::ranges::contains(replacedOperationsOf(opcode), Opcode::Check);
}

namespace {

struct RemainingCheck {
    FunctionID function;
    ValueID value;
    Wire::SourceLocation location;
    TypeSiteID site;
    CheckCause cause { CheckCause::Unknown };
};

} // anonymous namespace

static Wire::SourceLocation locationOf(const TypeSite& site)
{
    return { site.module.index(), site.position };
}

static Wire::RemainingCheckRecord remainingCheckRecord(const Program& program, const AOT::CompileOptions& options, const RemainingCheck& check)
{
    Wire::SourceLocation origin = check.location;
    TypeSiteKind siteKind = TypeSiteKind::Unknown;
    if (check.site) {
        origin = locationOf(program.at(check.site));
        siteKind = program.at(check.site).kind;
    }
    return {
        .id = remainingCheckIDOf(program, options, origin, siteKind),
        .witness = { findFunctionAsRead(&program, check.function).index(), check.value.index(), check.location },
        .origin = origin,
        .cause = check.cause,
        .siteKind = static_cast<uint32_t>(siteKind),
    };
}

// Flow says which checks always pass. Of one that it leaves without a word, the definition of the value says where a stranger can come from.
static CheckCause causeOfCheck(const Function& function, const Value& check)
{
    CheckCause cause = PayloadWord::cause(check.opcode) ? check.cause() : CheckCause::Unknown;
    if (cause == CheckCause::Unknown && check.opcode == Opcode::Check)
        return computeUnknownObjectCause(function, check.valueOperand());
    return cause;
}

static RemainingCheck describeCheck(const Function& function, FunctionID id, ValueID valueID)
{
    const Value& check = function.at(valueID);
    return {
        .function = check.inlineFrame ? function.inlineFrames[check.inlineFrame].callee : id,
        .value = valueID,
        .location = function.locationOf(valueID),
        .site = PayloadWord::site(check.opcode) ? check.site() : TypeSiteID(),
        .cause = causeOfCheck(function, check),
    };
}

Vector<Wire::RemainingCheckRecord> collectRemainingChecks(const Program& program, const AOT::CompileOptions& options, FunctionID id)
{
    Vector<Wire::RemainingCheckRecord> checks;
    const Function& function = program.at(id);
    CFG cfg(function);
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (isCheck(function.at(value).opcode))
                checks.append(remainingCheckRecord(program, options, describeCheck(function, id, value)));
        }
    }
    return checks;
}

static uint8_t distanceOf(const FlowSiteObservation& observation)
{
    constexpr uint8_t numberOfRarities = 3;
    return static_cast<uint8_t>(observation.place) * numberOfRarities + rarityOf(observation.frequency);
}

static constexpr uint8_t numberOfDistances = 6;

// The generic version is the slow half of every access in it, so a copy anywhere else decides what the access costs.
static uint8_t distanceForCostOf(const FlowSiteObservation& observation)
{
    return observation.isInGenericVersion * numberOfDistances + distanceOf(observation);
}

namespace {

struct NearestCopies {
    uint8_t rarity { UINT8_MAX };
    uint8_t distance { UINT8_MAX };
    uint8_t distanceForCost { UINT8_MAX };
};

} // anonymous namespace

static void foldObservation(FlowSiteLevel& folded, NearestCopies& nearest, const FlowSiteObservation& observation)
{
    // The slow half of a guarded access is a copy in a block that is rarer than its fast half, and so is the copy in the generic version of the function.
    if (!observation.isInGenericVersion)
        folded.heat += observation.heat;
    uint8_t rarity = rarityOf(observation.frequency);
    if (rarity < nearest.rarity) {
        nearest.rarity = rarity;
        folded.copies = 0;
    }
    if (rarity == nearest.rarity)
        ++folded.copies;

    uint8_t distanceForCost = distanceForCostOf(observation);
    if (distanceForCost < nearest.distanceForCost || (distanceForCost == nearest.distanceForCost && rankOf(observation.cost) < rankOf(folded.cost))) {
        nearest.distanceForCost = distanceForCost;
        folded.cost = observation.cost;
    }

    uint8_t distance = distanceOf(observation);
    if (distance > nearest.distance || (distance == nearest.distance && rankOf(observation.level) >= rankOf(folded.level)))
        return;
    nearest.distance = distance;
    folded.level = observation.level;
    folded.layout = observation.layout;
    folded.key = observation.key;
    folded.candidatesOfLeftOperation = observation.candidatesOfLeftOperation;
}

struct AccessSiteObservations::OfFunctions {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(OfFunctions);

    Vector<ObservationsOfFunction> observations;
};

AccessSiteObservations::AccessSiteObservations() = default;
AccessSiteObservations::AccessSiteObservations(std::unique_ptr<OfFunctions>&& ofFunctions)
    : m_ofFunctions(WTF::move(ofFunctions))
{
}
AccessSiteObservations::AccessSiteObservations(AccessSiteObservations&&) = default;
AccessSiteObservations& AccessSiteObservations::operator=(AccessSiteObservations&&) = default;
AccessSiteObservations::~AccessSiteObservations() = default;

std::unique_ptr<AccessSiteObservations::OfFunctions> AccessSiteObservations::take()
{
    return WTF::move(m_ofFunctions);
}

static Vector<FlowSiteLevel> foldSiteLevels(const FlowResults& flow, std::span<const ObservationsOfFunction> observationsOfFunctions)
{
    Vector<FlowSiteLevel> levels(flow.numberOfAccessSites());
    Vector<NearestCopies> nearestCopies(levels.size());
    for (const ObservationsOfFunction& observations : observationsOfFunctions) {
        for (const FlowSiteObservation& observation : observations.sites) {
            size_t index = observation.site.index();
            if (index < levels.size())
                foldObservation(levels[index], nearestCopies[index], observation);
        }
    }

    // Where the generic version has the only stamped copy, the typed version has an operation without a stamp or none at all. Such a site costs what a site without any copy costs.
    for (size_t index = 0; index < levels.size(); ++index) {
        levels[index].distance = nearestCopies[index].distance;
        levels[index].distanceForCost = nearestCopies[index].distanceForCost;
        uint8_t distance = nearestCopies[index].distanceForCost;
        if (distance >= numberOfDistances && distance != UINT8_MAX && levels[index].cost == AccessCost::ByName)
            levels[index].cost = AccessCost::Free;
    }
    return levels;
}

AccessSiteObservations observeAccessSites(const Program& program, const FlowResults& flow, BuildContext& build, const Heat* heat)
{
    // Closures and the host enter entrypoints, and an entrypoint may hold a copy of the body it stands for.
    BitVector hasNoCode;
    if (program.form() >= IRForm::Selected)
        hasNoCode = findBodiesWithoutCode(program, build);

    auto ofFunctions = makeUnique<AccessSiteObservations::OfFunctions>();
    ofFunctions->observations = collectInParallel<ObservationsOfFunction>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<ObservationsOfFunction>& found) {
        if (program.functions[index].hasBody() && !hasNoCode.get(index))
            found.append(observeAccessSites(program, flow, build, heat, FunctionID(index)));
    });
    return AccessSiteObservations(WTF::move(ofFunctions));
}

struct AccessSiteObserver::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    State(const Program& program, const FlowResults& flow, BuildContext& build, const Heat* heat)
        : program(program)
        , flow(flow)
        , build(build)
        , heat(heat)
        , observations(build.parallelLoops(), program.functions.size(), [](unsigned) { return ObservationsOfFunction(); })
        , calledFunctions(build.parallelLoops(), program.functions.size(), [](unsigned) { return Vector<FunctionID>(); })
    {
    }

    const Program& program;
    const FlowResults& flow;
    BuildContext& build;
    const Heat* heat;
    ParallelArray<ObservationsOfFunction> observations;
    ParallelArray<Vector<FunctionID>> calledFunctions;
};

AccessSiteObserver::AccessSiteObserver(const Program& program, const FlowResults& flow, BuildContext& build, const Heat* heat)
    : m_state(makeUnique<State>(program, flow, build, heat))
{
}

AccessSiteObserver::~AccessSiteObserver() = default;

void AccessSiteObserver::observe(FunctionID id)
{
    m_state->observations[id.index()] = observeAccessSites(m_state->program, m_state->flow, m_state->build, m_state->heat, id);
    Vector<FunctionID>& called = m_state->calledFunctions[id.index()];
    forEachCalledFunction(m_state->program.at(id), [&](FunctionID target) {
        called.append(target);
    });
}

AccessSiteObservations AccessSiteObserver::finish()
{
    const Program& program = m_state->program;
    RELEASE_ASSERT(program.form() >= IRForm::Selected && m_state->observations.size() == program.functions.size());
    BitVector hasNoCode = findBodiesWithoutCode(program, m_state->build, m_state->calledFunctions.span());
    auto ofFunctions = makeUnique<AccessSiteObservations::OfFunctions>();
    ofFunctions->observations = collectInParallel<ObservationsOfFunction>(m_state->build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<ObservationsOfFunction>& found) {
        if (program.functions[index].hasBody() && !hasNoCode.get(index))
            found.append(WTF::move(m_state->observations[index]));
    });
    m_state->build.destroyLater(WTF::move(m_state->observations));
    m_state->build.destroyLater(WTF::move(m_state->calledFunctions));
    return AccessSiteObservations(WTF::move(ofFunctions));
}


static Access memberAccessOf(AccessKind kind)
{
    switch (kind) {
    case AccessKind::Read:
    case AccessKind::Call:
    case AccessKind::Construct:
        return Access::Read;
    case AccessKind::Store:
        return Access::Store;
    case AccessKind::Define:
        return Access::Define;
    case AccessKind::Has:
        return Access::Has;
    case AccessKind::Unknown:
    case AccessKind::Delete:
    case AccessKind::Truthiness:
    case AccessKind::Equality:
    case AccessKind::Arithmetic:
    case AccessKind::Comparison:
    case AccessKind::Conversion:
    case AccessKind::TypeOf:
    case AccessKind::InstanceOf:
    case AccessKind::Iteration:
    case AccessKind::Spread:
        return Access::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowSiteLoss lossOfMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    MemberAnswer member = findMember(context, query);
    if (member.kind != MemberKind::Field)
        return { member.cause, LimitID::None, noWitness };
    const LocationFlow& location = flowOf(context.program, FieldReference { member.holder, member.field }).location;
    return { location.cause, LimitID::None, location.witness };
}

static FlowSiteLoss lossOfKnownReceiver(const PropertyAccessContext& context, AccessKind kind, const FlowSiteLevel& level)
{
    if (!level.layout)
        return { };
    if (Access access = memberAccessOf(kind); access != Access::None) {
        FlowSiteLoss loss = lossOfMember(context, { .start = MemberStart::Instance, .layout = level.layout, .key = level.key, .access = access, .isOwnLookup = false });
        if (loss.cause != UnprovenCause::Unknown)
            return loss;
    }
    const LayoutFlow& flow = context.program.at(level.layout).flow;
    return { flow.cause, LimitID::None, flow.witness };
}

// Flow lost nothing, and a pass left the operation as it was: with more receivers or callees than one dispatch has arms, that is why.
static FlowSiteLoss lossOfLeftOperation(const BuildContext& build, const FlowSiteLevel& level)
{
    if (level.candidatesOfLeftOperation > build.limit(LimitID::MaxDispatchCases))
        return { UnprovenCause::LimitReached, LimitID::MaxDispatchCases, noWitness };
    return { };
}

static FlowSiteLoss lossBelowProven(const Program& program, const FlowResults& flow, const BuildContext& build, AccessSiteID site, const FlowSiteLevel& level)
{
    FlowSiteLoss loss = flow.lossOf(site);
    if (loss.cause == UnprovenCause::Unknown)
        loss = lossOfKnownReceiver({ program, &flow }, flow.at(site).kind, level);
    if (loss.cause == UnprovenCause::Unknown)
        loss = lossOfLeftOperation(build, level);
    if (loss.cause == UnprovenCause::Unknown)
        loss.cause = UnprovenCause::LeftByPasses;
    // An operation that the compiler made has no place in the source. Its function has one.
    if (hasFunction(loss.witness) && !hasModule(loss.witness.location))
        loss.witness.location = program.at(FunctionID(loss.witness.function)).declarationLocation();
    // What no other operation explains begins at the site.
    if (!hasFunction(loss.witness) || !hasModule(loss.witness.location))
        loss.witness = { flow.at(site).function.index(), Wire::none, flow.at(site).location };
    // A typed body and a copy without types hold the values of the function as read, under the numbers they were read with.
    loss.witness.function = findFunctionAsRead(&program, FunctionID(loss.witness.function)).index();
    return loss;
}

// The kind of unsound edge that took the proof away. None where no edge did: the analysis stopped following a value or was turned off, or it has the proof and a pass made nothing of it.
static IllTypedCause illTypedCauseOf(UnprovenCause cause)
{
    switch (cause) {
    case UnprovenCause::Unknown:
    case UnprovenCause::ResultDisabled:
    case UnprovenCause::LimitReached:
    case UnprovenCause::Untracked:
    case UnprovenCause::LeftByPasses:
        return IllTypedCause::None;
    case UnprovenCause::UnknownObject:
    case UnprovenCause::UncheckedConversionOfUntracked:
        return IllTypedCause::FromLooseType;
    case UnprovenCause::UndescribedNativeResult:
        return IllTypedCause::DeclarationWithoutBody;
    case UnprovenCause::DataFromOutside:
    case UnprovenCause::ThrownByRunTime:
        return IllTypedCause::OutsideData;
    case UnprovenCause::LayoutOutsideFamily:
        return IllTypedCause::LayoutOutsideFamily;
    case UnprovenCause::NonLayoutObject:
    case UnprovenCause::ObjectWithoutLayout:
    case UnprovenCause::BuiltinBase:
    case UnprovenCause::UnknownBase:
        return IllTypedCause::MemberWithoutSlot;
    case UnprovenCause::ClassEvaluatedAgain:
    case UnprovenCause::OwnPropertyHidesMember:
        return IllTypedCause::LookupMayChange;
    case UnprovenCause::ReachesNativeCode:
        return IllTypedCause::NativeWritesOperand;
    case UnprovenCause::ReachesUnknownJavaScript:
    case UnprovenCause::UnknownWriter:
    case UnprovenCause::UnseenCode:
    case UnprovenCause::ContentsOfEscapedObject:
        return IllTypedCause::UnseenWriter;
    case UnprovenCause::ParameterOfEscapedFunction:
        return IllTypedCause::UnseenCaller;
    case UnprovenCause::UnknownCallee:
    case UnprovenCause::ResultOfUnknownCallee:
        return IllTypedCause::UnprovenCallee;
    case UnprovenCause::UnprovenContents:
        return IllTypedCause::ContainerWriterUnseen;
    case UnprovenCause::FieldDeleted:
    case UnprovenCause::UnknownKeyDelete:
    case UnprovenCause::PropertyRedefined:
    case UnprovenCause::PropertyAdded:
    case UnprovenCause::IntegrityChanged:
    case UnprovenCause::PrototypeChanged:
        return IllTypedCause::StateChanged;
    case UnprovenCause::UnknownKeyStore:
        return IllTypedCause::ComputedKeyStore;
    case UnprovenCause::StoreOfOtherType:
        return IllTypedCause::StoreOutsideFieldType;
    case UnprovenCause::OtherNewTarget:
        return IllTypedCause::OtherThis;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Wire::AccessRecord siteRecordOf(const Program& program, const FlowResults& flow, const BuildContext& build, AccessSiteID site, const FlowSiteLevel& level)
{
    const AccessSite& access = flow.at(site);
    FlowSiteLoss loss;
    if (level.level != KnowledgeLevel::Proven || level.cost != AccessCost::Free)
        loss = lossBelowProven(program, flow, build, site, level);
    return {
        .location = access.location,
        .kind = access.kind,
        .level = level.level,
        // Data from outside is an object out of code that nobody compiles, like any other: the cause says what it does to the access, and the `why` where it comes from.
        .cause = loss.cause == UnprovenCause::DataFromOutside ? UnprovenCause::UnknownObject : loss.cause,
        .limit = loss.limit,
        .layout = findLayoutAsRead(&program, level.layout).index(),
        .copies = level.copies,
        .distance = access.kind == AccessKind::Call || access.kind == AccessKind::Construct ? level.distance : level.distanceForCost,
        .witness = loss.witness,
        .function = findFunctionAsRead(&program, access.function).index(),
        .illTypedCause = illTypedCauseOf(loss.cause),
        .cost = level.cost,
        .hasOtherRoots = loss.hasOtherRoots,
        .heat = level.heat,
    };
}

// The operation is general for the reason that the read or the call that made its operand is: what is read out of a value nobody knows is a value nobody knows.
static FlowSiteLoss lossOfGeneralOperation(const Program& program, const FlowResults& flow, const BuildContext& build, std::span<const FlowSiteLevel> levels, const GeneralOperation& operation)
{
    FlowSiteLoss loss;
    if (operation.madeBy && operation.madeBy.index() < levels.size())
        loss = lossBelowProven(program, flow, build, operation.madeBy, levels[operation.madeBy.index()]);
    else if (operation.isParameter) {
        const FunctionFlow& function = flow.function(operation.function);
        loss = { function.cause, LimitID::None, function.witness };
    }
    if (loss.cause == UnprovenCause::Unknown || loss.cause == UnprovenCause::LeftByPasses)
        loss = { UnprovenCause::Untracked, LimitID::None, noWitness };
    // Those that nothing explains are listed function by function.
    if (!hasFunction(loss.witness) || !hasModule(loss.witness.location))
        loss.witness = { operation.function.index(), Wire::none, program.at(operation.function).declarationLocation() };
    if (!hasModule(loss.witness.location))
        loss.witness.location = operation.location;
    loss.witness.function = findFunctionAsRead(&program, FunctionID(loss.witness.function)).index();
    return loss;
}

static Vector<Wire::AccessRecord> buildRecordsOfGeneralOperations(const Program& program, const FlowResults& flow, const BuildContext& build, std::span<const FlowSiteLevel> levels, std::span<const ObservationsOfFunction> observationsOfFunctions)
{
    Vector<GeneralOperation> operations;
    for (const ObservationsOfFunction& observations : observationsOfFunctions)
        operations.appendVector(observations.generalOperations);
    // A copy that knows where its operand comes from speaks for the place.
    AOT::stableSort(operations, [](const GeneralOperation& a, const GeneralOperation& b) {
        return std::tuple { a.location.module, a.location.position, a.kind, !a.madeBy, a.madeBy.index(), !a.isParameter, a.function.index() }
            < std::tuple { b.location.module, b.location.position, b.kind, !b.madeBy, b.madeBy.index(), !b.isParameter, b.function.index() };
    });

    Vector<Wire::AccessRecord> records;
    for (const GeneralOperation& operation : operations) {
        if (!records.isEmpty() && records.last().location == operation.location && records.last().kind == operation.kind) {
            records.last().copies = saturatingSum<uint32_t>(records.last().copies, 1u);
            records.last().heat += operation.heat;
            continue;
        }
        FlowSiteLoss loss = lossOfGeneralOperation(program, flow, build, levels, operation);
        records.append(Wire::AccessRecord {
            .location = operation.location,
            .kind = operation.kind,
            .level = KnowledgeLevel::Unknown,
            .cause = loss.cause == UnprovenCause::DataFromOutside ? UnprovenCause::UnknownObject : loss.cause,
            .limit = loss.limit,
            .layout = Wire::none,
            .copies = 1,
            .distance = 0,
            .witness = loss.witness,
            .function = findFunctionAsRead(&program, operation.function).index(),
            .illTypedCause = illTypedCauseOf(loss.cause),
            .cost = AccessCost::ByName,
            .hasOtherRoots = loss.hasOtherRoots,
            .heat = operation.heat,
        });
    }
    return records;
}

GradedSites gradeAccessSites(const Program& program, const FlowResults& flow, AccessSiteObservations&& observations, BuildContext& build)
{
    std::unique_ptr<AccessSiteObservations::OfFunctions> ofFunctions = observations.take();
    if (!ofFunctions)
        return { };
    GradedSites sites;
    sites.levels = foldSiteLevels(flow, ofFunctions->observations.span());
    sites.generalOperations = buildRecordsOfGeneralOperations(program, flow, build, sites.levels.span(), ofFunctions->observations.span());
    build.destroyLater(WTF::move(ofFunctions));
    return sites;
}

namespace {

struct SiteRecord {
    Wire::AccessRecord record;
    bool isInGenericVersion { false };
};

} // anonymous namespace

static auto siteOrderKey(const Wire::AccessRecord& record)
{
    return std::tuple {
        record.location.module, record.location.position, record.kind, rankOf(record.level), record.cause,
        record.witness.function, record.witness.value, record.function, record.layout, record.limit, record.illTypedCause,
    };
}

static bool isSameSourceAccess(const Wire::AccessRecord& a, const Wire::AccessRecord& b)
{
    return a.location == b.location && a.kind == b.kind;
}

// The same goes for a generic version whose sites have numbers of their own: it decides the cost only where no other version has the access.
static Vector<Wire::AccessRecord> foldDuplicateRecords(std::span<const SiteRecord> sortedSites)
{
    Vector<Wire::AccessRecord> records;
    bool costIsOfGenericVersion = false;
    for (const SiteRecord& site : sortedSites) {
        if (records.isEmpty() || !isSameSourceAccess(records.last(), site.record)) {
            records.append(site.record);
            costIsOfGenericVersion = site.isInGenericVersion;
            continue;
        }
        Wire::AccessRecord& record = records.last();
        record.copies = saturatingSum<uint32_t>(record.copies, site.record.copies);
        record.heat += site.record.heat;
        if (site.isInGenericVersion && !costIsOfGenericVersion)
            continue;
        if (costIsOfGenericVersion != site.isInGenericVersion || rankOf(site.record.cost) < rankOf(record.cost)) {
            record.cost = site.record.cost;
            record.distance = site.record.distance;
        }
        costIsOfGenericVersion = site.isInGenericVersion;
    }
    return records;
}

Vector<Wire::AccessRecord> buildAccessRecords(const Program& program, const FlowResults& flow, std::span<const FlowSiteLevel> levels, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<SiteRecord> sites = collectInParallel<SiteRecord>(loops, levels.size(), [&](unsigned index, Vector<SiteRecord>& found) {
        AccessSiteID site(index);
        const AccessSite& access = flow.at(site);
        if (!flow.mayBeReached(access.function) || access.kind == AccessKind::Unknown || !hasModule(access.location))
            return;
        // The count describes the machine code.
        bool isCall = access.kind == AccessKind::Call || access.kind == AccessKind::Construct;
        if (!isCall && !levels[index].copies)
            return;
        found.append(SiteRecord { siteRecordOf(program, flow, build, site, levels[index]), program.at(access.function).role == FunctionRole::GenericVersion });
    });

    parallelSort(loops, sites.mutableSpan(), [](const SiteRecord& a, const SiteRecord& b) {
        return siteOrderKey(a.record) < siteOrderKey(b.record);
    });
    return foldDuplicateRecords(sites.span());
}

static std::array<uint32_t, numberOfEnumerators<KnowledgeLevel>>* countersOf(Wire::AccessCountRecord& record, AccessKind kind)
{
    switch (kind) {
    case AccessKind::Unknown:
        break;
    // The counts of a build are of property accesses and of calls.
    case AccessKind::Truthiness:
    case AccessKind::Equality:
    case AccessKind::Arithmetic:
    case AccessKind::Comparison:
    case AccessKind::Conversion:
    case AccessKind::TypeOf:
    case AccessKind::InstanceOf:
    case AccessKind::Iteration:
    case AccessKind::Spread:
        return nullptr;
    case AccessKind::Read:
    case AccessKind::Store:
    case AccessKind::Define:
    case AccessKind::Has:
    case AccessKind::Delete:
        return &record.accesses;
    case AccessKind::Call:
    case AccessKind::Construct:
        return &record.calls;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<Wire::AccessCountRecord> countAccesses(const Program& program, std::span<const Wire::AccessRecord> accesses, std::span<const Wire::TypeTestRecord> typeTests)
{
    Vector<Wire::AccessCountRecord> counts(FillWith { }, program.modules.size(), Wire::AccessCountRecord { });
    for (uint32_t module = 0; module < counts.size(); ++module)
        counts[module].module = module;
    for (const Wire::AccessRecord& access : accesses) {
        Wire::AccessCountRecord& record = counts[access.location.module];
        auto* countsByLevel = countersOf(record, access.kind);
        if (!countsByLevel)
            continue;
        ++(*countsByLevel)[rankOf(access.level)];
        if (countsByLevel == &record.accesses)
            ++record.accessCosts[rankOf(access.cost)];
    }
    for (const Wire::TypeTestRecord& typeTest : typeTests)
        ++counts[typeTest.location.module].typeTests;

    counts.removeAllMatching([](const Wire::AccessCountRecord& record) {
        return record == Wire::AccessCountRecord { .module = record.module };
    });
    return counts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
