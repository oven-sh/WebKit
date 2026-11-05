/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations: an operation that runs no other code ends what it writes in the objects that it writes it in, and the guards of the classes that it names. Every other object and class keeps what is known of it.
// At a suspension, and wherever other code may run: WalkState::invalidate(effects), as ever.
// At a throw: a writer that is judged here and throws has done no more than one that returns, so what is left standing holds on the edge to a handler too.
// Withheld: WalkState::invalidate(effects), by heap, for every holder that is not fresh.

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTCodeRole.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// ---- WalkState::endWhatIsWritten() with two questions: same lists, same costs

// WHAT IS JUDGED HERE, by holder or by class. Whatever else an operation writes goes to WalkState::endWhatIsWritten(), by heap, as ever: a variable, the bytes under a view, the entries of a collection,
// and whatever another file teaches WalkState to rest on such a heap.
static constexpr AbstractHeapSet heapsOfObjectsJudgedInKnownWriters {
    AbstractHeap::ObjectFields, AbstractHeap::EnvironmentFields, AbstractHeap::ShapeWords, AbstractHeap::NamedProperties, AbstractHeap::OrderWords, AbstractHeap::PrototypeLinks, AbstractHeap::Elements, AbstractHeap::ArrayStorage
};
static constexpr AbstractHeapSet heapsJudgedInKnownWriters = heapsOfObjectsJudgedInKnownWriters | AbstractHeapSet { AbstractHeap::MemberGuards };
static constexpr AbstractHeapSet heapsUnderElementsOfObjectsInKnownWriters { AbstractHeap::Elements, AbstractHeap::ArrayStorage };
static_assert(perObjectHeaps.containsAll(heapsOfObjectsJudgedInKnownWriters));
static_assert(!perObjectHeaps.contains(AbstractHeap::MemberGuards));

static constexpr OptionSet<FactOfCell> factsOfCellKnownInKnownWriters {
    FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot, FactOfCell::NoExtraProperties, FactOfCell::HasLayoutPrototype, FactOfCell::OrderWordIsKnown, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed
};

// factsOfCellEndedInWalkState(). A fact that another file has added since rests on what this file does not know: it ends with any write into its holder.
static OptionSet<FactOfCell> factsOfCellEndedInKnownWriters(const LocationMasks& writes)
{
    OptionSet<FactOfCell> ended = OptionSet<FactOfCell>::fromRaw(static_cast<OptionSet<FactOfCell>::StorageType>(~factsOfCellKnownInKnownWriters.toRaw()));
    if (writes.contains(AbstractHeap::ShapeWords))
        ended.add({ FactOfCell::NotFrozen, FactOfCell::OrderWordIsKnown, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed });
    if (writes.contains(AbstractHeap::NamedProperties))
        ended.add({ FactOfCell::NoFieldOutsideSlot, FactOfCell::NoExtraProperties, FactOfCell::OrderWordIsKnown, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed });
    if (writes.contains(AbstractHeap::OrderWords))
        ended.add({ FactOfCell::OrderWordIsKnown, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed });
    if (writes.contains(AbstractHeap::PrototypeLinks))
        ended.add(FactOfCell::HasLayoutPrototype);
    return ended;
}

void WalkState::invalidate(const Effects& effects, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard)>& mayEndGuard)
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
    if (!holdsNoRecord() && !effects.writes.isEmpty())
        heldToWrite().endWhatIsWritten(effects, mayBeWritten, mayEndGuard);
}

void WalkState::Held::endWhatIsWritten(const Effects& effects, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard)>& mayEndGuard)
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
    // As wide as the member is, however many kinds there are by now.
    using KindsOfPlaces = decltype(Holder::kindsOfPlaces);
    auto bitOfKind = [](PlaceKind kind) {
        return static_cast<KindsOfPlaces>(KindsOfPlaces(1) << static_cast<unsigned>(kind));
    };
    // A variable rests on a heap that is not judged here. A KIND OF PLACE THAT ANOTHER FILE HAS ADDED SINCE rests on what this file does not know: where it is a part of the holder, it ends with any write into the holder.
    KindsOfPlaces kindsThatRestOnWhatIsKnown = bitOfKind(PlaceKind::Slot) | bitOfKind(PlaceKind::Variable) | bitOfKind(PlaceKind::EnvironmentField) | bitOfKind(PlaceKind::Element) | bitOfKind(PlaceKind::GuardOfStores) | bitOfKind(PlaceKind::GuardOfReads);
    // WalkState::endPlacesOf().
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
        // Asked once, and only where something of the holder lies in what is written. On no object hang the variables and the guards.
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
                    ends = (guardsWritten & place.mask()) && mayEndGuard(LayoutID(place.first));
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
            endHolder(holder); // Another stands in its place now.
    }
    m_factsOfCellReachedByEffects = factsLeft;
    m_kindsOfPlacesReachedByEffects = kindsOfPlacesLeft;
    m_maskOfPlacesReachedByEffects = maskOfPlacesLeft;
    dropEndedRecordsIfMany();
}

