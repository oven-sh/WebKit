/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLedgerOfBuiltinObjects.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTSort.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include <wtf/HashMap.h>
#include <wtf/Locker.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isRaisedAtAll(const ValueFlowResultInputs& inputs, ValueFlowHostNode flag)
{
    return !inputs.sets().isEmpty(inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(flag)));
}

struct LedgerOfBuiltinObjects::Notes {
    struct Noted {
        Receiver receiver;
        uint32_t key;
        Write write;
    };
    HashMap<uint32_t, Vector<Receiver>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> receiversIn;
    Vector<Noted> underNames;
    Vector<Noted> underIndexes;
    Vector<Noted> underKeysNobodyKnows;
    Vector<Noted> ofPrototypeLinks;
    BitVector writtenByCodeNobodySees;
    Vector<uint64_t> propertiesOfOperands;
};

static uint64_t numberOfNode(ValueFlowName name, ValueSetID set)
{
    return (static_cast<uint64_t>(name.kind) << 32 | name.payload) * 0x9e3779b97f4a7c15ull ^ (static_cast<uint64_t>(set.index()) + 1);
}

LedgerOfBuiltinObjects::LedgerOfBuiltinObjects(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const FlowBuiltinTables& tables)
    : m_inputs(inputs)
    , m_builtins(inputs.program.builtins())
    , m_tables(tables)
    , m_unseenCodeChangesRealm(isRaisedAtAll(inputs, ValueFlowHostNode::UnseenCodeChangesRealm))
    , m_unseenCodeRuns(unseenCodeCanRun(inputs.solution))
    , m_nativeCodeWrites(isRaisedAtAll(inputs, ValueFlowHostNode::UnknownCodeWrites))
    , m_unknownObject(m_builtins.builtinCount())
    , m_untrackedObject(m_builtins.builtinCount() + 1)
    , m_keysWritten(m_builtins.builtinCount() + 2)
    , m_keysOfWritesUnderNames(m_builtins.builtinCount() + 2)
    , m_writesOfProperties(m_builtins.builtinCount() + 2)
    , m_writesOfPrototypeLinks(m_builtins.builtinCount() + 2)
{
    auto noteHoldersIn = [&](ValueSetID set, BitVector& holders) {
        for (BuiltinID holder : holdersIn(heap.maker(), set))
            holders.set(holder.index());
    };
    noteHoldersIn(inputs.solver().possibleValuesOf(ValueFlowProgramNodes::escaped()), m_escaped);
    noteHoldersIn(inputs.solver().possibleValuesOf(ValueFlowProgramNodes::lost()), m_lost);
    noteHoldersIn(inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed)), m_mayBeHeldWithoutBeingListed);
    noteWrites(heap.index());
}

Vector<BuiltinID> LedgerOfBuiltinObjects::holdersIn(ValueSetMaker& maker, ValueSetID set) const
{
    Vector<BuiltinID> found;
    m_inputs.sets().forEachMemberInLine(maker.intersect(set, m_inputs.universe().builtinObjects()), [&](ValueFlowObjectID object) {
        found.append(m_builtins.canonicalID(m_inputs.universe().builtinOf(object)));
    });
    AOT::sort(found);
    removeRepeatedElements(found);
    return found;
}

