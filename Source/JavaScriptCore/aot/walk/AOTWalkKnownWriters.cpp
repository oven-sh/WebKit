/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTCodeRole.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsOfObjectsJudgedInKnownWriters {
    AbstractHeap::ObjectFields, AbstractHeap::EnvironmentFields, AbstractHeap::ShapeWords, AbstractHeap::NamedProperties, AbstractHeap::OrderWords, AbstractHeap::PrototypeLinks, AbstractHeap::Elements, AbstractHeap::ArrayStorage
};
static constexpr AbstractHeapSet heapsJudgedInKnownWriters = heapsOfObjectsJudgedInKnownWriters | AbstractHeapSet { AbstractHeap::MemberGuards };
static constexpr AbstractHeapSet heapsUnderElementsOfObjectsInKnownWriters { AbstractHeap::Elements, AbstractHeap::ArrayStorage };
static_assert(perObjectHeaps.containsAll(heapsOfObjectsJudgedInKnownWriters));
static_assert(!perObjectHeaps.contains(AbstractHeap::MemberGuards));

static constexpr OptionSet<FactOfCell> factsOfCellKnownInKnownWriters {
    FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot, FactOfCell::NoExtraProperties, FactOfCell::HasLayoutPrototype, FactOfCell::OrderWordIsKnown, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed, FactOfCell::BearsNoMark
};

static OptionSet<FactOfCell> factsOfCellEndedInKnownWriters(const LocationMasks& writes)
{
    return factsOfCellEndedBy(writes) | OptionSet<FactOfCell>::fromRaw(static_cast<OptionSet<FactOfCell>::StorageType>(~factsOfCellKnownInKnownWriters.toRaw()));
}

void WalkState::invalidate(const Effects& effects, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard, PropertyKeyID nameOfGuard)>& mayEndGuard)
{
    if (effects.mayRunOtherCode()) {
        invalidate(effects);
        return;
    }
    noteWhatOperationDoes(effects);

    auto endsAnswer = [&](const KnownAnswer& answer, bool isWrittenOutsideObjects) {
        return isWrittenOutsideObjects || !answer.holder || mayBeWritten(answer.holder);
    };
    m_answers.endWhatIsWritten(effects, endsAnswer);
    if (!holdsNoRecord() && !effects.writes.isEmpty()) {
        heldToWrite().endWhatIsWritten(effects, mayBeWritten, mayEndGuard);
        noteKindsOfRecordsLeft();
    }
}

