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
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTValueFlowPositions.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// What a set lists beside the placeholders, undefined and null.
struct ListedReceivers {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ListedReceivers);
    // Ascending.
    Vector<LayoutID> layouts;
    // Of the instances of built-in classes and of the primitives.
    Vector<BuiltinClassID, 4> builtinClasses;
};

class ListedReceiversByName {
    WTF_MAKE_NONCOPYABLE(ListedReceiversByName);
public:
    ListedReceiversByName() = default;

    const ListedReceivers* find(uint64_t name)
    {
        Part& part = partFor(name);
        Locker locker { part.lock };
        auto found = part.byName.find(name);
        return found == part.byName.end() ? nullptr : found->value.get();
    }

    const ListedReceivers& add(uint64_t name, std::unique_ptr<ListedReceivers>&& listed)
    {
        Part& part = partFor(name);
        Locker locker { part.lock };
        return *part.byName.add(name, WTF::move(listed)).iterator->value;
    }

private:
    struct alignas(64) Part {
        Lock lock;
        HashMap<uint64_t, std::unique_ptr<ListedReceivers>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> byName WTF_GUARDED_BY_LOCK(lock);
    };

    Part& partFor(uint64_t name) { return m_parts[(name * 0x9e3779b97f4a7c15ull) >> 58]; }

    std::array<Part, 64> m_parts;
};

using ListedReceiversKnownToWorker = HashMap<uint64_t, const ListedReceivers*, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

class LayoutsWithFieldOfKey {
public:
    LayoutsWithFieldOfKey(const ValueFlowResultInputs& inputs, const FlowApplication& application)
    {
        const Program& program = inputs.program;
        m_holders = collectInParallel<uint64_t>(inputs.loops(), program.layouts.size(), [&](unsigned index, Vector<uint64_t>& found) {
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
        });
        sortAndRemoveDuplicates(inputs.loops(), m_holders);
    }

    std::span<const uint64_t> holdersOf(PropertyKeyID key) const LIFETIME_BOUND
    {
        uint64_t first = static_cast<uint64_t>(key.index()) << 32;
        auto begin = std::ranges::lower_bound(m_holders, first);
        auto end = std::ranges::lower_bound(begin, m_holders.end(), first + (1ull << 32));
        return m_holders.subspan(begin - m_holders.begin(), end - begin);
    }

private:
    Vector<uint64_t> m_holders;
};

// By member without a layout: the built-in class that the member is an instance or a primitive of.
class BuiltinClassesOfMembers {
    WTF_MAKE_NONCOPYABLE(BuiltinClassesOfMembers);
public:
    explicit BuiltinClassesOfMembers(const ValueFlowResultInputs& inputs)
        : m_classes(std::make_unique<std::atomic<uint16_t>[]>(inputs.universe().numberOfObjects()))
        , m_verifies(inputs.build.options().verifiesAfterEveryPass)
    {
    }

    BuiltinClassID ensure(ValueFlowObjectID member, NOESCAPE const Invocable<BuiltinClassID()> auto& find)
    {
        std::atomic<uint16_t>& entry = m_classes[member.index()];
        uint16_t known = entry.load(std::memory_order_relaxed);
        if (!known) {
            known = entryFor(find());
            entry.store(known, std::memory_order_relaxed);
        } else if (m_verifies)
            RELEASE_ASSERT(known == entryFor(find()));
        return known == noClass ? BuiltinClassID() : BuiltinClassID(known - 1u);
    }

private:
    static constexpr uint16_t noClass = UINT16_MAX;
    static_assert(BuiltinClassID::hashTableDeletedIndex < noClass);

    static uint16_t entryFor(BuiltinClassID builtinClass) { return builtinClass ? static_cast<uint16_t>(builtinClass.index() + 1) : noClass; }

    // Zero for a member that nobody has met, else one more than the index of the class.
    std::unique_ptr<std::atomic<uint16_t>[]> m_classes;
    bool m_verifies;
};

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

struct LayoutsMetByListing {
    Vector<uint32_t> lastListing;
    uint32_t numberOfListings { 0 };
};

