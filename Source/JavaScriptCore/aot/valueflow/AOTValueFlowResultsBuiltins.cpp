/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowUnseenCode.h"
#include "AOTKnownClosures.h"
#include "AOTPhaseTimes.h"
#include "AOTPrimitiveKeyTexts.h"
#include "AOTSchemaOperandPositions.h"
#include "AOTSort.h"
#include "AOTValueFlowResultsPlaces.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<BuiltinUse> callingBuiltinUses { BuiltinUse::Called, BuiltinUse::Constructed };
static constexpr OptionSet<BuiltinUse> accessingBuiltinUses { BuiltinUse::Read, BuiltinUse::Written };

static ValueSetID valueOfHostNode(const ValueFlowResultInputs& inputs, ValueFlowHostNode node)
{
    return inputs.solver().valueOf(inputs.bodies().nodes().ofHost(node));
}

namespace {

enum class BuiltinWriteForm : uint8_t {
    UnderName,
    UnderKeyNobodyKnows,
    OfPrototypeLink,
    OfSlot,
    Other,
};

} // anonymous namespace

static BuiltinWriteForm formOfWrite(ValueFlowNameKind kind)
{
    ValueFlowNameShape shape = shapeOf(kind);
    if (shape.reach == ValueFlowNameReach::OneKey)
        return BuiltinWriteForm::UnderName;
    if (shape.reach == ValueFlowNameReach::Elements || shape.reach == ValueFlowNameReach::UnknownKey || shape.sort == ValueFlowNameSort::Copy)
        return BuiltinWriteForm::UnderKeyNobodyKnows;
    if (shape.sort == ValueFlowNameSort::PrototypeLink)
        return BuiltinWriteForm::OfPrototypeLink;
    return kind == ValueFlowNameKind::Slot ? BuiltinWriteForm::OfSlot : BuiltinWriteForm::Other;
}

namespace {

class BuiltinHolders {
    WTF_MAKE_NONCOPYABLE(BuiltinHolders);
public:
    BuiltinHolders(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap)
        : m_inputs(inputs)
        , m_builtins(inputs.program.builtins())
        , m_unseenCodeRuns(!inputs.sets().isEmpty(valueOfHostNode(inputs, ValueFlowHostNode::UnseenCodeChangesRealm)))
        , m_unknownCodeWrites(!inputs.sets().isEmpty(valueOfHostNode(inputs, ValueFlowHostNode::UnknownCodeWrites)) || unseenCodeCanRun(inputs.solution))
        , m_unknownObject(m_builtins.builtinCount())
        , m_untrackedObject(m_builtins.builtinCount() + 1)
        , m_keysWritten(m_builtins.builtinCount() + 2)
    {
        auto noteHoldersIn = [&](ValueSetID set, BitVector& holders) {
            for (BuiltinID holder : holdersIn(heap.maker(), set))
                holders.set(holder.index());
        };
        noteHoldersIn(inputs.solver().valueOf(ValueFlowProgramNodes::escaped()), m_escaped);
        noteHoldersIn(inputs.solver().valueOf(ValueFlowProgramNodes::lost()), m_lost);
        noteWrites(heap.index());
        if (!m_unseenCodeRuns && isWrittenBehindAnalysis())
            noteHoldersIn(valueOfHostNode(inputs, ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), m_mayBeHeldWithoutBeingListed);
    }

    bool unseenCodeRuns() const { return m_unseenCodeRuns; }
    bool hasEscaped(BuiltinID holder) const { return m_escaped.get(holder.index()); }
    bool isLost(BuiltinID holder) const { return m_lost.get(holder.index()); }
    bool operandMayHaveProperty(BuiltinID builtin, BuiltinName property) const { return m_propertiesOfOperands.contains(static_cast<uint64_t>(builtin.index()) << 32 | property.index()); }

    bool isWrittenByProgram(BuiltinID holder, PropertyKeyID key) const
    {
        if (m_underKeysNobodyKnows.get(holder.index()) || (m_underIndexes.get(holder.index()) && m_inputs.program.isIndexLike(key)) || m_keysWritten[holder.index()].get(key.index()))
            return true;
        return (isLost(holder) && mayWriteThrough(m_untrackedObject, key)) || (m_mayBeHeldWithoutBeingListed.get(holder.index()) && mayWriteThrough(m_unknownObject, key));
    }

    bool mayBeWrittenBehindAnalysis(BuiltinID holder, PropertyKeyID key) const
    {
        return m_unseenCodeRuns || (hasEscaped(holder) && (m_unknownCodeWrites || mayWriteThrough(m_unknownObject, key)));
    }

    bool isIntact(BuiltinID holder) const
    {
        if (m_builtins.at(holder).flags.contains(BuiltinFlag::IsLockedObject))
            return true;
        if (m_unseenCodeRuns || m_underKeysNobodyKnows.get(holder.index()) || m_underIndexes.get(holder.index()) || m_ofPrototypeLinks.get(holder.index()))
            return false;
        return !(isLost(holder) && writesThrough(m_untrackedObject)) && !(hasEscaped(holder) && isWrittenBehindAnalysis());
    }

    bool mayWriteLockedLocationUnderKeyNobodyKnows() const
    {
        BitVector written = m_underKeysNobodyKnows;
        written.merge(m_underIndexes);
        for (size_t index : written) {
            BuiltinID holder(static_cast<uint32_t>(index));
            if (index >= m_unknownObject)
                break;
            if (m_builtins.at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::HasLockedMembers, BuiltinFlag::IsClosedToOtherNames }) || !m_builtins.closedKeysOf(holder).empty()
                || std::ranges::any_of(m_builtins.membersOf(holder), [&](BuiltinID member) { return isLockedLocation(m_builtins, member); }))
                return true;
        }
        return false;
    }

    void forEachNameWritten(NOESCAPE const Invocable<void(BuiltinID, PropertyKeyID)> auto& functor) const
    {
        for (uint32_t holder = 0; holder < m_unknownObject; ++holder) {
            for (size_t key : m_keysWritten[holder])
                functor(BuiltinID(holder), PropertyKeyID(static_cast<uint32_t>(key)));
        }
    }

    Vector<BuiltinID> holdersIn(ValueSetMaker& maker, ValueSetID set) const
    {
        Vector<BuiltinID> found;
        m_inputs.sets().forEachMember(maker.intersect(set, m_inputs.universe().builtinObjects()), [&](ValueFlowObjectID object) {
            found.append(m_builtins.canonicalID(m_inputs.universe().builtinOf(object)));
        });
        AOT::sort(found);
        removeRepeatedElements(found);
        return found;
    }

