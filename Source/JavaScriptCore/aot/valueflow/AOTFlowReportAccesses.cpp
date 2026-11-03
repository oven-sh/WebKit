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
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
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
    bool testsInOwnCode { false };
};

enum class CopyPlace : uint8_t { OwnBody, Caller };

struct FlowSiteObservation {
    AccessSiteID site;
    KnowledgeLevel level : 8 { KnowledgeLevel::Unknown };
    AccessCost cost : 8 { AccessCost::ByName };
    Expected<BlockFrequency> frequency { BlockFrequency::Normal };
    CopyPlace place { CopyPlace::OwnBody };
    bool isInGenericVersion { false };
    bool receiverIsClassObject { false };
    LayoutID layout;
    PropertyKeyID key;
    uint32_t candidatesOfLeftOperation { 0 };
};
static_assert(sizeof(FlowSiteObservation) == 24);

struct GeneralOperation {
    Wire::SourceLocation location;
    AccessKind kind { AccessKind::Unknown };
    FunctionID function;
    AccessSiteID madeBy;
    bool isParameter { false };
    double heat { 0 };
};

struct ObservationsOfFunction {
    struct WithHeat {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WithHeat);

        Vector<double> heatOfSites;
        Vector<GeneralOperation> generalOperations;
    };

    Vector<FlowSiteObservation> sites;
    std::unique_ptr<WithHeat> withHeat;
};

struct PassedTest {
    ValueID proof;
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

struct TestsMadeForAccesses {
    const Program& program;
    const Function& function;
    const CFG& cfg;
    Vector<PassedTest> tests { };
    bool didCollectTests { false };

    void collectTests()
    {
        didCollectTests = true;
        for (BlockID block : cfg.reversePostOrder()) {
            const Terminator& terminator = function.at(block).terminator;
            forEachEdgeResult(function, block, [&](const EdgeResult& result, ValueID parameter) {
                if (result.kind == EdgeResultKind::DeclaredResult)
                    return;
                uint32_t targetIndex = result.caseIndex.value_or(result.side == EdgeSide::Taken ? 0 : 1);
                tests.append(PassedTest {
                    .proof = parameter,
                    .narrowedValue = result.kind == EdgeResultKind::NarrowedOperand ? result.subject : ValueID(),
                    .enteredBlock = terminator.targets[targetIndex].block,
                    .origin = result.test ? function.at(result.test).origin() : terminator.origin(),
                    .proves = result.proof,
                });
            });
        }
        AOT::stableSortInLine(tests, { }, &PassedTest::proof);
    }

    bool isCounted(const PassedTest& test) const
    {
        if (test.proves == Proof::NotNullish || test.proves == Proof::InBounds)
            return false;
        return !test.narrowedValue || !program.typeOf(function, test.narrowedValue).withoutNullish().isSubsetOf(program.typeOf(function, test.proof), program);
    }

    bool standsInFrontOf(const PlaceOfAccess& place, const PassedTest& test) const
    {
        if (function.locationOf(test.origin) != place.location)
            return false;

        AOT_THRESHOLD(unsigned, maxBlocksInRow, 16);
        BlockID block = test.enteredBlock;
        for (unsigned count = 0; block != place.block; ++count) {
            const Terminator& terminator = function.at(block).terminator;
            bool isTestOfAccess = terminator.isConditionalBranch() && function.locationOf(terminator.origin()) == place.location;
            if (count == maxBlocksInRow || !(isTestOfAccess || terminator.kind == TerminatorKind::Jump))
                return false;
            block = terminator.targets[0].block;
        }
        return true;
    }