struct AccessesOfFunction {
    const ValueFlowResultInputs& inputs;
    FunctionID id;
    const Function& function;
    TypeArena& types;
    ValueFlowPositions& positions;
    ValueSetMaker& maker;
    FlowBodyUpdate& update;
    FlowSiteTables& sites;
    const FlowAccessSiteNumbering& accessSites;
    const LayoutsWithFieldOfKey& layoutsWithFieldOfKey;
    ListedReceiversByName& listedReceiversOfSets;
    ListedReceiversKnownToWorker& listedReceivers;
    ListedReceiversByName& listedBlocksOfAll;
    ListedReceiversKnownToWorker& listedBlocks;
    const Vector<uint32_t>& layoutsOfMembers;
    LayoutsMetByListing& layoutsMet;
    BuiltinClassesOfMembers& builtinClassesOfMembers;
    uint64_t numberOfUnknownReceivers { 0 };
};

} // anonymous namespace

Witness witnessOfStatement(const Program& program, const ValueFlowStatement& statement)
{
    if (!statement.function || statement.function.index() >= program.functions.size())
        return noWitness;
    const Function& function = program.at(statement.function);
    if (!statement.site)
        return { statement.function.index(), Wire::none, function.declarationLocation() };
    return { statement.function.index(), statement.site.index(), function.locationOf(statement.site) };
}

// A rule brings the unknown object into being by an edge out of a constant.
ValueFlowBirthsOfUnknownObject::ValueFlowBirthsOfUnknownObject(const ValueFlowResultInputs& inputs)
{
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowGraph& graph = solver.graph();
    uint32_t numberOfNodes = graph.numberOfNodes();
    m_birthOfNode.fill(none, numberOfNodes);

    BitVector holdsUnknownObject(numberOfNodes);
    BitVector isConstant(numberOfNodes);
    for (uint32_t index = 0; index < numberOfNodes; ++index) {
        ValueFlowNodeID node(index);
        if (!inputs.sets().contains(solver.valueOf(node), ValueFlowUniverse::unknownObject()))
            continue;
        holdsUnknownObject.quickSet(index);
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(node);
        if (key && key->kind == ValueFlowMadeNodeKind::Constant)
            isConstant.quickSet(index);
    }
    auto forEachEdgeBetweenHolders = [&](const auto& functor) {
        for (size_t index : holdsUnknownObject) {
            if (isConstant.quickGet(index))
                continue;
            graph.forEachPredecessor(ValueFlowNodeID(static_cast<uint32_t>(index)), [&](ValueFlowNodeID predecessor) {
                if (holdsUnknownObject.quickGet(predecessor.index()))
                    functor(predecessor.index(), static_cast<uint32_t>(index));
            });
        }
    };

    Vector<uint32_t> firstSuccessor;
    firstSuccessor.fill(0u, numberOfNodes + 1);
    Vector<uint32_t> found;
    forEachEdgeBetweenHolders([&](uint32_t from, uint32_t to) {
        if (!isConstant.quickGet(from)) {
            ++firstSuccessor[from + 1];
            return;
        }
        // The witness is of the source that the report names for the node.
        if (solver.unknownSourceOf(ValueFlowEdge { ValueFlowNodeID(from), ValueFlowNodeID(to) }) != solver.unknownSourceOf(ValueFlowNodeID(to)))
            return;
        ValueFlowStatementID logger = solver.statementThatLogged({ ValueFlowNodeID(from), ValueFlowNodeID(to) });
        Witness witness = logger ? witnessOfStatement(inputs.program, solver.statementAt(logger)) : noWitness;
        if (m_birthOfNode[to] == none) {
            m_birthOfNode[to] = m_witnesses.size();
            m_witnesses.append(witness);
            found.append(to);
            return;
        }
        keepLowerWitness(m_witnesses[m_birthOfNode[to]], witness);
    });
    for (uint32_t index = 0; index < numberOfNodes; ++index)
        firstSuccessor[index + 1] += firstSuccessor[index];
    Vector<uint32_t> successors;
    successors.grow(firstSuccessor.last());
    Vector<uint32_t> nextSuccessor(firstSuccessor.span().first(numberOfNodes));
    forEachEdgeBetweenHolders([&](uint32_t from, uint32_t to) {
        if (!isConstant.quickGet(from))
            successors[nextSuccessor[from]++] = to;
    });

    // Of the births that are nearest to a node it takes the lowest: the numbers of the nodes follow the order of the solve, and the answer must not.
    BitVector isInNextLevel(numberOfNodes);
    size_t endOfLevel = 0;
    for (size_t next = 0; next < found.size(); ++next) {
        if (next == endOfLevel) {
            endOfLevel = found.size();
            for (uint32_t node : found.subspan(next))
                isInNextLevel.quickClear(node);
        }
        uint32_t node = found[next];
        for (uint32_t successor : successors.subspan(firstSuccessor[node], firstSuccessor[node + 1] - firstSuccessor[node])) {
            uint32_t& birth = m_birthOfNode[successor];
            if (birth == none) {
                birth = m_birthOfNode[node];
                isInNextLevel.quickSet(successor);
                found.append(successor);
            } else if (isInNextLevel.quickGet(successor) && isLowerWitness(m_witnesses[m_birthOfNode[node]], m_witnesses[birth]))
                birth = m_birthOfNode[node];
        }
    }
}

