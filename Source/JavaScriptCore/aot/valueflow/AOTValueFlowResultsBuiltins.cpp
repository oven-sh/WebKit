/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowUnseenCode.h"
#include "AOTPhaseTimes.h"
#include "AOTPrimitiveKeyTexts.h"
#include "AOTSchemaOperandPositions.h"
#include "AOTSort.h"
#include "AOTValueFlowResultsPlaces.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// One that has escaped is held. The rules record a call of it once a reached call may have it as its callee: mayCallWhateverHasEscaped().
static constexpr OptionSet<BuiltinUse> callingBuiltinUses { BuiltinUse::Called, BuiltinUse::Constructed };
static constexpr OptionSet<BuiltinUse> accessingBuiltinUses { BuiltinUse::Read, BuiltinUse::Written };

static bool isRaised(const ValueFlowResultInputs& inputs, ValueFlowHostNode node)
{
    return !inputs.sets().isEmpty(inputs.solver().valueOf(inputs.bodies().nodes().ofHost(node)));
}

namespace {

enum class BuiltinWriteForm : uint8_t {
    UnderName,
    // Also under an index, and whatever is copied from another object.
    UnderKeyNobodyKnows,
    OfPrototypeLink,
    OfSlot,
    Other,
};

// What is done through a placeholder is done to any of the objects that the placeholder stands for.
struct WritesThroughPlaceholder {
    bool mayWriteUnder(PropertyKeyID key) const { return writesUnderKeysNobodyKnows || keys.get(key.index()); }
    bool writesAnything() const { return writesUnderKeysNobodyKnows || changesPrototypeLinks || !keys.isEmpty(); }

    BitVector keys;
    bool writesUnderKeysNobodyKnows { false };
    bool changesPrototypeLinks { false };
};

struct NameOnHolder {
    BuiltinID holder;
    PropertyKeyID key;
};

// What the solved program does to the built-in objects, read off the nodes that the rules have made: whatever is stored, defined or deleted lies in a node that is keyed by the receivers and the name.
class BuiltinHolders {
    WTF_MAKE_NONCOPYABLE(BuiltinHolders);
public:
    BuiltinHolders(const ValueFlowResultInputs&, const ValueFlowHeapIndex&);

    bool unseenCodeRuns() const { return m_unseenCodeRuns; }
    bool hasEscaped(BuiltinID holder) const { return m_escaped.get(holder.index()); }
    bool isLost(BuiltinID holder) const { return m_lost.get(holder.index()); }

    // An operation with the holder among its receivers may write the location.
    bool isWrittenByProgram(BuiltinID holder, PropertyKeyID) const;
    // Something may write the location that has the holder in its hands as a value nobody knows. The analysis leaves the objects of the realm out of such a write, so the run time has to refuse it.
    bool mayBeWrittenBehindAnalysis(BuiltinID holder, PropertyKeyID) const;
    // A lookup that misses on the holder goes on to the prototype that the table names, and nothing comes under a name without a record in the locations.
    bool isIntact(BuiltinID holder) const;
    BuiltinKind kindOf(BuiltinID builtin) const { return m_builtins.at(builtin).kind; }
    bool runsUnseenCodeFor(BuiltinID builtin, BuiltinName property) const
    {
        const BuiltinUnseenCode* row = m_builtins.unseenCodeOf(builtin);
        return property && row && row->condition && row->condition->hasProperty == property;
    }
    // Or be a text, where the name is the first text of a condition.
    bool operandMayHaveProperty(BuiltinID builtin, BuiltinName property) const { return m_propertiesOfOperands.contains(static_cast<uint64_t>(builtin.index()) << 32 | property.index()); }
    // A holder with a locked location is among the receivers of a write under a key of which the sets know the kind alone.
    bool mayWriteLockedLocationUnderKeyNobodyKnows() const;

    // Ascending by holder and key, each once.
    std::span<const NameOnHolder> namesWritten() const LIFETIME_BOUND { return m_namesWritten.span(); }

    void forEachHolderIn(ValueSetID, NOESCAPE const Invocable<void(BuiltinID)> auto&);
    Vector<BuiltinID> holdersIn(ValueSetMaker&, ValueSetID) const;

private:
    static auto orderOf(const NameOnHolder& name) { return std::pair { name.holder, name.key }; }
    using HoldersInSets = HashMap<uint32_t, Vector<BuiltinID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    struct Notes {
        explicit Notes(const BuiltinTable& builtins)
            : isWrittenUnderKeysNobodyKnows(builtins.builtinCount())
            , isWrittenUnderIndexes(builtins.builtinCount())
            , hasPrototypeLinkChangedByProgram(builtins.builtinCount())
        {
        }

        struct ThroughSet {
            Vector<BuiltinID> holders;
            Vector<uint32_t> keysWritten;
        };

        HashMap<uint32_t, ThroughSet, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> throughSet;
        BitVector isWrittenUnderKeysNobodyKnows;
        BitVector isWrittenUnderIndexes;
        BitVector hasPrototypeLinkChangedByProgram;
        WritesThroughPlaceholder throughUnknownObject;
        WritesThroughPlaceholder throughUntrackedObject;
        Vector<uint64_t> propertiesOfOperands;
    };

    bool isWrittenBehindAnalysis() const { return m_unknownCodeWrites || m_throughUnknownObject.writesAnything(); }
    void noteHoldersIn(ValueFlowNodeID, BitVector&);
    void noteWrites(const ValueFlowHeapIndex&);
    void noteWrite(Notes&, ValueSetMaker&, ValueSetID receivers, ValueFlowName) const;
    void findObjectsHeldWithoutBeingListed();

    const ValueFlowResultInputs& m_inputs;
    const BuiltinTable& m_builtins;
    BorrowedValueSetMaker m_borrowedMaker;
    ValueSetMaker& m_maker;
    const bool m_unseenCodeRuns;
    const bool m_unknownCodeWrites;
    HoldersInSets m_holdersInSet;

    BitVector m_escaped;
    BitVector m_lost;
    BitVector m_isWrittenUnderKeysNobodyKnows;
    // An index spells the name of no member but one that a number converts to: `holder[i] = value` leaves holder.parse what it was.
    BitVector m_isWrittenUnderIndexes;
    BitVector m_hasPrototypeLinkChangedByProgram;
    Vector<NameOnHolder> m_namesWritten;
    WritesThroughPlaceholder m_throughUnknownObject;
    WritesThroughPlaceholder m_throughUntrackedObject;
    // (built-in << 32 | name): some call of the built-in that was applied passes an operand that may have a property of the name.
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_propertiesOfOperands;

    BitVector m_mayBeHeldWithoutBeingListed;
};

} // anonymous namespace

