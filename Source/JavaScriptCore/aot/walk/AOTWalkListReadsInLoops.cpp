/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LoopInvariants with Guesses: what a loop does not change of a list is asked and read in front of it. The row of tests in front of a loop that is built twice asks whether the list is as lists are made and of the kind that is written.
// In the typed copy a read then runs no code and names the kind, so the look counts on no other code, and the pointer into the storage and the length leave the loop by Walker::build().
// At a suspension: a loop that may suspend is built once, and whatever suspends ends the note. At a throw: a loop that holds a handler block gets no such row.
// Withheld, any of LoopInvariants, Guesses, TypedOperations, Folding: no row and no note, and every read tests for itself. The bound behind `i < list.length` goes by the notes of Folding, in a loop or not.

#include "config.h"
#include "AOTWalkListReadsInLoops.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOperandKinds.h"
#include "AOTResultRules.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalkKnown.h"
#include "AOTWalkTextBuilders.h"
#include <array>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// The pointer, the length and the kind rest on the first. That a list is as made rests on all of them: heapsUnderAspectsInKnownElements.
static constexpr AbstractHeapSet heapsUnderListsReadInLoops {
    AbstractHeap::ArrayStorage, AbstractHeap::Elements, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords
};
static_assert(perObjectHeaps.containsAll(heapsUnderListsReadInLoops));

// ---- The note: Reference::bits() of the name, and above them the kind

static constexpr unsigned shiftOfStorageInListReadsInLoops = 32;
static constexpr unsigned shiftOfSecondKindInListReadsInLoops = 40;

static void noteListTestedInListReadsInLoops(Walker& walker, const ListTestedInFrontOfLoop& tested)
{
    uint64_t noted = tested.name.bits()
        | static_cast<uint64_t>(tested.storage) << shiftOfStorageInListReadsInLoops
        | static_cast<uint64_t>(tested.admitsInt32Storage) << shiftOfSecondKindInListReadsInLoops;
    noteKnown(walker, KnownNote::ListTestedInFrontOfLoop, tested.name, { }, 0, LocationMasks(heapsUnderListsReadInLoops), noted);
}

static std::optional<ListTestedInFrontOfLoop> findNoteInListReadsInLoops(Walker& walker, Reference list)
{
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::ListTestedInFrontOfLoop, list);
    if (!noted)
        return std::nullopt;
    return ListTestedInFrontOfLoop {
        Reference::fromBits(static_cast<uint32_t>(*noted)),
        static_cast<ArrayStorage>(*noted >> shiftOfStorageInListReadsInLoops & 0xff),
        !!(*noted >> shiftOfSecondKindInListReadsInLoops & 1)
    };
}

bool leavesListsTestedInFrontOfLoopAsTheyAre(const Effects& effects)
{
    return !effects.mayRunOtherCode() && !effects.writes.containsAny(heapsUnderListsReadInLoops);
}

// WHAT STANDS BEHIND A CAST IN FRONT OF THE LOOP NEED NOT BE ALIVE AT THE LOOP: a join binds anew only what is read behind it, so the value under such a cast may still be bound to what ONE arm in front of the join has built,
// which does not dominate the loop. Casts are followed only as far as the loop reaches: what the loop mentions is alive where it is entered.
static bool isCastInLoopsInListReadsInLoops(const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    return value.index() >= firstValueOfLoop && !index.isParameter(value) && opcodesThatStateTypes.contains(index.opcodeOf(value)) && index.value(value).operand(0);
}

static ValueID valueThatEntersLoopInListReadsInLoops(const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    while (isCastInLoopsInListReadsInLoops(index, value, firstValueOfLoop))
        value = index.value(value).operand(0);
    return value;
}

// ---- The read

std::optional<ListTestedInFrontOfLoop> findListTestedInFrontOfLoop(Walker& walker, const BodyValue& read)
{
    if (walker.state().answers().isEmpty())
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID outermost = index.loopOf(index.blockOf(read.id()));
    if (!outermost)
        return std::nullopt;
    while (LoopID around = index.loop(outermost).parent)
        outermost = around;
    uint32_t firstValueOfLoops = index.block(index.loop(outermost).head).firstParameter().index();
    // A cast of a loop may not be walked yet, or be bound to what another copy of the loop has built: no note is found under that, and the cast is passed over.
    for (ValueID input = read.objectOperand(); input; input = index.value(input).operand(0)) {
        // While a look stands in front of a head, the parameters of the head are bound to what enters, and a turn may bring another list.
        if (index.isParameter(input) && index.blockOf(input) == walker.inputBlockBeingWalked() && index.isLoopHead(index.blockOf(input)))
            return std::nullopt;
        if (Reference list = walker.referenceOf(input); list && list.isValue()) {
            if (std::optional<ListTestedInFrontOfLoop> tested = findNoteInListReadsInLoops(walker, list))
                return tested;
        }
        if (!isCastInLoopsInListReadsInLoops(index, input, firstValueOfLoops))
            break;
    }
    return std::nullopt;
}

