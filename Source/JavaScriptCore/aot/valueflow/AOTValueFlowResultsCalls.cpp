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
        AOT::sort(application.bodies[index].*stamps, { }, [](const auto& entry) { return entry.value; });
        inputs.build.statistics().count(statisticOfUnknownOperands, numberOfUnknownOperands);
    });
    TypeRemap remap = phase->finish(StringRemap { }, &inputs.loops());
    inputs.build.destroyLater(WTF::move(phase));
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        if (!remap.isIdentityFor(index))
            remapTypes(application.bodies[index], remap);
    });
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

void computeAccessResults(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    using ListedReceiversKnownToWorker = HashMap<uint64_t, const ListedReceivers*, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    struct Listings {
        ListedReceiversKnownToWorker sets;
        ListedReceiversKnownToWorker blocks;
        Vector<uint32_t> lastListing;
        uint32_t numberOfListings { 0 };
    };
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    FlowApplication& application = *tables.application;
    Vector<Listings> listingsOfWorker(inputs.loops().width());
    SharedMemo<ListedReceivers> listedSets;
    SharedMemo<ListedReceivers> listedBlocks;
    Vector<uint64_t> layoutsWithFieldOfKey = findLayoutsWithFieldOfKey(inputs, application);
    Vector<uint32_t> layoutsOfMembers = findLayoutsOfMembers(inputs);
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
    auto receiversSeenToArrive = [&](const StampedOperand& receiver) -> std::optional<LayoutSet> {
        if (!PayloadWord::key(receiver.statement.opcode) || ValueSets::isComplement(receiver.seen))
            return std::nullopt;
        PropertyKeyID key = receiver.function.at(receiver.statement.site).key();
        if (!key)
            return std::nullopt;
        const ListedReceivers& listed = listedReceiversOf(receiver);
        if (listed.layouts.isEmpty())
            return std::nullopt;
        if (BuiltinMemberName name = builtinMemberNameOf(program, key); name && !name.isPrivateAlias) {
            for (BuiltinClassID builtinClass : listed.builtinClasses) {
                if (program.builtins().findForInstance(builtinClass, name.name).outcome == BuiltinMemberLookup::Outcome::Found)
                    return std::nullopt;
            }
        }

        LayoutSet receivers;
        bool areTooMany = false;
        auto add = [&](LayoutID layout) {
            if (receivers.members.size() == inputs.build.limit(LimitID::MaxLayoutsInType))
                areTooMany = true;
            else
                receivers.members.append(layout);
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
        if (areTooMany || receivers.members.isEmpty())
            return std::nullopt;
        receivers.isComplete = false;
        canonicalize(receivers);
        return receivers;
    };

    ValueSetID arraysThatMustNotBeConverted = inputs.containers ? inputs.containers->arraysThatMustNotBeConverted : ValueSets::nothing;
    if (arraysThatMustNotBeConverted != ValueSets::nothing) {
        tables.sites.receiversThatMayHaveDecidedStorage.fill(true, tables.application->accessSites.numberOfSites());
        findOpensOfUndecidedStorage(inputs, *tables.application, arraysThatMustNotBeConverted);
    }
    stampOperands<hasReceiverStamp, receiverOperandOf, &FlowBodyUpdate::accesses>(inputs, types, tables, FlowResultKind::ReceiverSets, StatisticID::FlowAccessesOfUnknownReceivers, [&](const StampedOperand& receiver) {
        ValueID site = receiver.statement.site;
        if (arraysThatMustNotBeConverted != ValueSets::nothing && receiver.maker.intersect(receiver.held, arraysThatMustNotBeConverted) == ValueSets::nothing)
            tables.sites.receiversThatMayHaveDecidedStorage[tables.application->accessSites.siteOfValue(receiver.id, site).index()] = false;
        if (std::optional<LayoutSet> receivers = receiver.positions.receiversAt(receiver.family, receiver.held); receivers && receivers->isComplete) {
            receiver.update.accesses.append(FlowAccessStamp { site, receiver.types.intern(WTF::move(*receivers)) });
            return;
        }
        if (std::optional<LayoutSet> receivers = receiversSeenToArrive(receiver))
            receiver.update.accesses.append(FlowAccessStamp { site, receiver.types.intern(WTF::move(*receivers)) });
        FlowSiteLoss loss = lossOfStampedOperand(receiver, [&](FlowSiteLoss& loss) {
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
                if (isDataFromOutside(inputs, object))
                    loss.cause = UnprovenCause::DataFromOutside;
            });
            // No line of the program makes a built-in object, so the access is the place to show.
            if (mayBeBuiltinObject)
                loss = { .cause = UnprovenCause::NonLayoutObject, .hasOtherRoots = hasFunction(loss.witness) };
        });
        loss.seesConstructedObject = !ValueSets::isComplement(receiver.seen) && sets.meet(receiver.positions.withoutWhatPlaceholdersCover(receiver.seen), universe.constructedObjects());
        tables.sites.accessSiteLosses[application.accessSites.siteOfValue(receiver.id, site).index()] = loss;
    });
}

static bool callsValue(const ValueFlowStatement& statement)
{
    return statement.kind == ValueFlowStatementKind::Operation && flowRuleOf(statement.opcode) == FlowRule::Call;
}

ValueFlowStampedCalls computeCallResults(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = inputs.program.functions.size();
    auto stampCall = [&](const StampedOperand& callee) {
        std::optional<CalleeSet> callees = callee.positions.calleesAt(callee.family, callee.held, callee.seen, OperandPosition::newTarget(callee.statement.opcode) ? &callee.operand : nullptr);
        AccessSiteID site = application.accessSites.siteOfValue(callee.id, callee.statement.site);
        if (!callees || !callees->isComplete) {
            tables.sites.accessSiteLosses[site.index()] = lossOfStampedOperand(callee, [&](FlowSiteLoss& loss) {
                loss.cause = UnprovenCause::UnknownCallee;
                inputs.sets().forEachMember(callee.maker.intersect(callee.held, inputs.universe().otherCallables()), [&](ValueFlowObjectID object) {
                    if (!hasFunction(loss.witness))
                        loss.witness = witnessOfAllocation(inputs, object);
                });
            });
        }
        if (!callees || (!callees->isComplete && callees->functions.isEmpty() && callees->builtins.isEmpty()))
            return;

        tables.sites.callsOfBoundFunctions[site.index()] = false;
        callee.update.calls.append(FlowCallStamp { callee.statement.site, callee.types.intern(WTF::move(*callees)) });
    };
    stampOperands<callsValue, [](Opcode) { return 0u; }, &FlowBodyUpdate::calls>(inputs, types, tables, FlowResultKind::CalleeSets, StatisticID::FlowCallsOfUnknownCallees, stampCall);

    ValueFlowStampedCalls result;
    result.functionsInStamps = BitVector(numberOfFunctions);
    Vector<BitVector> functionsSeenBy(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index, unsigned worker) {
        if (application.bodies[index].calls.isEmpty())
            return;
        BitVector& functions = functionsSeenBy[worker];
        functions.ensureSize(numberOfFunctions);
        for (const FlowCallStamp& stamp : application.bodies[index].calls) {
            for (FunctionID function : types.at(stamp.callees).functions)
                functions.quickSet(function.index());
        }
    });
    for (const BitVector& functions : functionsSeenBy)
        result.functionsInStamps.merge(functions);
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
