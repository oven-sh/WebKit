/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTFlowStamps.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTValueFlowPositions.h"
#include <atomic>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static bool hasReceiverStamp(const ValueFlowStatement& statement)
{
    return statement.kind == ValueFlowStatementKind::Operation && PayloadWord::receivers(statement.opcode);
}

static unsigned receiverOperandOf(Opcode opcode)
{
    if (std::optional<unsigned> object = OperandPosition::object(opcode))
        return *object;
    std::optional<unsigned> base = OperandPosition::base(opcode);
    RELEASE_ASSERT(base);
    return *base;
}

namespace {

struct StampedOperand {
    FunctionID id;
    const Function& function;
    const ValueFlowStatement& statement;
    const ValueFlowOperand& operand;
    ProvenType typeByRules;
    const Type& family;
    ValueSetID held;
    ValueSetID seen;
    TypeArena& types;
    ValueFlowPositions& positions;
    ValueSetMaker& maker;
    FlowBodyUpdate& update;
    unsigned worker;
};

struct ListedReceivers {
    uint64_t name;
    Vector<LayoutID> layouts;
    Vector<BuiltinClassID, 4> builtinClasses;
};

} // anonymous namespace

static FlowSiteLoss lossOfStampedOperand(const StampedOperand& stamped, NOESCAPE const Invocable<void(FlowSiteLoss&)> auto& explain)
{
    FlowSiteLoss loss = stamped.positions.lossOf(stamped.positions.resultAt(stamped.typeByRules, stamped.held), stamped.operand.node);
    if (loss.cause == UnprovenCause::Unknown)
        explain(loss);
    if (!hasFunction(loss.witness))
        loss.witness = { stamped.id.index(), stamped.statement.site.index(), stamped.function.locationOf(stamped.statement.site) };
    return loss;
}

template<auto isStamped, auto operandIndexOf, auto stamps>
static void stampOperands(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables, FlowResultKind kind, StatisticID statisticOfUnknownOperands, const Invocable<void(const StampedOperand&)> auto& stamp)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = program.functions.size();
    RELEASE_ASSERT(tables.sites.accessSiteLosses.size() == application.accessSites.numberOfSites());

    ValueFlowPositionCounts counts(nullptr);
    ValueSetMakers makers(inputs);
    auto phase = makeUnique<TypeInterningPhase>(types, numberOfFunctions);
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index, unsigned worker) {
        FunctionID id(index);
        std::span<const ValueFlowStatement> statements = inputs.bodies().statementsOf(index);
        if (!inputs.isReached(id) || std::ranges::none_of(statements, isStamped))
            return;

        const Function& function = program.functions[index];
        if (!inputs.build.isEnabled(kind, index)) {
            for (const ValueFlowStatement& statement : statements) {
                if (isStamped(statement))
                    tables.sites.accessSiteLosses[application.accessSites.siteOfValue(id, statement.site).index()] = FlowSiteLoss { .cause = UnprovenCause::ResultDisabled };
            }
            return;
        }

        TypeArena arena(*phase, index);
        ValueSetMaker& maker = makers.of(worker);
        ValueFlowPositions positions(inputs, arena, counts, index, &maker);
        uint64_t numberOfUnknownOperands = 0;
        for (const ValueFlowStatement& statement : statements) {
            if (!isStamped(statement))
                continue;
            unsigned operandIndex = operandIndexOf(statement.opcode);
            const ValueFlowOperand& operand = inputs.bodies().operandsOf(statement)[operandIndex];
            ProvenType typeByRules = Proven::byRulesBehindChecks(function.at(function.at(statement.site).operand(operandIndex)).type);
            const Type& family = program.at(typeByRules);
            ValueSetID held = inputs.possibleValuesOf(maker, operand);
            ValueSetID seen = inputs.seenValuesOf(maker, operand);
            numberOfUnknownOperands += inputs.sets().contains(held, ValueFlowUniverse::unknownObject());
            stamp(StampedOperand { id, function, statement, operand, typeByRules, family, held, seen, arena, positions, maker, application.bodies[index], worker });
        }
        AOT::sortInLine(application.bodies[index].*stamps, { }, [](const auto& entry) { return entry.value; });
        inputs.build.statistics().count(statisticOfUnknownOperands, numberOfUnknownOperands);
    });
    TypeRemap remap = phase->finish(StringRemap { }, &inputs.loops());
    inputs.build.destroyLater(WTF::move(phase));
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        if (!remap.isIdentityFor(index))
            remapTypes(application.bodies[index], remap);
    });
}