// The rules that stand for code nobody sees write on whatever has come to it. A node that a statement of the program asks for too, or that nobody asks for any more, counts as the program's.
HashSet<uint64_t> LedgerOfBuiltinObjects::findNodesWrittenByCodeNobodySeesAlone() const
{
    const ValueFlowSolver& solver = m_inputs.solver();
    HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> usesByCodeNobodySees;
    unsigned numberOfRulesFound = 0;
    bool hasFoundHostRoots = false;
    auto noteUsesOf = [&](uint32_t statement) {
        ValueFlowStatementKind kind = solver.statementAt(ValueFlowStatementID(statement)).kind;
        if (kind != ValueFlowStatementKind::Escape && kind != ValueFlowStatementKind::HostRoots && kind != ValueFlowStatementKind::UnseenCode)
            return;
        // The roots of the host are one rule in several statements where the program has many entry points.
        if (kind != ValueFlowStatementKind::HostRoots || !std::exchange(hasFoundHostRoots, true))
            ++numberOfRulesFound;
        m_inputs.forEachIndexOfMadeNodesAskedForBy(ValueFlowStatementID(statement), kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false, [&](uint32_t madeNodes) {
            ++usesByCodeNobodySees.add(madeNodes, 0).iterator->value;
        });
    };
    // The statements of the host stand in the last body, and the rule of what has escaped is the first statement that the solver makes.
    uint32_t numberOfStatementsOfBodies = m_inputs.bodies().statements().size();
    for (uint32_t statement = numberOfStatementsOfBodies; statement-- && solver.statementAt(ValueFlowStatementID(statement)).kind != ValueFlowStatementKind::Operation;)
        noteUsesOf(statement);
    if (numberOfStatementsOfBodies < solver.numberOfStatements())
        noteUsesOf(numberOfStatementsOfBodies);
    RELEASE_ASSERT(numberOfRulesFound == 3);

    HashSet<uint64_t> alone;
    for (auto [madeNodes, uses] : usesByCodeNobodySees) {
        if (solver.numberOfUsesOfMadeNodesAt(madeNodes) == uses)
            alone.add(numberOfNode(solver.keyOfMadeNodesAt(madeNodes).name(), solver.keyOfMadeNodesAt(madeNodes).set));
    }
    return alone;
}

void LedgerOfBuiltinObjects::note(Notes& notes, ValueSetMaker& maker, const HashSet<uint64_t>& writtenByCodeNobodySeesAlone, ValueFlowName name, Write throughListed, const ValueFlowHeapIndex::WrittenNode& node) const
{
    const Vector<Receiver>& receivers = notes.receiversIn.ensure(node.objects.index(), [&] {
        Vector<Receiver> found = WTF::map(holdersIn(maker, node.objects), [](BuiltinID holder) { return holder.index(); });
        if (m_inputs.sets().contains(node.objects, ValueFlowUniverse::unknownObject()))
            found.append(m_unknownObject);
        if (m_inputs.sets().contains(node.objects, ValueFlowUniverse::untrackedObject()))
            found.append(m_untrackedObject);
        return found;
    }).iterator->value;
    if (receivers.isEmpty())
        return;
    if (name.kind == ValueFlowNameKind::Slot) {
        for (Receiver receiver : receivers) {
            if (receiver < m_unknownObject)
                notes.propertiesOfOperands.append(static_cast<uint64_t>(receiver) << 32 | (name.payload - m_builtins.slotCount()));
        }
        return;
    }
    bool isOfCodeNobodySees = !writtenByCodeNobodySeesAlone.isEmpty() && writtenByCodeNobodySeesAlone.contains(numberOfNode(name, node.objects));
    throughListed.arrives = node.value;
    Write throughOthers = throughListed;
    throughOthers.actor = Actor::ReceiversNobodyKnows;
    ValueFlowNameShape shape = shapeOf(name.kind);
    Vector<Notes::Noted>& noted = shape.reach == ValueFlowNameReach::OneKey ? notes.underNames : shape.reach == ValueFlowNameReach::Elements ? notes.underIndexes : shape.sort == ValueFlowNameSort::PrototypeLink ? notes.ofPrototypeLinks : notes.underKeysNobodyKnows;
    for (Receiver receiver : receivers) {
        // Whoever writes through an object that stands for others has been seen to. A holder itself tells the code of the program from the code that nobody sees.
        bool standsForOthers = receiver >= m_unknownObject;
        if (isOfCodeNobodySees && !standsForOthers)
            notes.writtenByCodeNobodySees.set(receiver);
        else
            noted.append({ receiver, name.payload, standsForOthers ? throughOthers : throughListed });
    }
}