Witness ValueFlowBirthsOfUnknownObject::witnessOf(ValueFlowNodeID node) const
{
    if (!node || node.index() >= m_birthOfNode.size() || m_birthOfNode[node.index()] == none)
        return noWitness;
    return m_witnesses[m_birthOfNode[node.index()]];
}

Witness witnessOfAllocation(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const ValueFlowUniverse& universe = inputs.universe();
    FunctionID id = universe.functionOf(object);
    if (!id || id.index() >= inputs.program.functions.size())
        return noWitness;
    const Function& function = inputs.program.at(id);
    const ValueFlowSite* site = universe.siteOf(object);
    if (!site || !site->value)
        return { id.index(), Wire::none, function.declarationLocation() };
    return { id.index(), site->value.index(), function.locationOf(site->value) };
}

static bool hasReceiverStamp(const ValueFlowStatement& statement)
{
    return statement.kind == ValueFlowStatementKind::Operation && PayloadWord::receivers(statement.opcode);
}

// The object in which the lookup starts. An access through `super` starts in the object above the home object, whatever it hands to an accessor as `this`.
static unsigned receiverOperandOf(Opcode opcode)
{
    if (std::optional<unsigned> object = OperandPosition::object(opcode))
        return *object;
    std::optional<unsigned> base = OperandPosition::base(opcode);
    RELEASE_ASSERT(base);
    return *base;
}

// The positions say what keeps the type of the receiver from being proven. Where nothing does, an object arrives that has no layout: a function, a built-in, an instance of a built-in class.
static FlowSiteLoss lossOfAccess(AccessesOfFunction& accesses, const ValueFlowStatement& access, const Type& family, ValueFlowNodeID receiver, ValueSetID held)
{
    const ValueFlowUniverse& universe = accesses.inputs.universe();
    FlowSiteLoss loss = accesses.positions.lossOf(accesses.positions.resultAt(family, held), receiver);
    if (loss.cause == UnprovenCause::Unknown) {
        loss.cause = UnprovenCause::NonLayoutObject;
        accesses.inputs.sets().forEachMember(accesses.maker.intersect(held, universe.knownObjects()), [&](ValueFlowObjectID object) {
            if (!hasFunction(loss.witness) && !universe.layoutOf(object))
                loss.witness = witnessOfAllocation(accesses.inputs, object);
        });
    }
    // What no earlier operation explains begins here.
    if (!hasFunction(loss.witness))
        loss.witness = { accesses.id.index(), access.site.index(), accesses.function.locationOf(access.site) };
    return loss;
}

static void listReceiver(AccessesOfFunction& accesses, ValueFlowObjectID member, uint32_t listing, ListedReceivers& listed)
{
    if (member == ValueFlowUniverse::unknownObject() || member == ValueFlowUniverse::untrackedObject())
        return;
    if (uint32_t layout = accesses.layoutsOfMembers[member.index()]) {
        if (std::exchange(accesses.layoutsMet.lastListing[layout - 1], listing) != listing)
            listed.layouts.append(LayoutID(layout - 1));
        return;
    }
    BuiltinClassID builtinClass = accesses.builtinClassesOfMembers.ensure(member, [&] {
        Type type = accesses.positions.typeOfObject(member);
        if (type.isUndefinedOrNull())
            return BuiltinClassID();
        BuiltinReceiver receiver = builtinReceiverOf(accesses.inputs.program, type.withoutNullish());
        bool isInstance = receiver.kind == BuiltinReceiverKind::Instance || receiver.kind == BuiltinReceiverKind::Primitive;
        return isInstance ? receiver.builtinClass : BuiltinClassID();
    });
    if (builtinClass && !listed.builtinClasses.contains(builtinClass))
        listed.builtinClasses.append(builtinClass);
}

static uint32_t beginListing(AccessesOfFunction& accesses)
{
    LayoutsMetByListing& met = accesses.layoutsMet;
    if (met.lastListing.isEmpty())
        met.lastListing.fill(0, accesses.inputs.program.layouts.size());
    return ++met.numberOfListings;
}

