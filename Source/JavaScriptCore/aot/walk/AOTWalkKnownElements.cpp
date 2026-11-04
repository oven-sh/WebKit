/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations: what is remembered of a list and of a view: its length, that it has no hole, the kind of its storage, in which aspects it is as made, that a position lies inside. It builds no test: lists-1 and lists-2 do.
// At a suspension: each of them rests on a heap, so it ends as at a call of unseen code, but for a list that is fresh.
// At a throw: a note is made behind the allocation or on the edge of the test that shows it, so no handler starts with one that its region has not earned.
// Withheld: nothing is noted and every question is answered with nothing.

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTEffectsDeclared.h"
#include "AOTTypeArena.h"
#include "ArrayConventions.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsUnderLengthInKnownElements { AbstractHeap::ArrayStorage };
static constexpr AbstractHeapSet heapsUnderDensityInKnownElements { AbstractHeap::ArrayStorage, AbstractHeap::Elements };
static constexpr AbstractHeapSet heapsUnderViewInKnownElements { AbstractHeap::TypedArrayStorage };
// Whether a cell is as its class makes it follows its structure, which nothing changes but what is done to the cell itself: a property of its own, another prototype, storage without a form.
static constexpr AbstractHeapSet heapsUnderAspectsInKnownElements {
    AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords, AbstractHeap::ArrayStorage, AbstractHeap::Elements
};
static_assert(perObjectHeaps.containsAll(heapsUnderAspectsInKnownElements));

// What no store of an element changes, whatever becomes of the storage.
static constexpr OptionSet<PristineAspects> aspectsLeftByStoreUnderIndexInKnownElements { PristineAspects::Names, PristineAspects::NamesKeptBelow, PristineAspects::Prototype };
// aspectsOfObjectAsMade of AOTWalkBuiltins.cpp. AsCreated is lists-3's to say.
static constexpr OptionSet<PristineAspects> aspectsOfNewListInKnownElements { PristineAspects::Storage, PristineAspects::Names, PristineAspects::NamesKeptBelow, PristineAspects::Prototype };

static Reference referenceToRecordInKnownElements(const Code& code, ValueID record)
{
    return code.isConstant(record) ? Reference::constant(record) : Reference::value(record);
}

