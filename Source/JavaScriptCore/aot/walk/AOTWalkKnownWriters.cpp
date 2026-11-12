/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTCallPlan.h"
#include "AOTCodeRole.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalkListReadsInLoops.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsOfObjectsJudgedInKnownWriters {
    AbstractHeap::ObjectFields, AbstractHeap::EnvironmentFields, AbstractHeap::ShapeWords, AbstractHeap::NamedProperties, AbstractHeap::OrderWords, AbstractHeap::PrototypeLinks, AbstractHeap::Elements, AbstractHeap::ArrayStorage
};
static constexpr AbstractHeapSet heapsJudgedInKnownWriters = heapsOfObjectsJudgedInKnownWriters | AbstractHeapSet { AbstractHeap::MemberGuards };
static constexpr AbstractHeapSet heapsUnderElementsOfObjectsInKnownWriters { AbstractHeap::Elements, AbstractHeap::ArrayStorage };
static_assert(perObjectHeaps.containsAll(heapsOfObjectsJudgedInKnownWriters));
static_assert(!perObjectHeaps.contains(AbstractHeap::MemberGuards));

static constexpr OptionSet<CellFact> handledCellFacts {
    CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot, CellFact::NoExtraProperties, CellFact::HasLayoutPrototype, CellFact::OrderWordIsKnown, CellFact::IsLikelyAsAllocated, CellFact::IsLikelyAsConstructed, CellFact::EnumeratorIsCurrent, CellFact::BearsNoMark
};

static_assert(handledCellFacts.toRaw() == (1u << numberOfCellFacts) - 1);

void WalkState::invalidate(const Effects& effects, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard, PropertyKeyID nameOfGuard)>& mayEndGuard, const MemoryLocation* writtenAlone, const WriterOfOneObject* writerOfOneObject, const FreshCellWriter* freshCellWriter)
{
    if (effects.mayRunOtherCode()) {
        invalidate(effects);
        return;
    }
    updateRestartMark(effects);

    auto endsAnswer = [&](const KnownAnswer& answer, bool isWrittenOutsideObjects) {
        return isWrittenOutsideObjects || !answer.holder || mayBeWritten(answer.holder);
    };
    m_answers.endWhatIsWritten(effects, endsAnswer);
    if (!holdsNoRecord() && !effects.writes.isEmpty()) {
        heldToWrite().endWhatIsWritten(m_program, effects, mayBeWritten, mayEndGuard, writtenAlone, writerOfOneObject, freshCellWriter);
        noteKindsOfRecordsLeft();
    }
}