void WalkState::Held::endWhatIsWritten(const Effects& effects, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard, PropertyKeyID nameOfGuard)>& mayEndGuard)
{
    const LocationMasks& writes = effects.writes;
    if (!writes.containsOnly(heapsJudgedInKnownWriters)) {
        Effects judgedByHeap = effects;
        judgedByHeap.writes.remove(heapsJudgedInKnownWriters);
        endWhatIsWritten(judgedByHeap);
        if (holdsNoRecord())
            return;
    }

    bool writesIntoObjects = writes.containsAny(heapsOfObjectsJudgedInKnownWriters);
    bool endsShapes = writes.contains(AbstractHeap::ShapeWords) && m_numberOfShapes;
    bool endsPlaces = writes.containsAny(heapsJudgedInKnownWriters) && m_numberOfPlaces;
    OptionSet<FactOfCell> factsEnded = writesIntoObjects && m_numberOfHoldersWithFactsOfCell ? factsOfCellEndedInKnownWriters(writes) : OptionSet<FactOfCell>();
    if (!endsShapes && !endsPlaces && factsEnded.isEmpty())
        return;

    uint64_t slotsWritten = writes.maskOf(AbstractHeap::ObjectFields);
    uint64_t environmentFieldsWritten = writes.maskOf(AbstractHeap::EnvironmentFields);
    uint64_t guardsWritten = writes.maskOf(AbstractHeap::MemberGuards);
    bool endsElements = endsPlaces && writes.containsAny(heapsUnderElementsOfObjectsInKnownWriters);
    using KindsOfPlaces = decltype(Holder::kindsOfPlaces);
    auto bitOfKind = [](PlaceKind kind) {
        return static_cast<KindsOfPlaces>(KindsOfPlaces(1) << static_cast<unsigned>(kind));
    };
    KindsOfPlaces kindsThatRestOnWhatIsKnown = bitOfKind(PlaceKind::Slot) | bitOfKind(PlaceKind::Variable) | bitOfKind(PlaceKind::EnvironmentField) | bitOfKind(PlaceKind::Element) | bitOfKind(PlaceKind::GuardOfStores) | bitOfKind(PlaceKind::GuardOfReads);
    auto endPlacesWhere = [&](uint32_t holder, bool isListOfElements, const auto& ends) {
        uint32_t previous = none;
        for (uint32_t position = isListOfElements ? m_holders[holder].firstElement : m_holders[holder].firstPlace; position != none;) {
            const Place& place = m_places[position];
            uint32_t next = place.next;
            bool hasEnded = !place.value;
            if (!hasEnded && ends(place)) {
                endPlace(position);
                hasEnded = true;
            }
            if (!hasEnded) {
                previous = position;
                position = next;
                continue;
            }
            m_places.mutableAt(position).isListed = false;
            if (previous != none)
                m_places.mutableAt(previous).next = next;
            else if (isListOfElements)
                m_holders.mutableAt(holder).firstElement = next;
            else
                m_holders.mutableAt(holder).firstPlace = next;
            position = next;
        }
    };
    auto endsAlways = [](const Place&) {
        return true;
    };

    bool reachesPlaces = (writtenUnderPlacesReachedByEffects(writes) & m_maskOfPlacesReachedByEffects) || ((m_kindsOfPlacesReachedByEffects & ~kindsThatRestOnWhatIsKnown) && writesIntoObjects);
    bool reachesSomething = (endsShapes && m_numberOfShapesReachedByEffects)
        || (m_numberOfHoldersWithFactsOfCellReachedByEffects && m_factsOfCellReachedByEffects.containsAny(factsEnded))
        || (m_numberOfPlacesReachedByEffects && reachesPlaces)
        || (endsElements && m_numberOfElementsReachedByEffects);
    if (!reachesSomething) {
        dropEndedRecordsIfMany();
        return;
    }
    if (goesByLists()) [[unlikely]] {
        endByLists({ .shapes = endsShapes, .elements = endsElements, .facts = factsEnded, .slots = slotsWritten, .environmentFields = environmentFieldsWritten, .guards = guardsWritten,
            .mayBeWritten = &mayBeWritten, .mayEndGuard = &mayEndGuard });
        dropEndedRecordsIfMany();
        return;
    }

    OptionSet<FactOfCell> factsLeft;
    KindsOfPlaces kindsOfPlacesLeft = 0;
    uint64_t maskOfPlacesLeft = 0;
    for (size_t index = 0; index < m_holdersReachedByEffects.size();) {
        uint32_t holder = m_holdersReachedByEffects[index];
        Reference object = m_holders[holder].object;
        std::optional<bool> isKnownToBeWritten;
        auto isWritten = [&] {
            if (!isKnownToBeWritten)
                isKnownToBeWritten = !object || mayBeWritten(object);
            return *isKnownToBeWritten;
        };

        if (endsShapes && m_holders[holder].shape && isWritten())
            endShape(holder);
        if (m_holders[holder].factsOfCell.containsAny(factsEnded) && isWritten())
            endFactsOfCell(holder, factsEnded);

        KindsOfPlaces kindsOfPlaces = m_holders[holder].kindsOfPlaces;
        uint64_t written = 0;
        if (kindsOfPlaces & bitOfKind(PlaceKind::Slot))
            written |= slotsWritten;
        if (kindsOfPlaces & bitOfKind(PlaceKind::EnvironmentField))
            written |= environmentFieldsWritten;
        if (kindsOfPlaces & (bitOfKind(PlaceKind::GuardOfStores) | bitOfKind(PlaceKind::GuardOfReads)))
            written |= guardsWritten;
        bool hasPlacesOfOtherKinds = kindsOfPlaces & ~kindsThatRestOnWhatIsKnown;
        if ((written & m_holders[holder].maskOfPlaces) || (hasPlacesOfOtherKinds && writesIntoObjects)) {
            noteRecordsGoneThrough(m_holders[holder].numberOfPlaces, placesFoundWithoutTable);
            KindsOfPlaces kindsLeft = 0;
            uint64_t maskLeft = 0;
            endPlacesWhere(holder, false, [&](const Place& place) {
                bool ends = false;
                if (place.kind == PlaceKind::Slot)
                    ends = (slotsWritten & place.mask()) && isWritten();
                else if (place.kind == PlaceKind::EnvironmentField)
                    ends = (environmentFieldsWritten & place.mask()) && isWritten();
                else if (place.kind == PlaceKind::GuardOfStores || place.kind == PlaceKind::GuardOfReads)
                    ends = (guardsWritten & place.mask()) && mayEndGuard(classOfGuardedSide(place.first), PropertyKeyID::fromBits(place.second));
                else if (!(bitOfKind(place.kind) & kindsThatRestOnWhatIsKnown))
                    ends = writesIntoObjects && isWritten();
                if (ends)
                    return true;
                kindsLeft |= bitOfKind(place.kind);
                maskLeft |= place.mask();
                return false;
            });
            Holder& changed = m_holders.mutableAt(holder);
            changed.kindsOfPlaces = kindsLeft;
            changed.maskOfPlaces = maskLeft;
        }
        if (endsElements && m_holders[holder].firstElement != none && isWritten())
            endPlacesWhere(holder, true, endsAlways);

        const Holder& record = m_holders[holder];
        if (record.shape || !record.factsOfCell.isEmpty() || record.firstPlace != none || record.firstElement != none) {
            factsLeft.add(record.factsOfCell);
            kindsOfPlacesLeft |= record.kindsOfPlaces;
            maskOfPlacesLeft |= record.maskOfPlaces;
            ++index;
        } else
            endHolder(holder);
    }
    m_factsOfCellReachedByEffects = factsLeft;
    m_kindsOfPlacesReachedByEffects = kindsOfPlacesLeft;
    m_maskOfPlacesReachedByEffects = maskOfPlacesLeft;
    dropEndedRecordsIfMany();
}