static BuiltinWriteForm formOfWrite(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::RedefinedMember:
        return BuiltinWriteForm::UnderName;
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::CopiedFrom:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
        return BuiltinWriteForm::UnderKeyNobodyKnows;
    case ValueFlowNameKind::PrototypeLink:
        return BuiltinWriteForm::OfPrototypeLink;
    case ValueFlowNameKind::Slot:
        return BuiltinWriteForm::OfSlot;
    default:
        return BuiltinWriteForm::Other;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinHolders::BuiltinHolders(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& index)
    : m_inputs(inputs)
    , m_builtins(inputs.program.builtins())
    , m_borrowedMaker(inputs.makerPool())
    , m_maker(m_borrowedMaker.get())
    , m_unseenCodeRuns(isRaised(inputs, ValueFlowHostNode::UnseenCodeChangesRealm))
    , m_unknownCodeWrites(isRaised(inputs, ValueFlowHostNode::UnknownCodeWrites) || unseenCodeCanRun(inputs.solution))
    , m_escaped(m_builtins.builtinCount())
    , m_lost(m_builtins.builtinCount())
    , m_isWrittenUnderKeysNobodyKnows(m_builtins.builtinCount())
    , m_isWrittenUnderIndexes(m_builtins.builtinCount())
    , m_hasPrototypeLinkChangedByProgram(m_builtins.builtinCount())
    , m_mayBeHeldWithoutBeingListed(m_builtins.builtinCount())
{
    noteHoldersIn(ValueFlowProgramNodes::escaped(), m_escaped);
    noteHoldersIn(ValueFlowProgramNodes::lost(), m_lost);
    noteWrites(index);

    // Code that the build has not seen writes what it likes, and the analysis has let it. Where nothing is written behind the analysis there is nothing to refuse.
    if (!m_unseenCodeRuns && isWrittenBehindAnalysis())
        findObjectsHeldWithoutBeingListed();
}

void BuiltinHolders::noteWrites(const ValueFlowHeapIndex& index)
{
    Vector<ValueFlowHeapIndex::Stretch> stretches = index.stretchesOfNodesWrittenUnder([](ValueFlowName name) {
        return formOfWrite(name.kind) != BuiltinWriteForm::Other;
    });

    const ParallelLoops& loops = m_inputs.loops();
    ValueSetMakers makers(m_inputs);
    Vector<std::unique_ptr<Notes>> notesOfWorker(loops.width());
    forEachIndexInParallel(loops, stretches.size(), [&](unsigned position, unsigned worker) {
        if (!notesOfWorker[worker])
            notesOfWorker[worker] = makeUniqueWithoutFastMallocCheck<Notes>(m_builtins);
        for (const ValueFlowHeapIndex::WrittenNode& node : stretches[position].nodes)
            noteWrite(*notesOfWorker[worker], makers.of(worker), node.objects, stretches[position].name);
    });

    auto merge = [](WritesThroughPlaceholder& writes, const WritesThroughPlaceholder& others) {
        writes.keys.merge(others.keys);
        writes.writesUnderKeysNobodyKnows |= others.writesUnderKeysNobodyKnows;
        writes.changesPrototypeLinks |= others.changesPrototypeLinks;
    };
    Vector<BitVector> keysWritten(m_builtins.builtinCount());
    for (std::unique_ptr<Notes>& notes : notesOfWorker) {
        if (!notes)
            continue;
        m_isWrittenUnderKeysNobodyKnows.merge(notes->isWrittenUnderKeysNobodyKnows);
        m_isWrittenUnderIndexes.merge(notes->isWrittenUnderIndexes);
        m_hasPrototypeLinkChangedByProgram.merge(notes->hasPrototypeLinkChangedByProgram);
        for (const Notes::ThroughSet& written : notes->throughSet.values()) {
            for (BuiltinID holder : written.holders) {
                for (uint32_t key : written.keysWritten)
                    keysWritten[holder.index()].set(key);
            }
        }
        merge(m_throughUnknownObject, notes->throughUnknownObject);
        merge(m_throughUntrackedObject, notes->throughUntrackedObject);
        for (uint64_t property : notes->propertiesOfOperands)
            m_propertiesOfOperands.add(property);
        notes = nullptr;
    }
    for (uint32_t holder = 0; holder < keysWritten.size(); ++holder) {
        for (size_t key : keysWritten[holder])
            m_namesWritten.append({ BuiltinID(holder), PropertyKeyID(static_cast<uint32_t>(key)) });
    }
}

Vector<BuiltinID> BuiltinHolders::holdersIn(ValueSetMaker& maker, ValueSetID set) const
{
    Vector<BuiltinID> found;
    m_inputs.sets().forEachMember(maker.intersect(set, m_inputs.universe().builtinObjects()), [&](ValueFlowObjectID object) {
        found.append(m_builtins.canonicalID(m_inputs.universe().builtinOf(object)));
    });
    AOT::sort(found);
    removeRepeatedElements(found);
    return found;
}

// Many names are read and written through one set.
void BuiltinHolders::forEachHolderIn(ValueSetID set, NOESCAPE const Invocable<void(BuiltinID)> auto& functor)
{
    const Vector<BuiltinID>& holders = m_holdersInSet.ensure(set.index(), [&] {
        return holdersIn(m_maker, set);
    }).iterator->value;
    for (BuiltinID holder : holders)
        functor(holder);
}

void BuiltinHolders::noteHoldersIn(ValueFlowNodeID node, BitVector& holders)
{
    forEachHolderIn(m_inputs.solver().valueOf(node), [&](BuiltinID holder) {
        holders.quickSet(holder.index());
    });
}

void BuiltinHolders::noteWrite(Notes& notes, ValueSetMaker& maker, ValueSetID receivers, ValueFlowName name) const
{
    BuiltinWriteForm form = formOfWrite(name.kind);
    if (form == BuiltinWriteForm::Other || (form == BuiltinWriteForm::OfSlot && name.payload < m_builtins.slotCount()))
        return;
    Notes::ThroughSet& written = notes.throughSet.ensure(receivers.index(), [&] {
        return Notes::ThroughSet { holdersIn(maker, receivers), { } };
    }).iterator->value;
    auto forEachHolderAmongReceivers = [&](NOESCAPE const Invocable<void(BuiltinID)> auto& functor) {
        for (BuiltinID holder : written.holders)
            functor(holder);
    };
    if (form == BuiltinWriteForm::OfSlot) {
        // A cell of native code on the built-in's own object, under the name of a property that a row of the table asks for.
        forEachHolderAmongReceivers([&](BuiltinID builtin) {
            notes.propertiesOfOperands.append(static_cast<uint64_t>(builtin.index()) << 32 | (name.payload - m_builtins.slotCount()));
        });
        return;
    }

    auto noteThrough = [&](WritesThroughPlaceholder& writes) {
        switch (form) {
        case BuiltinWriteForm::UnderName:
            writes.keys.set(name.payload);
            return;
        case BuiltinWriteForm::UnderKeyNobodyKnows:
            writes.writesUnderKeysNobodyKnows = true;
            return;
        case BuiltinWriteForm::OfPrototypeLink:
            writes.changesPrototypeLinks = true;
            return;
        case BuiltinWriteForm::OfSlot:
        case BuiltinWriteForm::Other:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    if (m_inputs.sets().contains(receivers, ValueFlowUniverse::unknownObject()))
        noteThrough(notes.throughUnknownObject);
    if (m_inputs.sets().contains(receivers, ValueFlowUniverse::untrackedObject()))
        noteThrough(notes.throughUntrackedObject);

    if (form == BuiltinWriteForm::UnderName) {
        if (!written.holders.isEmpty())
            written.keysWritten.append(name.payload);
        return;
    }
    forEachHolderAmongReceivers([&](BuiltinID holder) {
        switch (form) {
        case BuiltinWriteForm::UnderName:
            break;
        case BuiltinWriteForm::UnderKeyNobodyKnows:
            (name.kind == ValueFlowNameKind::Elements ? notes.isWrittenUnderIndexes : notes.isWrittenUnderKeysNobodyKnows).quickSet(holder.index());
            return;
        case BuiltinWriteForm::OfPrototypeLink:
            notes.hasPrototypeLinkChangedByProgram.quickSet(holder.index());
            return;
        case BuiltinWriteForm::OfSlot:
        case BuiltinWriteForm::Other:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

// No lookup hands out a member that lets in unseen code, so `globalThis[key]` lists no Function.
void BuiltinHolders::findObjectsHeldWithoutBeingListed()
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueSetID held = m_inputs.solver().valueOf(m_inputs.bodies().nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed));
    m_inputs.sets().forEachMember(m_maker.intersect(held, universe.builtinObjects()), [&](ValueFlowObjectID object) {
        m_mayBeHeldWithoutBeingListed.quickSet(m_builtins.canonicalID(universe.builtinOf(object)).index());
    });
}

bool BuiltinHolders::isWrittenByProgram(BuiltinID holder, PropertyKeyID key) const
{
    if (m_isWrittenUnderKeysNobodyKnows.get(holder.index()) || (m_isWrittenUnderIndexes.get(holder.index()) && m_inputs.program.isIndexLike(key)))
        return true;
    if (isLost(holder) && m_throughUntrackedObject.mayWriteUnder(key))
        return true;
    // The program holds such an object as one nobody knows, so its own store shows as one through the unknown object. Lookups read what is stored that way from it.
    if (m_mayBeHeldWithoutBeingListed.get(holder.index()) && m_throughUnknownObject.mayWriteUnder(key))
        return true;
    return std::ranges::binary_search(m_namesWritten, std::pair { holder, key }, { }, orderOf);
}

bool BuiltinHolders::mayWriteLockedLocationUnderKeyNobodyKnows() const
{
    BitVector written = m_isWrittenUnderKeysNobodyKnows;
    written.merge(m_isWrittenUnderIndexes);
    for (size_t index : written) {
        BuiltinID holder(static_cast<uint32_t>(index));
        if (m_builtins.at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::HasLockedMembers, BuiltinFlag::IsClosedToOtherNames }) || !m_builtins.closedKeysOf(holder).empty())
            return true;
        if (std::ranges::any_of(m_builtins.membersOf(holder), [&](BuiltinID member) { return isLockedLocation(m_builtins, member); }))
            return true;
    }
    return false;
}

bool BuiltinHolders::mayBeWrittenBehindAnalysis(BuiltinID holder, PropertyKeyID key) const
{
    if (m_unseenCodeRuns)
        return true;
    return hasEscaped(holder) && (m_unknownCodeWrites || m_throughUnknownObject.mayWriteUnder(key));
}

bool BuiltinHolders::isIntact(BuiltinID holder) const
{
    // The run time refuses whoever tries to change it, whoever that is.
    if (m_builtins.at(holder).flags.contains(BuiltinFlag::IsLockedObject))
        return true;
    if (m_unseenCodeRuns || m_isWrittenUnderKeysNobodyKnows.get(holder.index()) || m_isWrittenUnderIndexes.get(holder.index()) || m_hasPrototypeLinkChangedByProgram.get(holder.index()))
        return false;
    if (isLost(holder) && m_throughUntrackedObject.writesAnything())
        return false;
    return !hasEscaped(holder) || !isWrittenBehindAnalysis();
}

static Vector<BuiltinUseRecord> collectBuiltinUses(const ValueFlowResultInputs& inputs)
{
    Vector<BuiltinUseRecord> uses;
    for (uint32_t index = 0, count = inputs.program.builtins().builtinCount(); index < count; ++index) {
        OptionSet<BuiltinUse> usesOfBuiltin = inputs.solver().usesOf(BuiltinID(index));
        if (!usesOfBuiltin.isEmpty())
            uses.append(BuiltinUseRecord { index, static_cast<BuiltinUse>(usesOfBuiltin.toRaw()), noWitness });
    }
    return uses;
}

// Native code of the feature creates the value of a data property on the first read, and what the property holds is called as an unknown function.
static OptionSet<BuiltinUse> usesEnteringCode(BuiltinKind kind)
{
    switch (kind) {
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::NativeBinding:
        return callingBuiltinUses | accessingBuiltinUses;
    default:
        return callingBuiltinUses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findNamespacesOfModules(const BuiltinTable& builtins)
{
    BitVector namespaces(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
        const RuntimeModuleData& module = builtins.at(RuntimeModuleID(index));
        if (module.namespaceObject)
            namespaces.quickSet(module.namespaceObject.index());
    }
    return namespaces;
}

// A literal, a coroutine and a rest parameter create an instance without running the constructor. The prototype's entry lists the features every instance needs.
static void addPrototypesOfAllocatedClasses(const ValueFlowResultInputs& inputs, BitVector& builtinsWhoseCodeRuns)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    constexpr uint32_t objectsPerTask = 4096;
    uint32_t numberOfObjects = universe.numberOfObjects();
    Vector<BitVector> allocatedClassesOfWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), (numberOfObjects + objectsPerTask - 1) / objectsPerTask, [&](unsigned task, unsigned worker) {
        BitVector& allocatedClasses = allocatedClassesOfWorker[worker];
        allocatedClasses.ensureSize(builtins.classCount());
        for (uint32_t index = task * objectsPerTask; index < std::min(numberOfObjects, (task + 1) * objectsPerTask); ++index) {
            ValueFlowObjectID object(index);
            switch (universe.kindOf(object)) {
            case ValueFlowObjectKind::CoroutineResult:
            case ValueFlowObjectKind::RestArray:
            case ValueFlowObjectKind::Site:
                break;
            default:
                continue;
            }
            BuiltinClassID builtinClass = universe.classOf(object);
            if (builtinClass && inputs.isReached(universe.functionOf(object)))
                allocatedClasses.quickSet(builtinClass.index());
        }
    });
    BitVector allocatedClasses(builtins.classCount());
    for (const BitVector& classes : allocatedClassesOfWorker)
        allocatedClasses.merge(classes);
    for (size_t index : allocatedClasses) {
        const BuiltinClass& builtinClass = builtins.at(BuiltinClassID(index));
        if (BuiltinID entry = builtinClass.prototype ? builtinClass.prototype : builtinClass.constructor)
            builtinsWhoseCodeRuns.quickSet(builtins.canonicalID(entry).index());
    }
}

// The table attaches features to the entry of the object, and an access is recorded on the location that was accessed.
static BitVector findBuiltinsWhoseCodeRuns(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BitVector namespacesOfModules = findNamespacesOfModules(builtins);
    BitVector builtinsWhoseCodeRuns(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        OptionSet<BuiltinUse> enteringUses = usesEnteringCode(builtins.at(builtin).kind);
        if (namespacesOfModules.quickGet(index))
            enteringUses.add(accessingBuiltinUses);
        if (inputs.solver().usesOf(builtin).containsAny(enteringUses))
            builtinsWhoseCodeRuns.quickSet(builtins.canonicalID(builtin).index());
    }
    addPrototypesOfAllocatedClasses(inputs, builtinsWhoseCodeRuns);
    return builtinsWhoseCodeRuns;
}

// A build that admits code at run time is not solved.
static bool mayHold(const BuiltinHolders& holders, BuiltinID builtin, const BuiltinCondition* condition)
{
    if (!condition)
        return true;
    if (condition->buildAdmitsRuntimeCode)
        return false;
    if (holders.hasEscaped(builtin) && !condition->calledOn && !holders.runsUnseenCodeFor(builtin, condition->hasProperty))
        return true;
    if (condition->firstText() && isObjectKind(holders.kindOf(builtin)) && !holders.operandMayHaveProperty(builtin, condition->firstText()))
        return false;
    return !condition->hasProperty || holders.operandMayHaveProperty(builtin, condition->hasProperty);
}

static void forEachTypeOfValueThatOverloadMakes(const BuiltinOverload& overload, NOESCAPE const Invocable<void(BuiltinTypeID)> auto& functor)
{
    auto visit = [&](const BuiltinPlace& place) {
        if (place.kind == BuiltinPlaceKind::NewValue && place.newValueType)
            functor(place.newValueType);
    };
    if (overload.result.type)
        functor(overload.result.type);
    for (const BuiltinTransfer& transfer : overload.flow.transfers)
        visit(transfer.from);
    for (const BuiltinCallback& callback : overload.flow.callbacks) {
        visit(callback.newTarget);
        visit(callback.thisValue);
        visit(callback.spreadArguments);
        for (const BuiltinPlace& argument : callback.arguments)
            visit(argument);
    }
    for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
        visit(action.value);
    for (const BuiltinHandlerCall& call : overload.flow.handlerCalls) {
        visit(call.thisValue);
        for (const BuiltinPlace& argument : call.arguments)
            visit(argument);
    }
}

namespace {

class HostClassesRootedByTheirMakers {
public:
    HostClassesRootedByTheirMakers(const BuiltinTable& builtins, NOESCAPE const Invocable<bool(BuiltinID)> auto& isNamedByStatement)
        : m_builtins(builtins)
        , m_makersNameFeatures(builtins.classCount())
    {
        m_classOfBuiltin.fill(noClass, builtins.builtinCount());
        m_hostClassOfPrototype.fill(noClass, builtins.builtinCount());
        BitVector hangsOutsidePrototypeObjects(builtins.builtinCount());
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            BuiltinID holder = builtins.at(BuiltinID(index)).holder;
            if (holder && builtins.at(holder).kind != BuiltinKind::Prototype)
                hangsOutsidePrototypeObjects.quickSet(builtins.canonicalID(BuiltinID(index)).index());
        }
        for (uint32_t index = 0; index < builtins.classCount(); ++index) {
            const BuiltinClass& builtinClass = builtins.at(BuiltinClassID(index));
            if (builtinClass.origin != BuiltinOrigin::Host || !builtinClass.prototype)
                continue;
            m_hostClassOfPrototype[builtins.canonicalID(builtinClass.prototype).index()] = index;
            if (builtinClass.instancesAreEntries)
                continue;
            bool canBeNamed = builtinClass.constructor && hangsOutsidePrototypeObjects.quickGet(builtins.canonicalID(builtinClass.constructor).index());
            if (canBeNamed && (isNamedByStatement(builtinClass.constructor) || isNamedByStatement(builtinClass.prototype)))
                continue;
            m_makersNameFeatures.quickSet(index);
            m_classOfBuiltin[builtins.canonicalID(builtinClass.prototype).index()] = index;
            if (builtinClass.constructor && !canBeNamed)
                m_classOfBuiltin[builtins.canonicalID(builtinClass.constructor).index()] = index;
        }
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            BuiltinID holder = builtins.at(BuiltinID(index)).holder;
            uint32_t canonical = builtins.canonicalID(BuiltinID(index)).index();
            if (holder && m_classOfBuiltin[canonical] == noClass)
                m_classOfBuiltin[canonical] = m_classOfBuiltin[builtins.canonicalID(holder).index()];
        }
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index)
            checkMaker(builtins.canonicalID(BuiltinID(index)), BuiltinID(index));
    }

    bool isRootedByMakersOfClass(BuiltinID builtin, RemovableFeature feature) const
    {
        uint32_t owner = m_classOfBuiltin[builtin.index()];
        return owner != noClass && m_makersNameFeatures.quickGet(owner) && classNeeds(owner, feature);
    }

    // The native code of a method or an accessor of a host class tests its receiver first. Whoever takes it from the prototype object can run it without an instance, and it leaves with the features of the class.
    BuiltinID entryWithFeaturesOfClassOf(BuiltinID member) const
    {
        const Builtin& entry = m_builtins.at(member);
        if (!entry.holder || (entry.kind != BuiltinKind::Method && entry.kind != BuiltinKind::Accessor))
            return { };
        uint32_t owner = m_hostClassOfPrototype[m_builtins.canonicalID(entry.holder).index()];
        if (owner == noClass)
            return { };
        const BuiltinClass& builtinClass = m_builtins.at(BuiltinClassID(owner));
        BuiltinID prototype = m_builtins.canonicalID(builtinClass.prototype);
        if (!m_builtins.featureUsesOf(prototype).empty() || !builtinClass.constructor)
            return prototype;
        return m_builtins.canonicalID(builtinClass.constructor);
    }

private:
    static constexpr uint32_t noClass = UINT32_MAX;

    bool needsWithoutCondition(BuiltinID builtin, RemovableFeature feature) const
    {
        return std::ranges::any_of(m_builtins.featureUsesOf(builtin), [&](const BuiltinFeatureUse& line) {
            return line.feature == feature && !line.condition;
        });
    }

    bool classNeeds(uint32_t builtinClass, RemovableFeature feature) const
    {
        return needsWithoutCondition(m_builtins.canonicalID(m_builtins.at(BuiltinClassID(builtinClass)).prototype), feature);
    }

    void checkMakerOf(uint32_t made, BuiltinID maker)
    {
        uint32_t owner = m_classOfBuiltin[maker.index()];
        if (!m_makersNameFeatures.quickGet(made) || owner == made)
            return;
        for (const BuiltinFeatureUse& line : m_builtins.featureUsesOf(m_builtins.canonicalID(m_builtins.at(BuiltinClassID(made)).prototype))) {
            if (line.condition || needsWithoutCondition(maker, line.feature) || (owner != noClass && classNeeds(owner, line.feature)))
                continue;
            m_makersNameFeatures.quickClear(made);
            return;
        }
    }

    void checkMaker(BuiltinID maker, BuiltinID spelling)
    {
        Vector<BuiltinTypeID, 8> types;
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(spelling)) {
            forEachTypeOfValueThatOverloadMakes(overload, [&](BuiltinTypeID type) {
                types.appendIfNotContains(type);
            });
        }
        for (size_t position = 0; position < types.size(); ++position) {
            for (const BuiltinTypePart& part : m_builtins.at(types[position]).parts) {
                for (BuiltinClassID current = part.builtinClass; current; current = m_builtins.at(current).parent)
                    checkMakerOf(current.index(), maker);
                for (uint32_t other = 0; part.builtinClass && !part.isOfClassItself && other < m_builtins.classCount(); ++other) {
                    for (BuiltinClassID current = m_builtins.at(BuiltinClassID(other)).parent; current; current = m_builtins.at(current).parent) {
                        if (current == part.builtinClass)
                            checkMakerOf(other, maker);
                    }
                }
                for (const BuiltinSlotContents& contents : part.contents) {
                    if (contents.type)
                        types.appendIfNotContains(contents.type);
                }
            }
        }
    }

    const BuiltinTable& m_builtins;
    Vector<uint32_t> m_classOfBuiltin;
    Vector<uint32_t> m_hostClassOfPrototype;
    BitVector m_makersNameFeatures;
};

} // anonymous namespace