void nameKindOfListTestedInFrontOfLoop(Draft& read, const ListTestedInFrontOfLoop& tested)
{
    read.setArrayStorage(tested.storage);
    read.setAdmitsInt32Storage(tested.admitsInt32Storage);
}

// Null: it stands where the walk stands, so every turn would do it again, as the read does by itself. The sweep drops it.
static Reference buildInFrontOfTurnsInListReadsInLoops(Walker& walker, Draft&& draft)
{
    BlockID whereWalkStands = walker.currentBlock();
    Reference built = walker.build(WTF::move(draft));
    if (!built || !built.isRecord())
        return { };
    if (walker.code().blockOf(built.id()) == whereWalkStands)
        return { };
    return built;
}

void takeStorageAndLengthFromInFrontOfLoop(Walker& walker, Draft& read, const ListTestedInFrontOfLoop& tested)
{
    if (!walker.isInLoop() || walker.testsEveryPosition())
        return;
    Origin origin = read.origin();
    origin.isTailCall = false;
    if (hasArrayStorageForm(read.arrayStorage())) {
        Draft storage(Opcode::GetArrayStorage, origin);
        walker.chooseConservativeFields(storage);
        storage.setArrayOperand(tested.name);
        if (Reference pointer = buildInFrontOfTurnsInListReadsInLoops(walker, WTF::move(storage))) {
            read.setStorageOperand(pointer);
            walker.count(StatisticID::KnownInductionsUsed);
        }
    }
    // Such a read looks at no length.
    if (read.isInBounds())
        return;
    Draft length(Opcode::GetArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setArrayOperand(tested.name);
    if (Reference lengthInFront = buildInFrontOfTurnsInListReadsInLoops(walker, WTF::move(length))) {
        read.setLengthOperands(singleElementSpan(lengthInFront));
        walker.count(StatisticID::KnownInductionsUsed);
    }
}

bool positionIsKnownToLieInsideList(Walker& walker, Reference list, Reference position)
{
    return isKnownToLieInsideList(walker, list, position);
}

// ---- The looks

TypeID typeReadOutOfListTestedInFrontOfLoop(Walker& walker, const BodyIndex& index, const BodyValue& read, TypeID typeOfList, TypeID typeOfPosition)
{
    // findListTestedInFrontOfLoop() reads the body that the walk stands in, and a look may stand in the body of a callee.
    if (&index != &walker.currentBodyIndex() || !walker.has(Mechanism::TypedOperations) || !typeOfList || !typeOfPosition)
        return { };
    std::optional<ListTestedInFrontOfLoop> tested = findListTestedInFrontOfLoop(walker, read);
    if (!tested)
        return { };
    // Off undefined and null the read throws, in front of all else: splitNullishOffReceiverInWalkTyped().
    TypeID list = walker.types().withoutNullish(typeOfList);
    if (!readTakesIndexOfProvenList(walker.types(), list, typeOfPosition))
        return { };

    Draft asBuilt(Opcode::GetArrayElement);
    nameKindOfListTestedInFrontOfLoop(asBuilt, *tested);
    asBuilt.setLevel(KnowledgeLevel::Proven);
    static_assert(!OperandIndex::GetArrayElement::array && OperandIndex::GetArrayElement::position == 1);
    std::array<TypeID, 2> operands { list, typeOfPosition };
    return resultTypeOf(walker.typing(), asBuilt, OperandTypes { std::span<const TypeID>(operands) });
}

// ---- The row in front of the loop

static TypeID ownTypeInListReadsInLoops(const Walker& walker, Reference reference)
{
    return reference && reference.isRecord() ? walker.code().at(reference.id()).type : TypeID();
}

static bool isProvenListInListReadsInLoops(const Walker& walker, Reference name)
{
    return name && name.isValue() && hasKind(walker.types(), ownTypeInListReadsInLoops(walker, name), OperandKind::Array);
}

// No effect ends what is noted of a fresh object, and an append that the state follows changes its kind in silence. A read out of one that is as made runs no code as it is.
static bool isFreshInListReadsInLoops(Walker& walker, Reference name)
{
    ValueID cell = rootOfCellInSweptCode(walker.code(), name.id());
    return !walker.code().isConstant(cell) && walker.state().isFresh(Reference::value(cell));
}

// As LooksAtLoops::addEffectsOf() asks: the walk may build a join that gives a text as appends to a builder.
static bool turnMayAppendToTextInListReadsInLoops(Walker& walker, BlockID inputHead, const LoopPreview& preview)
{
    if (!mayKeepTextsInStringBuilders(walker))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (!operationsThatJoinTexts.contains(operation.opcode()))
                continue;
            size_t place = operation.id().index() - preview.firstValue;
            TypeID joined = operation.id().index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
            if (!joined || walker.types().at(joined).isAnyString())
                return true;
        }
    }
    return false;
}