void WalkState::Held::endWhatIsWritten(const ProgramOfState& program, const Effects& effects, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard, PropertyKeyID nameOfGuard)>& mayEndGuard, const MemoryLocation* writtenAlone, const WriterOfOneObject* writerOfOneObject, const FreshCellWriter* freshCellWriter)
{
    const LocationMasks& writes = effects.writes;
    if (!writes.containsOnly(heapsJudgedInKnownWriters)) {
        Effects judgedByHeap = effects;
        judgedByHeap.writes.remove(heapsJudgedInKnownWriters);
        endWhatIsWritten(program, judgedByHeap, writtenAlone);
        if (holdsNoRecord())
            return;
    }

    bool writesIntoObjects = writes.containsAny(heapsOfObjectsJudgedInKnownWriters);
    bool endsShapes = writes.contains(AbstractHeap::ShapeWords) && m_numberOfShapes;
    bool endsPlaces = writes.containsAny(heapsJudgedInKnownWriters) && m_numberOfPlaces;
    OptionSet<CellFact> factsEnded = writesIntoObjects && m_numberOfFactHolders ? cellFactsInvalidatedBy(writes) : OptionSet<CellFact>();
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
        || (m_reachedFactHolderCount && m_cellFactsReachedByEffects.containsAny(factsEnded))
        || (m_numberOfPlacesReachedByEffects && reachesPlaces)
        || (endsElements && m_numberOfElementsReachedByEffects);
    if (!reachesSomething) {
        dropEndedRecordsIfMany();
        return;
    }
    if (goesByLists()) [[unlikely]] {
        endByLists({ .shapes = endsShapes, .elements = endsElements, .facts = factsEnded, .slots = slotsWritten, .environmentFields = environmentFieldsWritten, .guards = guardsWritten,
            .mayBeWritten = &mayBeWritten, .mayEndGuard = &mayEndGuard, .writerOfOneObject = writerOfOneObject, .freshCellWriter = freshCellWriter, .program = &program, .writtenAlone = writtenAlone });
        dropEndedRecordsIfMany();
        return;
    }

    OptionSet<CellFact> factsLeft;
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
        if (m_holders[holder].cellFacts.containsAny(factsEnded) && isWritten())
            invalidateCellFacts(holder, factsEnded);

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
            KindsOfPlaces remainingKinds = 0;
            uint64_t remainingMask = 0;
            endPlacesWhere(holder, false, [&](const Place& place) {
                bool ends = false;
                if (place.kind == PlaceKind::Slot)
                    ends = (slotsWritten & place.mask()) && !liesBesideWhatIsWritten(program, place, slotsWritten, writtenAlone) && isWritten();
                else if (place.kind == PlaceKind::EnvironmentField)
                    ends = (environmentFieldsWritten & place.mask()) && !liesBesideWhatIsWritten(program, place, environmentFieldsWritten, writtenAlone) && isWritten();
                else if (place.kind == PlaceKind::GuardOfStores || place.kind == PlaceKind::GuardOfReads)
                    ends = (guardsWritten & place.mask()) && mayEndGuard(classOfGuardedSide(place.first), PropertyKeyID::fromBits(place.second));
                else if (!(bitOfKind(place.kind) & kindsThatRestOnWhatIsKnown))
                    ends = writesIntoObjects && isWritten();
                if (ends)
                    return true;
                remainingKinds |= bitOfKind(place.kind);
                remainingMask |= place.mask();
                return false;
            });
            Holder& changed = m_holders.mutableAt(holder);
            changed.kindsOfPlaces = remainingKinds;
            changed.maskOfPlaces = remainingMask;
        }
        if (endsElements && m_holders[holder].firstElement != none && isWritten())
            endPlacesWhere(holder, true, endsAlways);

        const Holder& record = m_holders[holder];
        if (record.shape || !record.cellFacts.isEmpty() || record.firstPlace != none || record.firstElement != none) {
            factsLeft.add(record.cellFacts);
            kindsOfPlacesLeft |= record.kindsOfPlaces;
            maskOfPlacesLeft |= record.maskOfPlaces;
            ++index;
        } else
            endHolder(holder);
    }
    m_cellFactsReachedByEffects = factsLeft;
    m_kindsOfPlacesReachedByEffects = kindsOfPlacesLeft;
    m_maskOfPlacesReachedByEffects = maskOfPlacesLeft;
    dropEndedRecordsIfMany();
}

static Reference rootInKnownWriters(const Code& code, Reference name)
{
    if (!name.isValue())
        return name;
    ValueID root = underlyingCellOf(code, name.id());
    return code.isConstant(root) ? Reference::constant(root) : Reference::value(root);
}

bool allocatesNewCell(const Operation& made)
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

bool isOfLayoutAlone(Walker& walker, Reference object, LayoutID layout)
{
    TypeID id = object.isRecord() ? walker.typeOf(object) : TypeID();
    if (!id)
        return false;
    const Type& type = walker.types().at(id);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return false;
    return singleLayoutOf(walker.types(), type) == layout;
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
        if (!cell.isValue() || !allocatesNewCell(code.at(cell.id())))
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

static ALWAYS_INLINE bool writesThroughDeclaredOperandInKnownWriters(Opcode opcode, uint32_t declaredIndex, uint32_t numberOfDeclaredOperands)
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
    static constexpr OpcodeSet definitionsOnOneObject { Opcode::DefineGetter, Opcode::DefineSetter, Opcode::DefineAccessors, Opcode::PropDefine, Opcode::ElemDefine };
    if (definitionsOnOneObject.contains(writer.opcode))
        return writer.objectOperand();
    if (writer.opcode == Opcode::DefineOwnProperty) {
        TypeID type = walker.typeOf(writer.objectOperand());
        return type && walker.types().at(type).hasOnlyLayouts() ? writer.objectOperand() : Reference();
    }
    return { };
}