static const ListedReceivers& listedReceiversOfBlock(AccessesOfFunction& accesses, uint64_t nameOfBlock)
{
    auto result = accesses.listedBlocks.add(nameOfBlock, nullptr);
    if (!result.isNewEntry)
        return *result.iterator->value;
    if (const ListedReceivers* known = accesses.listedBlocksOfAll.find(nameOfBlock)) {
        result.iterator->value = known;
        return *known;
    }
    auto listed = makeUnique<ListedReceivers>();
    uint32_t listing = beginListing(accesses);
    accesses.inputs.sets().forEachMemberOfBlock(nameOfBlock, [&](ValueFlowObjectID member) {
        listReceiver(accesses, member, listing, *listed);
    });
    listed->layouts.shrinkToFit();
    const ListedReceivers& kept = accesses.listedBlocksOfAll.add(nameOfBlock, WTF::move(listed));
    accesses.listedBlocks.set(nameOfBlock, &kept);
    return kept;
}

static const ListedReceivers& listedReceiversOf(AccessesOfFunction& accesses, ValueSetID held)
{
    auto result = accesses.listedReceivers.add(held.index(), nullptr);
    if (!result.isNewEntry)
        return *result.iterator->value;
    if (const ListedReceivers* known = accesses.listedReceiversOfSets.find(held.index())) {
        result.iterator->value = known;
        return *known;
    }

    constexpr uint32_t fewestMembersToListByBlock = 128;
    const ValueSets& sets = accesses.inputs.sets();
    auto listed = makeUnique<ListedReceivers>();
    if (ValueSets::isComplement(held) || sets.size(held) < fewestMembersToListByBlock) {
        uint32_t listing = beginListing(accesses);
        sets.forEachMember(held, [&](ValueFlowObjectID member) {
            listReceiver(accesses, member, listing, *listed);
        });
    } else {
        Vector<const ListedReceivers*, 64> blocks;
        sets.forEachBlock(held, [&](uint64_t nameOfBlock) {
            blocks.append(&listedReceiversOfBlock(accesses, nameOfBlock));
        });
        uint32_t listing = beginListing(accesses);
        for (const ListedReceivers* block : blocks) {
            for (LayoutID layout : block->layouts) {
                if (std::exchange(accesses.layoutsMet.lastListing[layout.index()], listing) != listing)
                    listed->layouts.append(layout);
            }
            for (BuiltinClassID builtinClass : block->builtinClasses) {
                if (!listed->builtinClasses.contains(builtinClass))
                    listed->builtinClasses.append(builtinClass);
            }
        }
    }
    AOT::sort(listed->layouts);
    listed->layouts.shrinkToFit();
    const ListedReceivers& kept = accesses.listedReceiversOfSets.add(held.index(), WTF::move(listed));
    accesses.listedReceivers.set(held.index(), &kept);
    return kept;
}

// Where something unknown may arrive at a receiver without a type, the layouts that are seen to arrive and have a field of the name are what the access expects.
static std::optional<LayoutSet> receiversSeenToArrive(AccessesOfFunction& accesses, const ValueFlowStatement& access, const Type& family, ValueSetID held)
{
    if (family.layouts || !PayloadWord::key(access.opcode) || ValueSets::isComplement(held))
        return std::nullopt;

    const Program& program = accesses.inputs.program;
    PropertyKeyID key = accesses.function.at(access.site).key();
    if (!key)
        return std::nullopt;
    const ListedReceivers& listed = listedReceiversOf(accesses, held);
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
        if (receivers.members.size() == accesses.inputs.build.limit(LimitID::MaxLayoutsInType))
            areTooMany = true;
        else
            receivers.members.append(layout);
    };
    std::span<const uint64_t> holders = accesses.layoutsWithFieldOfKey.holdersOf(key);
    if (holders.size() <= listed.layouts.size()) {
        for (uint64_t holder : holders) {
            LayoutID layout(static_cast<uint32_t>(holder));
            if (!areTooMany && std::ranges::binary_search(listed.layouts, layout))
                add(layout);
        }
    } else {
        for (LayoutID layout : listed.layouts) {
            if (!areTooMany && std::ranges::binary_search(holders, static_cast<uint64_t>(key.index()) << 32 | layout.index()))
                add(layout);
        }
    }
    if (areTooMany)
        return std::nullopt;
    if (receivers.members.isEmpty())
        return std::nullopt;
    receivers.isComplete = false;
    canonicalize(receivers);
    return receivers;
}