// n statements that hand over the same set ask about it once.
using SetsOfOnlyDataFromOutside = UncheckedKeyHashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

static void noteWhetherOnlyDataFromOutsideComesOut(const ValueFlowResultInputs& inputs, FlowResultTables& tables, const StampedOperand& stamped, SetsOfOnlyDataFromOutside& answers)
{
    if (!stamped.statement.result)
        return;
    ValueSetID result = inputs.solver().possibleValuesOf(stamped.statement.result);
    if (ValueSets::isComplement(result) || result == ValueSets::nothing)
        return;
    auto answer = answers.ensure(result.index(), [&] {
        const ValueFlowUniverse& universe = inputs.universe();
        const ValueSets& sets = inputs.sets();
        if (sets.contains(result, ValueFlowUniverse::unknownObject()))
            return false;
        ValueSetID objects = stamped.maker.intersect(result, universe.knownObjects());
        if (objects == ValueSets::nothing)
            return false;
        bool isOnlyDataFromOutside = true;
        sets.forEachMember(objects, [&](ValueFlowObjectID object) {
            isOnlyDataFromOutside = isOnlyDataFromOutside && stamped.positions.isDataFromOutside(object);
        });
        return isOnlyDataFromOutside;
    });
    if (answer.iterator->value)
        tables.sites.sitesThatHandOverOnlyDataFromOutside[tables.application->accessSites.siteOfValue(stamped.id, stamped.statement.site).index()] = true;
}

static Vector<uint64_t> findLayoutsWithFieldOfKey(const ValueFlowResultInputs& inputs, const FlowApplication& application)
{
    const Program& program = inputs.program;
    Vector<uint64_t> holders = collectInParallel<uint64_t>(inputs.loops(), program.layouts.size(), [&](unsigned index, Vector<uint64_t>& found) {
        auto add = [&](PropertyKeyID key) {
            if (key)
                found.append(static_cast<uint64_t>(key.index()) << 32 | index);
        };
        LayoutID current(index);
        for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
            const Layout& layout = program.at(current);
            for (const Field& field : layout.fields)
                add(field.key);
            current = layout.flow.baseState == BaseState::Layout ? layout.flow.base : LayoutID();
        }
        for (const FlowAppendedField& field : application.layouts[index].appendedFields)
            add(field.key);
        current = program.layouts[index].has(LayoutFlags::IsOfCreatedObject) ? LayoutID(index) : LayoutID();
        for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
            const ClassFlow& classFlow = application.layouts[current.index()].classFlow;
            for (const ClassMember& member : classFlow.namedMembers)
                add(member.key);
            current = classFlow.prototypeBase;
        }
    });
    sortAndRemoveDuplicates(inputs.loops(), holders);
    return holders;
}

static Vector<uint32_t> findLayoutsOfMembers(const ValueFlowResultInputs& inputs)
{
    const ValueFlowUniverse& universe = inputs.universe();
    Vector<uint32_t> layouts(universe.numberOfObjects());
    constexpr uint32_t membersPerTask = 4096;
    forEachIndexInParallel(inputs.loops(), (layouts.size() + membersPerTask - 1) / membersPerTask, [&](unsigned task) {
        for (uint32_t index = task * membersPerTask; index < std::min<size_t>(layouts.size(), (task + 1) * membersPerTask); ++index) {
            ValueFlowObjectID member(index);
            LayoutID layout = universe.isObject(member) && !ValueFlowResultInputs::isPlaceholder(member) ? universe.layoutOf(member) : LayoutID();
            layouts[index] = layout && !inputs.isLayoutOfDictionaries(layout) ? layout.index() + 1 : 0;
        }
    });
    return layouts;
}

// The reads that an iteration over an array becomes have no site of their own: the open says it for them.
static void findOpensOfUndecidedStorage(const ValueFlowResultInputs& inputs, FlowApplication& application, ValueSetID arraysThatMustNotBeConverted)
{
    ValueSetMakers makers(inputs);
    forEachIndexInParallel(inputs.loops(), inputs.program.functions.size(), [&](unsigned index, unsigned worker) {
        if (!inputs.isReached(FunctionID(index)) || !inputs.build.isEnabled(FlowResultKind::ReceiverSets, index))
            return;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::IteratorOpen)
                continue;
            ValueSetMaker& maker = makers.of(worker);
            ValueSetID held = inputs.possibleValuesOf(maker, inputs.bodies().operandsOf(statement)[OperandIndex::IteratorOpen::iterable]);
            if (maker.intersect(held, arraysThatMustNotBeConverted) == ValueSets::nothing)
                application.bodies[index].opensOfUndecidedStorage.append(statement.site);
        }
    });
}