static ALWAYS_INLINE std::optional<ElementStoreInKnownWriters> findElementStoreWithoutOtherCodeInKnownWriters(const Draft& writer, const Effects& effects)
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
    if (writesReceiverArrayAlone(walker, writer, effects) && walker.state().isFresh(rootInKnownWriters(walker.code(), writer.receiverOperand())))
        return ObjectsWrittenAloneInKnownWriters { writer.receiverOperand() };
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

static bool isOnlyClosureOfClassWhoseGuardsHaveBegunInKnownWriters(const Walker& walker, LayoutID made, TypeID newTarget)
{
    const Program& program = walker.program();
    LayoutID classLayout = declaredLayoutOf(program, made);
    AccessContext context { program, WalkFacts(walker.facts()), walker.plan(), walker.types(), walker.currentFunction() };
    if (!newTarget || (!walker.state().guardsHaveBegun(classLayout) && !walker.shouldIgnorePrototypeReads()) || !isConstructorPrototypeGuarded(context, classLayout))
        return false;
    Type type = walker.types().at(newTarget);
    return type.isObject() && type.hasOnlyClosures() && singleFunctionOf(walker.types(), type) == program.at(classLayout).constructor;
}

static bool liesBelowPrototypeReadBehindGuardsInKnownWriters(const Walker& walker, const Draft& birth)
{
    const Code& code = walker.code();
    Reference prototype = rootInKnownWriters(code, birth.prototypeOperand());
    if (!prototype.isValue())
        return false;
    const Operation& read = code.at(prototype.id());
    const Program& program = walker.program();
    if (read.opcode != Opcode::GetConstructPrototype || declaredLayoutOf(program, read.layout()) != declaredLayoutOf(program, birth.layout()) || !read.newTargetOperand())
        return false;
    return isOnlyClosureOfClassWhoseGuardsHaveBegunInKnownWriters(walker, birth.layout(), code.at(read.newTargetOperand()).type);
}

static GuardsEndedInKnownWriters guardsEndedByInKnownWriters(const Walker& walker, const Draft& writer)
{
    using Which = GuardsEndedInKnownWriters::Which;
    if (testsItsOwnGuess(writer))
        return { };
    const Program& program = walker.program();
    if (writer.opcode == Opcode::GetConstructPrototype && isOnlyClosureOfClassWhoseGuardsHaveBegunInKnownWriters(walker, writer.layout(), walker.typeOf(writer.newTargetOperand())))
        return { Which::None, { }, { } };
    if (writer.opcode == Opcode::NewObject) {
        LayoutID classLayout = declaredLayoutOf(program, writer.layout());
        if (writer.prototypeOperand() && !program.prototypeMaySelectOtherClass(writer.layout()) && liesBelowPrototypeReadBehindGuardsInKnownWriters(walker, writer))
            return { Which::None, { }, { } };
        if (writer.prototypeOperand())
            return program.prototypeMaySelectOtherClass(writer.layout()) ? GuardsEndedInKnownWriters { } : GuardsEndedInKnownWriters { Which::OfClassesInRowOf, classLayout, { } };
        if (program.at(classLayout).has(LayoutFlags::IsConstructorFunction) && !walker.state().guardsHaveBegun(classLayout))
            return { Which::OfClassesInRowOf, classLayout, { } };
        return { Which::None, { }, { } };
    }
    if (writer.opcode == Opcode::PropGet && !writer.isFreeName() && readsStaticMethod(walker, writer))
        return { Which::None, { }, { } };
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
        return walker.state().isFresh(root) && walker.isBuiltInRestartableIteration(root);
    });
}