ListsNotedAsTestedForLook::ListsNotedAsTestedForLook(Walker& walker, std::span<const ListTestedInFrontOfLoop> lists)
    : m_walker(walker)
    , m_lists(lists)
{
    for (const ListTestedInFrontOfLoop& list : m_lists)
        noteListTestedInListReadsInLoops(m_walker, list);
}

ListsNotedAsTestedForLook::~ListsNotedAsTestedForLook()
{
    for (const ListTestedInFrontOfLoop& list : m_lists)
        endKnown(m_walker, KnownNote::ListTestedInFrontOfLoop, list.name);
}

// One more look at the loop, as the look at the typed copy will be: the names of the guesses are bound to their proofs, and the lists are noted as tested. Behind it all is as it was.
static bool lookFindsListsLeftAsTheyAreInListReadsInLoops(Walker& walker, BlockID inputHead, std::span<const Reference> entering, std::span<const ListTestedInFrontOfLoop> lists, std::span<const GuessInFrontOfLoop> guesses, std::span<const Reference> proofs)
{
    Vector<std::pair<ValueID, Reference>, 8> boundBefore;
    for (size_t place = 0; place < guesses.size() && place < proofs.size(); ++place) {
        if (!proofs[place])
            continue;
        for (ValueID name : guesses[place].names) {
            boundBefore.append({ name, walker.referenceBoundTo(name) });
            walker.bind(name, proofs[place]);
        }
    }
    for (const ListTestedInFrontOfLoop& list : lists)
        noteListTestedInListReadsInLoops(walker, list);

    // As Walker::enterLoopHead() asks.
    Vector<TypeID, 8> entryTypes;
    for (Reference reference : entering)
        entryTypes.append(ownTypeInListReadsInLoops(walker, reference));
    if (walker.hasTextsInStringBuilders())
        typeTextsInStringBuildersForLooks(walker, entering, entryTypes);
    LoopPreview preview = previewLoop(walker, inputHead, entryTypes.span());

    // The tests are not built yet: on the edge where one fails nothing of this holds.
    for (const ListTestedInFrontOfLoop& list : lists)
        endKnown(walker, KnownNote::ListTestedInFrontOfLoop, list.name);
    for (size_t place = boundBefore.size(); place--;)
        walker.bind(boundBefore[place].first, boundBefore[place].second);
    // A text that a turn appends to lies in its builder all round the loop, and may enter the next loop so. The closed copy and the typed one would each hand it on in a way of their own.
    if (turnMayAppendToTextInListReadsInLoops(walker, inputHead, preview))
        return false;
    return !walker.hasFailed() && leavesListsTestedInFrontOfLoopAsTheyAre(preview.effects);
}

// The values of the body that stand for `value` and that the loop mentions: GuessInFrontOfLoop::names.
static Vector<ValueID, 2> namesThatLoopMentionsInListReadsInLoops(const Walker& walker, const BodyIndex& index, LoopID loop, uint32_t firstValueOfLoop, Reference value)
{
    Vector<ValueID, 2> names;
    auto mention = [&](ValueID operand) {
        if (operand && operand.index() < firstValueOfLoop && !names.contains(operand) && walker.referenceOf(operand) == value)
            names.append(operand);
    };
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values()) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                mention(operand);
            });
        }
        BodyTerminator end = block.terminator();
        end.forEachOperand([&](ValueID operand, uint32_t) {
            mention(operand);
        });
        for (BodyTarget target : end.targets()) {
            for (ValueID argument : target.arguments())
                mention(argument);
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                mention(argument);
        }
    }
    return names;
}