namespace {

struct ProxyMadeAtSite {
    ValueFlowObjectID object;
    ValueSetID handlers;
    ValueSetID targets;
};

} // anonymous namespace

// Sorted by object. A call that native code makes, or one with a list for its arguments, has no entry.
static Vector<ProxyMadeAtSite> findProxiesMadeAtSites(const ValueFlowResultInputs& inputs)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const BuiltinTable& builtins = inputs.program.builtins();
    if (!builtins.contains(Builtins::Proxy) || !builtins.contains(Builtins::ProxyRevocable))
        return { };
    if (inputs.solver().usesOf(Builtins::Proxy).isEmpty() && inputs.solver().usesOf(Builtins::ProxyRevocable).isEmpty())
        return { };
    ValueSetMakers makers(inputs);
    Vector<ProxyMadeAtSite> made = collectWithMakers<ProxyMadeAtSite>(inputs, makers, inputs.program.functions.size(), [&](unsigned function, ValueSetMaker& maker, Vector<ProxyMadeAtSite>& found) {
        if (!inputs.isReached(FunctionID(function)))
            return;
        for (const ValueFlowStatement& call : inputs.bodies().statementsOf(function)) {
            if (call.kind != ValueFlowStatementKind::Operation || (call.opcode != Opcode::Construct && call.opcode != Opcode::Call))
                continue;
            std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(call);
            constexpr unsigned firstArgument = 2;
            if (operands.size() < firstArgument + 2)
                continue;
            ValueSetID callees = inputs.possibleValuesOf(maker, operands[0]);
            bool answersProxy = call.opcode == Opcode::Construct;
            if (ValueSets::isComplement(callees) || !inputs.sets().contains(callees, universe.objectOf(answersProxy ? Builtins::Proxy : Builtins::ProxyRevocable)))
                continue;
            if (ValueFlowObjectID object = universe.siteObject(call, answersProxy ? 0 : 1))
                found.append({ object, inputs.seenValuesOf(maker, operands[firstArgument + 1]), inputs.seenValuesOf(maker, operands[firstArgument]) });
        }
    });
    AOT::sort(made, { }, &ProxyMadeAtSite::object);
    return made;
}