// ---- K6: who may be who

static Reference rootInKnownWriters(const Code& code, Reference name)
{
    if (!name.isValue())
        return name;
    ValueID root = rootOfCellInSweptCode(code, name.id());
    return code.isConstant(root) ? Reference::constant(root) : Reference::value(root);
}

// When it has run, its result is a cell that was not there before. TruncateNewArray has the rule of an allocation and hands its operand on. A template object is one cell for each site, and so is a RegExp that is never told apart.
// A call of a function of which Link says that each return hands over a cell that the call has made and that nothing else holds. What a coroutine returns goes to whoever steps it.
static bool makesNewCellInKnownWriters(const Walker& walker, const Operation& made)
{
    Opcode opcode = made.opcode;
    if (opcode == Opcode::CallDirect) {
        if (!returnsResultOfFunction(made.role()) || walker.program().at(made.function()).isCoroutine())
            return false;
        return walker.facts().linked().function(made.function()).resultIsFresh;
    }
    if (resultRuleOf(opcode) != ResultRule::Allocation || boundToOf(opcode) == BoundTo::Site || made.traits().mayRunOtherCode())
        return false;
    if (opcode == Opcode::NewRegExp && made.isNeverToldApart())
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

    // Both values are in sight at one point, so the definition of one dominates that of the other, and the value of the one that comes first was there when the cell was made. In a loop it holds at most the cell that an earlier turn has made.
    // Numbers follow that order but for what is built in front of a loop, which bears a later number than it should. For `other` that only loses the answer, and a cell is never made there: an allocation writes.
    auto isMadeBehind = [&](Reference cell, Reference other) {
        if (!cell.isValue() || !makesNewCellInKnownWriters(walker, code.at(cell.id())))
            return false;
        return other.isConstant() || other.id().index() < cell.id().index();
    };
    if (isMadeBehind(first, second) || isMadeBehind(second, first))
        return false;

    TypeID typeOfFirst = walker.typeOf(first);
    TypeID typeOfSecond = walker.typeOf(second);
    if (typeOfFirst && typeOfSecond && !walker.types().mayBeSameObject(typeOfFirst, typeOfSecond))
        return false;

    // A fresh object lies in no cell and in no variable, and has no name that the state does not follow. So whatever else is read, or handed back by a call, is another object.
    WalkState& state = walker.state();
    return !state.isFresh(first) && !state.isFresh(second);
}

// ---- K6: through which operands an operation writes

namespace {

using ObjectsWrittenAloneInKnownWriters = Vector<Reference, 2>;

struct ElementStoreInKnownWriters {
    Reference array;
};

} // anonymous namespace

static constexpr OperandUses usesThatCallOperandInKnownWriters { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly };
static constexpr OperandUses usesThatWriteOperandInKnownWriters {
    OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::ChangesIntegrity
};
// Whatever does one of these may write a cell that is no operand: what stands above or below the operand, what it holds, what code of the program has at hand.
static constexpr OperandUses usesThatReachBeyondOperandInKnownWriters {
    OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype, OperandUse::PassesToUserCode, OperandUse::AppliesTransitively, OperandUse::AppliesToContents
};

static uint32_t numberOfDeclaredOperandsInKnownWriters(Opcode opcode)
{
    return numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
}

static bool opcodeMayRunCodeOfOperandsInKnownWriters(Opcode opcode)
{
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperandsInKnownWriters(opcode); ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (!info.protocols.isEmpty() || info.uses.containsAny(usesThatCallOperandInKnownWriters))
            return true;
    }
    return false;
}

