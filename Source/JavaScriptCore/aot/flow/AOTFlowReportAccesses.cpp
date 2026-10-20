/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowReportInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTDiagnostics.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTResultRules.h"
#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::tuple_size_v<decltype(Wire::AccessCountRecord::accesses)> == numberOfEnumerators<KnowledgeLevel>);
static_assert(std::tuple_size_v<decltype(Wire::AccessCountRecord::calls)> == numberOfEnumerators<KnowledgeLevel>);

namespace {

struct AccessDescription {
    AccessClass accessClass { AccessClass::None };
    std::optional<KnowledgeLevel> level;
    PropertyKeyID key;
    std::span<const LayoutID> receivers;
    uint32_t numberOfCandidates { 0 };
};

// Which calls are inlined depends on heat, so a copy in a caller speaks for the site only where the function has lost its own body.
// A call enters the generic version of a function where flow does not know the caller, as an access takes its slow half where a prediction fails.
enum class CopyPlace : uint8_t { OwnBody, Caller, GenericVersion };

struct FlowSiteObservation {
    AccessSiteID site;
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    BlockFrequency frequency { BlockFrequency::Normal };
    CopyPlace place { CopyPlace::OwnBody };
    LayoutID layout;
    PropertyKeyID key;
    uint32_t candidatesOfLeftOperation { 0 };
};

} // anonymous namespace

static AccessDescription accessDescriptionOf(const Program& program, const Value& value)
{
    AccessDescription access { .accessClass = accessClassOf(value.opcode) };
    if (access.accessClass == AccessClass::None)
        return access;
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
    return knowsReceivers ? KnowledgeLevel::Proven : KnowledgeLevel::Predicted;
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

static CopyPlace placeOf(const Function& function, const Instruction& instruction)
{
    if (instruction.inlineFrame)
        return CopyPlace::Caller;
    return function.role == FunctionRole::GenericVersion ? CopyPlace::GenericVersion : CopyPlace::OwnBody;
}

template<typename InstructionType>
static void collectObservations(const Program& program, const FlowResults& flow, const Function& function, const InstructionType& instruction, BlockFrequency frequency, Vector<FlowSiteObservation>& observations)
{
    forEachDescribedSite(program, instruction, [&](AccessSiteID site, const AccessDescription& access) {
        FlowSiteObservation observation = observationOf(program, flow, access, site);
        observation.frequency = frequency;
        observation.place = placeOf(function, instruction);
        observations.append(observation);
    });
}

static bool isWrittenTypeCheck(Opcode opcode)
{
    return opcode == Opcode::Claim || std::ranges::contains(replacedOperationsOf(opcode), Opcode::Claim);
}

static Wire::RemainingCheckRecord remainingCheckRecord(const Program& program, const AOT::CompileOptions& options, FunctionID id, ValueID valueID)
{
    const Function& function = program.at(id);
    const Value& check = function.at(valueID);
    Wire::SourceLocation location = function.locationOf(valueID);
    Wire::SourceLocation origin = location;
    TypeSiteKind siteKind = TypeSiteKind::Unknown;
    if (TypeSiteID site = PayloadWord::site(check.opcode) ? check.site() : TypeSiteID()) {
        const TypeSite& typeSite = program.at(site);
        origin = { typeSite.module.index(), typeSite.position };
        siteKind = typeSite.kind;
    }
    FunctionID source = check.inlineFrame ? function.inlineFrames[check.inlineFrame].callee : id;
    return {
        .id = remainingCheckIDOf(program, options, origin, siteKind),
        .witness = { findFunctionAsRead(&program, source).index(), valueID.index(), location },
        .origin = origin,
        .cause = PayloadWord::cause(check.opcode) ? check.cause() : CheckCause::Unknown,
        .siteKind = static_cast<uint32_t>(siteKind),
    };
}

static Vector<FlowSiteObservation> observeAccessSites(const Program& program, const FlowResults& flow, FunctionID id)
{
    Vector<FlowSiteObservation> observations;
    const Function& function = program.at(id);
    CFG cfg(function);
    for (BlockID blockID : cfg.reversePostOrder()) {
        const Block& block = function.at(blockID);
        for (ValueID value : block.values)
            collectObservations(program, flow, function, function.at(value), block.frequency, observations);
        collectObservations(program, flow, function, block.terminator, block.frequency, observations);
    }
    return observations;
}

Vector<Wire::RemainingCheckRecord> collectRemainingChecks(const Program& program, const AOT::CompileOptions& options, FunctionID id)
{
    Vector<Wire::RemainingCheckRecord> checks;
    const Function& function = program.at(id);
    CFG cfg(function);
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (isWrittenTypeCheck(function.at(value).opcode))
                checks.append(remainingCheckRecord(program, options, id, value));
        }
    }
    return checks;
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

static uint8_t distanceOf(const FlowSiteObservation& observation)
{
    constexpr uint8_t numberOfRarities = 3;
    return static_cast<uint8_t>(observation.place) * numberOfRarities + rarityOf(observation.frequency);
}

namespace {

struct NearestCopies {
    uint8_t rarity { UINT8_MAX };
    uint8_t distance { UINT8_MAX };
};

} // anonymous namespace