private:
    struct Notes {
        struct ThroughSet {
            Vector<uint32_t> receivers;
            Vector<uint32_t> keys;
        };
        HashMap<uint32_t, ThroughSet, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> throughSet;
        BitVector underKeysNobodyKnows;
        BitVector underIndexes;
        BitVector ofPrototypeLinks;
        Vector<uint64_t> propertiesOfOperands;
    };

    bool writesUnderKeysNobodyKnowsThrough(uint32_t placeholder) const { return m_underKeysNobodyKnows.get(placeholder) || m_underIndexes.get(placeholder); }
    bool mayWriteThrough(uint32_t placeholder, PropertyKeyID key) const { return writesUnderKeysNobodyKnowsThrough(placeholder) || m_keysWritten[placeholder].get(key.index()); }
    bool writesThrough(uint32_t placeholder) const { return writesUnderKeysNobodyKnowsThrough(placeholder) || m_ofPrototypeLinks.get(placeholder) || !m_keysWritten[placeholder].isEmpty(); }
    bool isWrittenBehindAnalysis() const { return m_unknownCodeWrites || writesThrough(m_unknownObject); }

    void noteWrite(Notes& notes, ValueSetMaker& maker, ValueSetID receivers, ValueFlowName name) const
    {
        BuiltinWriteForm form = formOfWrite(name.kind);
        if (form == BuiltinWriteForm::Other || (form == BuiltinWriteForm::OfSlot && name.payload < m_builtins.slotCount()))
            return;
        Notes::ThroughSet& written = notes.throughSet.ensure(receivers.index(), [&] {
            Vector<uint32_t> found = WTF::map(holdersIn(maker, receivers), [](BuiltinID holder) { return holder.index(); });
            if (m_inputs.sets().contains(receivers, ValueFlowUniverse::unknownObject()))
                found.append(m_unknownObject);
            if (m_inputs.sets().contains(receivers, ValueFlowUniverse::untrackedObject()))
                found.append(m_untrackedObject);
            return Notes::ThroughSet { WTF::move(found), { } };
        }).iterator->value;
        if (form == BuiltinWriteForm::UnderName) {
            if (!written.receivers.isEmpty())
                written.keys.append(name.payload);
            return;
        }
        for (uint32_t receiver : written.receivers) {
            if (form == BuiltinWriteForm::OfPrototypeLink)
                notes.ofPrototypeLinks.set(receiver);
            else if (form == BuiltinWriteForm::UnderKeyNobodyKnows)
                (name.kind == ValueFlowNameKind::Elements ? notes.underIndexes : notes.underKeysNobodyKnows).set(receiver);
            else if (receiver < m_unknownObject)
                notes.propertiesOfOperands.append(static_cast<uint64_t>(receiver) << 32 | (name.payload - m_builtins.slotCount()));
        }
    }

    void noteWrites(const ValueFlowHeapIndex& index)
    {
        Vector<ValueFlowHeapIndex::Stretch> stretches = index.stretchesOfNodesWrittenUnder([](ValueFlowName name) {
            return formOfWrite(name.kind) != BuiltinWriteForm::Other;
        });
        ValueSetMakers makers(m_inputs);
        Vector<Notes> notesOfWorker(m_inputs.loops().width());
        forEachIndexInParallel(m_inputs.loops(), stretches.size(), [&](unsigned position, unsigned worker) {
            for (const ValueFlowHeapIndex::WrittenNode& node : stretches[position].nodes)
                noteWrite(notesOfWorker[worker], makers.of(worker), node.objects, stretches[position].name);
        });
        for (const Notes& notes : notesOfWorker) {
            m_underKeysNobodyKnows.merge(notes.underKeysNobodyKnows);
            m_underIndexes.merge(notes.underIndexes);
            m_ofPrototypeLinks.merge(notes.ofPrototypeLinks);
            for (const Notes::ThroughSet& written : notes.throughSet.values()) {
                for (uint32_t receiver : written.receivers) {
                    for (uint32_t key : written.keys)
                        m_keysWritten[receiver].set(key);
                }
            }
            m_propertiesOfOperands.addAll(notes.propertiesOfOperands);
        }
    }

    const ValueFlowResultInputs& m_inputs;
    const BuiltinTable& m_builtins;
    const bool m_unseenCodeRuns;
    const bool m_unknownCodeWrites;
    const uint32_t m_unknownObject;
    const uint32_t m_untrackedObject;
    BitVector m_escaped;
    BitVector m_lost;
    BitVector m_mayBeHeldWithoutBeingListed;
    BitVector m_underKeysNobodyKnows;
    BitVector m_underIndexes;
    BitVector m_ofPrototypeLinks;
    Vector<BitVector> m_keysWritten;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_propertiesOfOperands;
};

} // anonymous namespace