// A read in the turn goes by the kind that it finds where `number[]` is written (arrayStorageDeclaredBy()). The test in front of the loop passes for both kinds of numbers, and behind it what is read is a number.
static ArrayStorage kindThatHoldsWhatIsWrittenInListReadsInLoops(Walker& walker, TypeID list)
{
    TypeArena& types = walker.types();
    if (!list || holdsOnlyPrimitives(list))
        return ArrayStorage::Other;
    const ClassPart* part = singleBuiltinClassOf(types, types.at(list));
    if (!part || part->builtinClass != BuiltinClasses::Array)
        return ArrayStorage::Other;
    TypeID elements = part->expectedContentsAt(walker.program().builtins(), ContentsSlot::Element).predicted();
    if (!elements || types.at(elements).isAnything())
        return ArrayStorage::Other;
    return arrayStorageHolding(types, elements);
}

// `total += area(kept[i])`: Link's effects describe the callee's closed form. The look reads the operations of an inlined copy instead: LooksAtLoops::withWhatCopyReturns().
static bool lookReadsCopyOfCalleeInListReadsInLoops(Walker& walker, const BodyValue& call, FunctionID callee)
{
    if (walker.facts().bodies().at(callee).hasLoops())
        return false;
    Draft site(call.opcode());
    site.flags = static_cast<uint8_t>(call.flags());
    copyFields(call, site, [](ValueID operand) {
        return Reference::value(operand);
    });
    return mayCopyCallee(walker, site, callee, PlaceOfCall { walker.currentFunction(), true, &walker.currentBodyIndex(), call.id() });
}

template<typename TypeInLook>
static bool surelyEndsWhatIsShownOfListsInListReadsInLoops(Walker& walker, const BodyValue& operation, uint32_t firstValueOfLoop, const TypeInLook& typeInLook, bool countsOnTurnsThatBeginAgain)
{
    TypeArena& types = walker.types();
    if (operation.opcode() == Opcode::Call) {
        const LinkTables& linked = walker.facts().linked();
        FunctionID known = linked.calleeAt(walker.currentFunction(), operation.id());
        if (known && countsOnTurnsThatBeginAgain && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, known))
            return false;
        if (FunctionID callee = known)
            return !leavesListsTestedInFrontOfLoopAsTheyAre(linked.function(callee).effectsOnReturn) || !leavesListsTestedInFrontOfLoopAsTheyAre(linked.function(callee).effectsOnThrow);
        // The row of a built-in may say less than that anything runs.
        TypeID callee = typeInLook(operation.as<Opcode::Call>().calleeOperand());
        return !callee || !singleBuiltinOf(types, types.at(callee));
    }
    // An element out of a list of values is anything behind the row too. What comes from in front of the loop may have a test of its own in the row.
    // Where the looks count on turns that begin again, what is expected of the element decides.
    if (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.objectOperand().index() >= firstValueOfLoop && !countsOnTurnsThatBeginAgain) {
        TypeID object = typeInLook(operation.objectOperand());
        return !object || types.at(object).isAnything();
    }
    return false;
}