static Vector<FeatureUseRecord> collectFeatureUses(const ValueFlowResultInputs& inputs, const BuiltinHolders& holders, const BitVector& builtinsWhoseCodeRuns)
{
    auto orderOf = [](const FeatureUseRecord& use) {
        return std::tuple(use.feature, use.builtin);
    };
    const BuiltinTable& builtins = inputs.program.builtins();
    bool refusesEvaluatedText = inputs.solver().input().refusesEvaluatedText;
    ValueSetID namedByStatements = inputs.solver().valueOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::BuiltinsThatStatementsName));
    auto isNamedByStatement = [&](BuiltinID builtin) {
        return inputs.sets().contains(namedByStatements, inputs.universe().objectOf(builtin));
    };
    HostClassesRootedByTheirMakers hostClasses(builtins, isNamedByStatement);
    Vector<FeatureUseRecord> uses;
    for (size_t index : builtinsWhoseCodeRuns) {
        if (refusesEvaluatedText && builtins.at(BuiltinID(index)).flags.contains(BuiltinFlag::EvaluatesText))
            continue;
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(BuiltinID(index))) {
            if (mayHold(holders, BuiltinID(index), line.condition) && !hostClasses.isRootedByMakersOfClass(BuiltinID(index), line.feature))
                uses.append(FeatureUseRecord { static_cast<uint32_t>(line.feature), static_cast<uint32_t>(index), noWitness });
        }
        BuiltinID entryOfClass = hostClasses.entryWithFeaturesOfClassOf(BuiltinID(index));
        if (!entryOfClass || !isNamedByStatement(builtins.canonicalID(builtins.at(BuiltinID(index)).holder)))
            continue;
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(entryOfClass)) {
            if (!line.condition && !hostClasses.isRootedByMakersOfClass(BuiltinID(index), line.feature))
                uses.append(FeatureUseRecord { static_cast<uint32_t>(line.feature), static_cast<uint32_t>(index), noWitness });
        }
    }
    AOT::sort(uses, { }, orderOf);
    uses.shrink(std::ranges::unique(uses, { }, orderOf).begin() - uses.begin());
    return uses;
}