static ALWAYS_INLINE bool writesObjectsOfTurnAloneInKnownWriters(Walker& walker, const Draft& draft, const Effects& effects)
{
    if (effects.mayRunOtherCode() || !effects.writes.containsAny(perObjectHeaps) || !walker.canRestartIteration())
        return false;
    return writesFreshObjectsOfTurnAloneInKnownWriters(walker, draft, effects);
}

static std::optional<Vector<uint32_t, 2>> objectsMadeHereInKnownWriters(Walker& walker, const ObjectsWrittenAloneInKnownWriters& written)
{
    const Code& code = walker.code();
    Vector<uint32_t, 2> made;
    auto append = [&](Reference object) {
        Reference root = rootInKnownWriters(code, object);
        if (!root.isValue() || !allocatesNewCell(code.at(root.id())))
            return false;
        if (!made.contains(root.id().index()))
            made.append(root.id().index());
        return true;
    };
    for (Reference object : written) {
        if (append(object))
            continue;
        Reference root = rootInKnownWriters(code, object);
        if (!root.isValue() || code.at(root.id()).opcode != Opcode::Parameter || code.at(code.at(root.id()).block()).isLoopHead)
            return std::nullopt;
        BlockID block = code.at(root.id()).block();
        std::span<const ValueID> parameters = code.at(block).parameters();
        size_t position = std::ranges::find(parameters, root.id()) - parameters.begin();
        bool areAllocatedHere = position < parameters.size() && code.at(block).firstEdgeIn();
        code.forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
            areAllocatedHere = areAllocatedHere && position < edge.arguments().size() && append(Reference::value(edge.arguments()[position]));
        });
        if (!areAllocatedHere)
            return std::nullopt;
    }
    return made;
}

static std::optional<Vector<uint32_t, 2>> listMadeHereThatLoopOnlyFillsInKnownWriters(Walker& walker, Reference receiver)
{
    if (walker.shouldAssumeLocalArrayReceivers()) [[unlikely]]
        return Vector<uint32_t, 2> { };
    std::optional<BlockID> head = walker.innermostTrialVisitHeader();
    std::optional<Vector<uint32_t, 2>> made = head ? objectsMadeHereInKnownWriters(walker, ObjectsWrittenAloneInKnownWriters { receiver }) : std::nullopt;
    if (!made || !std::ranges::all_of(*made, [&](uint32_t array) { return hasOnlyOtherArrayReads(walker, *head, ValueID(array)); }) || (!walker.hasElementArithmeticInLoop(*head) && !walker.hasElementComparisonInLoop(*head) && !readsNumericArray(walker, *head)))
        return std::nullopt;
    return made;
}

bool writesReceiverArrayAlone(const TerminatorDraft& tried, const Effects& effects)
{
    static constexpr std::array methods { InlineBuiltin::ArrayPush, InlineBuiltin::ArrayPop, InlineBuiltin::ArrayShift, InlineBuiltin::ArrayUnshift };
    return tried.kind == TerminatorKind::TryCallInline && !effects.mayRunOtherCode() && effects.writes.containsAny(perObjectHeaps) && std::ranges::contains(methods, tried.inlineBuiltin()) && tried.numberOfOperands() && tried.operand(0);
}

std::optional<Vector<uint32_t, 2>> receiverMadeHereThatAloneIsWritten(Walker& walker, const TerminatorDraft& tried, const Effects& effects)
{
    if (!writesReceiverArrayAlone(tried, effects))
        return std::nullopt;
    return listMadeHereThatLoopOnlyFillsInKnownWriters(walker, tried.operand(0));
}

void endFreshArrayWrites(Walker& walker, Reference array, const Effects& effects)
{
    WalkState& state = walker.state();
    Reference written = rootInKnownWriters(walker.code(), array);
    auto mayBeWritten = [&](Reference holder) {
        return rootInKnownWriters(walker.code(), holder) == written;
    };
    auto mayEndGuard = [](LayoutID, PropertyKeyID) {
        return true;
    };
    auto findRoot = [&](Reference holder) {
        return rootInKnownWriters(walker.code(), holder);
    };
    auto rootOf = scopedLambda<Reference(Reference)>(findRoot);
    WalkState::FreshCellWriter writer { singleElementSpan(written), rootOf };
    state.markEscaped(written);
    state.invalidate(effects, mayBeWritten, mayEndGuard, nullptr, nullptr, &writer);
    state.noteFresh(written);
}