static OptionSet<BuiltinUse> usesEnteringCode(BuiltinKind kind)
{
    switch (kind) {
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::NativeBinding:
        return callingBuiltinUses | accessingBuiltinUses;
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
    case BuiltinKind::Constant:
        return callingBuiltinUses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findBuiltinsWhoseCodeRuns(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    BitVector namespacesOfModules;
    for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
        if (BuiltinID namespaceObject = builtins.at(RuntimeModuleID(index)).namespaceObject)
            namespacesOfModules.set(namespaceObject.index());
    }
    BitVector builtinsWhoseCodeRuns(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        OptionSet<BuiltinUse> enteringUses = usesEnteringCode(entry.kind);
        bool mayHoldFunctionsWithoutEntry = (entry.kind == BuiltinKind::Function || entry.kind == BuiltinKind::Method) && !entry.flags.contains(BuiltinFlag::ListsAllMembers);
        if (namespacesOfModules.get(index) || mayHoldFunctionsWithoutEntry)
            enteringUses.add(accessingBuiltinUses);
        if (inputs.solver().usesOf(BuiltinID(index)).containsAny(enteringUses))
            builtinsWhoseCodeRuns.quickSet(builtins.canonicalID(BuiltinID(index)).index());
    }

    constexpr uint32_t objectsPerTask = 4096;
    uint32_t numberOfObjects = universe.numberOfObjects();
    Vector<BitVector> allocatedClassesOfWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), (numberOfObjects + objectsPerTask - 1) / objectsPerTask, [&](unsigned task, unsigned worker) {
        for (uint32_t index = task * objectsPerTask; index < std::min(numberOfObjects, (task + 1) * objectsPerTask); ++index) {
            ValueFlowObjectID object(index);
            switch (universe.kindOf(object)) {
            case ValueFlowObjectKind::CoroutineResult:
            case ValueFlowObjectKind::RestArray:
            case ValueFlowObjectKind::Site:
                break;
            case ValueFlowObjectKind::Primitive:
            case ValueFlowObjectKind::UnknownObject:
            case ValueFlowObjectKind::Builtin:
            case ValueFlowObjectKind::ModuleBuiltin:
            case ValueFlowObjectKind::Namespace:
            case ValueFlowObjectKind::Closure:
            case ValueFlowObjectKind::FunctionPrototype:
            case ValueFlowObjectKind::ConstructedObject:
            case ValueFlowObjectKind::Instance:
            case ValueFlowObjectKind::Prototype:
            case ValueFlowObjectKind::ResultOfBuiltin:
            case ValueFlowObjectKind::Key:
                continue;
            }
            BuiltinClassID builtinClass = universe.classOf(object);
            if (builtinClass && inputs.isReached(universe.functionOf(object)))
                allocatedClassesOfWorker[worker].set(builtinClass.index());
        }
    });
    BitVector allocatedClasses;
    for (const BitVector& classes : allocatedClassesOfWorker)
        allocatedClasses.merge(classes);
    for (size_t index : allocatedClasses) {
        const BuiltinClass& builtinClass = builtins.at(BuiltinClassID(index));
        if (BuiltinID entry = builtinClass.prototype ? builtinClass.prototype : builtinClass.constructor)
            builtinsWhoseCodeRuns.quickSet(builtins.canonicalID(entry).index());
    }
    return builtinsWhoseCodeRuns;
}

static bool mayHold(const BuiltinTable& builtins, const BuiltinHolders& holders, BuiltinID builtin, const BuiltinCondition* condition)
{
    if (!condition)
        return true;
    if (condition->buildAdmitsRuntimeCode)
        return false;
    const BuiltinUnseenCode* unseenCode = builtins.unseenCodeOf(builtin);
    bool runsUnseenCodeForProperty = condition->hasProperty && unseenCode && unseenCode->condition && unseenCode->condition->hasProperty == condition->hasProperty;
    if (holders.hasEscaped(builtin) && !condition->calledOn && !runsUnseenCodeForProperty)
        return true;
    if (condition->firstText() && isObjectKind(builtins.at(builtin).kind) && !holders.operandMayHaveProperty(builtin, condition->firstText()))
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
        std::ranges::for_each(callback.arguments, visit);
    }
    for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
        visit(action.value);
    for (const BuiltinHandlerCall& call : overload.flow.handlerCalls) {
        visit(call.thisValue);
        std::ranges::for_each(call.arguments, visit);
    }
}

static BuiltinID outermostHolderWithFeature(const BuiltinTable& builtins, BuiltinID builtin, uint32_t feature)
{
    BuiltinID outermost = builtin;
    for (BuiltinID holder = builtins.at(builtin).holder; holder; holder = builtins.at(holder).holder) {
        holder = builtins.canonicalID(holder);
        bool hasFeature = std::ranges::any_of(builtins.featureUsesOf(holder), [&](const BuiltinFeatureUse& line) {
            return static_cast<uint32_t>(line.feature) == feature;
        });
        if (hasFeature)
            outermost = holder;
    }
    return outermost;
}

static void markUnnamedFeatureUses(const ValueFlowResultInputs& inputs, Vector<FeatureUseRecord>& uses)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    auto candidateOf = [&](const FeatureUseRecord& use) {
        if (builtins.at(BuiltinID(use.builtin)).origin == BuiltinOrigin::Host)
            return BuiltinID();
        return outermostHolderWithFeature(builtins, BuiltinID(use.builtin), use.feature);
    };
    BitVector candidates(builtins.builtinCount());
    Vector<ValueFlowNamedBuiltins::Member> members;
    for (const FeatureUseRecord& use : uses) {
        if (BuiltinID candidate = candidateOf(use)) {
            candidates.quickSet(candidate.index());
            members.append({ BuiltinID(use.builtin), candidate });
        }
    }
    ValueFlowNamedBuiltins named(inputs, candidates, members.span());
    for (FeatureUseRecord& use : uses) {
        if (BuiltinID candidate = candidateOf(use); candidate && !named.contains(candidate))
            use.kind = FeatureUseKind::Unnamed;
    }
}