void LedgerOfBuiltinObjects::noteWrites(const ValueFlowHeapIndex& index)
{
    HashSet<uint64_t> writtenByCodeNobodySeesAlone = findNodesWrittenByCodeNobodySeesAlone();
    Vector<ValueFlowHeapIndex::Stretch> stretches = index.stretchesOfNodesWrittenUnder([&](ValueFlowName name) {
        ValueFlowNameShape shape = shapeOf(name.kind);
        return shape.reach == ValueFlowNameReach::OneKey || shape.reach == ValueFlowNameReach::Elements || shape.reach == ValueFlowNameReach::UnknownKey || shape.sort == ValueFlowNameSort::Copy
            || shape.sort == ValueFlowNameSort::PrototypeLink || (name.kind == ValueFlowNameKind::Slot && name.payload >= m_builtins.slotCount());
    });
    ValueSetMakers makers(m_inputs);
    Vector<Notes> notesOfWorker(m_inputs.loops().width());
    forEachIndexInParallel(m_inputs.loops(), stretches.size(), [&](unsigned position, unsigned worker) {
        // A holder that the write lists is written as itself, whatever stands beside it.
        ValueFlowName name = stretches[position].name;
        ValueFlowStatement someStatement;
        Write throughListed { actorOf(m_inputs, someStatement, ValueSets::nothing, name), formOf(someStatement, name), { }, { }, { }, { } };
        for (const ValueFlowHeapIndex::WrittenNode& node : stretches[position].nodes)
            note(notesOfWorker[worker], makers.of(worker), writtenByCodeNobodySeesAlone, name, throughListed, node);
    });

    for (Notes& notes : notesOfWorker) {
        m_writtenByCodeNobodySees.merge(notes.writtenByCodeNobodySees);
        m_propertiesOfOperands.addAll(notes.propertiesOfOperands);
        for (const Notes::Noted& noted : notes.underNames) {
            m_keysWritten[noted.receiver].set(noted.key);
            m_keysOfWritesUnderNames[noted.receiver].append(noted.key);
            m_writesOfProperties[noted.receiver].append(noted.write);
        }
        for (const Notes::Noted& noted : notes.ofPrototypeLinks) {
            m_ofPrototypeLinks.set(noted.receiver);
            m_writesOfPrototypeLinks[noted.receiver].append(noted.write);
        }
    }
    for (Notes& notes : notesOfWorker) {
        for (const Notes::Noted& noted : notes.underIndexes) {
            m_underIndexes.set(noted.receiver);
            m_writesOfProperties[noted.receiver].append(noted.write);
        }
    }
    for (size_t receiver : m_underIndexes)
        m_numberOfWritesUnderIndexes.add(static_cast<Receiver>(receiver), static_cast<uint32_t>(m_writesOfProperties[receiver].size() - m_keysOfWritesUnderNames[receiver].size()));
    for (Notes& notes : notesOfWorker) {
        for (const Notes::Noted& noted : notes.underKeysNobodyKnows) {
            m_underKeysNobodyKnows.set(noted.receiver);
            m_writesOfProperties[noted.receiver].append(noted.write);
        }
    }
}

std::span<const Write> LedgerOfBuiltinObjects::writesUnderIndexesAndKeysNobodyKnows(Receiver receiver) const
{
    return m_writesOfProperties[receiver].subspan(m_keysOfWritesUnderNames[receiver].size());
}

std::span<const Write> LedgerOfBuiltinObjects::writesUnderKeysNobodyKnows(Receiver receiver) const
{
    return writesUnderIndexesAndKeysNobodyKnows(receiver).subspan(m_underIndexes.get(receiver) ? m_numberOfWritesUnderIndexes.get(receiver) : 0);
}

void LedgerOfBuiltinObjects::appendWritesUnder(Lists& seen, Receiver receiver, PropertyKeyID key) const
{
    if (!key || !m_keysWritten[receiver].get(key.index()))
        return;
    const Vector<uint32_t>& keys = m_keysOfWritesUnderNames[receiver];
    for (size_t position = 0; position < keys.size(); ++position) {
        if (keys[position] == key.index())
            seen.append(m_writesOfProperties[receiver].subspan(position, 1));
    }
}

void LedgerOfBuiltinObjects::appendWritesThrough(Lists& seen, Receiver placeholder, PropertyKeyID key) const
{
    appendWritesUnder(seen, placeholder, key);
    seen.append(writesUnderIndexesAndKeysNobodyKnows(placeholder));
}

// Unseen code that stays out of the objects of the realm comes to one as native code does: through what has escaped.
OptionSet<Actor> LedgerOfBuiltinObjects::codeNobodySeesThatWrites(BuiltinID holder) const
{
    OptionSet<Actor> open;
    if (m_unseenCodeChangesRealm)
        open.add(Actor::UnseenCode);
    if (m_writtenByCodeNobodySees.get(holder.index()) || (m_escaped.get(holder.index()) && (m_nativeCodeWrites || m_unseenCodeRuns)))
        open.add(Actor::NativeCode);
    return open;
}