static Reference rootInKnownWriters(const Code& code, Reference name)
{
    if (!name.isValue())
        return name;
    ValueID root = rootOfCellInSweptCode(code, name.id());
    return code.isConstant(root) ? Reference::constant(root) : Reference::value(root);
}

static bool makesNewCellInKnownWriters(const Operation& made)
{
    Opcode opcode = made.opcode;
    if (resultRuleOf(opcode) != ResultRule::Allocation || boundToOf(opcode) == BoundTo::Site || made.traits().mayRunOtherCode())
        return false;
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (operandUsesOf(opcode, operandIndex).contains(OperandUse::Returns))
            return false;
    }
    return true;
}

bool mayBeSameObject(Walker& walker, Reference first, Reference second)
{
    const Code& code = walker.code();
    first = rootInKnownWriters(code, first);
    second = rootInKnownWriters(code, second);
    if (!first.isRecord() || !second.isRecord())
        return false;
    if (first == second)
        return true;

    auto isMadeBehind = [&](Reference cell, Reference other) {
        if (!cell.isValue() || !makesNewCellInKnownWriters(code.at(cell.id())))
            return false;
        return other.isConstant() || other.id().index() < cell.id().index();
    };
    if (isMadeBehind(first, second) || isMadeBehind(second, first))
        return false;

    TypeID typeOfFirst = walker.typeOf(first);
    TypeID typeOfSecond = walker.typeOf(second);
    if (typeOfFirst && typeOfSecond && !walker.types().mayBeSameObject(typeOfFirst, typeOfSecond))
        return false;

    WalkState& state = walker.state();
    return !state.isFresh(first) && !state.isFresh(second);
}

namespace {

using ObjectsWrittenAloneInKnownWriters = Vector<Reference, 2>;

struct ElementStoreInKnownWriters {
    Reference array;
};

} // anonymous namespace