static Vector<FeatureUseRecord> collectFeatureUses(const ValueFlowResultInputs& inputs, const BuiltinHolders& holders, const BitVector& builtinsWhoseCodeRuns)
{
    constexpr uint32_t noClass = UINT32_MAX;
    const BuiltinTable& builtins = inputs.program.builtins();
    ValueSetID namedByStatements = valueOfHostNode(inputs, ValueFlowHostNode::BuiltinsThatStatementsName);
    auto isNamedByStatement = [&](BuiltinID builtin) {
        return inputs.sets().contains(namedByStatements, inputs.universe().objectOf(builtin));
    };
    auto canonical = [&](BuiltinID builtin) {
        return builtins.canonicalID(builtin).index();
    };

    BitVector makersNameFeatures(builtins.classCount());
    Vector<uint32_t> classOfBuiltin;
    classOfBuiltin.fill(noClass, builtins.builtinCount());
    Vector<uint32_t> hostClassOfPrototype;
    hostClassOfPrototype.fill(noClass, builtins.builtinCount());
    BitVector hangsOutsidePrototypeObjects(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID holder = builtins.at(BuiltinID(index)).holder;
        if (holder && builtins.at(holder).kind != BuiltinKind::Prototype)
            hangsOutsidePrototypeObjects.quickSet(canonical(BuiltinID(index)));
    }
    for (uint32_t index = 0; index < builtins.classCount(); ++index) {
        const BuiltinClass& builtinClass = builtins.at(BuiltinClassID(index));
        if (builtinClass.origin != BuiltinOrigin::Host || !builtinClass.prototype)
            continue;
        hostClassOfPrototype[canonical(builtinClass.prototype)] = index;
        if (builtinClass.instancesAreEntries)
            continue;
        bool canBeNamed = builtinClass.constructor && hangsOutsidePrototypeObjects.quickGet(canonical(builtinClass.constructor));
        if (canBeNamed && (isNamedByStatement(builtinClass.constructor) || isNamedByStatement(builtinClass.prototype)))
            continue;
        makersNameFeatures.quickSet(index);
        classOfBuiltin[canonical(builtinClass.prototype)] = index;
        if (builtinClass.constructor && !canBeNamed)
            classOfBuiltin[canonical(builtinClass.constructor)] = index;
    }
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID holder = builtins.at(BuiltinID(index)).holder;
        if (holder && classOfBuiltin[canonical(BuiltinID(index))] == noClass)
            classOfBuiltin[canonical(BuiltinID(index))] = classOfBuiltin[canonical(holder)];
    }
    auto needsWithoutCondition = [&](BuiltinID builtin, RemovableFeature feature) {
        return std::ranges::any_of(builtins.featureUsesOf(builtin), [&](const BuiltinFeatureUse& line) { return line.feature == feature && !line.condition; });
    };
    auto prototypeOf = [&](uint32_t builtinClass) {
        return builtins.canonicalID(builtins.at(BuiltinClassID(builtinClass)).prototype);
    };
    auto checkMakerOf = [&](uint32_t made, BuiltinID maker) {
        uint32_t owner = classOfBuiltin[maker.index()];
        if (!makersNameFeatures.quickGet(made) || owner == made)
            return;
        bool namesFeatures = std::ranges::all_of(builtins.featureUsesOf(prototypeOf(made)), [&](const BuiltinFeatureUse& line) {
            return line.condition || needsWithoutCondition(maker, line.feature) || (owner != noClass && needsWithoutCondition(prototypeOf(owner), line.feature));
        });
        makersNameFeatures.quickSet(made, namesFeatures);
    };
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID maker = builtins.canonicalID(BuiltinID(index));
        Vector<BuiltinTypeID, 8> types;
        for (const BuiltinOverload& overload : builtins.overloadsOf(BuiltinID(index)))
            forEachTypeOfValueThatOverloadMakes(overload, [&](BuiltinTypeID type) { types.appendIfNotContains(type); });
        for (size_t position = 0; position < types.size(); ++position) {
            for (const BuiltinTypePart& part : builtins.at(types[position]).parts) {
                for (BuiltinClassID current = part.builtinClass; current; current = builtins.at(current).parent)
                    checkMakerOf(current.index(), maker);
                for (uint32_t other = 0; part.builtinClass && !part.isOfClassItself && other < builtins.classCount(); ++other) {
                    for (BuiltinClassID current = builtins.at(BuiltinClassID(other)).parent; current; current = builtins.at(current).parent) {
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

    auto entryWithFeaturesOfClassOf = [&](const Builtin& member) -> BuiltinID {
        if (!member.holder || (member.kind != BuiltinKind::Method && member.kind != BuiltinKind::Accessor))
            return { };
        uint32_t owner = hostClassOfPrototype[canonical(member.holder)];
        if (owner == noClass)
            return { };
        BuiltinID constructor = builtins.at(BuiltinClassID(owner)).constructor;
        if (!builtins.featureUsesOf(prototypeOf(owner)).empty() || !constructor)
            return prototypeOf(owner);
        return builtins.canonicalID(constructor);
    };

    bool refusesEvaluatedText = inputs.solver().input().refusedCode.evaluatedText;
    Vector<FeatureUseRecord> uses;
    for (size_t index : builtinsWhoseCodeRuns) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        if (refusesEvaluatedText && entry.flags.contains(BuiltinFlag::EvaluatesText))
            continue;
        uint32_t owner = classOfBuiltin[index];
        auto addUnlessRootedByMakersOfClass = [&](RemovableFeature feature) {
            if (owner == noClass || !makersNameFeatures.quickGet(owner) || !needsWithoutCondition(prototypeOf(owner), feature))
                uses.append(FeatureUseRecord { static_cast<uint32_t>(feature), static_cast<uint32_t>(index), noWitness });
        };
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(BuiltinID(index))) {
            if (mayHold(builtins, holders, BuiltinID(index), line.condition))
                addUnlessRootedByMakersOfClass(line.feature);
        }
        BuiltinID entryOfClass = entryWithFeaturesOfClassOf(entry);
        if (!entryOfClass || !isNamedByStatement(builtins.canonicalID(entry.holder)))
            continue;
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(entryOfClass)) {
            if (!line.condition)
                addUnlessRootedByMakersOfClass(line.feature);
        }
    }
    auto orderOf = [](const FeatureUseRecord& use) {
        return std::tuple(use.feature, use.builtin);
    };
    AOT::sort(uses, { }, orderOf);
    uses.shrink(std::ranges::unique(uses, { }, orderOf).begin() - uses.begin());
    markUnnamedFeatureUses(inputs, uses);
    return uses;
}

static void sortAndRemoveRepeatedRecords(Vector<UnseenCodeRecord>& records)
{
    auto orderOf = [](const UnseenCodeRecord& record) {
        return std::tuple(record.cause, record.builtin, record.witness.function, record.witness.value);
    };
    AOT::sort(records, { }, orderOf);
    records.shrink(std::ranges::unique(records, { }, orderOf).begin() - records.begin());
}

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
    if (filesOfProgramAreLoadedByName(inputs.solution) && !inputs.solver().input().refusedCode.codeOutsideImage && !std::ranges::any_of(records, loadsCodeOutsideImage, &UnseenCodeRecord::cause))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::LoadsCodeByName });
    if (modulesAreNamedAtRunTime && !std::ranges::any_of(records, loadsModulesByName, &UnseenCodeRecord::cause))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::LoadsModuleByName });
    if (!inputs.build.isEnabledForProgram(FlowResultKind::ReachedFunctions))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::FlowResultDisabled });
    sortAndRemoveRepeatedRecords(records);
    return records;
}