auto LedgerOfBuiltinObjects::accountOfProperty(BuiltinID holder, PropertyKeyID key) const -> Account
{
    Receiver receiver = holder.index();
    Account account { { }, codeNobodySeesThatWrites(holder) };
    appendWritesUnder(account.seen, receiver, key);
    account.seen.append(key ? writesUnderKeysNobodyKnows(receiver) : writesUnderIndexesAndKeysNobodyKnows(receiver));
    if (m_underKeysNobodyKnows.get(receiver))
        account.open.add(Actor::KeysNobodyKnows);
    if (m_lost.get(receiver) && mayWriteThrough(m_untrackedObject, key)) {
        appendWritesThrough(account.seen, m_untrackedObject, key);
        account.open.add(Actor::ReceiversNobodyKnows);
    }
    if (mayWriteThrough(m_unknownObject, key)) {
        // A holder that has only escaped may be what the unknown object stands for. Nobody has followed it to the write.
        if (m_mayBeHeldWithoutBeingListed.get(receiver))
            appendWritesThrough(account.seen, m_unknownObject, key);
        if (m_mayBeHeldWithoutBeingListed.get(receiver) || m_escaped.get(receiver))
            account.open.add(Actor::ReceiversNobodyKnows);
    }
    addWritersOfLinkToMembers(account, holder);
    return account;
}

// Whoever gives such a holder another prototype takes away what the table lists on it.
void LedgerOfBuiltinObjects::addWritersOfLinkToMembers(Account& account, BuiltinID holder) const
{
    if (!m_builtins.has(BuiltinMark::AnswersNullAsItsPrototype, holder) || m_builtins.hasLockedPrototypeLink(holder))
        return;
    Account ofLink = accountOfPrototypeLink(holder);
    account.seen.appendVector(ofLink.seen);
    account.open.add(ofLink.open);
}

auto LedgerOfBuiltinObjects::accountOfEveryOwnProperty(BuiltinID holder) const -> Account
{
    Receiver receiver = holder.index();
    Account account { { }, codeNobodySeesThatWrites(holder) };
    account.seen.append(m_writesOfProperties[receiver].span());
    if (writesUnderKeysNobodyKnowsThrough(receiver))
        account.open.add(Actor::KeysNobodyKnows);
    if ((m_lost.get(receiver) && writesPropertiesThrough(m_untrackedObject)) || (m_escaped.get(receiver) && writesPropertiesThrough(m_unknownObject)))
        account.open.add(Actor::ReceiversNobodyKnows);
    addWritersOfLinkToMembers(account, holder);
    return account;
}

auto LedgerOfBuiltinObjects::accountOfPrototypeLink(BuiltinID holder) const -> Account
{
    Receiver receiver = holder.index();
    Account account { { }, codeNobodySeesThatWrites(holder) };
    account.seen.append(m_writesOfPrototypeLinks[receiver].span());
    if ((m_lost.get(receiver) && m_ofPrototypeLinks.get(m_untrackedObject)) || (m_escaped.get(receiver) && m_ofPrototypeLinks.get(m_unknownObject)))
        account.open.add(Actor::ReceiversNobodyKnows);
    return account;
}

// Most places are written in one way at the most: their account names the list as it lies in the ledger.
Writers LedgerOfBuiltinObjects::keep(PlaceKind kind, const Account& account) const
{
    OptionSet<Actor> open = account.open & actorsThatCanWrite(kind);
    std::span<const Write> only;
    unsigned numberOfLists = 0;
    for (std::span<const Write> list : account.seen) {
        if (list.empty())
            continue;
        only = list;
        ++numberOfLists;
    }
    if (numberOfLists <= 1)
        return { only, open };
    auto joined = makeUnique<List>();
    for (std::span<const Write> list : account.seen)
        joined->append(list);
    Locker locker { m_listsLock };
    m_lists.append(WTF::move(joined));
    return { m_lists.last()->span(), open };
}