static constexpr OperandUses usesThatWriteOperandInKnownWriters {
    OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::ChangesIntegrity
};
static constexpr OperandUses usesThatReachBeyondOperandInKnownWriters {
    OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype, OperandUse::PassesToUserCode, OperandUse::AppliesTransitively, OperandUse::AppliesToContents
};
static constexpr OperandUses usesThatWriteNoOperandInKnownWriters {
    OperandUse::Inspects, OperandUse::Retains, OperandUse::Returns, OperandUse::ReadsContents, OperandUse::RetainsContents
};
static_assert(everyOperandUseIsInExactlyOneOf({ usesThatCallOperand, usesThatWriteOperandInKnownWriters, usesThatReachBeyondOperandInKnownWriters, usesThatWriteNoOperandInKnownWriters }));

static uint32_t numberOfDeclaredOperandsInKnownWriters(Opcode opcode)
{
    return numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
}

static bool opcodeMayRunCodeOfOperandsInKnownWriters(Opcode opcode, uint32_t numberOfDeclaredOperands)
{
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (!info.protocols.isEmpty() || info.uses.containsAny(usesThatCallOperand))
            return true;
    }
    return false;
}

static bool writesThroughDeclaredOperandInKnownWriters(Opcode opcode, uint32_t declaredIndex, uint32_t numberOfDeclaredOperands)
{
    if (operandUsesOf(opcode, declaredIndex).containsAny(usesThatWriteOperandInKnownWriters))
        return true;
    Access access = accessOf(opcode);
    if ((access == Access::Store || access == Access::Define) && receiverOf(opcode) == declaredIndex)
        return true;
    for (uint32_t retainedIndex = 0; retainedIndex < numberOfDeclaredOperands; ++retainedIndex) {
        if (retainedInOf(opcode, retainedIndex) == RetainedIn::Operand && retainingOperandOf(opcode, retainedIndex) == declaredIndex)
            return true;
    }
    return false;
}

static bool writesThroughDeclaredOperandsAloneInKnownWriters(Opcode opcode, Vector<bool, 8>& writtenThrough)
{
    uint32_t numberOfDeclaredOperands = numberOfDeclaredOperandsInKnownWriters(opcode);
    if (opcodeMayRunCodeOfOperandsInKnownWriters(opcode, numberOfDeclaredOperands))
        return false;
    bool namesWrittenOperand = false;
    for (uint32_t declaredIndex = 0; declaredIndex < numberOfDeclaredOperands; ++declaredIndex) {
        if (operandUsesOf(opcode, declaredIndex).containsAny(usesThatReachBeyondOperandInKnownWriters))
            return false;
        bool isWrittenThrough = writesThroughDeclaredOperandInKnownWriters(opcode, declaredIndex, numberOfDeclaredOperands);
        writtenThrough.append(isWrittenThrough);
        namesWrittenOperand |= isWrittenThrough;
    }
    return namesWrittenOperand;
}

static Reference onlyObjectDefinedOnInKnownWriters(Walker& walker, const Draft& writer)
{
    static constexpr OpcodeSet definitionsOnOneObject { Opcode::DefineGetter, Opcode::DefineSetter, Opcode::DefineAccessors, Opcode::PropDefine };
    if (definitionsOnOneObject.contains(writer.opcode))
        return writer.objectOperand();
    if (writer.opcode == Opcode::DefineOwnProperty) {
        TypeID type = walker.typeOf(writer.objectOperand());
        return type && walker.types().at(type).hasOnlyLayouts() ? writer.objectOperand() : Reference();
    }
    return { };
}

static std::optional<ElementStoreInKnownWriters> findElementStoreWithoutOtherCodeInKnownWriters(const Draft& writer, const Effects& effects)
{
    if (writer.opcode == Opcode::PutArrayElement) {
        if (writer.level() != KnowledgeLevel::Proven)
            return std::nullopt;
        return ElementStoreInKnownWriters { writer.arrayOperand() };
    }
    if (writer.opcode != Opcode::ElemSet || !effects.writes.isSubsetOf(declaredBoundInTable(Opcode::PutArrayElement).writes))
        return std::nullopt;
    return ElementStoreInKnownWriters { writer.objectOperand() };
}