static bool isCallOfProgramUnderComputedName(const Program& program, const Witness& witness)
{
    const Function& function = program.at(FunctionID(witness.function));
    const Value& call = function.at(ValueID(witness.value));
    constexpr unsigned positionOfName = 2;
    if (!isWrittenInProgram(function) || call.opcode != Opcode::Call || call.numberOfOperands() <= positionOfName || !call.operand(positionOfName))
        return false;
    return function.at(definitionThroughSameValues(function, call.operand(positionOfName))).opcode != Opcode::String;
}

static void warnAboutLoadsUnderComputedNames(const Program& program, BuildContext& build, std::span<const UnseenCodeRecord> records)
{
    Vector<SourceLocation> reportedLocations;
    for (const UnseenCodeRecord& record : records) {
        if (record.cause != UnseenCodeCause::LoadsModuleByName || record.witness.value == Wire::none)
            continue;
        bool isCallOfLoader = record.builtin != Wire::none;
        if (isCallOfLoader && !isCallOfProgramUnderComputedName(program, record.witness))
            continue;
        if (hasModule(record.witness.location)) {
            if (reportedLocations.contains(record.witness.location))
                continue;
            reportedLocations.append(record.witness.location);
        }
        build.diagnostics().add(UserDiagnostic {
            .kind = isCallOfLoader ? UserDiagnosticKind::ModuleNamedAtRunTimeIsRefused : UserDiagnosticKind::CodeOutsideImageIsRefused,
            .function = FunctionID(record.witness.function),
            .value = ValueID(record.witness.value),
            .location = record.witness.location,
        });
    }
}

static BuiltinLocationState stateOfLocation(const ValueFlowResultInputs& inputs, const BuiltinHolders& holders, BuiltinID member)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    if (isLockedLocation(builtins, member))
        return BuiltinLocationState::Constant;
    if (builtins.at(member).mutability == BuiltinMutability::ChangesOnItsOwn)
        return BuiltinLocationState::Unknown;

    BuiltinID holder = builtins.canonicalID(builtins.at(member).holder);
    PropertyKeyID key = inputs.program.keyOf(builtins.at(member).name);
    OptionSet<BuiltinUse> uses = inputs.solver().usesOf(member);
    bool isWritten = holders.unseenCodeRuns() ? uses.contains(BuiltinUse::Written) : holders.isWrittenByProgram(holder, key);
    if (isWritten || !inputs.build.isEnabled(FlowResultKind::ConstantBuiltinLocations, member.index()))
        return BuiltinLocationState::Unknown;
    if (!holders.mayBeWrittenBehindAnalysis(holder, key))
        return BuiltinLocationState::Constant;
    bool isLockedAtStartUp = !uses.isEmpty() && canLockAtStartUp(builtins, member) && !(holders.unseenCodeRuns() && holder == Builtins::GlobalThis);
    return isLockedAtStartUp ? BuiltinLocationState::Constant : BuiltinLocationState::Unknown;
}

static void sortAndMergeLocations(Vector<BuiltinLocationFlow>& locations)
{
    auto orderOf = [](const BuiltinLocationFlow& location) {
        return std::tuple(location.holder, location.key);
    };
    AOT::stableSort(locations, { }, orderOf);
    size_t size = 0;
    for (const BuiltinLocationFlow& location : locations) {
        if (!size || orderOf(locations[size - 1]) != orderOf(location)) {
            locations[size++] = location;
            continue;
        }
        if (location.state == BuiltinLocationState::Unknown)
            locations[size - 1].state = BuiltinLocationState::Unknown;
        keepLowerWitness(locations[size - 1].witness, location.witness);
    }
    locations.shrink(size);
}