void computeAccessResults(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    using ListedReceiversKnownToWorker = HashMap<uint64_t, const ListedReceivers*, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    struct Listings {
        ListedReceiversKnownToWorker sets;
        ListedReceiversKnownToWorker blocks;
        Vector<uint32_t> lastListing;
        uint32_t numberOfListings { 0 };
        // Many accesses have the same receivers, and many of those the same key.
        HashMap<uint32_t, FlowSiteLoss, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lossesOfReceivers;
        HashMap<uint64_t, std::optional<LayoutSet>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> receiversSeenUnderKeys;
        Vector<FlowProxiesSeenAtSite> proxiesSeenAtSites;
    };
    AOT_THRESHOLD(uint32_t, fewestMembersToRemember, 32);
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    FlowApplication& application = *tables.application;
    Vector<Listings> listingsOfWorker(inputs.loops().width());
    SharedMemo<ListedReceivers> listedSets;
    SharedMemo<ListedReceivers> listedBlocks;
    Vector<uint64_t> layoutsWithFieldOfKey = findLayoutsWithFieldOfKey(inputs, application);
    Vector<uint32_t> layoutsOfMembers = findLayoutsOfMembers(inputs);
    if (inputs.build.isEnabledEverywhere(FlowResultKind::ReceiverSets)) {
        BitVector privateNamesOfAppendedFields;
        for (const FlowLayoutUpdate& layout : application.layouts) {
            for (const FlowAppendedField& field : layout.appendedFields) {
                if (program.at(field.key).kind == PropertyKeyKind::PrivateName)
                    privateNamesOfAppendedFields.set(field.key.index());
            }
        }
        for (size_t key : privateNamesOfAppendedFields) {
            auto begin = std::ranges::lower_bound(layoutsWithFieldOfKey, static_cast<uint64_t>(key) << 32);
            auto end = std::ranges::lower_bound(begin, layoutsWithFieldOfKey.end(), static_cast<uint64_t>(key + 1) << 32);
            if (static_cast<uint64_t>(end - begin) > inputs.build.limit(LimitID::MaxLayoutsInType))
                continue;
            application.layoutsWithPrivateNames.append({ PropertyKeyID(static_cast<uint32_t>(key)), Vector<LayoutID>(static_cast<size_t>(end - begin), [&](size_t index) {
                return LayoutID(static_cast<uint32_t>(begin[index]));
            }) });
        }
    }
    constexpr uint16_t noClass = UINT16_MAX;
    static_assert(BuiltinClassID::hashTableDeletedIndex < noClass);
    auto builtinClassesOfMembers = std::make_unique<std::atomic<uint16_t>[]>(universe.numberOfObjects());
    bool verifies = inputs.build.options().verifiesAfterEveryPass;

    auto beginListing = [&](Listings& listings) {
        if (listings.lastListing.isEmpty())
            listings.lastListing.fill(0, program.layouts.size());
        return ++listings.numberOfListings;
    };
    auto listReceiver = [&](const StampedOperand& receiver, ValueFlowObjectID member, uint32_t listing, ListedReceivers& listed) {
        if (member == ValueFlowUniverse::unknownObject() || member == ValueFlowUniverse::untrackedObject())
            return;
        if (uint32_t layout = layoutsOfMembers[member.index()]) {
            if (std::exchange(listingsOfWorker[receiver.worker].lastListing[layout - 1], listing) != listing)
                listed.layouts.append(LayoutID(layout - 1));
            return;
        }
        auto findClass = [&]() -> uint16_t {
            Type type = receiver.positions.typeOfObject(member);
            if (type.isUndefinedOrNull())
                return noClass;
            BuiltinReceiver builtinReceiver = builtinReceiverOf(program, type.withoutNullish());
            bool isInstance = builtinReceiver.kind == BuiltinReceiverKind::Instance || builtinReceiver.kind == BuiltinReceiverKind::Primitive;
            return isInstance && builtinReceiver.builtinClass ? static_cast<uint16_t>(builtinReceiver.builtinClass.index() + 1) : noClass;
        };
        std::atomic<uint16_t>& entry = builtinClassesOfMembers[member.index()];
        uint16_t known = entry.load(std::memory_order_relaxed);
        if (!known) {
            known = findClass();
            entry.store(known, std::memory_order_relaxed);
        } else if (verifies)
            RELEASE_ASSERT(known == findClass());
        if (known != noClass && !listed.builtinClasses.contains(BuiltinClassID(known - 1u)))
            listed.builtinClasses.append(BuiltinClassID(known - 1u));
    };
    auto listedUnder = [&](ListedReceiversKnownToWorker& known, SharedMemo<ListedReceivers>& knownToAll, uint64_t name, const auto& list) -> const ListedReceivers& {
        auto result = known.add(name, nullptr);
        if (!result.isNewEntry)
            return *result.iterator->value;
        uint32_t hash = static_cast<uint32_t>((name * 0x9e3779b97f4a7c15ull) >> 32);
        auto hasName = [&](const ListedReceivers& listed) {
            return listed.name == name;
        };
        if (const ListedReceivers* found = knownToAll.find(hash, hasName)) {
            result.iterator->value = found;
            return *found;
        }
        ListedReceivers listed { name, { }, { } };
        list(listed);
        listed.layouts.shrinkToFit();
        const ListedReceivers& kept = knownToAll.add(hash, WTF::move(listed), hasName);
        known.set(name, &kept);
        return kept;
    };
    auto listedReceiversOf = [&](const StampedOperand& receiver) -> const ListedReceivers& {
        Listings& listings = listingsOfWorker[receiver.worker];
        return listedUnder(listings.sets, listedSets, receiver.seen.index(), [&](ListedReceivers& listed) {
            AOT_THRESHOLD(uint32_t, fewestMembersToListByBlock, 128);
            if (ValueSets::isComplement(receiver.seen) || sets.size(receiver.seen) < fewestMembersToListByBlock) {
                uint32_t listing = beginListing(listings);
                sets.forEachMember(receiver.seen, [&](ValueFlowObjectID member) {
                    listReceiver(receiver, member, listing, listed);
                });
            } else {
                Vector<const ListedReceivers*, 64> blocks;
                sets.forEachBlock(receiver.seen, [&](uint64_t nameOfBlock) {
                    blocks.append(&listedUnder(listings.blocks, listedBlocks, nameOfBlock, [&](ListedReceivers& listedInBlock) {
                        uint32_t listing = beginListing(listings);
                        sets.forEachMemberOfBlock(nameOfBlock, [&](ValueFlowObjectID member) {
                            listReceiver(receiver, member, listing, listedInBlock);
                        });
                    }));
                });
                uint32_t listing = beginListing(listings);
                for (const ListedReceivers* block : blocks) {
                    for (LayoutID layout : block->layouts) {
                        if (std::exchange(listings.lastListing[layout.index()], listing) != listing)
                            listed.layouts.append(layout);
                    }
                    for (BuiltinClassID builtinClass : block->builtinClasses) {
                        if (!listed.builtinClasses.contains(builtinClass))
                            listed.builtinClasses.append(builtinClass);
                    }
                }
            }
            AOT::sort(listed.layouts);
        });
    };
    // An empty set: no object that was seen to arrive has the name.
    auto receiversSeenToArrive = [&](const StampedOperand& receiver) -> std::optional<LayoutSet> {
        if (!PayloadWord::key(receiver.statement.opcode) || ValueSets::isComplement(receiver.seen))
            return std::nullopt;
        PropertyKeyID key = receiver.function.at(receiver.statement.site).key();
        if (!key)
            return std::nullopt;
        const ListedReceivers& listed = listedReceiversOf(receiver);
        if (listed.layouts.isEmpty())
            return LayoutSet::open({ });
        std::optional<LayoutSet>* remembered = nullptr;
        if (listed.layouts.size() >= fewestMembersToRemember) {
            auto known = listingsOfWorker[receiver.worker].receiversSeenUnderKeys.add(static_cast<uint64_t>(receiver.seen.index()) << 32 | key.index(), std::nullopt);
            if (!known.isNewEntry)
                return known.iterator->value;
            remembered = &known.iterator->value;
        }
        if (BuiltinMemberName name = builtinMemberNameOf(program, key); name && !name.isPrivateAlias) {
            for (BuiltinClassID builtinClass : listed.builtinClasses) {
                if (program.builtins().findForInstance(builtinClass, name.name).outcome == BuiltinMemberLookup::Outcome::Found)
                    return std::nullopt;
            }
        }

        Vector<LayoutID> seen;
        bool areTooMany = false;
        auto add = [&](LayoutID layout) {
            if (seen.size() == inputs.build.limit(LimitID::MaxLayoutsInType))
                areTooMany = true;
            else
                seen.append(layout);
        };
        uint64_t firstHolder = static_cast<uint64_t>(key.index()) << 32;
        auto begin = std::ranges::lower_bound(layoutsWithFieldOfKey, firstHolder);
        auto end = std::ranges::lower_bound(begin, layoutsWithFieldOfKey.end(), firstHolder + (1ull << 32));
        std::span<const uint64_t> holders = layoutsWithFieldOfKey.subspan(begin - layoutsWithFieldOfKey.begin(), end - begin);
        if (holders.size() <= listed.layouts.size()) {
            for (uint64_t holder : holders) {
                LayoutID layout(static_cast<uint32_t>(holder));
                if (!areTooMany && std::ranges::binary_search(listed.layouts, layout))
                    add(layout);
            }
        } else {
            for (LayoutID layout : listed.layouts) {
                if (!areTooMany && std::ranges::binary_search(holders, firstHolder | layout.index()))
                    add(layout);
            }
        }
        if (areTooMany)
            return std::nullopt;
        LayoutSet receivers = LayoutSet::open(WTF::move(seen));
        canonicalize(receivers);
        if (remembered)
            *remembered = receivers;
        return receivers;
    };

    ValueSetID arraysThatMustNotBeConverted = inputs.containers ? inputs.containers->arraysThatMustNotBeConverted : ValueSets::nothing;
    tables.sites.keepsStorageOfListThatMayReadUndefined = inputs.containers && inputs.containers->keepsStorageOfListThatMayReadUndefined;
    if (arraysThatMustNotBeConverted != ValueSets::nothing) {
        tables.sites.receiversThatMayHaveDecidedStorage.fill(true, tables.application->accessSites.numberOfSites());
        findOpensOfUndecidedStorage(inputs, *tables.application, arraysThatMustNotBeConverted);
    }
    tables.sites.receiversSeenAtSites.fill(0, tables.application->accessSites.numberOfSites());
    tables.sites.sitesThatHandOverOnlyDataFromOutside.fill(false, tables.application->accessSites.numberOfSites());
    Vector<SetsOfOnlyDataFromOutside> answersOfWorker(inputs.loops().width());
    Vector<ProxyMadeAtSite> proxiesMade = findProxiesMadeAtSites(inputs);
    ValueSetID objectsOfProxiesMade = ValueSets::nothing;
    {
        ValueSetMakers makers(inputs);
        for (const ProxyMadeAtSite& made : proxiesMade)
            objectsOfProxiesMade = makers.of(0).unite(objectsOfProxiesMade, makers.of(0).of(made.object));
    }
    // A value that many places hand on to many others brings a proxy wherever it brings everything else. A proxy is the usual receiver where little else is seen to arrive.
    AOT_THRESHOLD(uint32_t, mostOtherObjectsSeenBesideProxies, 4);
    auto noteProxiesSeenAt = [&](const StampedOperand& receiver) {
        if (objectsOfProxiesMade == ValueSets::nothing || ValueSets::isComplement(receiver.seen) || !sets.meet(receiver.seen, objectsOfProxiesMade))
            return;
        if (sets.size(receiver.maker.intersect(receiver.seen, receiver.maker.intersect(universe.knownObjects(), ValueSets::complement(objectsOfProxiesMade)))) > mostOtherObjectsSeenBesideProxies)
            return;
        ValueSetID handlers = ValueSets::nothing;
        ValueSetID targets = ValueSets::nothing;
        sets.forEachMember(receiver.maker.intersect(receiver.seen, objectsOfProxiesMade), [&](ValueFlowObjectID object) {
            const ProxyMadeAtSite& made = *std::ranges::lower_bound(proxiesMade, object, { }, &ProxyMadeAtSite::object);
            handlers = receiver.maker.unite(handlers, made.handlers);
            targets = receiver.maker.unite(targets, made.targets);
        });
        bool seesLists = false;
        auto layoutsIn = [&](ValueSetID objects) {
            Vector<LayoutID, 4> layouts;
            seesLists = false;
            if (ValueSets::isComplement(objects))
                return layouts;
            bool areTooMany = false;
            sets.forEachMember(objects, [&](ValueFlowObjectID object) {
                uint32_t layout = layoutsOfMembers[object.index()];
                if (!layout && universe.isObject(object) && !ValueFlowResultInputs::isPlaceholder(object)) {
                    Type type = receiver.positions.typeOfObject(object);
                    const ClassPart* part = type.singleBuiltinClass();
                    // What is neither is not told apart: no list of targets is kept.
                    if (part && part->builtinClass == BuiltinClasses::Array)
                        seesLists = true;
                    else
                        areTooMany = true;
                }
                if (!layout || layouts.contains(LayoutID(layout - 1)))
                    return;
                if (layouts.size() == layouts.capacity())
                    areTooMany = true;
                else
                    layouts.append(LayoutID(layout - 1));
            });
            if (areTooMany) {
                layouts.clear();
                seesLists = false;
            }
            AOT::sort(layouts);
            return layouts;
        };
        Vector<LayoutID, 4> layoutsOfHandlers = layoutsIn(handlers);
        // A handler that is a list has no trap that anybody knows.
        if (seesLists)
            layoutsOfHandlers.clear();
        Vector<LayoutID, 4> layoutsOfTargets = layoutsIn(targets);
        FlowProxiesSeenAtSite seenAtSite { application.accessSites.siteOfValue(receiver.id, receiver.statement.site), WTF::move(layoutsOfHandlers), WTF::move(layoutsOfTargets) };
        seenAtSite.seesListsAsTargets = seesLists;
        listingsOfWorker[receiver.worker].proxiesSeenAtSites.append(WTF::move(seenAtSite));
    };
    AOT_THRESHOLD(uint32_t, fewestReceiversToRemember, 32);
    Vector<UncheckedKeyHashMap<uint32_t, LayoutSetID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> receiversOfWorker(inputs.loops().width());
    stampOperands<hasReceiverStamp, receiverOperandOf, &FlowBodyUpdate::accesses>(inputs, types, tables, FlowResultKind::ReceiverSets, StatisticID::FlowAccessesOfUnknownReceivers, [&](const StampedOperand& receiver) {
        ValueID site = receiver.statement.site;
        noteProxiesSeenAt(receiver);
        noteWhetherOnlyDataFromOutsideComesOut(inputs, tables, receiver, answersOfWorker[receiver.worker]);
        if (arraysThatMustNotBeConverted != ValueSets::nothing && receiver.maker.intersect(receiver.held, arraysThatMustNotBeConverted) == ValueSets::nothing)
            tables.sites.receiversThatMayHaveDecidedStorage[tables.application->accessSites.siteOfValue(receiver.id, site).index()] = false;
        // n accesses of a value that may be any of n objects name the same set: its layouts are listed, hashed and interned once for a worker.
        bool remembersReceivers = !ValueSets::isComplement(receiver.held) && sets.size(receiver.held) >= fewestReceiversToRemember;
        if (remembersReceivers) {
            auto& known = receiversOfWorker[receiver.worker];
            if (auto found = known.find(receiver.held.index()); found != known.end()) {
                if (std::optional<LayoutSetID> again = receiver.types.useAgain(found->value)) {
                    receiver.update.accesses.append(FlowAccessStamp { site, *again });
                    return;
                }
            }
        }
        if (std::optional<LayoutSet> receivers = receiver.positions.receiversAt(receiver.held)) {
            LayoutSetID interned = receiver.types.intern(WTF::move(*receivers));
            if (remembersReceivers)
                receiversOfWorker[receiver.worker].set(receiver.held.index(), interned);
            receiver.update.accesses.append(FlowAccessStamp { site, interned });
            return;
        }
        if (std::optional<LayoutSet> receivers = receiversSeenToArrive(receiver)) {
            if (receivers->written().isEmpty())
                tables.sites.receiversSeenAtSites[application.accessSites.siteOfValue(receiver.id, site).index()] = 1;
            else
                receiver.update.accesses.append(FlowAccessStamp { site, receiver.types.intern(WTF::move(*receivers)) });
        }
        auto explain = [&](FlowSiteLoss& loss) {
            loss.cause = UnprovenCause::NonLayoutObject;
            bool mayBeBuiltinObject = false;
            sets.forEachMember(receiver.maker.intersect(receiver.held, universe.knownObjects()), [&](ValueFlowObjectID object) {
                if (universe.layoutOf(object))
                    return;
                Witness witness = witnessOfAllocation(inputs, object);
                if (!hasFunction(witness)) {
                    mayBeBuiltinObject = true;
                    return;
                }
                if (hasFunction(loss.witness)) {
                    loss.hasOtherRoots |= witness != loss.witness;
                    return;
                }
                loss.witness = witness;
                if (receiver.positions.isDataFromOutside(object))
                    loss.cause = UnprovenCause::DataFromOutside;
            });
            // No line of the program makes a built-in object, so the access is the place to show.
            if (mayBeBuiltinObject)
                loss = { .cause = UnprovenCause::NonLayoutObject, .hasOtherRoots = hasFunction(loss.witness) };
        };
        FlowSiteLoss loss = lossOfStampedOperand(receiver, [&](FlowSiteLoss& loss) {
            if (loss != FlowSiteLoss { } || sets.size(receiver.held) < fewestMembersToRemember) {
                explain(loss);
                return;
            }
            auto known = listingsOfWorker[receiver.worker].lossesOfReceivers.add(receiver.held.index(), FlowSiteLoss { });
            if (known.isNewEntry)
                explain(known.iterator->value);
            loss = known.iterator->value;
        });
        loss.seesConstructedObject = !ValueSets::isComplement(receiver.seen) && sets.meet(receiver.positions.withoutWhatPlaceholdersCover(receiver.seen), universe.constructedObjects());
        tables.sites.accessSiteLosses[application.accessSites.siteOfValue(receiver.id, site).index()] = loss;
    });
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        for (const FlowAccessStamp& stamp : application.bodies[index].accesses)
            tables.sites.receiversSeenAtSites[application.accessSites.siteOfValue(FunctionID(index), stamp.value).index()] = stamp.receivers.index() + 2;
    });
    for (Listings& listings : listingsOfWorker)
        tables.sites.proxiesSeenAtSites.appendVector(WTF::move(listings.proxiesSeenAtSites));
    AOT::sort(tables.sites.proxiesSeenAtSites, { }, &FlowProxiesSeenAtSite::site);
}