// The image admits at run time what its records name and refuses the rest, so they name what the solve has let in, no more and no less.
static Vector<UnseenCodeRecord> collectUnseenCode(const ValueFlowResultInputs& inputs, Vector<UnseenCodeRecord>& loadsOfMachineCode)
{
    bool unseenCodeRuns = unseenCodeCanRun(inputs.solution);
    bool modulesAreNamedAtRunTime = modulesAreLoadedByName(inputs.solution);

    Vector<UnseenCodeRecord> records = unseenCodeRecordsOf(inputs.solution);
    records.removeAllMatching([&](const UnseenCodeRecord& record) {
        if (record.cause == UnseenCodeCause::RunsMachineCode) {
            loadsOfMachineCode.append(record);
            return true;
        }
        return admitsUnknownJavaScript(record.cause) ? !unseenCodeRuns : !modulesAreNamedAtRunTime;
    });
    if (unseenCodeRuns && !std::ranges::any_of(records, admitsUnknownJavaScript, &UnseenCodeRecord::cause))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::RunsUnseenCode });
    if (modulesAreNamedAtRunTime && !std::ranges::any_of(records, loadsModulesByName, &UnseenCodeRecord::cause))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::LoadsModuleByName });
    if (!inputs.build.isEnabledForProgram(FlowResultKind::ReachedFunctions))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::FlowResultDisabled });

    auto orderOf = [](const UnseenCodeRecord& record) {
        return std::tuple(record.cause, record.builtin, record.witness.function, record.witness.value);
    };
    AOT::sort(records, { }, orderOf);
    records.shrink(std::ranges::unique(records, { }, orderOf).begin() - records.begin());
    return records;
}