static bool writesThroughDeclaredOperandInKnownWriters(Opcode opcode, uint32_t declaredIndex)
{
    if (operandUsesOf(opcode, declaredIndex).containsAny(usesThatWriteOperandInKnownWriters))
        return true;
    Access access = accessOf(opcode);
    if ((access == Access::Store || access == Access::Define) && receiverOf(opcode) == declaredIndex)
        return true;
    for (uint32_t retainedIndex = 0; retainedIndex < numberOfDeclaredOperandsInKnownWriters(opcode); ++retainedIndex) {
        if (retainedInOf(opcode, retainedIndex) == RetainedIn::Operand && retainingOperandOf(opcode, retainedIndex) == declaredIndex)
            return true;
    }
    return false;
}

// An opcode that names no operand that it writes through writes a cell that it finds by itself.
static bool writesThroughDeclaredOperandsAloneInKnownWriters(Opcode opcode)
{
    if (opcodeMayRunCodeOfOperandsInKnownWriters(opcode))
        return false;
    bool namesWrittenOperand = false;
    for (uint32_t declaredIndex = 0; declaredIndex < numberOfDeclaredOperandsInKnownWriters(opcode); ++declaredIndex) {
        if (operandUsesOf(opcode, declaredIndex).containsAny(usesThatReachBeyondOperandInKnownWriters))
            return false;
        namesWrittenOperand |= writesThroughDeclaredOperandInKnownWriters(opcode, declaredIndex);
    }
    return namesWrittenOperand;
}