    void chargeTestsBehind(ValueID operand, const PlaceOfAccess& place, AccessCost& cost)
    {
        while (operand) {
            const Value& value = function.at(operand);
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

            auto test = value.opcode == Opcode::Parameter ? std::ranges::lower_bound(tests, operand, { }, &PassedTest::proof) : tests.end();
            if (test == tests.end() || test->proof != operand)
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

    AccessCost charge(const Instruction& access, const PlaceOfAccess& place)
    {
        if (!didCollectTests)
            collectTests();
        AccessCost cost = AccessCost::Free;
        if (tests.isEmpty() || !hasModule(place.location))
            return cost;
        access.forEachChild([&](ValueID operand) {
            chargeTestsBehind(operand, place, cost);
        });
        return cost;
    }
};

} // anonymous namespace

static KnowledgeLevel levelFromSiteLoss(const FlowResults& flow, AccessSiteID site)
{
    UnprovenCause cause = flow.lossOf(site).cause;
    return cause == UnprovenCause::Unknown || cause == UnprovenCause::NonLayoutObject ? KnowledgeLevel::Proven : KnowledgeLevel::Expected;
}

static LayoutID findReportedReceiver(const Program& program, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    auto unproven = std::ranges::find_if(receivers, [&](LayoutID layout) {
        std::optional<FieldReference> field = findField(program, layout, key, BaseChain::Proven);
        return !field || fieldTypeStateOf(program, *field) != FieldTypeState::Proven;
    });
    if (unproven != receivers.end())
        return *unproven;
    return receivers.empty() ? LayoutID() : receivers.front();
}

static void forEachDescribedSite(const Program& program, const Value& value, const auto& functor)
{
    AccessDescription access { .accessClass = accessClassOf(value.opcode) };
    if (access.accessClass == AccessClass::None)
        return;
    access.testsInOwnCode = globalBindingAccesses.contains(value.opcode);
    if (PayloadWord::level(value.opcode))
        access.level = value.level();
    if (PayloadWord::key(value.opcode))
        access.key = value.key();
    if (PayloadWord::receivers(value.opcode)) {
        if (LayoutSetID receivers = value.receivers()) {
            access.receivers = program.at(receivers).written().span();
            if (program.at(receivers).listsEveryLayout())
                access.numberOfCandidates = access.receivers.size();
        }
    }
    if (PayloadWord::callees(value.opcode)) {
        if (std::optional<ProvenValue<Callees>> callees = value.callees() ? program.at(value.callees()).all() : std::nullopt)
            access.numberOfCandidates = callees->value().functions.size() + callees->value().builtins.size();
    }

    if (Value::opcodesWithPlacedKeys.contains(value.opcode)) {
        for (PlacedKey placedKey : value.placedKeys()) {
            if (placedKey.accessSite)
                functor(placedKey.accessSite, AccessDescription { access.accessClass, placedKey.level, placedKey.key, access.receivers });
        }
        return;
    }
    AccessSiteID stampedSite = PayloadWord::accessSite(value.opcode) ? value.accessSite() : AccessSiteID();
    AccessSiteID secondSite = PayloadWord::secondAccessSite(value.opcode) ? value.secondAccessSite() : AccessSiteID();
    forEachAccessSite(value, [&](AccessSiteID site) {
        if (site == stampedSite)
            return functor(site, access);
        functor(site, AccessDescription { .accessClass = access.accessClass, .key = secondSite && site == secondSite ? value.secondKey() : PropertyKeyID() });
    });
}

static void forEachDescribedSite(const Program&, const Terminator& terminator, const auto& functor)
{
    AccessDescription access { .accessClass = accessClassOf(terminator.kind) };
    if (access.accessClass == AccessClass::None)
        return;
    forEachAccessSite(terminator, [&](AccessSiteID site) {
        functor(site, access);
    });
}

template<typename InstructionType>
static void collectObservations(const Program& program, const FlowResults& flow, const Function& function, const InstructionType& instruction, BlockID block, TestsMadeForAccesses& tests, Vector<FlowSiteObservation>& observations)
{
    forEachDescribedSite(program, instruction, [&](AccessSiteID site, const AccessDescription& access) {
        FlowSiteObservation observation { .site = site, .key = access.key };
        switch (access.accessClass) {
        case AccessClass::Slot:
            observation.level = access.level ? *access.level : levelFromSiteLoss(flow, site);
            if (observation.level != KnowledgeLevel::Proven)
                observation.layout = findReportedReceiver(program, access.receivers, access.key);
            if (access.testsInOwnCode)
                observation.cost = AccessCost::TestsReceiver;
            else
                observation.cost = site.index() >= flow.numberOfAccessSites() ? AccessCost::Free : tests.charge(instruction, { block, flow.at(site).location });
            break;
        case AccessClass::ByName:
        case AccessClass::IndirectCall:
            observation.layout = access.receivers.empty() ? LayoutID() : access.receivers.front();
            observation.candidatesOfLeftOperation = access.numberOfCandidates;
            observation.cost = AccessCost::ByName;
            if constexpr (std::is_same_v<InstructionType, Value>) {
                if (auto receiver = receiverOf(instruction.opcode); receiver && PayloadWord::accessSite(instruction.opcode) && site == instruction.accessSite())
                    observation.receiverIsClassObject = program.typeOf(function, instruction.operand(*receiver)).isClassObjectOrNullish();
            }
            break;
        case AccessClass::DirectCall:
            observation.level = levelFromSiteLoss(flow, site);
            observation.cost = observation.level == KnowledgeLevel::Proven ? AccessCost::Free : AccessCost::TestsReceiver;
            break;
        case AccessClass::None:
            RELEASE_ASSERT_NOT_REACHED();
        }
        observation.frequency = function.at(block).frequency;
        observation.place = instruction.inlineFrame ? CopyPlace::Caller : CopyPlace::OwnBody;
        observation.isInGenericVersion = program.sourceFunctionOf(function, instruction.origin()).role == FunctionRole::GenericVersion;
        observations.append(observation);
    });
}

static uint8_t rarityOf(Expected<BlockFrequency> frequency)
{
    switch (frequency.predicted()) {
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

static void gradeSlowHalvesOfUnstampedOperations(const FlowResults& flow, const Function& function, const CFG& cfg, Vector<FlowSiteObservation>& observations)
{
    struct SlowHalf {
        Wire::SourceLocation location;
        uint8_t rarityOfOtherHalf { UINT8_MAX };
    };
    auto isCandidate = [&](const FlowSiteObservation& observation) {
        return observation.cost == AccessCost::ByName && observation.frequency.predicted() != BlockFrequency::Normal && !observation.isInGenericVersion
            && observation.site.index() < flow.numberOfAccessSites() && hasModule(flow.at(observation.site).location);
    };

    Vector<SlowHalf, 8> slowHalves;
    for (const FlowSiteObservation& observation : observations) {
        if (isCandidate(observation))
            slowHalves.append(SlowHalf { flow.at(observation.site).location });
    }
    if (slowHalves.isEmpty())
        return;
    auto orderKey = [](const SlowHalf& half) {
        return std::tuple { half.location.module, half.location.position };
    };
    AOT::sortInLine(slowHalves, { }, orderKey);

    auto find = [&](Wire::SourceLocation location) -> SlowHalf* {
        auto found = std::ranges::lower_bound(slowHalves, orderKey({ location }), { }, orderKey);
        return found != slowHalves.end() && found->location == location ? &*found : nullptr;
    };

    for (BlockID blockID : cfg.reversePostOrder()) {
        const Block& block = function.at(blockID);
        uint8_t rarity = rarityOf(block.frequency);
        if (rarity == rarityOf(BlockFrequency::Rare))
            continue;
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            AccessClass accessClass = accessClassOf(value.opcode);
            if (accessClass == AccessClass::ByName || accessClass == AccessClass::IndirectCall || isCastOrConversion(value.opcode) || constants.contains(value.opcode))
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

static void collectGeneralOperation(const Program& program, const Function& function, FunctionID id, const Value& value, double heat, Vector<GeneralOperation>& operations)
{
    AccessKind kind = kindOfGeneralOperation(value);
    if (kind == AccessKind::Unknown)
        return;
    Wire::SourceLocation location = function.locationOf(value.origin());
    if (!hasModule(location) || program.sourceFunctionOf(function, value.origin()).role == FunctionRole::GenericVersion)
        return;
    FunctionID inlined = value.inlineFrame ? function.inlineFrames[value.inlineFrame].callee : FunctionID();
    GeneralOperation operation { .location = location, .kind = kind, .function = inlined ? inlined : id, .heat = heat };
    bool hasMaker = false;
    value.forEachChild([&](ValueID operand) {
        if (hasMaker || constants.contains(function.at(operand).opcode) || !program.typeOf(function, operand).hasObjects())
            return;
        const Value* definition = &function.at(sameObjectThroughCasts(function, operand));
        if (definition->opcode == Opcode::ResultAt)
            definition = &function.at(sameObjectThroughCasts(function, definition->resultsOperand()));
        operation.isParameter = definition->opcode == Opcode::Parameter;
        if (!operation.isParameter && PayloadWord::accessSite(definition->opcode))
            operation.madeBy = definition->accessSite();
        hasMaker = operation.isParameter || operation.madeBy;
    });
    operations.append(operation);
}

static ObservationsOfFunction observeAccessSites(const Program& program, const FlowResults& flow, BuildContext& build, const Heat* heat, FunctionID id)
{
    ObservationsOfFunction observations;
    const Function& function = program.at(id);
    CFG cfg(function);
    TestsMadeForAccesses tests { program, function, cfg };
    std::optional<FunctionAnalyses> analyses;
    double weightOfFunction = 0;
    if (heat) {
        analyses.emplace(AnalysisContext { build, program, &flow, nullptr, id }, function);
        weightOfFunction = 100.0 / (101 - std::min<unsigned>(heat->percentileOf(program, id).predicted(), 100));
        observations.withHeat = makeUnique<ObservationsOfFunction::WithHeat>();
    }
    for (BlockID blockID : cfg.reversePostOrder()) {
        const Block& block = function.at(blockID);
        for (ValueID value : block.values)
            collectObservations(program, flow, function, function.at(value), blockID, tests, observations.sites);
        collectObservations(program, flow, function, block.terminator, blockID, tests, observations.sites);
        if (!heat)
            continue;
        double heatOfBlock = weightOfFunction * analyses->blockFrequencies().frequencyOf(blockID).predicted();
        Vector<double>& heatOfSites = observations.withHeat->heatOfSites;
        heatOfSites.insertFill(heatOfSites.size(), heatOfBlock, observations.sites.size() - heatOfSites.size());
        for (ValueID value : block.values)
            collectGeneralOperation(program, function, id, function.at(value), heatOfBlock, observations.withHeat->generalOperations);
    }
    gradeSlowHalvesOfUnstampedOperations(flow, function, cfg, observations.sites);
    observations.sites.shrinkToFit();
    if (heat) {
        observations.withHeat->heatOfSites.shrinkToFit();
        observations.withHeat->generalOperations.shrinkToFit();
    }
    return observations;
}

static constexpr uint8_t numberOfDistances = 6;

static Vector<FlowSiteLevel> foldSiteLevels(const FlowResults& flow, std::span<const ObservationsOfFunction> observationsOfFunctions, Vector<double>& heat)
{
    Vector<FlowSiteLevel> levels(flow.numberOfAccessSites());
    if (std::ranges::any_of(observationsOfFunctions, [](const ObservationsOfFunction& observations) { return !!observations.withHeat; }))
        heat.fill(0, levels.size());
    Vector<uint8_t> nearestRarities(FillWith { }, levels.size(), uint8_t { UINT8_MAX });
    for (const ObservationsOfFunction& observations : observationsOfFunctions) {
        for (size_t position = 0; position < observations.sites.size(); ++position) {
            const FlowSiteObservation& observation = observations.sites[position];
            size_t index = observation.site.index();
            if (index >= levels.size())
                continue;
            FlowSiteLevel& folded = levels[index];
            if (observations.withHeat && !observation.isInGenericVersion)
                heat[index] += observations.withHeat->heatOfSites[position];
            uint8_t rarity = rarityOf(observation.frequency);
            if (rarity < nearestRarities[index]) {
                nearestRarities[index] = rarity;
                folded.copies = 0;
            }
            if (rarity == nearestRarities[index])
                ++folded.copies;

            constexpr uint8_t numberOfRarities = 3;
            uint8_t distance = static_cast<uint8_t>(observation.place) * numberOfRarities + rarity;
            uint8_t distanceForCost = observation.isInGenericVersion * numberOfDistances + distance;
            if (distanceForCost < folded.distanceForCost || (distanceForCost == folded.distanceForCost && rankOf(observation.cost) < rankOf(folded.cost))) {
                folded.distanceForCost = distanceForCost;
                folded.cost = observation.cost;
            }

            if (distance > folded.distance || (distance == folded.distance && rankOf(observation.level) >= rankOf(folded.level)))
                continue;
            folded.distance = distance;
            folded.level = observation.level;
            folded.layout = observation.layout;
            folded.key = observation.key;
            folded.candidatesOfLeftOperation = observation.candidatesOfLeftOperation;
            folded.receiverOfLeftOperationIsClassObject = observation.receiverIsClassObject;
        }
    }

    for (FlowSiteLevel& level : levels) {
        if (level.distanceForCost >= numberOfDistances && level.distanceForCost != UINT8_MAX && level.cost == AccessCost::ByName)
            level.cost = AccessCost::Free;
    }
    return levels;
}

struct AccessSiteObservations::OfFunctions {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(OfFunctions);

    Vector<ObservationsOfFunction> observations;
};

AccessSiteObservations::AccessSiteObservations() = default;
AccessSiteObservations::AccessSiteObservations(AccessSiteObservations&&) = default;
AccessSiteObservations& AccessSiteObservations::operator=(AccessSiteObservations&&) = default;
AccessSiteObservations::~AccessSiteObservations() = default;

static AccessSiteObservations collectObservationsOfFunctions(const Program& program, BuildContext& build, const BitVector& hasNoCode, const auto& observationsOf)
{
    AccessSiteObservations result;
    result.ofFunctions = makeUnique<AccessSiteObservations::OfFunctions>();
    result.ofFunctions->observations = collectInParallel<ObservationsOfFunction>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<ObservationsOfFunction>& found) {
        if (program.functions[index].hasBody() && !hasNoCode.get(index))
            found.append(observationsOf(index));
    });
    return result;
}

AccessSiteObservations observeAccessSites(const Program& program, const FlowResults& flow, BuildContext& build, const Heat* heat)
{
    BitVector hasNoCode;
    if (program.form() >= IRForm::Selected)
        hasNoCode = findBodiesWithoutCode(program, build);
    return collectObservationsOfFunctions(program, build, hasNoCode, [&](unsigned index) {
        return observeAccessSites(program, flow, build, heat, FunctionID(index));
    });
}

struct AccessSiteObserver::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    const Program& program;
    const FlowResults& flow;
    BuildContext& build;
    const Heat* heat;
    ParallelArray<ObservationsOfFunction> observations { build.parallelLoops(), static_cast<unsigned>(program.functions.size()), [](unsigned) { return ObservationsOfFunction(); } };
    ParallelArray<Vector<FunctionID>> calledFunctions { build.parallelLoops(), static_cast<unsigned>(program.functions.size()), [](unsigned) { return Vector<FunctionID>(); } };
};

AccessSiteObserver::AccessSiteObserver(const Program& program, const FlowResults& flow, BuildContext& build, const Heat* heat)
    : m_state(new State { program, flow, build, heat })
{
}

AccessSiteObserver::~AccessSiteObserver() = default;

void AccessSiteObserver::observe(FunctionID id)
{
    m_state->observations[id.index()] = observeAccessSites(m_state->program, m_state->flow, m_state->build, m_state->heat, id);
    Vector<FunctionID>& called = m_state->calledFunctions[id.index()];
    forEachCalledFunction(m_state->program, m_state->program.at(id), [&](FunctionID target) {
        called.append(target);
    });
    called.shrinkToFit();
}

AccessSiteObservations AccessSiteObserver::finish()
{
    const Program& program = m_state->program;
    RELEASE_ASSERT(program.form() >= IRForm::Selected && m_state->observations.size() == program.functions.size());
    BitVector hasNoCode = findBodiesWithoutCode(program, m_state->build, m_state->calledFunctions.span());
    AccessSiteObservations result = collectObservationsOfFunctions(program, m_state->build, hasNoCode, [&](unsigned index) {
        return WTF::move(m_state->observations[index]);
    });
    m_state->build.destroyLater(WTF::move(m_state->observations));
    m_state->build.destroyLater(WTF::move(m_state->calledFunctions));
    return result;
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

static FlowSiteLoss lossOfKnownReceiver(const PropertyAccessContext& context, AccessKind kind, const FlowSiteLevel& level)
{
    if (!level.layout)
        return { };
    if (Access access = memberAccessOf(kind); access != Access::None) {
        MemberAnswer member = findMember(context, { .start = MemberStart::Instance, .layout = level.layout, .key = level.key, .access = access, .isOwnLookup = false });
        if (member.kind != MemberKind::Field) {
            if (member.cause != UnprovenCause::Unknown)
                return { member.cause, LimitID::None, noWitness };
        } else if (const LocationFlow& location = flowOf(context.program, FieldReference { member.holder, member.field }).location; location.cause != UnprovenCause::Unknown)
            return { location.cause, LimitID::None, location.witness };
    }
    const LayoutFlow& flow = context.program.at(level.layout).flow;
    return { flow.cause, LimitID::None, flow.witness };
}

static FlowSiteLoss lossBelowProven(const Program& program, const FlowResults& flow, const BuildContext& build, AccessSiteID site, const FlowSiteLevel& level)
{
    // What flow lost, it lost before the tests were placed. Behind them the receiver of the operation that is left may have its class.
    FlowSiteLoss loss = level.receiverOfLeftOperationIsClassObject ? FlowSiteLoss { UnprovenCause::LeftByPasses, LimitID::None, noWitness } : flow.lossOf(site);
    if (loss.cause == UnprovenCause::Unknown)
        loss = lossOfKnownReceiver({ program, &flow }, flow.at(site).kind, level);
    if (loss.cause == UnprovenCause::Unknown)
        loss = level.candidatesOfLeftOperation > build.limit(LimitID::MaxDispatchCases) ? FlowSiteLoss { UnprovenCause::LimitReached, LimitID::MaxDispatchCases, noWitness } : FlowSiteLoss { };
    if (loss.cause == UnprovenCause::Unknown)
        loss.cause = UnprovenCause::LeftByPasses;
    if (hasFunction(loss.witness) && !hasModule(loss.witness.location))
        loss.witness.location = program.at(FunctionID(loss.witness.function)).declarationLocation();
    if (!hasFunction(loss.witness) || !hasModule(loss.witness.location))
        loss.witness = { flow.at(site).function.index(), Wire::none, flow.writtenLocationOf(site) };
    loss.witness.function = findFunctionAsRead(&program, FunctionID(loss.witness.function)).index();
    return loss;
}

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

static void fillLoss(Wire::AccessRecord& record, const FlowSiteLoss& loss)
{
    record.cause = loss.cause == UnprovenCause::DataFromOutside ? UnprovenCause::UnknownObject : loss.cause;
    record.limit = loss.limit;
    record.witness = loss.witness;
    record.illTypedCause = illTypedCauseOf(loss.cause);
    record.hasOtherRoots = loss.hasOtherRoots;
    record.otherOrigins = loss.otherOrigins;
}

static FlowSiteLoss lossOfGeneralOperation(const Program& program, const FlowResults& flow, const BuildContext& build, std::span<const FlowSiteLevel> levels, const GeneralOperation& operation)
{
    FlowSiteLoss loss;
    if (operation.madeBy && operation.madeBy.index() < levels.size()) {
        // Of the value that the access made, what flow lost is still what is not known.
        FlowSiteLevel level = levels[operation.madeBy.index()];
        level.receiverOfLeftOperationIsClassObject = false;
        loss = lossBelowProven(program, flow, build, operation.madeBy, level);
    }
    else if (operation.isParameter)
        loss = flow.lossOfParameters(operation.function);
    if (loss.cause == UnprovenCause::Unknown || loss.cause == UnprovenCause::LeftByPasses)
        loss = { UnprovenCause::Untracked, LimitID::None, noWitness };
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
    for (const ObservationsOfFunction& observations : observationsOfFunctions) {
        if (observations.withHeat)
            operations.appendVector(observations.withHeat->generalOperations);
    }
    AOT::stableSort(operations, { }, [](const GeneralOperation& a) {
        return std::tuple { a.location.module, a.location.position, a.kind, !a.madeBy, a.madeBy.index(), !a.isParameter, a.function.index() };
    });

    Vector<Wire::AccessRecord> records;
    for (const GeneralOperation& operation : operations) {
        if (!records.isEmpty() && records.last().location == operation.location && records.last().kind == operation.kind) {
            records.last().copies = saturatingSum<uint32_t>(records.last().copies, 1u);
            records.last().heat += operation.heat;
            continue;
        }
        records.append(Wire::AccessRecord {
            .location = operation.location,
            .kind = operation.kind,
            .level = KnowledgeLevel::Unknown,
            .layout = Wire::none,
            .copies = 1,
            .distance = 0,
            .function = findFunctionAsRead(&program, operation.function).index(),
            .cost = AccessCost::ByName,
            .heat = operation.heat,
        });
        fillLoss(records.last(), lossOfGeneralOperation(program, flow, build, levels, operation));
    }
    return records;
}

GradedSites gradeAccessSites(const Program& program, const FlowResults& flow, AccessSiteObservations&& observations, BuildContext& build)
{
    std::unique_ptr<AccessSiteObservations::OfFunctions> ofFunctions = WTF::move(observations.ofFunctions);
    if (!ofFunctions)
        return { };
    GradedSites sites;
    sites.levels = foldSiteLevels(flow, ofFunctions->observations.span(), sites.heat);
    sites.generalOperations = buildRecordsOfGeneralOperations(program, flow, build, sites.levels.span(), ofFunctions->observations.span());
    build.destroyLater(WTF::move(ofFunctions));
    return sites;
}

static bool isReported(const FlowResults& flow, const AccessSite& access, const FlowSiteLevel& level)
{
    if (!flow.mayBeReached(access.function) || access.kind == AccessKind::Unknown || !hasModule(access.location))
        return false;
    return access.kind == AccessKind::Call || access.kind == AccessKind::Construct || level.copies;
}

Vector<Wire::AccessRecord> buildAccessRecords(const Program& program, const FlowResults& flow, std::span<const FlowSiteLevel> levels, std::span<const double> heat, BuildContext& build)
{
    struct SiteRecord {
        Wire::AccessRecord record;
        PropertyKeyID key;
        bool isInGenericVersion { false };
    };
    const ParallelLoops& loops = build.parallelLoops();
    Vector<SiteRecord> sites = collectInParallel<SiteRecord>(loops, levels.size(), [&](unsigned index, Vector<SiteRecord>& found) {
        AccessSiteID site(index);
        const AccessSite& access = flow.at(site);
        const FlowSiteLevel& level = levels[index];
        if (!isReported(flow, access, level))
            return;
        bool isCall = access.kind == AccessKind::Call || access.kind == AccessKind::Construct;
        found.append(SiteRecord { {
            .location = flow.writtenLocationOf(site),
            .kind = access.kind,
            .level = level.level,
            .layout = findLayoutAsRead(&program, level.layout).index(),
            .copies = level.copies,
            .distance = isCall ? level.distance : level.distanceForCost,
            .function = findFunctionAsRead(&program, access.function).index(),
            .cost = level.cost,
            .heat = heat.empty() ? 0 : heat[index],
        }, level.key, program.at(access.function).role == FunctionRole::GenericVersion });
        fillLoss(found.last().record, level.level != KnowledgeLevel::Proven || level.cost != AccessCost::Free ? lossBelowProven(program, flow, build, site, level) : FlowSiteLoss { });
    });

    auto orderKey = [](const SiteRecord& site) {
        const Wire::AccessRecord& record = site.record;
        return std::tuple {
            record.location.module, record.location.position, record.kind, !site.key, site.key.index(), rankOf(record.level), record.cause,
            record.witness.function, record.witness.value, record.function, record.layout, record.limit, record.illTypedCause,
        };
    };
    parallelSort(loops, sites.mutableSpan(), [&](const SiteRecord& a, const SiteRecord& b) {
        return orderKey(a) < orderKey(b);
    });

    // One place may hold accesses to several names.
    Vector<Wire::AccessRecord> records;
    PropertyKeyID keyOfLastRecord;
    bool costIsOfGenericVersion = false;
    for (const SiteRecord& site : sites) {
        if (records.isEmpty() || records.last().location != site.record.location || records.last().kind != site.record.kind || keyOfLastRecord != site.key) {
            records.append(site.record);
            keyOfLastRecord = site.key;
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

static std::array<uint32_t, numberOfEnumerators<KnowledgeLevel>>* countersOf(Wire::AccessCountRecord& record, AccessKind kind)
{
    switch (kind) {
    case AccessKind::Unknown:
        break;
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

static Vector<Wire::AccessCountRecord> countByModule(const Program& program, std::span<const Wire::TypeTestRecord> typeTests, const auto& forEachAccess)
{
    Vector<Wire::AccessCountRecord> counts(FillWith { }, program.modules.size(), Wire::AccessCountRecord { });
    for (uint32_t module = 0; module < counts.size(); ++module)
        counts[module].module = module;
    forEachAccess([&](uint32_t module, AccessKind kind, unsigned rankOfLevel, unsigned rankOfCost) {
        Wire::AccessCountRecord& record = counts[module];
        auto* countsByLevel = countersOf(record, kind);
        if (!countsByLevel)
            return;
        ++(*countsByLevel)[rankOfLevel];
        if (countsByLevel == &record.accesses)
            ++record.accessCosts[rankOfCost];
    });
    for (const Wire::TypeTestRecord& typeTest : typeTests)
        ++counts[typeTest.location.module].typeTests;

    counts.removeAllMatching([](const Wire::AccessCountRecord& record) {
        return record == Wire::AccessCountRecord { .module = record.module };
    });
    return counts;
}

Vector<Wire::AccessCountRecord> countAccesses(const Program& program, std::span<const Wire::AccessRecord> accesses, std::span<const Wire::TypeTestRecord> typeTests)
{
    return countByModule(program, typeTests, [&](const auto& count) {
        for (const Wire::AccessRecord& access : accesses)
            count(access.location.module, access.kind, rankOf(access.level), rankOf(access.cost));
    });
}

Vector<Wire::AccessCountRecord> countAccessesWithoutRecords(const Program& program, const FlowResults& flow, std::span<const FlowSiteLevel> levels, std::span<const Wire::TypeTestRecord> typeTests, BuildContext& build)
{
    struct CountedSite {
        uint32_t module { 0 };
        uint32_t position { 0 };
        PropertyKeyID key;
        uint8_t kind { 0 };
        uint8_t rankOfLevel { 0 };
        uint8_t rankOfCost { 0 };
        bool isInGenericVersion { false };
    };
    static_assert(sizeof(CountedSite) == 16);
    const ParallelLoops& loops = build.parallelLoops();
    Vector<CountedSite> sites = collectInParallel<CountedSite>(loops, levels.size(), [&](unsigned index, Vector<CountedSite>& found) {
        AccessSiteID site(index);
        const AccessSite& access = flow.at(site);
        const FlowSiteLevel& level = levels[index];
        if (!isReported(flow, access, level))
            return;
        Wire::SourceLocation location = flow.writtenLocationOf(site);
        found.append(CountedSite {
            location.module, location.position, level.key, static_cast<uint8_t>(access.kind), static_cast<uint8_t>(rankOf(level.level)), static_cast<uint8_t>(rankOf(level.cost)),
            program.at(access.function).role == FunctionRole::GenericVersion,
        });
    });
    // One record stands for the sites of one place, kind and name: with the least level, and with the least cost outside generic versions, or of all where there is none outside.
    auto orderKey = [](const CountedSite& site) {
        return std::tuple { site.module, site.position, site.kind, site.key.index(), site.isInGenericVersion, site.rankOfCost };
    };
    parallelSort(loops, sites.mutableSpan(), [&](const CountedSite& a, const CountedSite& b) {
        return orderKey(a) < orderKey(b);
    });
    return countByModule(program, typeTests, [&](const auto& count) {
        for (size_t first = 0; first < sites.size();) {
            const CountedSite& site = sites[first];
            uint8_t rankOfLevel = site.rankOfLevel;
            size_t end = first + 1;
            for (; end < sites.size() && sites[end].module == site.module && sites[end].position == site.position && sites[end].kind == site.kind && sites[end].key == site.key; ++end)
                rankOfLevel = std::min(rankOfLevel, sites[end].rankOfLevel);
            count(site.module, static_cast<AccessKind>(site.kind), rankOfLevel, site.rankOfCost);
            first = end;
        }
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