// A function and a copy of it hold the same load, and the program's author wrote it once.
static void warnAboutLoadsUnderComputedNames(BuildContext& build, std::span<const UnseenCodeRecord> records)
{
    Vector<SourceLocation> reportedLocations;
    for (const UnseenCodeRecord& record : records) {
        if (record.cause != UnseenCodeCause::LoadsModuleByName || record.builtin != Wire::none || record.witness.value == Wire::none)
            continue;
        if (hasModule(record.witness.location)) {
            if (reportedLocations.contains(record.witness.location))
                continue;
            reportedLocations.append(record.witness.location);
        }
        build.diagnostics().add(UserDiagnostic {
            .kind = UserDiagnosticKind::CodeOutsideImageIsRefused,
            .function = FunctionID(record.witness.function),
            .value = ValueID(record.witness.value),
            .location = record.witness.location,
        });
    }
}

// A store through the unknown object is a use of the member too, wherever the holder has escaped.
static BuiltinLocationState stateOfLocation(const ValueFlowResultInputs& inputs, const BuiltinHolders& holders, BuiltinID member)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    if (isLockedLocation(builtins, member))
        return BuiltinLocationState::Constant;
    switch (builtins.at(member).mutability) {
    case BuiltinMutability::AlwaysLocked:
        return BuiltinLocationState::Constant;
    case BuiltinMutability::ChangesOnItsOwn:
        return BuiltinLocationState::Unknown;
    case BuiltinMutability::ConstantUnlessWritten:
        break;
    }

    BuiltinID holder = builtins.canonicalID(builtins.at(member).holder);
    PropertyKeyID key = inputs.program.keyOf(builtins.at(member).name);
    OptionSet<BuiltinUse> uses = inputs.solver().usesOf(member);
    bool isWritten = holders.unseenCodeRuns() ? uses.contains(BuiltinUse::Written) : holders.isWrittenByProgram(holder, key);
    if (isWritten || !inputs.build.isEnabled(FlowResultKind::ConstantBuiltinLocations, member.index()))
        return BuiltinLocationState::Unknown;
    if (!holders.mayBeWrittenBehindAnalysis(holder, key))
        return BuiltinLocationState::Constant;
    // Start-up makes a location immutable that is listed as constant, if it finds the holder: whoever writes it behind the analysis is refused.
    bool isLockedAtStartUp = !uses.isEmpty() && canLockAtStartUp(builtins, member) && !(holders.unseenCodeRuns() && holder == Builtins::GlobalThis);
    return isLockedAtStartUp ? BuiltinLocationState::Constant : BuiltinLocationState::Unknown;
}

static auto orderOfLocation(const BuiltinLocationFlow& location)
{
    return std::tuple(location.holder, location.key);
}

// Two entries may describe one location. It is constant if both say so.
static void sortAndMergeLocations(Vector<BuiltinLocationFlow>& locations)
{
    AOT::stableSort(locations, { }, orderOfLocation);
    size_t size = 0;
    for (const BuiltinLocationFlow& location : locations) {
        if (!size || orderOfLocation(locations[size - 1]) != orderOfLocation(location)) {
            locations[size++] = location;
            continue;
        }
        BuiltinLocationFlow& merged = locations[size - 1];
        if (location.state == BuiltinLocationState::Unknown)
            merged.state = BuiltinLocationState::Unknown;
        keepLowerWitness(merged.witness, location.witness);
    }
    locations.shrink(size);
}

// The operand that a store, a definition or a deletion converts to the key.
static std::optional<unsigned> computedKeyOfWrite(const Value& operation)
{
    switch (operation.opcode) {
    case Opcode::ElemSet:
    case Opcode::ElemDefine:
    case Opcode::ElemDelete:
        return OperandPosition::property(operation.opcode);
    case Opcode::DefineGetter:
    case Opcode::DefineSetter:
        return operation.key() ? std::nullopt : OperandPosition::computedKey(operation.opcode);
    default:
        return std::nullopt;
    }
}

static bool writesUnderKey(BuiltinPropertyActionKind kind)
{
    switch (kind) {
    case BuiltinPropertyActionKind::StoresByKey:
    case BuiltinPropertyActionKind::DeletesByKey:
    case BuiltinPropertyActionKind::DefinesName:
    case BuiltinPropertyActionKind::TakesDescriptor:
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
        return true;
    default:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findBuiltinsThatWriteUnderKeys(const BuiltinTable& builtins)
{
    BitVector builtinsThatWrite(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        bool writes = std::ranges::any_of(builtins.overloadsOf(BuiltinID(index)), [](const BuiltinOverload& overload) {
            return std::ranges::any_of(overload.flow.propertyActions, writesUnderKey, &BuiltinPropertyAction::kind);
        });
        if (writes)
            builtinsThatWrite.quickSet(index);
    }
    return builtinsThatWrite;
}

static std::optional<BuiltinCallMode> modeOfCallOperation(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Call:
    case Opcode::CallList:
        return BuiltinCallMode::Call;
    case Opcode::Construct:
    case Opcode::ConstructList:
        return BuiltinCallMode::Construct;
    default:
        return std::nullopt;
    }
}

// The value that a call which spells out its arguments passes at the position.
static ValueID valuePassedAt(const Value& call, BuiltinOperand position)
{
    if (call.opcode != Opcode::Call && call.opcode != Opcode::Construct)
        return { };
    switch (position.kind()) {
    case BuiltinOperand::Kind::Receiver:
        return call.operand(1);
    case BuiltinOperand::Kind::Argument:
        return 2 + position.argumentIndex() < call.numberOfOperands() ? call.operand(2 + position.argumentIndex()) : ValueID();
    case BuiltinOperand::Kind::ArgumentsFrom:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// The run time refuses a write to a locked location, so the location stays constant. The record says that the program tries, and the witness says where.
class WritesOfLockedLocations {
public:
    WritesOfLockedLocations(const ValueFlowResultInputs& inputs, BuiltinHolders& holders, ValueFlowHeapView& heap)
        : m_inputs(inputs)
        , m_program(inputs.program)
        , m_holders(holders)
        , m_heap(heap)
        , m_borrowedMaker(inputs.makerPool())
        , m_maker(m_borrowedMaker.get())
    {
    }

    void collect();

    Vector<BuiltinLocationFlow> records;
    Vector<ValueFlowWriteUnderNewName> underNewNames;

private:
    using LowestWitnesses = HashMap<uint64_t, Witness, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    struct Notes {
        ValueSetMaker& maker;
        LowestWitnesses& lowestWitnessOfWrite;
        Vector<ValueFlowWriteUnderNewName>& underNewNames;
        Vector<uint64_t>& callsOfBuiltinFunctionsAlone;
    };

    static void note(const Notes&, ValueSetID receivers, PropertyKeyID, const Witness&);
    void recordNotedWrites();
    void record(ValueSetID receivers, PropertyKeyID, const Witness&);
    void noteUnderText(const Notes&, ValueSetID receivers, StringID, const Witness&) const;
    void noteUnderNamesAmong(const Notes&, ValueSetID keys, FunctionID, ValueID keyValue, ValueSetID receivers, const Witness&) const;
    void noteOperation(const Notes&, const ValueFlowStatement&) const;
    void noteApplication(const Notes&, const ValueFlowBuiltinApplication&) const;
    bool listsItsObjects(ValueSetID) const;
    bool listsItsNames(ValueSetMaker&, ValueSetID keys, FunctionID, ValueID keyValue) const;
    bool callsOnlyBuiltinFunctions(ValueSetMaker&, const ValueFlowStatement&) const;
    static uint64_t siteOf(FunctionID function, ValueID site) { return static_cast<uint64_t>(function.index()) << 32 | site.index(); }

    const ValueFlowResultInputs& m_inputs;
    const Program& m_program;
    BuiltinHolders& m_holders;
    ValueFlowHeapView& m_heap;
    BorrowedValueSetMaker m_borrowedMaker;
    ValueSetMaker& m_maker;
    BitVector m_builtinsThatWriteUnderKeys;
    ValueSetID m_callablesThatAreNoBuiltinFunctions { ValueSets::nothing };
    Vector<uint64_t> m_callsOfBuiltinFunctionsAlone;
    LowestWitnesses m_lowestWitnessOfWrite;
};

} // anonymous namespace

// In untyped code an object may be a locked built-in, a callee a built-in that writes, and a key the name of a locked member, as each of them may be anything else.
bool WritesOfLockedLocations::listsItsObjects(ValueSetID objects) const
{
    return !ValueSets::isComplement(objects) && !m_inputs.sets().meet(objects, m_inputs.universe().objectsThatMayBeAnything());
}

// To the sets a text that names no property anywhere else in the program is a string like any other: the type of the value spells such texts out.
bool WritesOfLockedLocations::listsItsNames(ValueSetMaker& maker, ValueSetID keys, FunctionID function, ValueID keyValue) const
{
    if (ValueSets::isComplement(keys))
        return false;
    Vector<ValueFlowObjectID, 8> primitives;
    for (auto [primitive, text] : primitiveKeyTexts)
        primitives.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(primitive)));
    if (keyValue) {
        const Type& type = m_program.typeOf(m_program.at(function), keyValue);
        if (!type.stringLiterals.isEmpty() && !(type.primitives & Primitive::String))
            primitives.append(ValueFlowUniverse::primitive(PrimitiveSet::String));
    }
    return m_inputs.sets().isSubset(keys, maker.unite(m_inputs.universe().keys(), maker.of(primitives.span())));
}