static bool isMethodOfListThatWritesItsReceiverAloneInKnownWriters(const Walker&, const Draft&);

bool writesReceiverArrayAlone(const Walker& walker, const Draft& call, const Effects& effects)
{
    return call.opcode == Opcode::CallBuiltin && !testsItsOwnGuess(call) && !effects.mayRunOtherCode() && call.receiverOperand() && isMethodOfListThatWritesItsReceiverAloneInKnownWriters(walker, call);
}

static bool isMethodOfListThatWritesItsReceiverAloneInKnownWriters(const Walker& walker, const Draft& call)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID called = table.canonicalID(table.at(call.overload()).builtin);
    for (BuiltinID method : { BuiltinID(Builtins::ArrayPrototypePush), BuiltinID(Builtins::ArrayPrototypePop), BuiltinID(Builtins::ArrayPrototypeShift), BuiltinID(Builtins::ArrayPrototypeUnshift) }) {
        if (table.contains(method) && called == table.canonicalID(method))
            return true;
    }
    return false;
}

std::optional<Vector<uint32_t, 2>> objectsMadeHereThatAloneAreWritten(Walker& walker, const Draft& draft, const Effects& effects)
{
    if (draft.opcode == Opcode::CallBuiltin && !testsItsOwnGuess(draft) && !effects.mayRunOtherCode() && draft.receiverOperand() && isMethodOfListThatWritesItsReceiverAloneInKnownWriters(walker, draft))
        return listMadeHereThatLoopOnlyFillsInKnownWriters(walker, draft.receiverOperand());
    std::optional<ObjectsWrittenAloneInKnownWriters> written = objectsWrittenAloneByInKnownWriters(walker, draft, effects, findElementStoreWithoutOtherCodeInKnownWriters(draft, effects));
    return written && !written->isEmpty() ? objectsMadeHereInKnownWriters(walker, *written) : std::nullopt;
}

static constexpr AbstractHeapSet heapsEndedByWriterInKnownWriters = perObjectHeaps | AbstractHeapSet { AbstractHeap::MemberGuards };
static constexpr uint64_t objectsOfAnyLayoutInKnownWriters = UINT64_MAX;

std::optional<MemoryLocation> locationThatStoreNames(const Program& program, const Draft& draft, const Effects& effects)
{
    ASSERT(storesThatNameOnePlace.contains(draft.opcode));
    if (effects.mayRunOtherCode())
        return std::nullopt;
    MemoryLocation location = draft.opcode == Opcode::PutModuleVariable ? MemoryLocation::moduleVariable(draft.variable())
        : draft.opcode == Opcode::PutEnvironmentField ? MemoryLocation::environmentField(draft.layout(), draft.field())
        : MemoryLocation::objectField(draft.key());
    uint64_t mask = maskOf(program, location);
    return std::has_single_bit(mask) && effects.writes.maskOf(location.heap()) == mask ? std::optional(location) : std::nullopt;
}

