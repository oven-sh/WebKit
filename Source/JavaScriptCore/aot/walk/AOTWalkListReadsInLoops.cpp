/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

static constexpr AbstractHeapSet heapsUnderListsReadInLoops {
    AbstractHeap::ArrayStorage, AbstractHeap::Elements, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords
};
static_assert(perObjectHeaps.containsAll(heapsUnderListsReadInLoops));

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
    for (ValueID input = read.objectOperand(); input; input = index.value(input).operand(0)) {
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

TypeID typeReadOutOfListTestedInFrontOfLoop(Walker& walker, const BodyIndex& index, const BodyValue& read, TypeID typeOfList, TypeID typeOfPosition)
{
    if (&index != &walker.currentBodyIndex() || !walker.has(Mechanism::TypedOperations) || !typeOfList || !typeOfPosition)
        return { };
    std::optional<ListTestedInFrontOfLoop> tested = findListTestedInFrontOfLoop(walker, read);
    if (!tested)
        return { };
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

static TypeID ownTypeInListReadsInLoops(const Walker& walker, Reference reference)
{
    return reference && reference.isRecord() ? walker.code().at(reference.id()).type : TypeID();
}

static bool isProvenListInListReadsInLoops(const Walker& walker, Reference name)
{
    return name && name.isValue() && hasKind(walker.types(), ownTypeInListReadsInLoops(walker, name), OperandKind::Array);
}

static bool isFreshInListReadsInLoops(Walker& walker, Reference name)
{
    ValueID cell = rootOfCellInSweptCode(walker.code(), name.id());
    return !walker.code().isConstant(cell) && walker.state().isFresh(Reference::value(cell));
}

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

    Vector<TypeID, 8> entryTypes;
    for (Reference reference : entering)
        entryTypes.append(ownTypeInListReadsInLoops(walker, reference));
    if (walker.hasTextsInStringBuilders())
        typeTextsInStringBuildersForLooks(walker, entering, entryTypes);
    LoopPreview preview = previewLoop(walker, inputHead, entryTypes.span());

    for (const ListTestedInFrontOfLoop& list : lists)
        endKnown(walker, KnownNote::ListTestedInFrontOfLoop, list.name);
    for (size_t place = boundBefore.size(); place--;)
        walker.bind(boundBefore[place].first, boundBefore[place].second);
    if (turnMayAppendToTextInListReadsInLoops(walker, inputHead, preview))
        return false;
    return !walker.hasFailed() && leavesListsTestedInFrontOfLoopAsTheyAre(preview.effects);
}

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
        TypeID callee = typeInLook(operation.as<Opcode::Call>().calleeOperand());
        return !callee || !singleBuiltinOf(types, types.at(callee));
    }
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
        if (block.isHandlerBlock())
            return { };
        for (BodyValue operation : block.values()) {
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
            if (findNoteInListReadsInLoops(walker, name))
                continue;

            ArrayStorage written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, list);
            if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, input).type : TypeID())
                written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(expected));
            bool holdsNumbers = holdsOnlyNumbers(written);
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