static std::optional<unsigned> computedKeyOfWrite(const Value& operation)
{
    if (operation.opcode == Opcode::ElemSet || operation.opcode == Opcode::ElemDefine || operation.opcode == Opcode::ElemDelete)
        return OperandPosition::property(operation.opcode);
    if ((operation.opcode == Opcode::DefineGetter || operation.opcode == Opcode::DefineSetter) && !operation.key())
        return OperandPosition::computedKey(operation.opcode);
    return std::nullopt;
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
    case BuiltinPropertyActionKind::ReadsByKey:
    case BuiltinPropertyActionKind::AssignsFrom:
    case BuiltinPropertyActionKind::TakesDescriptorMap:
    case BuiltinPropertyActionKind::ChangesIntegrity:
    case BuiltinPropertyActionKind::ChangesPrototype:
    case BuiltinPropertyActionKind::BecomesPrototype:
    case BuiltinPropertyActionKind::ReadsGetter:
    case BuiltinPropertyActionKind::ReadsSetter:
    case BuiltinPropertyActionKind::TestsByKey:
    case BuiltinPropertyActionKind::DescribesOwnProperties:
    case BuiltinPropertyActionKind::DescribesResult:
    case BuiltinPropertyActionKind::DefinesFrom:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<BuiltinCallMode> modeOfCallOperation(Opcode opcode)
{
    if (opcode == Opcode::Call || opcode == Opcode::CallList)
        return BuiltinCallMode::Call;
    if (opcode == Opcode::Construct || opcode == Opcode::ConstructList)
        return BuiltinCallMode::Construct;
    return std::nullopt;
}

static ValueID valuePassedAt(const Value& call, BuiltinOperand position)
{
    if ((call.opcode != Opcode::Call && call.opcode != Opcode::Construct) || position.kind() == BuiltinOperand::Kind::ArgumentsFrom)
        return { };
    unsigned index = position.isReceiver() ? 1 : 2 + position.argumentIndex();
    return index < call.numberOfOperands() ? call.operand(index) : ValueID();
}

static Vector<ValueFlowWriteUnderNewName> collectWritesOfLockedLocations(const ValueFlowResultInputs& inputs, const BuiltinHolders& holders, ValueFlowHeapView& heap, Vector<BuiltinLocationFlow>& locations)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowBodies& bodies = inputs.bodies();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    bool writesLockedLocation = false;
    holders.forEachNameWritten([&](BuiltinID holder, PropertyKeyID key) {
        if (!isLockedLocation(program, holder, key))
            return;
        locations.append(BuiltinLocationFlow { holder, key, BuiltinLocationState::Constant, noWitness });
        writesLockedLocation = true;
    });
    if (!writesLockedLocation && !holders.mayWriteLockedLocationUnderKeyNobodyKnows())
        return { };

    struct Write {
        BuiltinID holder;
        PropertyKeyID key;
        StringID text;
        Witness witness;
    };
    struct Notes {
        ValueSetMaker& maker;
        Vector<Write>& writes;
    };
    auto listsItsObjects = [&](ValueSetID objects) {
        return !ValueSets::isComplement(objects) && !sets.meet(objects, universe.objectsThatMayBeAnything());
    };
    auto spelledOutTextsOf = [&](FunctionID function, ValueID keyValue) -> std::span<const StringID> {
        return keyValue ? program.typeOf(program.at(function), keyValue).stringLiterals.span() : std::span<const StringID>();
    };
    auto note = [&](const Notes& notes, ValueSetID receivers, PropertyKeyID key, const Witness& witness) {
        const ValueFlowHeapTables& tables = universe.builtinTables().heap();
        ValueSetID holdersThatMayLockKey = tables.under(key).holdersWithLockedLocation;
        if (program.isIndexLike(key))
            holdersThatMayLockKey = notes.maker.unite(holdersThatMayLockKey, tables.holdersWithLockedIndexes());
        if (!sets.meet(receivers, holdersThatMayLockKey))
            return;
        for (BuiltinID holder : holders.holdersIn(notes.maker, notes.maker.intersect(receivers, holdersThatMayLockKey))) {
            if (isLockedLocation(program, holder, key))
                notes.writes.append({ holder, key, { }, witness });
        }
    };
    auto noteUnderText = [&](const Notes& notes, ValueSetID receivers, StringID text, const Witness& witness) {
        if (std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text })) {
            note(notes, receivers, *key, witness);
            return;
        }
        for (BuiltinID holder : holders.holdersIn(notes.maker, receivers)) {
            if (builtins.at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) || builtins.closesIndexKeys(holder))
                notes.writes.append({ holder, { }, text, witness });
        }
    };
    auto noteUnderNamesAmong = [&](const Notes& notes, ValueSetID keys, FunctionID function, ValueID keyValue, ValueSetID receivers, const Witness& witness) {
        if (ValueSets::isComplement(keys))
            return;
        std::span<const StringID> spelledOutTexts = spelledOutTextsOf(function, keyValue);
        Vector<ValueFlowObjectID, 8> primitives;
        for (auto [primitive, text] : primitiveKeyTexts)
            primitives.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(primitive)));
        if (!spelledOutTexts.empty() && !(program.typeOf(program.at(function), keyValue).primitives & Primitive::String))
            primitives.append(ValueFlowUniverse::primitive(PrimitiveSet::String));
        if (!sets.isSubset(keys, notes.maker.unite(universe.keys(), notes.maker.of(primitives.span()))))
            return;
        ValueSetID namesAmongKeys = notes.maker.intersect(keys, universe.keys());
        if (sets.size(namesAmongKeys) <= inputs.build.limit(LimitID::MaxValueFlowMembersToldApart)) {
            sets.forEachMember(namesAmongKeys, [&](ValueFlowObjectID key) {
                if (universe.kindOf(key) == ValueFlowObjectKind::Key)
                    note(notes, receivers, universe.keyOf(key), witness);
            });
        }
        for (auto [primitive, text] : primitiveKeyTexts) {
            StringID string = sets.contains(keys, ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(primitive))) ? program.strings.find(byteCast<uint8_t>(text.span())) : StringID();
            if (string)
                noteUnderText(notes, receivers, string, witness);
        }
        if (!sets.contains(keys, ValueFlowUniverse::primitive(PrimitiveSet::String)))
            return;
        for (StringID text : spelledOutTexts)
            noteUnderText(notes, receivers, text, witness);
    };

    ValueSetMakers makers(inputs);
    Vector<Write> writes = collectWithMakers<Write>(inputs, makers, program.functions.size(), [&](unsigned function, ValueSetMaker& maker, Vector<Write>& found) {
        if (!inputs.isReached(FunctionID(function)))
            return;
        Notes notes { maker, found };
        for (const ValueFlowStatement& statement : bodies.statementsOf(function)) {
            std::optional<unsigned> object = statement.kind == ValueFlowStatementKind::Operation && statement.site && !modeOfCallOperation(statement.opcode) ? OperandPosition::object(statement.opcode) : std::nullopt;
            std::span<const ValueFlowOperand> operands = bodies.operandsOf(statement);
            if (!object || !listsItsObjects(heap.valueOf(maker, operands[*object])))
                continue;
            Witness witness = witnessOfStatement(program, statement);
            ValueFlowStatementID id(static_cast<uint32_t>(&statement - bodies.statements().data()));
            inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID) {
                if (formOfWrite(key.name().kind) == BuiltinWriteForm::UnderName)
                    note(notes, key.set, PropertyKeyID(key.payload), witness);
            });
            const Value& operation = program.at(statement.function).at(statement.site);
            if (std::optional<unsigned> key = computedKeyOfWrite(operation))
                noteUnderNamesAmong(notes, heap.valueOf(maker, operands[*key]), statement.function, operation.operand(*key), heap.valueOf(maker, operands[*object]), witness);
        }
    });

    auto siteOf = [](FunctionID function, ValueID site) {
        return static_cast<uint64_t>(function.index()) << 32 | site.index();
    };
    std::span<const ValueFlowHeapIndex::CallOfValue> calls = heap.index().callsThatMayApplyBuiltins();
    ValueSetID callablesThatAreNoBuiltinFunctions = heap.maker().intersect(universe.callables(), ValueSets::complement(universe.builtinFunctions()));
    Vector<uint64_t> callsOfBuiltinFunctionsAlone = WTF::map(findIndicesInParallel(inputs.loops(), calls.size(), [&](unsigned index) {
        return modeOfCallOperation(bodies.statements()[calls[index].statement].opcode) && !ValueSets::isComplement(calls[index].callees) && !sets.meet(calls[index].callees, callablesThatAreNoBuiltinFunctions);
    }), [&](uint32_t index) {
        const ValueFlowStatement& call = bodies.statements()[calls[index].statement];
        return siteOf(call.function, call.site);
    });
    AOT::sort(callsOfBuiltinFunctionsAlone);
    BitVector builtinsThatWriteUnderKeys = builtinsWithOverload(builtins, [](const BuiltinOverload& overload) {
        return std::ranges::any_of(overload.flow.propertyActions, writesUnderKey, &BuiltinPropertyAction::kind);
    });
    Notes notes { heap.maker(), writes };
    forEachBuiltinApplication(inputs, heap, builtinsThatWriteUnderKeys, [&](const ValueFlowBuiltinApplication& application) {
        if (!application.site || !builtinsThatWriteUnderKeys.quickGet(application.builtin.index()) || !std::ranges::binary_search(callsOfBuiltinFunctionsAlone, siteOf(application.function, application.site)))
            return;
        const Function& function = program.at(application.function);
        Witness witness { application.function.index(), application.site.index(), function.locationOf(application.site) };
        for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
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
                        note(notes, receivers, program.keyOf(name), witness);
                    break;
                case BuiltinKeyKind::Operand:
                    noteUnderNamesAmong(notes, application.valueAt(*action.key.operand, notes.maker), application.function, valuePassedAt(function.at(application.site), *action.key.operand), receivers, witness);
                    break;
                case BuiltinKeyKind::None:
                case BuiltinKeyKind::Any:
                case BuiltinKeyKind::Indexes:
                case BuiltinKeyKind::ElementsOf:
                    break;
                }
            }
        }
    });

    Vector<ValueFlowWriteUnderNewName> underNewNames;
    for (const Write& write : writes) {
        if (write.key)
            locations.append(BuiltinLocationFlow { write.holder, write.key, BuiltinLocationState::Constant, write.witness });
        else
            underNewNames.append({ write.holder, write.text, write.witness });
    }
    return underNewNames;
}