bool WritesOfLockedLocations::callsOnlyBuiltinFunctions(ValueSetMaker& maker, const ValueFlowStatement& call) const
{
    ValueSetID callees = m_heap.valueOf(maker, m_inputs.bodies().operandsOf(call)[0]);
    return !ValueSets::isComplement(callees) && !m_inputs.sets().meet(callees, m_callablesThatAreNoBuiltinFunctions);
}

void WritesOfLockedLocations::note(const Notes& notes, ValueSetID receivers, PropertyKeyID key, const Witness& witness)
{
    auto result = notes.lowestWitnessOfWrite.add(static_cast<uint64_t>(receivers.index()) << 32 | key.index(), witness);
    if (!result.isNewEntry)
        keepLowerWitness(result.iterator->value, witness);
}

void WritesOfLockedLocations::recordNotedWrites()
{
    for (auto& [write, witness] : m_lowestWitnessOfWrite)
        record(ValueSetID(static_cast<uint32_t>(write >> 32)), PropertyKeyID(static_cast<uint32_t>(write)), witness);
    m_lowestWitnessOfWrite.clear();
}

void WritesOfLockedLocations::record(ValueSetID receivers, PropertyKeyID key, const Witness& witness)
{
    // Most receivers hold no object that locks anything under the key, and a set of what has escaped holds every built-in object.
    const ValueFlowHeapTables& tables = m_inputs.universe().builtinTables().heap();
    ValueSetID holdersThatMayLockKey = tables.under(key).holdersWithLockedLocation;
    if (m_program.isIndexLike(key))
        holdersThatMayLockKey = m_maker.unite(holdersThatMayLockKey, tables.holdersWithLockedIndexes());
    if (!m_inputs.sets().meet(receivers, holdersThatMayLockKey))
        return;
    m_holders.forEachHolderIn(m_maker.intersect(receivers, holdersThatMayLockKey), [&](BuiltinID holder) {
        if (isLockedLocation(m_program, holder, key))
            records.append(BuiltinLocationFlow { holder, key, BuiltinLocationState::Constant, witness });
    });
}

void WritesOfLockedLocations::noteUnderText(const Notes& notes, ValueSetID receivers, StringID text, const Witness& witness) const
{
    if (std::optional<PropertyKeyID> key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text })) {
        note(notes, receivers, *key, witness);
        return;
    }
    // The table has no member under such a text.
    const BuiltinTable& builtins = m_program.builtins();
    for (BuiltinID holder : m_holders.holdersIn(notes.maker, receivers)) {
        if (builtins.at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) || builtins.closesIndexKeys(holder))
            notes.underNewNames.append({ holder, text, witness });
    }
}

// A key names one property where it is a key of the program, where the language makes one text of it, and where it is a text that the type of the value spells out.
void WritesOfLockedLocations::noteUnderNamesAmong(const Notes& notes, ValueSetID keys, FunctionID function, ValueID keyValue, ValueSetID receivers, const Witness& witness) const
{
    const ValueSets& sets = m_inputs.sets();
    if (!listsItsNames(notes.maker, keys, function, keyValue))
        return;
    // A key that may be any of a crowd of names says as little about the property that is written as a key nobody knows.
    ValueSetID namesAmongKeys = notes.maker.intersect(keys, m_inputs.universe().keys());
    if (sets.size(namesAmongKeys) <= m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart)) {
        sets.forEachMember(namesAmongKeys, [&](ValueFlowObjectID key) {
            if (m_inputs.universe().kindOf(key) == ValueFlowObjectKind::Key)
                note(notes, receivers, m_inputs.universe().keyOf(key), witness);
        });
    }
    for (auto [primitive, text] : primitiveKeyTexts) {
        if (!sets.contains(keys, ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(primitive))))
            continue;
        if (StringID string = m_program.strings.find(byteCast<uint8_t>(text.span())))
            noteUnderText(notes, receivers, string, witness);
    }
    if (!keyValue || !sets.contains(keys, ValueFlowUniverse::primitive(PrimitiveSet::String)))
        return;
    for (StringID text : m_program.typeOf(m_program.at(function), keyValue).stringLiterals)
        noteUnderText(notes, receivers, text, witness);
}

// An operation has asked for the node of whatever it writes under a name.
void WritesOfLockedLocations::noteOperation(const Notes& notes, const ValueFlowStatement& statement) const
{
    const ValueFlowBodies& bodies = m_inputs.bodies();
    if (modeOfCallOperation(statement.opcode)) {
        if (callsOnlyBuiltinFunctions(notes.maker, statement))
            notes.callsOfBuiltinFunctionsAlone.append(siteOf(statement.function, statement.site));
        return;
    }
    std::optional<unsigned> object = OperandPosition::object(statement.opcode);
    if (!object || !listsItsObjects(m_heap.valueOf(notes.maker, bodies.operandsOf(statement)[*object])))
        return;

    Witness witness = witnessOfStatement(m_program, statement);
    ValueFlowStatementID id(static_cast<uint32_t>(&statement - bodies.statements().data()));
    m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID) {
        if (formOfWrite(key.name().kind) == BuiltinWriteForm::UnderName)
            note(notes, key.set, PropertyKeyID(key.payload), witness);
    });

    const Value& operation = m_program.at(statement.function).at(statement.site);
    std::optional<unsigned> key = computedKeyOfWrite(operation);
    if (!key)
        return;
    std::span<const ValueFlowOperand> operands = bodies.operandsOf(statement);
    noteUnderNamesAmong(notes, m_heap.valueOf(notes.maker, operands[*key]), statement.function, operation.operand(*key), m_heap.valueOf(notes.maker, operands[*OperandPosition::object(operation.opcode)]), witness);
}

// Which overload a call selects depends on its operands. Every overload of the mode counts. What all calls of one set of callees share has no place in the program.
void WritesOfLockedLocations::noteApplication(const Notes& notes, const ValueFlowBuiltinApplication& application) const
{
    if (!application.site || !m_builtinsThatWriteUnderKeys.quickGet(application.builtin.index()))
        return;
    if (!std::ranges::binary_search(m_callsOfBuiltinFunctionsAlone, siteOf(application.function, application.site)))
        return;
    const Function& function = m_program.at(application.function);
    const Value& call = function.at(application.site);
    Witness witness { application.function.index(), application.site.index(), function.locationOf(application.site) };
    for (const BuiltinOverload& overload : m_program.builtins().overloadsOf(application.builtin)) {
        if (overload.mode != application.mode())
            continue;
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            if (!writesUnderKey(action.kind))
                continue;
            ValueSetID receivers = application.valueAt(action.object, notes.maker);
            if (!listsItsObjects(receivers))
                continue;
            switch (action.key.kind) {
            case BuiltinKeyKind::Names:
                for (BuiltinName name : action.key.names)
                    note(notes, receivers, m_program.keyOf(name), witness);
                break;
            case BuiltinKeyKind::Operand:
                noteUnderNamesAmong(notes, application.valueAt(*action.key.operand, notes.maker), application.function, valuePassedAt(call, *action.key.operand), receivers, witness);
                break;
            default:
                break;
            }
        }
    }
}