ListsReadInLoop findListsReadInLoop(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses)
{
    ListsReadInLoop found;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Folding))
        return found;

    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto typeInLook = [&](ValueID input) -> TypeID {
        if (input.index() < firstValueOfLoop)
            return ownTypeInListReadsInLoops(walker, walker.referenceOf(input));
        size_t place = input.index() - preview.firstValue;
        return input.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
    };

    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        // The state of a handler is met anew, and with Handlers withheld it is empty: a read behind it would find no note where the look has counted on one.
        if (block.isHandlerBlock())
            return { };
        for (BodyValue operation : block.values()) {
            // THE LOOK THAT IS TRIED COSTS AS MUCH AS THE LOOP IS LONG, and most loops that read a list cannot end it well, whatever is shown of their lists. Both questions are asked as LooksAtLoops::addEffectsOf() asks them.
            if (surelyEndsWhatIsShownOfListsInListReadsInLoops(walker, operation, firstValueOfLoop, typeInLook, member != inputHead ? walker.looksCountOnTurnsThatBeginAgain(inputHead) && index.loopOf(member) == loop : walker.looksCountOnTurnsThatBeginAgain(inputHead)))
                return { };
            if (operation.opcode() != Opcode::ElemGet)
                continue;
            ValueID input = valueThatEntersLoopInListReadsInLoops(index, operation.objectOperand(), firstValueOfLoop);
            if (input.index() >= firstValueOfLoop)
                continue;
            Reference name = walker.referenceOf(input);
            if (!name || !name.isValue())
                continue;
            bool isListed = found.lists.containsIf([&](const ListTestedInFrontOfLoop& list) {
                return list.name == name;
            });
            if (isListed)
                continue;

            TypeID proven = ownTypeInListReadsInLoops(walker, name);
            for (const GuessInFrontOfLoop& guess : guesses) {
                if (guess.value == name)
                    proven = types.intersectionOf(proven, guess.tested);
            }
            TypeID list = types.withoutNullish(proven);
            if (!readTakesIndexOfProvenList(types, list, typeInLook(operation.propertyOperand())))
                continue;
            if (isFreshInListReadsInLoops(walker, name))
                continue;
            // A row in front of a loop around this one has asked.
            if (findNoteInListReadsInLoops(walker, name))
                continue;

            // As buildArrayReadOfTypedElements() guesses: numbers are read out of Int32 storage and out of Double storage alike.
            ArrayStorage written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, list);
            // The kind is a guess, so what is only expected of the list may name it: a test for Array need not keep what is written of the elements.
            if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, input).type : TypeID())
                written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(expected));
            bool holdsNumbers = holdsOnlyNumbers(written);
            // Link has seen all that comes out of the read, and it is int32s. Then the list holds them as int32s, unless it is empty or the engine has given its storage up: one kind to ask for, and one arm in the read.
            TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id());
            if (seenByLink && types.at(seenByLink).isInt32OrNullish()) {
                found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Int32, false });
                continue;
            }
            found.lists.append(ListTestedInFrontOfLoop { name, holdsNumbers ? ArrayStorage::Double : written, holdsNumbers });
        }
    }
    if (found.lists.isEmpty())
        return found;

    if (guesses.isEmpty()) {
        if (!lookFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, entering, found.lists.span(), { }, { }))
            return { };
        found.areLeftAsTheyAre = true;
    }

    for (const ListTestedInFrontOfLoop& list : found.lists) {
        bool hasGuess = guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
            return guess.value == list.name;
        });
        if (hasGuess || isProvenListInListReadsInLoops(walker, list.name))
            continue;
        guesses.append(GuessInFrontOfLoop { list.name, types.builtinClass(BuiltinClasses::Array), { }, namesThatLoopMentionsInListReadsInLoops(walker, index, loop, firstValueOfLoop, list.name) });
    }
    return found;
}

// False: no way leads on.
static bool goOnWhereTestPassesInListReadsInLoops(Walker& walker, Reference test, BlockID& closedStart)
{
    if (!test)
        return false;
    BlockID passed = walker.addBlock();
    if (!closedStart)
        closedStart = walker.addBlock();
    walker.branch(test, passed, closedStart);
    walker.continueIn(passed);
    return !walker.isUnreachable();
}

void buildTestsOfListsReadInLoop(Walker& walker, BlockID inputHead, std::span<const Reference> entering, ListsReadInLoop& found, std::span<const GuessInFrontOfLoop> guesses, std::span<const Reference> proofs, BlockID& closedStart, Origin origin)
{
    if (found.lists.isEmpty() || walker.isUnreachable() || walker.hasFailed())
        return;

    for (ListTestedInFrontOfLoop& list : found.lists) {
        for (size_t place = 0; place < guesses.size() && place < proofs.size(); ++place) {
            if (guesses[place].value != list.name)
                continue;
            list.name = proofs[place];
            break;
        }
    }
    found.lists.removeAllMatching([&](const ListTestedInFrontOfLoop& list) {
        return !isProvenListInListReadsInLoops(walker, list.name);
    });
    if (found.lists.isEmpty())
        return;
    if (!found.areLeftAsTheyAre && !lookFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, entering, found.lists.span(), guesses, proofs))
        return;

    for (const ListTestedInFrontOfLoop& list : found.lists) {
        // One bit of the cell answers for both aspects. Names: a negative integer names a property.
        Draft isAsMade(Opcode::IsPristine, origin);
        walker.chooseConservativeFields(isAsMade);
        isAsMade.setValueOperand(list.name);
        isAsMade.setBuiltinClass(BuiltinClasses::Array);
        isAsMade.setAspects({ PristineAspects::Storage, PristineAspects::Names });
        if (!goOnWhereTestPassesInListReadsInLoops(walker, walker.build(WTF::move(isAsMade)), closedStart))
            return;
        if (!hasArrayStorageForm(list.storage))
            continue;
        Draft hasKindWritten(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(hasKindWritten);
        hasKindWritten.setArrayOperand(list.name);
        hasKindWritten.setArrayStorage(list.storage);
        hasKindWritten.setAdmitsInt32Storage(list.admitsInt32Storage);
        if (!goOnWhereTestPassesInListReadsInLoops(walker, walker.build(WTF::move(hasKindWritten)), closedStart))
            return;
    }
    for (const ListTestedInFrontOfLoop& list : found.lists)
        noteListTestedInListReadsInLoops(walker, list);
    found.areTested = true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