static std::optional<ObjectsWrittenAloneInKnownWriters> objectsWrittenAloneByInKnownWriters(Walker& walker, const Draft& writer, const Effects& effects, const std::optional<ElementStoreInKnownWriters>& elementStore)
{
    if (testsItsOwnGuess(writer) || effectRuleOf(writer.opcode) == EffectRule::Call)
        return std::nullopt;
    if (writer.opcode == Opcode::SetLiteralPrototype)
        return ObjectsWrittenAloneInKnownWriters { writer.objectOperand(), writer.prototypeOperand() };
    if (writer.opcode == Opcode::TryCopyFields)
        return ObjectsWrittenAloneInKnownWriters { writer.objectOperand() };
    if (Reference object = onlyObjectDefinedOnInKnownWriters(walker, writer))
        return ObjectsWrittenAloneInKnownWriters { object };
    if (elementStore)
        return ObjectsWrittenAloneInKnownWriters { elementStore->array };
    Vector<bool, 8> writtenThrough;
    if (!writesThroughDeclaredOperandsAloneInKnownWriters(writer.opcode, writtenThrough))
        return std::nullopt;
    UNUSED_PARAM(effects);

    ObjectsWrittenAloneInKnownWriters written;
    uint32_t numberOfDeclaredOperands = writtenThrough.size();
    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(writer.opcode);
    for (uint32_t operandIndex = 0; operandIndex < writer.numberOfOperands(); ++operandIndex) {
        if (!writer.operand(operandIndex))
            continue;
        uint32_t declaredIndex = std::min(operandIndex, numberOfNamedOperands);
        if (declaredIndex < numberOfDeclaredOperands ? writtenThrough[declaredIndex] : writesThroughDeclaredOperandInKnownWriters(writer.opcode, declaredIndex, numberOfDeclaredOperands))
            written.append(writer.operand(operandIndex));
    }
    return written;
}

namespace {

struct GuardsEndedInKnownWriters {
    enum class Which : uint8_t { None, OfClassesInRowOf, UnderNameOrNone, All };

    Which which { Which::All };
    LayoutID classLayout;
    PropertyKeyID name;
};

} // anonymous namespace

static bool rowAboveHoldsInKnownWriters(const Walker& walker, LayoutID from, LayoutID sought)
{
    const Program& program = walker.program();
    Vector<LayoutID, 8> toLookAt { from };
    size_t numberOfLooksLeft = program.layouts.size();
    while (!toLookAt.isEmpty()) {
        LayoutID current = toLookAt.takeLast();
        if (declaredLayoutOf(program, current) == sought)
            return true;
        if (!numberOfLooksLeft--)
            return true;
        const Layout& layout = program.at(current);
        ClassFacts facts = walker.facts().classOf(current);
        for (LayoutID above : { layout.base, writtenBaseOf(layout), facts.base, facts.prototypeBase }) {
            if (above && above != current && !toLookAt.contains(above))
                toLookAt.append(above);
        }
    }
    return false;
}

static GuardsEndedInKnownWriters guardsEndedByInKnownWriters(const Walker& walker, const Draft& writer)
{
    using Which = GuardsEndedInKnownWriters::Which;
    if (testsItsOwnGuess(writer))
        return { };
    const Program& program = walker.program();
    if (writer.opcode == Opcode::NewObject) {
        LayoutID classLayout = declaredLayoutOf(program, writer.layout());
        if (writer.prototypeOperand())
            return program.prototypeMaySelectOtherClass(writer.layout()) ? GuardsEndedInKnownWriters { } : GuardsEndedInKnownWriters { Which::OfClassesInRowOf, classLayout, { } };
        if (program.at(classLayout).has(LayoutFlags::IsConstructorFunction) && !walker.state().guardsHaveBegun(classLayout))
            return { Which::OfClassesInRowOf, classLayout, { } };
        return { Which::None, { }, { } };
    }
    if (writer.opcode == Opcode::BeginMemberGuards && walker.state().guardsHaveBegun(declaredLayoutOf(program, writer.layout())))
        return { Which::None, { }, { } };
    if (writer.opcode == Opcode::NewClass || writer.opcode == Opcode::DefineMethod || writer.opcode == Opcode::BeginMemberGuards)
        return { Which::OfClassesInRowOf, declaredLayoutOf(program, writer.layout()), { } };
    if (writer.opcode == Opcode::PutSlotOrAdd)
        return writer.orderWordAfter() ? GuardsEndedInKnownWriters { Which::None, { }, { } } : GuardsEndedInKnownWriters { Which::UnderNameOrNone, { }, writer.key() };
    return { };
}