void WritesOfLockedLocations::collect()
{
    for (const NameOnHolder& name : m_holders.namesWritten()) {
        if (isLockedLocation(m_program, name.holder, name.key))
            records.append(BuiltinLocationFlow { name.holder, name.key, BuiltinLocationState::Constant, noWitness });
    }
    if (records.isEmpty() && !m_holders.mayWriteLockedLocationUnderKeyNobodyKnows())
        return;

    struct FoundInFunctions {
        Vector<ValueFlowWriteUnderNewName> underNewNames;
        Vector<uint64_t> callsOfBuiltinFunctionsAlone;
    };
    constexpr uint32_t functionsPerTask = 32;
    const ParallelLoops& loops = m_inputs.loops();
    m_callablesThatAreNoBuiltinFunctions = m_maker.intersect(m_inputs.universe().callables(), ValueSets::complement(m_inputs.universe().builtinFunctions()));
    uint32_t numberOfFunctions = m_program.functions.size();
    Vector<FoundInFunctions> foundInTask((numberOfFunctions + functionsPerTask - 1) / functionsPerTask);
    Vector<LowestWitnesses> lowestWitnessesOfWorker(loops.width());
    {
        ValueSetMakers makers(m_inputs);
        forEachIndexInParallel(loops, foundInTask.size(), [&](unsigned task, unsigned worker) {
            FoundInFunctions& found = foundInTask[task];
            Notes notes { makers.of(worker), lowestWitnessesOfWorker[worker], found.underNewNames, found.callsOfBuiltinFunctionsAlone };
            for (uint32_t index = task * functionsPerTask; index < std::min(numberOfFunctions, (task + 1) * functionsPerTask); ++index) {
                if (!m_inputs.isReached(FunctionID(index)))
                    continue;
                for (const ValueFlowStatement& statement : m_inputs.bodies().statementsOf(index)) {
                    if (statement.kind == ValueFlowStatementKind::Operation && statement.site)
                        noteOperation(notes, statement);
                }
            }
            AOT::sort(found.callsOfBuiltinFunctionsAlone);
        });
    }
    for (FoundInFunctions& found : foundInTask) {
        underNewNames.appendVector(WTF::move(found.underNewNames));
        m_callsOfBuiltinFunctionsAlone.appendVector(WTF::move(found.callsOfBuiltinFunctionsAlone));
    }
    foundInTask.clear();
    for (LowestWitnesses& lowestWitnesses : lowestWitnessesOfWorker) {
        for (auto& [write, witness] : lowestWitnesses) {
            auto result = m_lowestWitnessOfWrite.add(write, witness);
            if (!result.isNewEntry)
                keepLowerWitness(result.iterator->value, witness);
        }
        lowestWitnesses.clear();
    }

    m_builtinsThatWriteUnderKeys = findBuiltinsThatWriteUnderKeys(m_program.builtins());
    Vector<uint64_t> noCalls;
    Notes notes { m_heap.maker(), m_lowestWitnessOfWrite, underNewNames, noCalls };
    forEachBuiltinApplication(m_inputs, m_heap, m_builtinsThatWriteUnderKeys, [&](const ValueFlowBuiltinApplication& application) {
        noteApplication(notes, application);
    });
    recordNotedWrites();
}

static Vector<ValueFlowWriteUnderNewName> computeBuiltinLocations(const ValueFlowResultInputs& inputs, BuiltinHolders& holders, ValueFlowHeapView& heap, FlowBuiltinTables& tables)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();

    tables.constantMembers = BitVector(builtins.builtinCount());
    tables.intactHolders = BitVector(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID member(index);
        if (inputs.build.isEnabled(FlowResultKind::ConstantBuiltinLocations, index) && holders.isIntact(builtins.canonicalID(member)))
            tables.intactHolders.quickSet(index);

        const Builtin& entry = builtins.at(member);
        if (!entry.holder || !entry.name)
            continue;
        BuiltinLocationState state = stateOfLocation(inputs, holders, member);
        // Whether a name that the realm may lack is there is known when the program runs: a read looks it up, so `typeof`, `in` and a test for truth answer for the realm of the run.
        // Its record says Constant all the same: start-up locks the name, there or not, so what the first lookup finds holds for the whole run, whatever a script declares or stores behind the analysis.
        if (state == BuiltinLocationState::Constant && !builtins.has(BuiltinMark::MayBeAbsent, member))
            tables.constantMembers.quickSet(index);
        if (!inputs.solver().usesOf(member).isEmpty())
            tables.locations.append(BuiltinLocationFlow { entry.holder, program.keyOf(entry.name), state, noWitness });
    }

    // What the program puts on a holder under a name of its own. Whoever asks whether the holder gains the name finds the record.
    for (const NameOnHolder& name : holders.namesWritten()) {
        if (!findOwnMember(program, name.holder, name.key) && !isLockedLocation(program, name.holder, name.key))
            tables.locations.append(BuiltinLocationFlow { name.holder, name.key, BuiltinLocationState::Unknown, noWitness });
    }
    WritesOfLockedLocations writes(inputs, holders, heap);
    writes.collect();
    tables.locations.appendVector(WTF::move(writes.records));
    sortAndMergeLocations(tables.locations);
    return WTF::move(writes.underNewNames);
}

void recordLockedWritesUnderNewNames(const Program& program, TypeTable& types, std::span<const ValueFlowWriteUnderNewName> writes, FlowResultTables& tables)
{
    if (writes.empty())
        return;
    for (const ValueFlowWriteUnderNewName& write : writes) {
        PropertyKeyID key = types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = write.text });
        if (isLockedLocation(program, write.holder, key))
            tables.builtins.locations.append(BuiltinLocationFlow { write.holder, key, BuiltinLocationState::Constant, write.witness });
    }
    sortAndMergeLocations(tables.builtins.locations);
}

// The instances of one VM are counted: no object of another VM gets here, so a worker, which runs the program once more in a VM of its own, is none of them.
static ProgramInstances countProgramInstances(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    if (!inputs.build.isEnabledForProgram(FlowResultKind::ReachedFunctions))
        return ProgramInstances::Unknown;
    bool unseenCodeFindsEveryBuiltin = unseenCodeHasLoader(inputs.solution);
    // Two names of one built-in share the uses of neither.
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        if (!builtins.at(builtin).flags.contains(BuiltinFlag::StartsInstance) && !builtins.at(builtins.canonicalID(builtin)).flags.contains(BuiltinFlag::StartsInstance))
            continue;
        if (unseenCodeFindsEveryBuiltin)
            return ProgramInstances::Unknown;
        if (inputs.solver().usesOf(builtin).containsAny(usesEnteringCode(builtins.at(builtin).kind)))
            return ProgramInstances::Unknown;
        // Whoever was handed the function calls it without a use that anybody records.
        ValueFlowObjectID object = inputs.universe().objectOf(builtin);
        if (object && (inputs.hasEscaped(object) || inputs.isLost(object)))
            return ProgramInstances::Unknown;
    }
    return ProgramInstances::AtMostOne;
}

// RegExp.$1 and its like change with every match. A read under a key nobody knows is a use of every member of the holder.
static LegacyRegExpStatics findReadsOfLegacyRegExpStatics(const ValueFlowResultInputs& inputs, const BuiltinHolders& holders)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BuiltinID constructor = builtins.canonicalID(builtins.at(BuiltinClasses::RegExp).constructor);
    if (!inputs.build.isEnabled(FlowResultKind::ObjectUses, constructor.index()) || holders.unseenCodeRuns() || holders.hasEscaped(constructor) || holders.isLost(constructor))
        return LegacyRegExpStatics::Unknown;

    bool isMentioned = std::ranges::any_of(builtins.membersOf(constructor), [&](BuiltinID member) {
        if (builtins.at(member).mutability != BuiltinMutability::ChangesOnItsOwn)
            return false;
        return !inputs.solver().usesOf(member).isEmpty() || !inputs.solver().usesOf(builtins.canonicalID(member)).isEmpty();
    });
    return isMentioned ? LegacyRegExpStatics::Unknown : LegacyRegExpStatics::NeverRead;
}

// Every overload of the mode counts: which one a call selects depends on its operands.
static void forEachPartAllocatedAsResult(const BuiltinTable& builtins, BuiltinID builtin, BuiltinCallMode mode, NOESCAPE const Invocable<void(const BuiltinTypePart&)> auto& functor)
{
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
        if (overload.mode != mode || !overload.result.type)
            continue;
        for (const BuiltinTypePart& part : builtins.at(overload.result.type).parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Instance:
            case BuiltinTypePartKind::InstanceOfReceiverClass:
            case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            case BuiltinTypePartKind::Layout:
                functor(part);
                break;
            default:
                break;
            }
        }
    }
}