static NEVER_INLINE void endWhatWriterWritesInKnownWriters(Walker& walker, const Draft& draft, const Effects& effects)
{
    if (draft.opcode == Opcode::SetArrayLength && walker.shouldIgnoreLengthStores()) [[unlikely]]
        return;
    WalkState& state = walker.state();
    std::optional<MemoryLocation> locationWrittenAlone = storeMayLeaveWhatLiesBeside(state, draft) ? locationThatStoreNames(walker.program(), draft, effects) : std::nullopt;
    const MemoryLocation* writtenAlone = locationWrittenAlone ? &*locationWrittenAlone : nullptr;
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
    auto noObjectIsWritten = [](Reference) {
        return false;
    };
    if (!ended.writes.containsAny(perObjectHeaps)) {
        state.invalidate(ended, noObjectIsWritten, mayEndGuard, writtenAlone);
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
        state.invalidate(ended, noObjectIsWritten, mayEndGuard);
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
        auto findClassOf = [&](Reference holder) -> WalkState::ClassOfHolder {
            Reference root = rootInKnownWriters(code, holder);
            TypeID type = root.isRecord() ? walker.typeOf(root) : TypeID();
            LayoutID only = type ? singleLayoutOf(walker.types(), walker.types().at(type)) : LayoutID();
            if (!only || !isOfLayoutAlone(walker, root, only))
                return { };
            return { only, holder == root && root.isValue() && allocatesNewCell(code.at(root.id())) };
        };
        auto rootIsOfLayoutAlone = [&](Reference holder, LayoutID layout) {
            return isOfLayoutAlone(walker, rootInKnownWriters(code, holder), layout);
        };
        auto askedOfRoots = scopedLambda<bool(Reference, LayoutID)>(rootIsOfLayoutAlone);
        auto askedOfHolders = scopedLambda<WalkState::ClassOfHolder(Reference)>(findClassOf);
        Reference rootWritten = written && written->size() == 1 ? rootInKnownWriters(code, written->first()) : Reference();
        WalkState::WriterOfOneObject writer { rootWritten, rootWritten ? findClassOf(rootWritten) : WalkState::ClassOfHolder(), askedOfRoots, askedOfHolders };
        state.invalidate(ended, mayBeWritten, mayEndGuard, writtenAlone, writer.classOfRoot.layoutAlone ? &writer : nullptr);
    }
}

template<bool netOfEffectsWatchesTurns>
static ALWAYS_INLINE bool endWhatOperationWritesOrRunsInKnownWriters(Walker& walker, const Draft& draft, const Effects& effects)
{
    WalkState& state = walker.state();
    bool writesWhatTurnHasMade = writesObjectsOfTurnAloneInKnownWriters(walker, draft, effects);
    if constexpr (netOfEffectsWatchesTurns)
        walker.noteWhetherOperationWritesWhatTurnHasMade(writesWhatTurnHasMade);
    if (!writesWhatTurnHasMade)
        state.updateRestartMark(effects);
    if (draft.opcode == Opcode::ReleaseEnvironment)
        state.clearRestartMark();
    if (!walker.has(Mechanism::TypedOperations) || effects.mayRunOtherCode() || !effects.writes.containsAny(heapsEndedByWriterInKnownWriters) || state.isEmpty()) {
        if (storeMayLeaveWhatLiesBeside(state, draft)) [[unlikely]] {
            std::optional<MemoryLocation> writtenAlone = locationThatStoreNames(walker.program(), draft, effects);
            state.invalidate(effects, writtenAlone ? &*writtenAlone : nullptr);
            return writesWhatTurnHasMade;
        }
        state.invalidate(effects);
        return writesWhatTurnHasMade;
    }
    endWhatWriterWritesInKnownWriters(walker, draft, effects);
    return writesWhatTurnHasMade;
}