static Reference recordNotedInKnownElements(std::optional<uint64_t> noted)
{
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

KnownOfElementAccess knownOfElementAccess(Walker& walker, Reference holder, Reference position)
{
    KnownOfElementAccess known;
    if (!walker.has(Mechanism::TypedOperations) || walker.state().answers().isEmpty())
        return known;
    known.length = recordNotedInKnownElements(findKnown(walker, KnownNote::LengthOfList, holder));
    known.isDense = findKnown(walker, KnownNote::ListIsDense, holder).has_value();
    if (std::optional<uint64_t> storage = findKnown(walker, KnownNote::StorageOfList, holder))
        known.storage = static_cast<ArrayStorage>(*storage);
    if (position)
        known.isInBounds = (known.length && isKnownToLieInside(walker, position, known.length)) || findKnown(walker, KnownNote::IsInsideView, holder, position).has_value();
    return known;
}

// `const xs = [a, b, c]; keep(xs); xs.length`, once nothing that runs in between can write a list.
void noteKnownOfNewList(Walker& walker, Reference list)
{
    if (!walker.has(Mechanism::TypedOperations) || !list.isValue())
        return;
    const Operation& made = walker.code().at(list.id());
    // With room asked for, the allocation may be one that says its length in another way.
    if (made.opcode != Opcode::NewArray || made.capacityOperand() || made.keepsLength() || made.isFilledToCapacity())
        return;

    Draft length(Opcode::Integer);
    length.setInteger(static_cast<int64_t>(made.elementsOperands().size() + made.holes().size()));
    bool hasNoHole = made.holes().empty();
    // constant() may move the records of Code: `made` is not read behind it.
    Reference lengthOfList = walker.constant(WTF::move(length));
    if (!lengthOfList)
        return;
    noteKnown(walker, KnownNote::LengthOfList, list, { }, 0, LocationMasks(heapsUnderLengthInKnownElements), lengthOfList.bits());
    if (hasNoHole)
        noteKnown(walker, KnownNote::ListIsDense, list, { }, 0, LocationMasks(heapsUnderDensityInKnownElements), 1);
    noteAspectsSeenAsMade(walker, list, hasNoHole ? aspectsOfNewListInKnownElements | PristineAspects::Dense : aspectsOfNewListInKnownElements);
}

OptionSet<PristineAspects> aspectsSeenAsMade(Walker& walker, Reference object)
{
    if (!walker.has(Mechanism::TypedOperations))
        return { };
    std::optional<uint64_t> seen = findKnown(walker, KnownNote::AspectsAsMade, object);
    return seen ? OptionSet<PristineAspects>::fromRaw(static_cast<OptionSet<PristineAspects>::StorageType>(*seen)) : OptionSet<PristineAspects>();
}

void noteAspectsSeenAsMade(Walker& walker, Reference object, OptionSet<PristineAspects> aspects)
{
    if (!walker.has(Mechanism::TypedOperations) || aspects.isEmpty())
        return;
    noteKnown(walker, KnownNote::AspectsAsMade, object, { }, 0, LocationMasks(heapsUnderAspectsInKnownElements), (aspectsSeenAsMade(walker, object) | aspects).toRaw());
}

// A test that has failed says nothing that anybody asks for.
void noteWhatTestOfElementsShows(Walker& walker, ValueID test, bool isTrue)
{
    if (!isTrue || !walker.has(Mechanism::TypedOperations))
        return;
    const Code& code = walker.code();
    const Operation& record = code.at(test);
    switch (record.opcode) {
    case Opcode::HasArrayStorage:
        // A test that passes for two kinds names neither.
        if (!record.admitsInt32Storage() && hasArrayStorageForm(record.arrayStorage()))
            noteKnown(walker, KnownNote::StorageOfList, referenceToRecordInKnownElements(code, record.arrayOperand()), { }, 0, LocationMasks(heapsUnderLengthInKnownElements), static_cast<uint64_t>(record.arrayStorage()));
        return;
    case Opcode::IsPristine:
        noteAspectsSeenAsMade(walker, referenceToRecordInKnownElements(code, record.valueOperand()), record.aspects());
        return;
    case Opcode::IsInsideView:
        if (!record.mayEqualLength())
            noteKnown(walker, KnownNote::IsInsideView, referenceToRecordInKnownElements(code, record.viewOperand()), referenceToRecordInKnownElements(code, record.positionOperand()), 0, LocationMasks(heapsUnderViewInKnownElements), 1);
        return;
    default:
        return;
    }
}

// `ring[i & 7] = x`, `pair[1] = y`: the store may lengthen the vector, move it, or copy one that is shared with a literal. The vector keeps its form: it takes the value as it is, and no store this near the start makes an array sparse.
// Below the length a store fills a hole or replaces an element, so the length is the number that it was. Where the list has no hole the element lies in the vector, which then stays one.
KnownLeftByElementStore findKnownLeftByElementStore(Walker& walker, Reference list, Reference position, Reference stored)
{
    KnownLeftByElementStore left;
    left.list = list;
    TypeID typeOfPosition = walker.typeOf(position);
    // TODO(known-state-and-effects-2a): K7 is off: it stands under its reader's mark (density behind a store below the length), and no run with a red control answers that. A store into a list leaves nothing that was noted of the list.
    bool isAnsweredByRun = false;
    if (!isAnsweredByRun || !walker.has(Mechanism::TypedOperations) || !typeOfPosition || walker.state().answers().isEmpty())
        return left;
    TypeArena& types = walker.types();
    Type index = types.at(typeOfPosition);
    bool isNearStart = index.isInteger() && index.integers.minimum >= 0 && index.integers.maximum < MIN_BEYOND_LENGTH_SPARSE_INDEX;
    std::optional<uint64_t> length = findKnown(walker, KnownNote::LengthOfList, list);
    bool liesBelowLength = length && isKnownToLieInside(walker, position, recordNotedInKnownElements(length));
    if (!isNearStart && !liesBelowLength)
        return left;

    TypeID typeOfStored = walker.typeOf(stored);
    if (liesBelowLength) {
        left.length = length;
        // What a read of a hole gave makes a hole again.
        left.isDense = typeOfStored && !(types.at(typeOfStored).primitives & Primitive::Absent) && findKnown(walker, KnownNote::ListIsDense, list).has_value();
    }
    bool vectorKeepsForm = isNearStart || left.isDense;
    if (OptionSet<PristineAspects> seen = aspectsSeenAsMade(walker, list); !seen.isEmpty()) {
        OptionSet<PristineAspects> kept = aspectsLeftByStoreUnderIndexInKnownElements;
        if (vectorKeepsForm)
            kept.add(PristineAspects::Storage);
        if (left.isDense)
            kept.add(PristineAspects::Dense);
        if (OptionSet<PristineAspects> aspects = seen & kept; !aspects.isEmpty())
            left.aspects = aspects.toRaw();
    }
    std::optional<uint64_t> storage = vectorKeepsForm ? findKnown(walker, KnownNote::StorageOfList, list) : std::nullopt;
    if (storage && typeOfStored && hasArrayStorageForm(static_cast<ArrayStorage>(*storage)) && canStoreWithoutConversion(types, static_cast<ArrayStorage>(*storage), typeOfStored))
        left.storage = storage;
    return left;
}

void noteAgainKnownLeftByElementStore(Walker& walker, const KnownLeftByElementStore& left)
{
    if (left.length)
        noteKnown(walker, KnownNote::LengthOfList, left.list, { }, 0, LocationMasks(heapsUnderLengthInKnownElements), *left.length);
    if (left.isDense)
        noteKnown(walker, KnownNote::ListIsDense, left.list, { }, 0, LocationMasks(heapsUnderDensityInKnownElements), 1);
    if (left.storage)
        noteKnown(walker, KnownNote::StorageOfList, left.list, { }, 0, LocationMasks(heapsUnderLengthInKnownElements), *left.storage);
    if (left.aspects)
        noteKnown(walker, KnownNote::AspectsAsMade, left.list, { }, 0, LocationMasks(heapsUnderAspectsInKnownElements), *left.aspects);
}

// `[...a, ...b]`: a list that the program has not seen refuses nothing, so the append fails only where there is no room for the elements. The sweep drops such an append with a list that nobody comes to read.
// AppendIterable is not among them: what it steps may throw for itself.
std::optional<Effects> effectsOfAppendToListAsMade(Walker& walker, const Draft& append)
{
    static constexpr OpcodeSet appendsThatRunNoCode { Opcode::AppendElements, Opcode::AppendMembers };
    if (!appendsThatRunNoCode.contains(append.opcode) || !walker.has(Mechanism::TypedOperations) || !isFreshListAsMade(walker, append.arrayOperand()))
        return std::nullopt;
    Effects effects = declaredEffectsOfOperation(walker.program(), append.opcode, append);
    if (effects.mayRunOtherCode())
        return std::nullopt;
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