static Vector<ValueFlowWriteUnderNewName> computeBuiltinLocations(const ValueFlowResultInputs& inputs, const BuiltinHolders& holders, ValueFlowHeapView& heap, FlowBuiltinTables& tables)
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
        tables.constantMembers.quickSet(index, state == BuiltinLocationState::Constant && !builtins.has(BuiltinMark::MayBeAbsent, member));
        if (!inputs.solver().usesOf(member).isEmpty())
            tables.locations.append(BuiltinLocationFlow { entry.holder, program.keyOf(entry.name), state, noWitness });
    }
    holders.forEachNameWritten([&](BuiltinID holder, PropertyKeyID key) {
        if (!findOwnMember(program, holder, key) && !isLockedLocation(program, holder, key))
            tables.locations.append(BuiltinLocationFlow { holder, key, BuiltinLocationState::Unknown, noWitness });
    });
    Vector<ValueFlowWriteUnderNewName> underNewNames = collectWritesOfLockedLocations(inputs, holders, heap, tables.locations);
    sortAndMergeLocations(tables.locations);
    return underNewNames;
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

static ProgramInstances countProgramInstances(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    if (!inputs.build.isEnabledForProgram(FlowResultKind::ReachedFunctions))
        return ProgramInstances::Unknown;
    bool unseenCodeFindsEveryBuiltin = unseenCodeHasLoader(inputs.solution);
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        if (!builtins.at(builtin).flags.contains(BuiltinFlag::StartsInstance) && !builtins.at(builtins.canonicalID(builtin)).flags.contains(BuiltinFlag::StartsInstance))
            continue;
        ValueFlowObjectID object = inputs.universe().objectOf(builtin);
        if (unseenCodeFindsEveryBuiltin || inputs.solver().usesOf(builtin).containsAny(usesEnteringCode(builtins.at(builtin).kind)) || (object && (inputs.hasEscaped(object) || inputs.isLost(object))))
            return ProgramInstances::Unknown;
    }
    return ProgramInstances::AtMostOne;
}

static LegacyRegExpStatics findReadsOfLegacyRegExpStatics(const ValueFlowResultInputs& inputs, const BuiltinHolders& holders)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BuiltinID constructor = builtins.canonicalID(builtins.at(BuiltinClasses::RegExp).constructor);
    if (!inputs.build.isEnabled(FlowResultKind::ObjectUses, constructor.index()) || holders.unseenCodeRuns() || holders.hasEscaped(constructor) || holders.isLost(constructor))
        return LegacyRegExpStatics::Unknown;
    bool isMentioned = std::ranges::any_of(builtins.membersOf(constructor), [&](BuiltinID member) {
        return builtins.at(member).mutability == BuiltinMutability::ChangesOnItsOwn && (!inputs.solver().usesOf(member).isEmpty() || !inputs.solver().usesOf(builtins.canonicalID(member)).isEmpty());
    });
    return isMentioned ? LegacyRegExpStatics::Unknown : LegacyRegExpStatics::NeverRead;
}