static void foldObservation(FlowSiteLevel& folded, NearestCopies& nearest, const FlowSiteObservation& observation)
{
    // The slow half of a guarded access is a copy in a block one class rarer than its fast half. The fast half decides the level of the site, so the slow half is not counted.
    uint8_t rarity = rarityOf(observation.frequency);
    if (rarity < nearest.rarity) {
        nearest.rarity = rarity;
        folded.copies = 0;
    }
    if (rarity == nearest.rarity)
        ++folded.copies;

    uint8_t distance = distanceOf(observation);
    if (distance > nearest.distance || (distance == nearest.distance && rankOf(observation.level) >= rankOf(folded.level)))
        return;
    nearest.distance = distance;
    folded.level = observation.level;
    folded.layout = observation.layout;
    folded.key = observation.key;
    folded.candidatesOfLeftOperation = observation.candidatesOfLeftOperation;
}

static Vector<FlowSiteLevel> foldSiteLevels(const FlowResults& flow, std::span<const Vector<FlowSiteObservation>> observationsOfFunctions)
{
    Vector<FlowSiteLevel> levels(flow.numberOfAccessSites());
    Vector<NearestCopies> nearestCopies(levels.size());
    for (const Vector<FlowSiteObservation>& observations : observationsOfFunctions) {
        for (const FlowSiteObservation& observation : observations) {
            size_t index = observation.site.index();
            if (index < levels.size())
                foldObservation(levels[index], nearestCopies[index], observation);
        }
    }
    return levels;
}

Vector<FlowSiteLevel> gradeAccessSites(const Program& program, const FlowResults& flow, BuildContext& build)
{
    auto observations = collectInParallel<Vector<FlowSiteObservation>>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<Vector<FlowSiteObservation>>& found) {
        if (program.functions[index].hasBody())
            found.append(observeAccessSites(program, flow, FunctionID(index)));
    });
    Vector<FlowSiteLevel> levels = foldSiteLevels(flow, observations.span());
    build.destroyLater(WTF::move(observations));
    return levels;
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
    return loss;
}

static Wire::AccessRecord accessRecordOf(const Program& program, const FlowResults& flow, const BuildContext& build, AccessSiteID site, const FlowSiteLevel& level)
{
    const AccessSite& access = flow.at(site);
    FlowSiteLoss loss;
    if (level.level != KnowledgeLevel::Proven)
        loss = lossBelowProven(program, flow, build, site, level);
    return {
        .location = access.location,
        .kind = access.kind,
        .level = level.level,
        .cause = loss.cause,
        .limit = loss.limit,
        .layout = findLayoutAsRead(&program, level.layout).index(),
        .copies = level.copies,
        .witness = loss.witness,
        .function = access.function.index(),
    };
}

static auto accessOrderKey(const Wire::AccessRecord& record)
{
    return std::tuple {
        record.location.module, record.location.position, record.kind, rankOf(record.level), record.cause,
        record.witness.function, record.witness.value, record.function, record.layout, record.limit,
    };
}

static bool isSameSourceAccess(const Wire::AccessRecord& a, const Wire::AccessRecord& b)
{
    return a.location == b.location && a.kind == b.kind;
}

static void foldDuplicateRecords(Vector<Wire::AccessRecord>& sortedRecords)
{
    size_t recordCount = 0;
    for (Wire::AccessRecord record : sortedRecords) {
        if (recordCount && isSameSourceAccess(sortedRecords[recordCount - 1], record)) {
            uint32_t& copies = sortedRecords[recordCount - 1].copies;
            copies = saturatingSum<uint32_t>(copies, record.copies);
            continue;
        }
        sortedRecords[recordCount++] = record;
    }
    sortedRecords.shrink(recordCount);
}

Vector<Wire::AccessRecord> buildAccessRecords(const Program& program, const FlowResults& flow, std::span<const FlowSiteLevel> levels, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<Wire::AccessRecord> records = collectInParallel<Wire::AccessRecord>(loops, levels.size(), [&](unsigned index, Vector<Wire::AccessRecord>& found) {
        AccessSiteID site(index);
        const AccessSite& access = flow.at(site);
        if (!flow.mayBeReached(access.function) || access.kind == AccessKind::Unknown || !hasModule(access.location))
            return;
        found.append(accessRecordOf(program, flow, build, site, levels[index]));
    });

    parallelSort(loops, records.mutableSpan(), [](const Wire::AccessRecord& a, const Wire::AccessRecord& b) {
        return accessOrderKey(a) < accessOrderKey(b);
    });
    foldDuplicateRecords(records);
    return records;
}

static std::array<uint32_t, numberOfEnumerators<KnowledgeLevel>>& countersOf(Wire::AccessCountRecord& record, AccessKind kind)
{
    switch (kind) {
    case AccessKind::Unknown:
        break;
    case AccessKind::Read:
    case AccessKind::Store:
    case AccessKind::Define:
    case AccessKind::Has:
    case AccessKind::Delete:
        return record.accesses;
    case AccessKind::Call:
    case AccessKind::Construct:
        return record.calls;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<Wire::AccessCountRecord> countAccesses(const Program& program, std::span<const Wire::AccessRecord> accesses, std::span<const Wire::TypeTestRecord> typeTests)
{
    Vector<Wire::AccessCountRecord> counts(FillWith { }, program.modules.size(), Wire::AccessCountRecord { });
    for (uint32_t module = 0; module < counts.size(); ++module)
        counts[module].module = module;
    for (const Wire::AccessRecord& access : accesses)
        ++countersOf(counts[access.location.module], access.kind)[rankOf(access.level)];
    for (const Wire::TypeTestRecord& typeTest : typeTests)
        ++counts[typeTest.location.module].typeTests;

    counts.removeAllMatching([](const Wire::AccessCountRecord& record) {
        return record == Wire::AccessCountRecord { .module = record.module };
    });
    return counts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