Writers LedgerOfBuiltinObjects::writersOfMemberWithoutLocks(BuiltinID member) const
{
    const Builtin& entry = m_builtins.at(member);
    PropertyKeyID key = m_inputs.program.keyOf(entry.name);
    Account account = accountOfProperty(m_builtins.canonicalID(entry.holder), m_inputs.program.isIndexLike(key) ? PropertyKeyID() : key);
    // The row says that the engine changes the member whenever it likes.
    if (entry.mutability == BuiltinMutability::ChangesOnItsOwn)
        account.open.add(Actor::NativeCode);
    return keep(PlaceKind::Property, account);
}

Writers LedgerOfBuiltinObjects::behindLocks(BuiltinID member, const Writers& withoutLocks) const
{
    if (std::optional<LockOnPlace> lock = Locks::on(m_builtins, m_tables, member))
        return lock->writers();
    return withoutLocks;
}

Writers LedgerOfBuiltinObjects::writersOfMember(BuiltinID member) const
{
    return behindLocks(member, writersOfMemberWithoutLocks(member));
}

Writers LedgerOfBuiltinObjects::writersOfEveryOwnProperty(BuiltinID holder) const
{
    if (std::optional<LockOnPlace> lock = Locks::ofWholeObject(m_builtins, holder))
        return lock->writers();
    return keep(PlaceKind::EveryOwnProperty, accountOfEveryOwnProperty(holder));
}

Writers LedgerOfBuiltinObjects::writersOfPrototypeLink(BuiltinID holder) const
{
    if (std::optional<LockOnPlace> lock = Locks::ofWholeObject(m_builtins, holder))
        return lock->writers();
    return keep(PlaceKind::PrototypeLink, accountOfPrototypeLink(holder));
}

Reachers LedgerOfBuiltinObjects::reachersOf(BuiltinID holder) const
{
    OptionSet<Actor> open;
    if (m_unseenCodeChangesRealm)
        open.add(Actor::UnseenCode);
    if (m_escaped.get(holder.index()))
        open.add({ Actor::NativeCode, Actor::ReceiversNobodyKnows });
    if (m_lost.get(holder.index()))
        open.add(Actor::ReceiversNobodyKnows);
    return { { }, open };
}

std::optional<Reachers> LedgerOfBuiltinObjects::reachersOf(ValueFlowObjectID object) const
{
    if (!m_inputs.sets().contains(m_inputs.universe().builtinObjects(), object))
        return std::nullopt;
    return reachersOf(m_builtins.canonicalID(m_inputs.universe().builtinOf(object)));
}

std::optional<Writers> LedgerOfBuiltinObjects::writersOf(const Place& place) const
{
    if (!place.liesOnObjects() || place.kind() == PlaceKind::Cell || place.search() != Place::Search::Own)
        return std::nullopt;
    if (ValueSets::isComplement(place.holders()) || !m_inputs.sets().isSubset(place.holders(), m_inputs.universe().builtinObjects()))
        return std::nullopt;
    Account account;
    m_inputs.sets().forEachMember(place.holders(), [&](ValueFlowObjectID object) {
        BuiltinID holder = m_builtins.canonicalID(m_inputs.universe().builtinOf(object));
        bool namesKey = shapeOf(place.name().kind).reach == ValueFlowNameReach::OneKey;
        PropertyKeyID key = namesKey ? PropertyKeyID(place.name().payload) : PropertyKeyID();
        BuiltinID member = namesKey ? findOwnMember(m_inputs.program, holder, key) : BuiltinID();
        if (Locks::ofWholeObject(m_builtins, holder) || (member && Locks::on(m_builtins, m_tables, member)))
            return;
        Account ofHolder = [&] {
            switch (place.kind()) {
            case PlaceKind::Property:
            case PlaceKind::Elements:
            case PlaceKind::Accessor:
                return accountOfProperty(holder, key);
            case PlaceKind::EveryOwnProperty:
                return accountOfEveryOwnProperty(holder);
            case PlaceKind::PrototypeLink:
                return accountOfPrototypeLink(holder);
            case PlaceKind::Variable:
            case PlaceKind::EnvironmentField:
            case PlaceKind::Field:
            case PlaceKind::Cell:
                break;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }();
        if (member && m_builtins.at(member).mutability == BuiltinMutability::ChangesOnItsOwn)
            ofHolder.open.add(Actor::NativeCode);
        account.seen.appendVector(ofHolder.seen);
        account.open.add(ofHolder.open);
    });
    return keep(place.kind(), account);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