ValueFlowClassesOfBuiltinResults computeClassesOfBuiltinResults(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    using Noted = std::pair<ValueFlowObjectID, BuiltinClassID>;
    ValueSetMakers makers(inputs);
    Vector<Noted> noted = collectWithMakers<Noted>(inputs, makers, inputs.program.functions.size(), [&](unsigned function, ValueSetMaker& maker, Vector<Noted>& found) {
        if (!inputs.isReached(FunctionID(function)))
            return;
        for (const ValueFlowStatement& call : inputs.bodies().statementsOf(function)) {
            std::optional<BuiltinCallMode> mode = call.kind == ValueFlowStatementKind::Operation ? modeOfCallOperation(call.opcode) : std::nullopt;
            ValueFlowObjectID object = mode ? universe.siteObject(call) : ValueFlowObjectID();
            if (!object || universe.layoutOf(object))
                continue;
            ValueSetID callees = inputs.valueOf(maker, inputs.bodies().operandsOf(call)[0]);
            if (ValueSets::isComplement(callees) || inputs.sets().meet(callees, universe.otherCallables())) {
                found.append({ object, { } });
                continue;
            }
            ValueSetID builtinCallees = maker.intersect(callees, universe.builtinFunctions());
            inputs.sets().forEachMember(builtinCallees, [&](ValueFlowObjectID callee) {
                for (const BuiltinOverload& overload : builtins.overloadsOf(universe.builtinOf(callee))) {
                    if (overload.mode != *mode || !overload.result.type)
                        continue;
                    for (const BuiltinTypePart& part : builtins.at(overload.result.type).parts) {
                        switch (part.kind) {
                        case BuiltinTypePartKind::Instance:
                            found.append({ object, part.builtinClass });
                            break;
                        case BuiltinTypePartKind::InstanceOfReceiverClass:
                        case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
                        case BuiltinTypePartKind::Layout:
                            found.append({ object, { } });
                            break;
                        case BuiltinTypePartKind::Builtin:
                        case BuiltinTypePartKind::Operand:
                        case BuiltinTypePartKind::Slot:
                        case BuiltinTypePartKind::IteratedValues:
                        case BuiltinTypePartKind::CallbackResult:
                        case BuiltinTypePartKind::Prototype:
                            break;
                        }
                    }
                }
            });
            if (builtinCallees != ValueSets::nothing && inputs.sets().isSubset(builtinCallees, maker.of(universe.objectOf(builtins.canonicalID(Builtins::ObjectEntries)))))
                found.append({ universe.siteObject(call, 1), BuiltinClasses::Array });
        }
    });
    ValueFlowClassesOfBuiltinResults classes;
    for (auto [object, builtinClass] : noted) {
        auto result = classes.add(object.index(), builtinClass);
        if (result.iterator->value != builtinClass)
            result.iterator->value = BuiltinClassID();
    }
    return classes;
}

static void recordPlacesOfUnseenCode(const BuiltinTable& builtins, const ValueFlowPlacesInProgram& places, Vector<UnseenCodeRecord>& records)
{
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
    auto evaluator = std::ranges::find_if(records, admitsUnknownJavaScript, &UnseenCodeRecord::cause);
    for (UnseenCodeRecord& record : records) {
        if (evaluator != records.end() && record.cause == UnseenCodeCause::LoadsModuleByName && record.builtin == Wire::none && !hasFunction(record.witness)) {
            record.builtin = evaluator->builtin;
            record.witness = evaluator->witness;
        }
    }
    sortAndRemoveRepeatedRecords(records);
}

static void recordPlaces(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BitVector builtinsAskedAbout(builtins.builtinCount());
    auto askAboutEach = [&](const auto& records) {
        for (const auto& record : records) {
            if (record.builtin != Wire::none)
                builtinsAskedAbout.quickSet(builtins.canonicalID(BuiltinID(record.builtin)).index());
        }
    };
    askAboutEach(tables.builtins.uses);
    askAboutEach(tables.wholeProgram.unseenCode);
    askAboutEach(tables.wholeProgram.loadsOfMachineCode);

    ValueFlowPlacesInProgram places(inputs, builtinsAskedAbout);
    auto whereNamed = [&](uint32_t builtin) {
        return builtin == Wire::none ? noWitness : places.whereNamed(builtins.canonicalID(BuiltinID(builtin)));
    };
    for (BuiltinUseRecord& use : tables.builtins.uses)
        use.witness = whereNamed(use.builtin);
    for (FeatureUseRecord& use : tables.builtins.featureUses)
        use.witness = whereNamed(use.builtin);
    AOT::sort(tables.builtins.featureUses, { }, [](const FeatureUseRecord& use) {
        return std::tuple(use.feature, use.witness.location.module, use.witness.location.position, use.builtin);
    });
    for (FlowLocaleUse& use : tables.builtins.localeUses) {
        use.witness = places.wayTo(use.witness);
        if (!hasFunction(use.witness) && use.builtin)
            use.witness = whereNamed(use.builtin.index());
    }
    recordPlacesOfUnseenCode(builtins, places, tables.wholeProgram.unseenCode);
    recordPlacesOfUnseenCode(builtins, places, tables.wholeProgram.loadsOfMachineCode);
}

Vector<ValueFlowWriteUnderNewName> computeBuiltinResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    ValueFlowHeapView heap(inputs);
    BuiltinHolders holders(inputs, heap);
    times.didFinish("built-ins: holders"_s);
    Vector<ValueFlowWriteUnderNewName> writesUnderNewNames = computeBuiltinLocations(inputs, holders, heap, tables.builtins);
    times.didFinish("built-ins: locations"_s);
    BitVector builtinsWhoseCodeRuns = findBuiltinsWhoseCodeRuns(inputs);
    tables.wholeProgram.unseenCode = collectUnseenCode(inputs, tables.wholeProgram.loadsOfMachineCode);
    warnAboutLoadsUnderComputedNames(inputs.program, inputs.build, tables.wholeProgram.unseenCode.span());
    tables.wholeProgram.instances = countProgramInstances(inputs);
    times.didFinish("built-ins: code that runs, unseen code, instances"_s);
    tables.builtins.uses = { };
    for (uint32_t index = 0; index < inputs.program.builtins().builtinCount(); ++index) {
        if (OptionSet<BuiltinUse> uses = inputs.solver().usesOf(BuiltinID(index)); !uses.isEmpty())
            tables.builtins.uses.append(BuiltinUseRecord { index, static_cast<BuiltinUse>(uses.toRaw()), noWitness });
    }
    times.didFinish("built-ins: uses"_s);
    tables.builtins.legacyRegExpStatics = findReadsOfLegacyRegExpStatics(inputs, holders);
    tables.builtins.featureUses = collectFeatureUses(inputs, holders, builtinsWhoseCodeRuns);
    tables.builtins.builtinsThatMayBeTrimmed = findBuiltinsThatMayBeTrimmed(inputs.program.builtins(), tables.builtins.featureUses.span(), inputs.build.options());
    tables.builtins.localeUses = collectLocaleUses(inputs, heap, builtinsWhoseCodeRuns, tables.wholeProgram.unseenCode.span());
    tables.builtins.workerEntries = collectWorkerEntries(inputs, heap);
    times.didFinish("built-ins: statics, features, locales"_s);
    if (hasFlag(inputs.build.options().reports, ReportSet::Flow)) {
        recordPlaces(inputs, tables);
        times.didFinish("built-ins: places for the report"_s);
    }
    tables.builtins.areFilled = true;
    return writesUnderNewNames;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