static NEVER_INLINE bool writesFreshObjectsOfTurnAloneInKnownWriters(Walker& walker, const Draft& draft, const Effects& effects)
{
    Effects beside = effects;
    beside.writes.remove(perObjectHeaps);
    if (!beside.mayBeRunAgainUnseen())
        return false;
    std::optional<ObjectsWrittenAloneInKnownWriters> written = objectsWrittenAloneByInKnownWriters(walker, draft, effects, findElementStoreWithoutOtherCodeInKnownWriters(draft, effects));
    return written && std::ranges::all_of(*written, [&](Reference object) {
        Reference root = rootInKnownWriters(walker.code(), object);
        return walker.state().isFresh(root) && walker.isBuiltInTurnThatMayBeginAgain(root);
    });
}

static ALWAYS_INLINE bool writesObjectsOfTurnAloneInKnownWriters(Walker& walker, const Draft& draft, const Effects& effects)
{
    if (effects.mayRunOtherCode() || !effects.writes.containsAny(perObjectHeaps) || !walker.mayBeginTurnAgainInClosedCopy())
        return false;
    return writesFreshObjectsOfTurnAloneInKnownWriters(walker, draft, effects);
}

static constexpr AbstractHeapSet heapsEndedByWriterInKnownWriters = perObjectHeaps | AbstractHeapSet { AbstractHeap::MemberGuards };
static constexpr uint64_t objectsOfAnyLayoutInKnownWriters = UINT64_MAX;

static NEVER_INLINE void endWhatWriterWritesInKnownWriters(Walker& walker, const Draft& draft, const Effects& effects)
{
    WalkState& state = walker.state();
    Effects ended = effects;
    GuardsEndedInKnownWriters guards;
    if (effects.writes.contains(AbstractHeap::MemberGuards)) {
        guards = guardsEndedByInKnownWriters(walker, draft);
        if (guards.which == GuardsEndedInKnownWriters::Which::None)
            ended.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
    }
    auto mayEndGuard = [&](LayoutID classOfGuard, PropertyKeyID nameOfGuard) {
        if (guards.which == GuardsEndedInKnownWriters::Which::UnderNameOrNone)
            return !nameOfGuard || nameOfGuard == guards.name;
        if (guards.which != GuardsEndedInKnownWriters::Which::OfClassesInRowOf)
            return true;
        return rowAboveHoldsInKnownWriters(walker, classOfGuard, guards.classLayout) || rowAboveHoldsInKnownWriters(walker, guards.classLayout, declaredLayoutOf(walker.program(), classOfGuard));
    };
    if (!ended.writes.containsAny(perObjectHeaps)) {
        auto noObjectIsWritten = [](Reference) {
            return false;
        };
        state.invalidate(ended, noObjectIsWritten, mayEndGuard);
        return;
    }

    const Code& code = walker.code();
    std::optional<ElementStoreInKnownWriters> elementStore = findElementStoreWithoutOtherCodeInKnownWriters(draft, effects);
    std::optional<ObjectsWrittenAloneInKnownWriters> written = objectsWrittenAloneByInKnownWriters(walker, draft, effects, elementStore);
    uint64_t layoutsWritten = !written && effectRuleOf(draft.opcode) == EffectRule::Call ? layoutsWrittenBy(walker, draft) : objectsOfAnyLayoutInKnownWriters;

    bool everyObjectWrittenIsFresh = written && std::ranges::all_of(*written, [&](Reference object) {
        return state.isFresh(rootInKnownWriters(code, object));
    });
    if (everyObjectWrittenIsFresh) {
        for (Reference object : *written)
            state.answers().endWhatIsWrittenIn(rootInKnownWriters(code, object), ended);
        ended.writes.remove(perObjectHeaps);
        auto noOtherObjectIsWritten = [](Reference) {
            return false;
        };
        state.invalidate(ended, noOtherObjectIsWritten, mayEndGuard);
    } else {
        Vector<Reference, 4> freshOperands;
        if (!written) {
            for (Reference operand : draft.operands()) {
                Reference root = rootInKnownWriters(code, operand);
                if (root.isValue() && state.isFresh(root))
                    freshOperands.append(root);
            }
        }
        if (written) {
            for (Reference object : *written) {
                if (Reference root = rootInKnownWriters(code, object); state.isFresh(root))
                    state.answers().endWhatIsWrittenIn(root, ended);
            }
        }
        for (Reference root : freshOperands)
            state.answers().endWhatIsWrittenIn(root, ended);
        auto mayBeWritten = [&](Reference holder) {
            if (TypeID type = walker.typeOf(holder); type && walker.types().at(type).isOpaque(OpaqueKind::Enumerator))
                return true;
            if (written) {
                return std::ranges::any_of(*written, [&](Reference object) {
                    return mayBeSameObject(walker, object, holder);
                });
            }
            if (Reference root = rootInKnownWriters(code, holder); state.isFresh(root))
                return freshOperands.contains(root);
            if (layoutsWritten == objectsOfAnyLayoutInKnownWriters)
                return true;
            TypeID type = walker.typeOf(holder);
            return !type || (bitsOfLayoutsOf(walker, type) & layoutsWritten);
        };
        state.invalidate(ended, mayBeWritten, mayEndGuard);
    }
}