// The one object that the writer defines a property on. `effects` say that it runs no other code. Null: it may reach anything.
// DefineMethod, NewClass and BeginMemberGuards are not among them: they write the prototype object of a class, which is no operand of theirs.
static Reference onlyObjectDefinedOnInKnownWriters(Walker& walker, const Draft& writer)
{
    static constexpr OpcodeSet definitionsOnOneObject { Opcode::DefineGetter, Opcode::DefineSetter, Opcode::DefineAccessors, Opcode::PropDefine };
    if (definitionsOnOneObject.contains(writer.opcode))
        return writer.objectOperand();
    // A compiled object is defined on as an ordinary one. Anything else may be a proxy.
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

// `counts[k] = 1; out.push(k)`, `p.z = 1; o.c`: whatever the writer writes of the heaps that belong to one object, it writes in these. Nothing: nobody can tell.
static std::optional<ObjectsWrittenAloneInKnownWriters> objectsWrittenAloneByInKnownWriters(Walker& walker, const Draft& writer, const Effects& effects, const std::optional<ElementStoreInKnownWriters>& elementStore)
{
    // What a callee writes it may find through what it captures.
    if (testsItsOwnGuess(writer) || effectRuleOf(writer.opcode) == EffectRule::Call)
        return std::nullopt;
    // An object remembers that it has been a prototype.
    if (writer.opcode == Opcode::SetLiteralPrototype)
        return ObjectsWrittenAloneInKnownWriters { writer.objectOperand(), writer.prototypeOperand() };
    if (writer.opcode == Opcode::TryCopyFields)
        return ObjectsWrittenAloneInKnownWriters { writer.objectOperand() };
    if (Reference object = onlyObjectDefinedOnInKnownWriters(walker, writer))
        return ObjectsWrittenAloneInKnownWriters { object };
    if (elementStore)
        return ObjectsWrittenAloneInKnownWriters { elementStore->array };
    if (!writesThroughDeclaredOperandsAloneInKnownWriters(writer.opcode))
        return std::nullopt;
    UNUSED_PARAM(effects);

    ObjectsWrittenAloneInKnownWriters written;
    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(writer.opcode);
    for (uint32_t operandIndex = 0; operandIndex < writer.numberOfOperands(); ++operandIndex) {
        if (writer.operand(operandIndex) && writesThroughDeclaredOperandInKnownWriters(writer.opcode, std::min(operandIndex, numberOfNamedOperands)))
            written.append(writer.operand(operandIndex));
    }
    return written;
}

// ---- K8: the guards of which classes a writer may end

namespace {

struct GuardsEndedInKnownWriters {
    enum class Which : uint8_t { None, OfClassesInRowOf, All };

    Which which { Which::All };
    LayoutID classLayout;
};

} // anonymous namespace

// By every word that says what stands above a class: whoever answers for what stands above an object goes up by one of them (findParentNamedByClassRecordForTypedAccess()).
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

// A test that has passed goes on passing.
static GuardsEndedInKnownWriters guardsEndedByInKnownWriters(const Walker& walker, const Draft& writer)
{
    using Which = GuardsEndedInKnownWriters::Which;
    if (testsItsOwnGuess(writer))
        return { };
    const Program& program = walker.program();
    if (writer.opcode == Opcode::NewObject) {
        LayoutID classLayout = declaredLayoutOf(program, writer.layout());
        // The guards of a class speak of every instance, so the first instance with another prototype ends them.
        if (writer.prototypeOperand())
            return program.prototypeMaySelectOtherClass(writer.layout()) ? GuardsEndedInKnownWriters { } : GuardsEndedInKnownWriters { Which::OfClassesInRowOf, classLayout };
        // The class of a constructor function begins with its first object. Until then a guard takes what it says of that class to speak of no object, so the first object may end it.
        if (program.at(classLayout).has(LayoutFlags::IsConstructorFunction))
            return { Which::OfClassesInRowOf, classLayout };
        return { Which::None, { } };
    }
    if (writer.opcode == Opcode::NewClass || writer.opcode == Opcode::DefineMethod || writer.opcode == Opcode::BeginMemberGuards)
        return { Which::OfClassesInRowOf, declaredLayoutOf(program, writer.layout()) };
    return { };
}

// `function Below(a, b) { Base.call(this, a); this.b = b; }`, copied into a turn: a store into an object that this very turn has made is gone with the object where the turn begins again.
static bool writesObjectsOfTurnAloneInKnownWriters(Walker& walker, const Draft& draft, const Effects& effects)
{
    if (effects.mayRunOtherCode() || !effects.writes.containsAny(perObjectHeaps) || !walker.mayBeginTurnAgainInClosedCopy())
        return false;
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

// ---- What build() asks

static constexpr AbstractHeapSet heapsEndedByWriterInKnownWriters = perObjectHeaps | AbstractHeapSet { AbstractHeap::MemberGuards };
// objectsOfAnyLayout of facts/AOTLinkKnownEffects.h.
static constexpr uint64_t objectsOfAnyLayoutInKnownWriters = UINT64_MAX;

void endWhatOperationWrites(Walker& walker, const Draft& draft, const Effects& effects)
{
    WalkState& state = walker.state();
    // The effects as they are: below, what is written into a fresh object is taken out of them, and an object that is fresh since in front of the loop would be written twice.
    if (!writesObjectsOfTurnAloneInKnownWriters(walker, draft, effects))
        state.noteWhatOperationDoes(effects);
    // What hands memory back does it once, though nobody sees what it writes: the other copy would be handed a released cell.
    if (draft.opcode == Opcode::ReleaseEnvironment)
        state.noteSomethingWasDoneInTurn();
    if (!walker.has(Mechanism::TypedOperations) || effects.mayRunOtherCode() || !effects.writes.containsAny(heapsEndedByWriterInKnownWriters) || state.isEmpty()) {
        state.invalidate(effects);
        return;
    }

    Effects ended = effects;
    GuardsEndedInKnownWriters guards;
    if (effects.writes.contains(AbstractHeap::MemberGuards)) {
        guards = guardsEndedByInKnownWriters(walker, draft);
        if (guards.which == GuardsEndedInKnownWriters::Which::None)
            ended.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
    }
    auto mayEndGuard = [&](LayoutID classOfGuard) {
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

    // `const n = new Node(); n.left = a; n.right = b`, a thousand times over: nothing else can be a fresh object, so nobody else is asked, however much the state holds.
    // What WalkState itself knows of a fresh object no effect ends: markEscapingOperands() has followed the store.
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
        // An operand that is fresh and stays so: markEscapingOperands() has followed a store into it, of which the answers know nothing.
        Vector<Reference, 4> freshOperands;
        if (!written) {
            for (Reference operand : draft.operands()) {
                Reference root = rootInKnownWriters(code, operand);
                if (root.isValue() && state.isFresh(root))
                    freshOperands.append(root);
            }
        }
        // What rests on a fresh object is passed by below, and nothing else can be that object.
        if (written) {
            for (Reference object : *written) {
                if (Reference root = rootInKnownWriters(code, object); state.isFresh(root))
                    state.answers().endWhatIsWrittenIn(root, ended);
            }
        }
        for (Reference root : freshOperands)
            state.answers().endWhatIsWrittenIn(root, ended);
        auto mayBeWritten = [&](Reference holder) {
            // What is noted of the enumerator of a for-in speaks of the keys of other objects.
            if (TypeID type = walker.typeOf(holder); type && walker.types().at(type).isOpaque(OpaqueKind::Enumerator))
                return true;
            if (written) {
                return std::ranges::any_of(*written, [&](Reference object) {
                    return mayBeSameObject(walker, object, holder);
                });
            }
            // Whatever reaches beyond its operands finds no fresh object.
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