// One object stands for all that the built-in functions among the callees of a call operation allocate as its result. A group of callees and a placeholder apply a built-in with objects of the built-in.
ValueFlowClassesOfBuiltinResults computeClassesOfBuiltinResults(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    // All threads look at the calls. What is noted of an object comes out the same in any order: one class, or none once two differ.
    using Noted = std::pair<ValueFlowObjectID, BuiltinClassID>;
    auto noteCall = [&](const ValueFlowStatement& call, ValueSetMaker& maker, Vector<Noted>& noted) {
        auto note = [&](ValueFlowObjectID object, BuiltinClassID builtinClass) {
            noted.append({ object, builtinClass });
        };
        std::optional<BuiltinCallMode> mode = call.kind == ValueFlowStatementKind::Operation ? modeOfCallOperation(call.opcode) : std::nullopt;
        ValueFlowObjectID object = mode ? universe.siteObject(call) : ValueFlowObjectID();
        if (!object || universe.layoutOf(object))
            return;
        ValueSetID callees = inputs.valueOf(maker, inputs.bodies().operandsOf(call)[0]);
        if (ValueSets::isComplement(callees) || inputs.sets().meet(callees, universe.otherCallables())) {
            note(object, { });
            return;
        }
        inputs.sets().forEachMember(maker.intersect(callees, universe.builtinFunctions()), [&](ValueFlowObjectID callee) {
            forEachPartAllocatedAsResult(builtins, universe.builtinOf(callee), *mode, [&](const BuiltinTypePart& part) {
                note(object, part.kind == BuiltinTypePartKind::Instance ? part.builtinClass : BuiltinClassID());
            });
        });
        // A second object stands for what the callees make to put below the result. The pairs of Object.entries are arrays.
        ValueSetID builtinCallees = maker.intersect(callees, universe.builtinFunctions());
        if (builtinCallees != ValueSets::nothing && inputs.sets().isSubset(builtinCallees, maker.of(universe.objectOf(builtins.canonicalID(Builtins::ObjectEntries)))))
            note(universe.siteObject(call, 1), BuiltinClasses::Array);
    };

    constexpr size_t functionsPerShare = 256;
    size_t numberOfFunctions = inputs.program.functions.size();
    Vector<Vector<Noted>> notedOfShare((numberOfFunctions + functionsPerShare - 1) / functionsPerShare);
    ValueSetMakers makers(inputs);
    forEachIndexInParallel(inputs.loops(), notedOfShare.size(), [&](unsigned share, unsigned worker) {
        for (size_t index = share * functionsPerShare; index < std::min(numberOfFunctions, (share + 1) * functionsPerShare); ++index) {
            if (!inputs.isReached(FunctionID(index)))
                continue;
            for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index))
                noteCall(statement, makers.of(worker), notedOfShare[share]);
        }
    });
    ValueFlowClassesOfBuiltinResults classes;
    for (const Vector<Noted>& noted : notedOfShare) {
        for (auto [object, builtinClass] : noted) {
            auto result = classes.add(object.index(), builtinClass);
            if (result.iterator->value != builtinClass)
                result.iterator->value = BuiltinClassID();
        }
    }
    return classes;
}

// A report says where. A place in the library says nothing to whoever reads it, so the statement of the program that leads there stands for it.
static void recordPlacesOfUnseenCode(const ValueFlowResultInputs& inputs, const ValueFlowPlacesInProgram& places, Vector<UnseenCodeRecord>& records)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    auto kindOf = [](const UnseenCodeRecord& record) {
        return std::pair(record.cause, record.builtin);
    };
    auto isInProgram = [&](const UnseenCodeRecord& record) {
        return hasFunction(record.witness) && places.wayTo(record.witness) == record.witness;
    };
    Vector<std::pair<UnseenCodeCause, uint32_t>> kindsInProgram;
    for (const UnseenCodeRecord& record : records) {
        if (isInProgram(record))
            kindsInProgram.appendIfNotContains(kindOf(record));
    }
    records.removeAllMatching([&](const UnseenCodeRecord& record) {
        return !isInProgram(record) && kindsInProgram.contains(kindOf(record));
    });
    for (UnseenCodeRecord& record : records) {
        if (Witness way = places.wayTo(record.witness); hasFunction(way))
            record.witness = way;
        else if (!hasFunction(record.witness) && record.builtin != Wire::none)
            record.witness = places.whereNamed(builtins.canonicalID(BuiltinID(record.builtin)));
    }
    // Text that is evaluated imports by name. No statement of the program does, so the evaluator is the place.
    auto evaluator = std::ranges::find_if(records, admitsUnknownJavaScript, &UnseenCodeRecord::cause);
    for (UnseenCodeRecord& record : records) {
        if (evaluator != records.end() && record.cause == UnseenCodeCause::LoadsModuleByName && record.builtin == Wire::none && !hasFunction(record.witness)) {
            record.builtin = evaluator->builtin;
            record.witness = evaluator->witness;
        }
    }
    auto orderOf = [](const UnseenCodeRecord& record) {
        return std::tuple(record.cause, record.builtin, record.witness.function, record.witness.value);
    };
    AOT::sort(records, { }, orderOf);
    records.shrink(std::ranges::unique(records, { }, orderOf).begin() - records.begin());
}

static void recordPlaces(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BitVector builtinsAskedAbout(builtins.builtinCount());
    auto ask = [&](uint32_t builtin) {
        if (builtin != Wire::none)
            builtinsAskedAbout.quickSet(builtins.canonicalID(BuiltinID(builtin)).index());
    };
    for (const BuiltinUseRecord& use : tables.builtins.uses)
        ask(use.builtin);
    for (const UnseenCodeRecord& record : tables.wholeProgram.unseenCode)
        ask(record.builtin);
    for (const UnseenCodeRecord& record : tables.wholeProgram.loadsOfMachineCode)
        ask(record.builtin);

    ValueFlowPlacesInProgram places(inputs, builtinsAskedAbout);
    auto whereNamed = [&](uint32_t builtin) {
        return builtin == Wire::none ? noWitness : places.whereNamed(builtins.canonicalID(BuiltinID(builtin)));
    };
    for (BuiltinUseRecord& use : tables.builtins.uses)
        use.witness = whereNamed(use.builtin);
    for (FeatureUseRecord& use : tables.builtins.featureUses)
        use.witness = whereNamed(use.builtin);
    // The first record of a feature speaks for it: one with a place in the program, the first in the program.
    AOT::sort(tables.builtins.featureUses, { }, [](const FeatureUseRecord& use) {
        return std::tuple(use.feature, use.witness.location.module, use.witness.location.position, use.builtin);
    });
    for (FlowLocaleUse& use : tables.builtins.localeUses) {
        use.witness = places.wayTo(use.witness);
        if (!hasFunction(use.witness) && use.builtin)
            use.witness = whereNamed(use.builtin.index());
    }
    recordPlacesOfUnseenCode(inputs, places, tables.wholeProgram.unseenCode);
    recordPlacesOfUnseenCode(inputs, places, tables.wholeProgram.loadsOfMachineCode);
}

Vector<ValueFlowWriteUnderNewName> computeBuiltinResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    ValueFlowHeapView heap(inputs);
    BuiltinHolders holders(inputs, heap.index());
    times.didFinish("built-ins: holders"_s);
    Vector<ValueFlowWriteUnderNewName> writesUnderNewNames = computeBuiltinLocations(inputs, holders, heap, tables.builtins);
    times.didFinish("built-ins: locations"_s);
    BitVector builtinsWhoseCodeRuns = findBuiltinsWhoseCodeRuns(inputs);
    tables.wholeProgram.unseenCode = collectUnseenCode(inputs, tables.wholeProgram.loadsOfMachineCode);
    warnAboutLoadsUnderComputedNames(inputs.build, tables.wholeProgram.unseenCode.span());
    tables.wholeProgram.instances = countProgramInstances(inputs);
    times.didFinish("built-ins: code that runs, unseen code, instances"_s);
    tables.builtins.uses = collectBuiltinUses(inputs);
    times.didFinish("built-ins: uses"_s);
    tables.builtins.legacyRegExpStatics = findReadsOfLegacyRegExpStatics(inputs, holders);
    tables.builtins.featureUses = collectFeatureUses(inputs, holders, builtinsWhoseCodeRuns);
    tables.builtins.localeUses = collectLocaleUses(inputs, heap, builtinsWhoseCodeRuns, tables.wholeProgram.unseenCode.span());
    times.didFinish("built-ins: statics, features, locales"_s);
    if (hasFlag(inputs.build.options().reports, ReportSet::Flow)) {
        recordPlaces(inputs, tables);
        times.didFinish("built-ins: places for the report"_s);
    }
    return writesUnderNewNames;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