static void endWhatOperationWritesOrRunsInKnownWriters(Walker& walker, const Draft& draft, const Effects& effects)
{
    WalkState& state = walker.state();
    if (!writesObjectsOfTurnAloneInKnownWriters(walker, draft, effects))
        state.noteWhatOperationDoes(effects);
    if (draft.opcode == Opcode::ReleaseEnvironment)
        state.noteSomethingWasDoneInTurn();
    if (!walker.has(Mechanism::TypedOperations) || effects.mayRunOtherCode() || !effects.writes.containsAny(heapsEndedByWriterInKnownWriters) || state.isEmpty()) {
        state.invalidate(effects);
        return;
    }
    endWhatWriterWritesInKnownWriters(walker, draft, effects);
}

static void noteThatGuardsOfRowHaveBegunInKnownWriters(Walker& walker, LayoutID classLayout)
{
    const Program& program = walker.program();
    do {
        walker.state().noteThatGuardsHaveBegun(classLayout);
        classLayout = parentNamedByClassRecord(program, walker.facts(), classLayout).layout;
    } while (classLayout && program.at(classLayout).has(LayoutFlags::IsConstructorFunction) && walker.plan().hasClassRecord(classLayout) && !walker.state().guardsHaveBegun(classLayout));
}

bool writerEndsNoGuard(const Walker& walker, const Draft& writer)
{
    return guardsEndedByInKnownWriters(walker, writer).which == GuardsEndedInKnownWriters::Which::None;
}

void endWhatOperationWritesOrRuns(Walker& walker, const Draft& draft, const Effects& effects)
{
    endWhatOperationWritesOrRunsInKnownWriters(walker, draft, effects);
    if (draft.opcode == Opcode::BeginMemberGuards)
        noteThatGuardsOfRowHaveBegunInKnownWriters(walker, declaredLayoutOf(walker.program(), draft.layout()));
    static_assert(!declaredBoundOf(Opcode::PutSlot).writes.contains(AbstractHeap::NamedProperties) && !declaredBoundOf(Opcode::PutSlotOrAdd).writes.contains(AbstractHeap::NamedProperties));
    if ((draft.opcode != Opcode::PutSlot && draft.opcode != Opcode::PutSlotOrAdd) || testsItsOwnGuess(draft) || !effects.writes.contains(AbstractHeap::NamedProperties))
        return;
    const Code& code = walker.code();
    for (Reference name = draft.objectOperand(); name.isValue(); name = Reference::value(code.at(name.id()).operand(0))) {
        walker.state().endFactsOfCell(name, FactOfCell::BearsNoMark);
        if (!opcodesThatGiveSameCell.contains(code.at(name.id()).opcode))
            break;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