// A set that is complete says what arrives. One that is not says what is expected, and the site keeps its loss.
static void stampAccess(AccessesOfFunction& accesses, const ValueFlowStatement& access)
{
    unsigned operandIndex = receiverOperandOf(access.opcode);
    ValueFlowNodeID receiver = accesses.inputs.bodies().operandsOf(access)[operandIndex].node;
    const Type& family = accesses.inputs.program.typeOf(accesses.function, accesses.function.at(access.site).operand(operandIndex));
    ValueSetID held = accesses.inputs.valueOf(accesses.maker, accesses.inputs.bodies().operandsOf(access)[operandIndex]);
    accesses.numberOfUnknownReceivers += accesses.inputs.sets().contains(held, ValueFlowUniverse::unknownObject());

    if (std::optional<LayoutSet> receivers = accesses.positions.receiversAt(family, held); receivers && receivers->isComplete) {
        accesses.update.accesses.append(FlowAccessStamp { access.site, accesses.types.intern(WTF::move(*receivers)) });
        return;
    }
    if (std::optional<LayoutSet> receivers = receiversSeenToArrive(accesses, access, family, held))
        accesses.update.accesses.append(FlowAccessStamp { access.site, accesses.types.intern(WTF::move(*receivers)) });
    FlowSiteLoss loss = lossOfAccess(accesses, access, family, receiver, held);
    loss.seesConstructedObject = !ValueSets::isComplement(held) && accesses.inputs.sets().meet(held, accesses.inputs.universe().constructedObjects());
    accesses.sites.accessSiteLosses[accesses.accessSites.siteOfValue(accesses.id, access.site).index()] = loss;
}

void computeAccessResults(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = program.functions.size();
    RELEASE_ASSERT(tables.sites.accessSiteLosses.size() == application.accessSites.numberOfSites());

    // The types count what lies outside them. Here the same sets are only looked at again.
    ValueFlowPositionCounts counts(nullptr);
    ValueSetMakers makers(inputs);
    Vector<ListedReceiversKnownToWorker> listedReceivers(inputs.loops().width());
    ListedReceiversByName listedReceiversOfSets;
    Vector<ListedReceiversKnownToWorker> listedBlocks(inputs.loops().width());
    ListedReceiversByName listedBlocksOfAll;
    LayoutsWithFieldOfKey layoutsWithFieldOfKey(inputs, application);
    Vector<uint32_t> layoutsOfMembers = findLayoutsOfMembers(inputs);
    Vector<LayoutsMetByListing> layoutsMet(inputs.loops().width());
    BuiltinClassesOfMembers builtinClassesOfMembers(inputs);
    auto phase = makeUnique<TypeInterningPhase>(types, numberOfFunctions);
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index, unsigned worker) {
        FunctionID id(index);
        std::span<const ValueFlowStatement> statements = inputs.bodies().statementsOf(index);
        if (!inputs.isReached(id) || std::ranges::none_of(statements, hasReceiverStamp))
            return;

        const Function& function = program.functions[index];
        if (!inputs.build.isEnabled(FlowResultKind::ReceiverSets, index)) {
            for (const ValueFlowStatement& statement : statements) {
                if (hasReceiverStamp(statement))
                    tables.sites.accessSiteLosses[application.accessSites.siteOfValue(id, statement.site).index()] = FlowSiteLoss { .cause = UnprovenCause::ResultDisabled };
            }
            return;
        }

        TypeArena arena(*phase, index);
        ValueFlowPositions positions(inputs, arena, counts, index, &makers.of(worker));
        AccessesOfFunction accesses { inputs, id, function, arena, positions, makers.of(worker), application.bodies[index], tables.sites, application.accessSites, layoutsWithFieldOfKey, listedReceiversOfSets, listedReceivers[worker], listedBlocksOfAll, listedBlocks[worker], layoutsOfMembers, layoutsMet[worker], builtinClassesOfMembers };
        for (const ValueFlowStatement& statement : statements) {
            if (hasReceiverStamp(statement))
                stampAccess(accesses, statement);
        }
        AOT::sort(accesses.update.accesses, { }, &FlowAccessStamp::value);
        inputs.build.statistics().count(StatisticID::FlowAccessesOfUnknownReceivers, accesses.numberOfUnknownReceivers);
    });
    TypeRemap remap = phase->finish(StringRemap { }, &inputs.loops());
    inputs.build.destroyLater(WTF::move(phase));
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        if (!remap.isIdentityFor(index))
            remapTypes(application.bodies[index], remap);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