static bool callsValue(const ValueFlowStatement& statement)
{
    return statement.kind == ValueFlowStatementKind::Operation && flowRuleOf(statement.opcode) == FlowRule::Call;
}

ValueFlowStampedCalls computeCallResults(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = inputs.program.functions.size();
    // Where nothing is constructed the callees follow from the two sets of values. n calls of a value that may be any of n functions name the same two sets:
    // the set of callees is made, hashed and interned once for a worker, and each task takes it again from the storage that its arena holds.
    AOT_THRESHOLD(uint32_t, fewestMembersToRemember, 32);
    struct CalleesOfValues {
        bool isKnown { false };
        bool listsEveryCallee { false };
        bool isEmpty { true };
        CalleeSetID interned;
    };
    using CalleesOfWorker = UncheckedKeyHashMap<uint64_t, CalleesOfValues, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    Vector<CalleesOfWorker> calleesOfWorker(inputs.loops().width());
    auto findCallees = [&](const StampedOperand& callee, bool isConstruction) {
        std::optional<CalleeSet> found = callee.positions.calleesAt(callee.held, callee.seen, isConstruction ? &callee.operand : nullptr);
        if (!found)
            return CalleesOfValues { };
        CalleesOfValues callees { true, found->listsEveryCallee(), found->written().isEmpty(), { } };
        if (callees.listsEveryCallee || !callees.isEmpty)
            callees.interned = callee.types.intern(WTF::move(*found));
        return callees;
    };
    auto stampCall = [&](const StampedOperand& callee) {
        bool isConstruction = !!OperandPosition::newTarget(callee.statement.opcode);
        CalleesOfValues found;
        if (isConstruction || ValueSets::isComplement(callee.held) || ValueSets::isComplement(callee.seen) || inputs.sets().size(callee.held) < fewestMembersToRemember)
            found = findCallees(callee, isConstruction);
        else {
            auto added = calleesOfWorker[callee.worker].add(static_cast<uint64_t>(callee.held.index()) << 32 | callee.seen.index(), CalleesOfValues { });
            CalleesOfValues& known = added.iterator->value;
            std::optional<CalleeSetID> again;
            if (!added.isNewEntry && known.interned)
                again = callee.types.useAgain(known.interned);
            if (added.isNewEntry || (known.interned && !again))
                known = findCallees(callee, false);
            else if (again)
                known.interned = *again;
            found = known;
        }
        const CalleesOfValues* callees = found.isKnown ? &found : nullptr;
        AccessSiteID site = application.accessSites.siteOfValue(callee.id, callee.statement.site);
        if (!callees || !callees->listsEveryCallee) {
            tables.sites.accessSiteLosses[site.index()] = lossOfStampedOperand(callee, [&](FlowSiteLoss& loss) {
                loss.cause = UnprovenCause::UnknownCallee;
                inputs.sets().forEachMember(callee.maker.intersect(callee.held, inputs.universe().otherCallables()), [&](ValueFlowObjectID object) {
                    if (!hasFunction(loss.witness))
                        loss.witness = witnessOfAllocation(inputs, object);
                });
            });
        }
        if (!callees || (!callees->listsEveryCallee && callees->isEmpty))
            return;

        tables.sites.callsOfBoundFunctions[site.index()] = false;
        callee.update.calls.append(FlowCallStamp { callee.statement.site, callees->interned });
    };
    stampOperands<callsValue, [](Opcode) { return 0u; }, &FlowBodyUpdate::calls>(inputs, types, tables, FlowResultKind::CalleeSets, StatisticID::FlowCallsOfUnknownCallees, stampCall);

    ValueFlowStampedCalls result;
    result.functionsInStamps = BitVector(numberOfFunctions);
    Vector<BitVector> functionsSeenBy(inputs.loops().width());
    Vector<BitVector> setsSeenBy(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index, unsigned worker) {
        if (application.bodies[index].calls.isEmpty())
            return;
        BitVector& functions = functionsSeenBy[worker];
        functions.ensureSize(numberOfFunctions);
        for (const FlowCallStamp& stamp : application.bodies[index].calls) {
            if (setsSeenBy[worker].set(stamp.callees.index()))
                continue;
            for (FunctionID function : types.at(stamp.callees).followed().value())
                functions.quickSet(function.index());
        }
    });
    for (const BitVector& functions : functionsSeenBy)
        result.functionsInStamps.merge(functions);
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