static NEVER_INLINE bool endWhatOperationWritesOrRunsWhereNetOfEffectsWatchesTurns(Walker& walker, const Draft& draft, const Effects& effects)
{
    uint32_t restartMarkBefore = walker.state().restartMark();
    bool writesWhatTurnHasMade = endWhatOperationWritesOrRunsInKnownWriters<true>(walker, draft, effects);
    if (walker.shouldIgnoreIterationEffects())
        walker.state().setRestartMarkForTesting(restartMarkBefore);
    return writesWhatTurnHasMade;
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

static bool makesConstantOnce(const Walker& walker, const Draft& draft)
{
    return draft.opcode == Opcode::InitializeBuiltin || (draft.opcode == Opcode::PutModuleVariable && walker.plan().deferredDefinitions().findDeferred(draft.variable()));
}

ALWAYS_INLINE_INTO_OTHER_FILES void endWhatOperationWritesOrRuns(Walker& walker, const Draft& draft, const Effects& effects)
{
    if (walker.state().restartMark() && makesConstantOnce(walker, draft)) [[unlikely]] {
        uint32_t restartMark = walker.state().restartMark();
        walker.state().invalidate(effects);
        walker.state().restoreRestartMark(restartMark);
        if (walker.netOfEffectsWatchesTurns()) [[unlikely]]
            walker.noteWhetherOperationWritesWhatTurnHasMade(true);
        return;
    }
    bool isUndoableVariableStore = draft.opcode == Opcode::PutModuleVariable && walker.isVariableStoreUndoable(draft.variable());
    uint32_t newestUndoableStoreBefore = isUndoableVariableStore ? walker.state().newestUndoableStore() : 0;
    bool writesWhatTurnHasMade = walker.netOfEffectsWatchesTurns() ? endWhatOperationWritesOrRunsWhereNetOfEffectsWatchesTurns(walker, draft, effects) : endWhatOperationWritesOrRunsInKnownWriters<false>(walker, draft, effects);
    if (isUndoableVariableStore) {
        walker.state().recordUndoableStore(walker.iterationRestartPoint(), newestUndoableStoreBefore);
        if (walker.netOfEffectsWatchesTurns()) [[unlikely]]
            walker.noteWhetherOperationWritesWhatTurnHasMade(true);
    }
    if (draft.opcode == Opcode::BeginMemberGuards) {
        walker.noteGuardsBegunInTries(declaredLayoutOf(walker.program(), draft.layout()));
        noteThatGuardsOfRowHaveBegunInKnownWriters(walker, declaredLayoutOf(walker.program(), draft.layout()));
    }
    static_assert(!declaredBoundOf(Opcode::PutSlot).writes.contains(AbstractHeap::NamedProperties) && !declaredBoundOf(Opcode::PutSlotOrAdd).writes.contains(AbstractHeap::NamedProperties));
    if (draft.opcode == Opcode::PutEnvironmentField || draft.opcode == Opcode::PutStaticField || (draft.opcode == Opcode::PutModuleVariable && !isUndoableVariableStore)) {
        uint32_t undoableStore = walker.takePendingUndoableStore();
        if (undoableStore && !writesWhatTurnHasMade) {
            walker.state().recordUndoableStore(walker.iterationRestartPoint(), undoableStore);
            walker.saveUndoableStoreState(undoableStore);
        }
        if (walker.netOfEffectsWatchesTurns() && (undoableStore || walker.isUndoingStores())) [[unlikely]]
            walker.noteWhetherOperationWritesWhatTurnHasMade(true);
        return;
    }
    if (draft.opcode == Opcode::PutDoubleIndex || draft.opcode == Opcode::PutContiguousIndex || draft.opcode == Opcode::PutPropertyOfHeir || draft.opcode == Opcode::PutGlobalBinding || draft.opcode == Opcode::PutKeyedProperty) {
        if (uint32_t undoableStore = walker.takePendingUndoableStore(); undoableStore && !writesWhatTurnHasMade) {
            walker.state().recordUndoableStore(walker.iterationRestartPoint(), undoableStore);
            walker.saveUndoableStoreState(undoableStore);
        }
        return;
    }
    if (draft.opcode != Opcode::PutSlot && draft.opcode != Opcode::PutSlotOrAdd)
        return;
    if (uint32_t undoableStore = walker.takePendingUndoableStore(); undoableStore && !writesWhatTurnHasMade) {
        walker.state().recordUndoableStore(walker.iterationRestartPoint(), undoableStore);
        walker.saveUndoableStoreState(undoableStore);
        if (walker.netOfEffectsWatchesTurns() && walker.isUndoableStoreIntoIterationObject(undoableStore)) [[unlikely]]
            walker.noteWhetherOperationWritesWhatTurnHasMade(true);
    }
    if (testsItsOwnGuess(draft) || !effects.writes.contains(AbstractHeap::NamedProperties))
        return;
    const Code& code = walker.code();
    for (Reference name = draft.objectOperand(); name.isValue(); name = Reference::value(code.at(name.id()).operand(0))) {
        walker.state().invalidateCellFacts(name, CellFact::BearsNoMark);
        if (!cellPreservingOpcodes.contains(code.at(name.id()).opcode))
            break;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
