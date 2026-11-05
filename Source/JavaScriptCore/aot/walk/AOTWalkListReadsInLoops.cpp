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
#include "AOTTypedArrayKinds.h"
#include "AOTWalkKnown.h"
#include "AOTWalkTextBuilders.h"
#include <array>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsUnderListsReadInLoops {
    AbstractHeap::ArrayStorage, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords
};
static_assert(perObjectHeaps.containsAll(heapsUnderListsReadInLoops));
static constexpr AbstractHeapSet heapsUnderViewsReadInLoops {
    AbstractHeap::TypedArrayStorage, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords
};

static BuiltinClassID classOfViewInListReadsInLoops(const Walker& walker, TypeID type)
{
    const ClassPart* part = type && walker.types().at(type).isObject() ? singleBuiltinClassOf(walker.types(), walker.types().at(type)) : nullptr;
    return part && isTypedView(walker.program().builtins().typedArrayTypeOf(part->builtinClass)) ? part->builtinClass : BuiltinClassID();
}

static BuiltinClassID classOfProvenViewInListReadsInLoops(const Walker& walker, Reference name)
{
    return name && name.isValue() ? classOfViewInListReadsInLoops(walker, walker.code().at(name.id()).type) : BuiltinClassID();
}

static constexpr unsigned shiftOfStorageInListReadsInLoops = 32;
static constexpr unsigned shiftOfSecondKindInListReadsInLoops = 40;
static constexpr unsigned shiftOfWritableInListReadsInLoops = 41;
static constexpr unsigned shiftOfStoresInListReadsInLoops = 42;

static void noteListTestedInListReadsInLoops(Walker& walker, const ListTestedInFrontOfLoop& tested)
{
    uint64_t noted = tested.name.bits()
        | static_cast<uint64_t>(tested.storage) << shiftOfStorageInListReadsInLoops
        | static_cast<uint64_t>(tested.admitsInt32Storage) << shiftOfSecondKindInListReadsInLoops
        | static_cast<uint64_t>(tested.isWritable) << shiftOfWritableInListReadsInLoops
        | static_cast<uint64_t>(tested.turnsStoreIntoLists) << shiftOfStoresInListReadsInLoops;
    noteKnown(walker, KnownNote::ListTestedInFrontOfLoop, tested.name, { }, 0, LocationMasks(classOfProvenViewInListReadsInLoops(walker, tested.name) ? heapsUnderViewsReadInLoops : heapsUnderListsReadInLoops), noted);
}

static std::optional<ListTestedInFrontOfLoop> findNoteInListReadsInLoops(Walker& walker, Reference list)
{
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::ListTestedInFrontOfLoop, list);
    if (!noted)
        return std::nullopt;
    return ListTestedInFrontOfLoop {
        Reference::fromBits(static_cast<uint32_t>(*noted)),
        static_cast<ArrayStorage>(*noted >> shiftOfStorageInListReadsInLoops & 0xff),
        !!(*noted >> shiftOfSecondKindInListReadsInLoops & 1),
        !!(*noted >> shiftOfWritableInListReadsInLoops & 1),
        !!(*noted >> shiftOfStoresInListReadsInLoops & 1)
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
    if (!outermost && !walker.isInLoop())
        return std::nullopt;
    while (LoopID around = outermost ? index.loop(outermost).parent : LoopID())
        outermost = around;
    uint32_t firstValueOfLoops = outermost ? index.block(index.loop(outermost).head).firstParameter().index() : 0;
    for (ValueID input = read.objectOperand(); input; input = index.value(input).operand(0)) {
        if (index.isParameter(input) && index.blockOf(input) == walker.inputBlockBeingWalked() && index.isLoopHead(index.blockOf(input)))
            return std::nullopt;
        if (Reference list = walker.referenceOf(input); list && list.isValue()) {
            if (std::optional<ListTestedInFrontOfLoop> tested = findNoteInListReadsInLoops(walker, list))
                return outermost || !tested->turnsStoreIntoLists ? tested : std::nullopt;
        }
        if (!isCastInLoopsInListReadsInLoops(index, input, firstValueOfLoops))
            break;
    }
    return std::nullopt;
}

std::optional<ListTestedInFrontOfLoop> findListTestedInFrontOfLoop(Walker& walker, Reference list)
{
    if (walker.state().answers().isEmpty() || !list || !list.isValue())
        return std::nullopt;
    return findNoteInListReadsInLoops(walker, list);
}

bool readsLengthOfViewTestedInFrontOfLoop(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (read.opcode() != Opcode::PropGet || read.isFreeName() || read.key() != walker.program().keyOf(BuiltinNames::length))
        return false;
    if (read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words())
        return false;
    std::optional<ListTestedInFrontOfLoop> tested = findListTestedInFrontOfLoop(walker, read);
    return tested && classOfProvenViewInListReadsInLoops(walker, tested->name);
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

Reference storageOfListFromInFrontOfLoop(Walker& walker, const ListTestedInFrontOfLoop& tested, Origin origin)
{
    if (!walker.isInLoop())
        return { };
    Draft storage(Opcode::GetArrayStorage, origin);
    walker.chooseConservativeFields(storage);
    storage.setArrayOperand(tested.name);
    return buildInFrontOfTurnsInListReadsInLoops(walker, WTF::move(storage));
}
bool positionIsKnownToLieInsideList(Walker& walker, Reference list, Reference position)
{
    return isKnownToLieInsideList(walker, list, position);
}

bool askPlainVectorBehindRowOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::TypedOperations) || walker.testsEveryPosition())
        return false;
    std::optional<ListTestedInFrontOfLoop> tested = findListTestedInFrontOfLoop(walker, walker.referenceOf(value.arrayOperand()));
    if (!tested || tested->storage != ArrayStorage::Contiguous)
        return false;
    Draft draft = walker.draftOf(value);
    Reference storage = storageOfListFromInFrontOfLoop(walker, *tested, draft.origin());
    if (!storage)
        return false;
    draft.setStorageOperand(storage);
    if (value.opcode() == Opcode::HasContiguousIndex)
        draft.setIsInBounds(positionIsKnownToLieInsideList(walker, tested->name, walker.referenceOf(value.positionOperand())));
    walker.bind(value.id(), walker.build(WTF::move(draft)));
    return true;
}

bool holeLeavesTurnBehindRow(const BodyIndex& index, const BodyValue& read, const ListTestedInFrontOfLoop& tested)
{
    if (!holdsOnlyNumbers(tested.storage))
        return false;
    if (tested.turnsStoreIntoLists)
        return true;
    std::optional<BodyUser> user = index.onlyUser(read.id());
    while (user && user->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(user->value)))
        user = index.onlyUser(user->value);
    return user && user->kind == BodyUseKind::EdgeArgument;
}

bool readLeavesTurnAtHoleBehindRow(Walker& walker, const BodyValue& read)
{
    if (!walker.standsInTurnThatLooksCountOn())
        return false;
    std::optional<ListTestedInFrontOfLoop> tested = findListTestedInFrontOfLoop(walker, read);
    return tested && holeLeavesTurnBehindRow(walker.currentBodyIndex(), read, *tested);
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

static bool turnMayAppendToTextInListReadsInLoops(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const ListTestedInFrontOfLoop> lists, std::span<const GuessInFrontOfLoop> guesses, std::span<const Reference> proofs)
{
    if (!mayKeepTextsInStringBuilders(walker))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto isNumberBehindTestsOfLists = [&](ValueID operand) {
        ValueID root = index.valueBehindCasts(operand);
        if (root.index() >= firstValueOfLoop && !index.isParameter(root) && index.opcodeOf(root) == Opcode::ElemGet) {
            ValueID entersLoop = valueThatEntersLoopInListReadsInLoops(index, index.value(root).objectOperand(), firstValueOfLoop);
            Reference name = entersLoop.index() < firstValueOfLoop ? walker.referenceOf(entersLoop) : Reference();
            Reference proof;
            for (size_t place = 0; name && place < guesses.size() && place < proofs.size(); ++place) {
                if (guesses[place].value == name)
                    proof = proofs[place];
            }
            if (name && std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) { return (list.name == name || (proof && list.name == proof)) && holdsOnlyNumbers(list.storage); }))
                return true;
        }
        ValueID step = root.index() >= firstValueOfLoop && !index.isParameter(root) && index.opcodeOf(root) == Opcode::ResultAt ? index.value(root).operand(0) : ValueID();
        if (const Walker::IterationByIndex* stepped = step && !index.isParameter(step) ? findListSteppedByIndexAlone(walker, index, index.value(step)) : nullptr) {
            if (std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) { return list.name == stepped->list && holdsOnlyNumbers(list.storage); }))
                return true;
        }
        size_t place = operand.index() - preview.firstValue;
        TypeID known = operand.index() < firstValueOfLoop ? ownTypeInListReadsInLoops(walker, walker.referenceOf(operand)) : operand.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
        return known && walker.types().at(known).isNumber();
    };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (!operationsThatJoinTexts.contains(operation.opcode()))
                continue;
            if (operation.opcode() == Opcode::Add && isNumberBehindTestsOfLists(operation.operand(0)) && isNumberBehindTestsOfLists(operation.operand(1)))
                continue;
            size_t place = operation.id().index() - preview.firstValue;
            TypeID joined = operation.id().index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
            if (!joined || walker.types().at(joined).isAnyString())
                return true;
        }
    }
    return false;
}

static bool rowFindsListsLeftAsTheyAreInListReadsInLoops(Walker& walker, BlockID inputHead, std::span<const ListTestedInFrontOfLoop> lists, std::span<const GuessInFrontOfLoop> guesses, std::span<const Reference> proofs)
{
    if (walker.listsAreStruckInRow())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    for (ValueID ran : walker.operationsThatRanOtherCodeOfRow()) {
        if (!ran)
            return false;
        BodyValue operation = index.value(ran);
        bool readsList = false;
        bool isStore = operation.opcode() == Opcode::ElemSet;
        if (operation.opcode() == Opcode::ElemGet || isStore || (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == walker.program().keyOf(BuiltinNames::length))) {
            ValueID entersLoop = valueThatEntersLoopInListReadsInLoops(index, operation.objectOperand(), firstValueOfLoop);
            Reference name = entersLoop ? walker.referenceOf(entersLoop) : Reference();
            Reference proof;
            for (size_t place = 0; name && place < guesses.size() && place < proofs.size(); ++place) {
                if (guesses[place].value == name)
                    proof = proofs[place];
            }
            readsList = name && std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) { return (list.name == name || (proof && list.name == proof)) && (!isStore || list.isWritable); });
        } else if (const Walker::IterationByIndex* stepped = findListSteppedByIndexAlone(walker, index, operation))
            readsList = std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) { return list.name == stepped->list; });
        if (readsList || effectRuleOf(operation.opcode()) == EffectRule::Operator)
            continue;
        const LoopPreview* row = walker.previewOutOfRow();
        auto typeInRow = [&](ValueID input) -> TypeID {
            TypeID known;
            if (row && input.index() >= row->firstValue && input.index() - row->firstValue < row->typesOfValues.size())
                known = row->typesOfValues[input.index() - row->firstValue];
            else if (Reference built = walker.referenceOf(input))
                known = walker.typeOf(built);
            return known ? known : TypeID(TypeTable::anything);
        };
        if (!walker.looksAskOnlyWhetherTurnsRunOtherCode(inputHead) || !guessesMayTypeOperationInCleanTurn(walker, operation, TypesOfInputs(typeInRow)))
            return false;
    }
    if (walker.previewOutOfRow() && turnMayAppendToTextInListReadsInLoops(walker, inputHead, *walker.previewOutOfRow(), lists, guesses, proofs))
        return false;
    return leavesListsTestedInFrontOfLoopAsTheyAre(walker.effectsOfQuietDraftsOfRow());
}

Vector<ValueID, 2> namesInFrontThatLoopMentions(const Walker& walker, const BodyIndex& index, LoopID loop, uint32_t firstValueOfLoop, Reference value)
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

void reserveRoomOfListsThatLoopFills(Walker& walker, BlockID inputHead, std::span<const Reference> entering)
{
    if (!walker.has(Mechanism::TypedOperations))
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();

    BodyTerminator test = index.terminator(inputHead);
    if (!test.isConditionalBranch() || !index.isInLoop(test.taken().block(), loop) || index.isInLoop(test.notTaken().block(), loop))
        return;
    ValueID condition = test.operand(0);
    if (!condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::Less || index.blockOf(condition) != inputHead)
        return;
    ValueID counter = index.valueBehindCasts(index.value(condition).operand(0));
    ValueID bound = index.valueBehindCasts(index.value(condition).operand(1));
    if (!index.isParameter(counter) || index.blockOf(counter) != inputHead || bound.index() >= firstValueOfLoop)
        return;
    size_t placeOfCounter = counter.index() - firstValueOfLoop;
    if (placeOfCounter >= entering.size() || !entering[placeOfCounter])
        return;
    const Type& first = types.at(walker.typeOf(entering[placeOfCounter]));
    if (!first.isInteger() || first.integers != Range::one(0))
        return;

    Vector<BlockID, 2> blocksThatComeRound;
    for (BlockID member : index.membersOf(loop)) {
        if (index.block(member).isHandlerBlock())
            return;
        BodyTerminator end = index.terminator(member);
        if (!end.numberOfTargets())
            return;
        for (BodyTarget target : end.targets()) {
            if (!index.isInLoop(target.block(), loop)) {
                if (member != inputHead)
                    return;
                continue;
            }
            if (target.block() != inputHead)
                continue;
            ValueID next = placeOfCounter < target.arguments().size() ? index.valueBehindCasts(target.arguments()[placeOfCounter]) : ValueID();
            if (!next || index.isParameter(next) || index.opcodeOf(next) != Opcode::Increment || index.valueBehindCasts(index.value(next).operand(0)) != counter)
                return;
            blocksThatComeRound.append(member);
        }
    }

    auto listWhoseLengthIsRead = [&](ValueID value) -> ValueID {
        if (index.isParameter(value) || index.opcodeOf(value) != Opcode::PropGet || index.value(value).isFreeName() || index.value(value).key() != walker.program().keyOf(BuiltinNames::length))
            return { };
        ValueID input = index.valueBehindCasts(index.value(value).objectOperand());
        return input.index() < firstValueOfLoop && isProvenListInListReadsInLoops(walker, walker.referenceOf(input)) ? input : ValueID();
    };
    auto isPlainPrimitive = [&](ValueID value) {
        value = index.valueBehindCasts(value);
        if (value == counter)
            return true;
        TypeID type = TypeTable::anything;
        if (value.index() < firstValueOfLoop)
            type = walker.referenceOf(value) ? walker.typeOf(walker.referenceOf(value)) : type;
        else if (!index.isParameter(value))
            type = widestTypeOfOperation(walker, index.value(value));
        return type && types.at(type).hasOnlyPrimitives(Primitive::Number | Primitive::AnyString | Primitive::Boolean | Primitive::Undefined | Primitive::Null);
    };

    Vector<Reference, 2> lists;
    for (BlockID member : index.membersOf(loop)) {
        bool standsInEveryTurn = std::ranges::all_of(blocksThatComeRound, [&](BlockID last) { return index.dominates(member, last); });
        for (BodyValue operation : index.block(member).values()) {
            if (listWhoseLengthIsRead(operation.id()))
                continue;
            if (operation.opcode() == Opcode::ElemSet) {
                ValueID input = index.valueBehindCasts(operation.objectOperand());
                ValueID position = index.valueBehindCasts(operation.propertyOperand());
                Reference list = input.index() < firstValueOfLoop ? walker.referenceOf(input) : Reference();
                if (!standsInEveryTurn || (position != counter && (position.index() < firstValueOfLoop || listWhoseLengthIsRead(position) != input)) || !isProvenListInListReadsInLoops(walker, list))
                    return;
                if (!lists.contains(list))
                    lists.append(list);
                continue;
            }
            Effects declared = declaredEffectsOfOperation(walker.program(), operation.opcode(), operation);
            if (!declared.mayThrow.byRule() && !declared.mayRunOtherCode())
                continue;
            bool takesPlainPrimitives = effectRuleOf(operation.opcode()) == EffectRule::Operator;
            operation.forEachOperand([&](ValueID operand, auto...) {
                takesPlainPrimitives &= !operand || isPlainPrimitive(operand);
            });
            if (!takesPlainPrimitives)
                return;
        }
    }
    if (lists.isEmpty())
        return;

    Origin origin = walker.originOf(index.value(condition));
    Reference room = walker.referenceOf(bound);
    if (!room)
        return;
    if (!types.at(walker.typeOf(room)).isNumber())
        return;
    auto behindForms = [&](ValueID value) {
        while (conversionsOfForm.contains(walker.code().at(value).opcode))
            value = walker.code().at(value).valueOperand();
        return value;
    };
    for (Reference list : lists) {
        const Operation& born = walker.code().at(rootOfCellInSweptCode(walker.code(), list.id()));
        if ((born.opcode == Opcode::NewArrayLike || born.opcode == Opcode::NewContiguousArray) && room.isValue() && behindForms(born.lengthOperand()) == behindForms(room.id()))
            continue;
        Draft reserve(Opcode::ReserveArrayRoom, origin);
        walker.chooseConservativeFields(reserve);
        reserve.setArrayOperand(list);
        reserve.setRoomOperand(room);
        walker.build(WTF::move(reserve));
    }
}

static void forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(Walker& walker, FunctionID callee, std::span<const ValueID> arguments, unsigned depth, const Invocable<void(ValueID argument, ValueID key, FunctionID reader, ValueID read)> auto& take)
{
    const BodyIndex& body = walker.facts().bodies().at(callee);
    BlockID entry = BlockID(0u);
    auto argumentBehind = [&](ValueID value) -> ValueID {
        value = body.valueBehindCasts(value);
        if (!value || !body.isParameter(value) || body.blockOf(value) != entry)
            return { };
        size_t place = value.index() - body.block(entry).firstParameter().index();
        return place < arguments.size() ? arguments[place] : ValueID();
    };
    for (uint32_t block = 0; block < body.numberOfBlocks(); ++block) {
        for (BodyValue operation : body.block(BlockID(block)).values()) {
            if (operation.opcode() == Opcode::ElemGet) {
                ValueID key = body.valueBehindCasts(operation.propertyOperand());
                bool keyIsWritten = key && !body.isParameter(key) && body.opcodeOf(key) == Opcode::Integer;
                ValueID keyHandedIn = keyIsWritten ? ValueID() : argumentBehind(key);
                if (ValueID argument = keyIsWritten || keyHandedIn ? argumentBehind(operation.objectOperand()) : ValueID())
                    take(argument, keyHandedIn, callee, operation.id());
                continue;
            }
            if (operation.opcode() != Opcode::Call || !depth)
                continue;
            FunctionID inner = walker.facts().linked().calleeAt(callee, operation.id());
            if (!inner || walker.facts().bodies().at(inner).hasLoops())
                continue;
            Vector<ValueID, 4> handedOn;
            for (ValueID argument : operation.as<Opcode::Call>().argumentsOperands())
                handedOn.append(argument ? argumentBehind(argument) : ValueID());
            forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, inner, handedOn.span(), depth - 1, take);
        }
    }
}

template<typename TypeInLook>
static bool surelyEndsWhatIsShownOfListsInListReadsInLoops(Walker& walker, const BodyValue& operation, uint32_t firstValueOfLoop, const TypeInLook& typeInLook, bool countsOnTurnsThatBeginAgain)
{
    TypeArena& types = walker.types();
    if (operation.opcode() == Opcode::Call) {
        if (walker.previewOutOfRow() && std::ranges::find(walker.operationsThatRanOtherCodeOfRow(), operation.id()) == walker.operationsThatRanOtherCodeOfRow().end())
            return false;
        const LinkTables& linked = walker.facts().linked();
        FunctionID known = linked.calleeAt(walker.currentFunction(), operation.id());
        if (TypeID called = known ? TypeID() : typeInLook(operation.as<Opcode::Call>().calleeOperand()))
            known = singleFunctionOf(types, types.at(called));
        if (known && countsOnTurnsThatBeginAgain && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, known))
            return false;
        if (FunctionID callee = known)
            return !leavesListsTestedInFrontOfLoopAsTheyAre(linked.function(callee).effectsOnReturn()) || !leavesListsTestedInFrontOfLoopAsTheyAre(linked.function(callee).effectsOnThrow());
        TypeID callee = typeInLook(operation.as<Opcode::Call>().calleeOperand());
        return !callee || !singleBuiltinOf(types, types.at(callee));
    }
    if (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.objectOperand().index() >= firstValueOfLoop && !countsOnTurnsThatBeginAgain) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID handedOut = index.valueBehindCasts(operation.objectOperand());
        ValueID step = !index.isParameter(handedOut) && index.opcodeOf(handedOut) == Opcode::ResultAt ? index.value(handedOut).operand(0) : ValueID();
        if (step && !index.isParameter(step) && findListSteppedByIndexAlone(walker, index, index.value(step)))
            return false;
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
            if (const Walker::IterationByIndex* stepped = findListSteppedByIndexAlone(walker, index, operation); stepped && stepped->isDeadBehindLoop && stepped->list.isValue()) {
                Reference name = stepped->list;
                bool isListed = found.lists.containsIf([&](const ListTestedInFrontOfLoop& list) {
                    return list.name == name;
                });
                if (isListed || isFreshInListReadsInLoops(walker, name) || findNoteInListReadsInLoops(walker, name))
                    continue;
                ArrayStorage written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(ownTypeInListReadsInLoops(walker, name)));
                if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, index.value(operation.operand(0)).as<Opcode::IteratorOpen>().iterableOperand()).type : TypeID())
                    written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(expected));
                bool holdsNumbers = holdsOnlyNumbers(written);
                TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id());
                if (seenByLink && types.at(seenByLink).isInt32OrNullish())
                    found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Int32, false });
                else
                    found.lists.append(ListTestedInFrontOfLoop { name, holdsNumbers ? ArrayStorage::Double : written, holdsNumbers });
                continue;
            }
            if (FunctionID copied = operation.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()) : FunctionID(); copied && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, copied)) {
                forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, copied, operation.as<Opcode::Call>().argumentsOperands(), 2, [&](ValueID argument, ValueID key, FunctionID reader, ValueID read) {
                    ValueID input = valueThatEntersLoopInListReadsInLoops(index, argument, firstValueOfLoop);
                    Reference name = input.index() < firstValueOfLoop ? walker.referenceOf(input) : Reference();
                    if (!name || !name.isValue() || isFreshInListReadsInLoops(walker, name) || findNoteInListReadsInLoops(walker, name))
                        return;
                    TypeID proven = ownTypeInListReadsInLoops(walker, name);
                    for (const GuessInFrontOfLoop& guess : guesses) {
                        if (guess.value == name)
                            proven = types.intersectionOf(proven, guess.tested);
                    }
                    TypeID list = types.withoutNullish(proven);
                    if (!hasKind(types, list, OperandKind::Array) || (key && !readTakesIndexOfProvenList(types, list, typeInLook(key))))
                        return;
                    if (found.lists.containsIf([&](const ListTestedInFrontOfLoop& list) { return list.name == name; }))
                        return;
                    TypeID seenByLink = walker.facts().linked().resultOfReadByKey(reader, read);
                    if (seenByLink && types.at(seenByLink).isInt32OrNullish()) {
                        found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Int32, false });
                        return;
                    }
                    ArrayStorage written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, list);
                    if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, input).type : TypeID())
                        written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(expected));
                    if (written == ArrayStorage::Other)
                        return;
                    bool holdsNumbers = holdsOnlyNumbers(written);
                    found.lists.append(ListTestedInFrontOfLoop { name, holdsNumbers ? ArrayStorage::Double : written, holdsNumbers });
                });
                continue;
            }
            bool isStore = operation.opcode() == Opcode::ElemSet && walker.looksCountOnTurnsThatBeginAgain(inputHead);
            bool readsLength = operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == walker.program().keyOf(BuiltinNames::length);
            bool asksPlainVector = operation.opcode() == Opcode::HasContiguousIndex;
            bool storesByKey = operation.opcode() == Opcode::ElemSet;
            if (operation.opcode() != Opcode::ElemGet && !storesByKey && !readsLength && !asksPlainVector)
                continue;
            ValueID input = valueThatEntersLoopInListReadsInLoops(index, asksPlainVector ? operation.arrayOperand() : operation.objectOperand(), firstValueOfLoop);
            if (input.index() >= firstValueOfLoop)
                continue;
            Reference name = walker.referenceOf(input);
            if (!name || !name.isValue())
                continue;
            size_t placeInList = found.lists.findIf([&](const ListTestedInFrontOfLoop& list) {
                return list.name == name;
            });
            bool isListed = placeInList != notFound;
            ArrayStorage kindStoredInto = ArrayStorage::Other;
            if (isStore) {
                TypeID stored = typeInLook(operation.valueOperand());
                stored = stored ? types.withoutPrimitives(stored, Primitive::Undefined) : TypeID();
                if (!stored || isNeverType(stored) || !types.at(stored).isNumber())
                    continue;
                kindStoredInto = types.at(stored).primitives & Primitive::FractionalNumber ? ArrayStorage::Double : ArrayStorage::Int32;
            }
            if (isStore && isListed) {
                ListTestedInFrontOfLoop& list = found.lists[placeInList];
                bool isOfEitherKind = list.storage == ArrayStorage::Double && list.admitsInt32Storage;
                if (list.isWritable && list.storage != kindStoredInto)
                    kindStoredInto = ArrayStorage::Double;
                else if (!list.isWritable && !isOfEitherKind && list.storage != kindStoredInto)
                    continue;
                list.storage = kindStoredInto;
                list.admitsInt32Storage = false;
                list.isWritable = true;
            }
            if (isListed)
                continue;

            TypeID proven = ownTypeInListReadsInLoops(walker, name);
            for (const GuessInFrontOfLoop& guess : guesses) {
                if (guess.value == name)
                    proven = types.intersectionOf(proven, guess.tested);
            }
            if (BuiltinClassID classOfView = readsLength || asksPlainVector ? BuiltinClassID() : classOfViewInListReadsInLoops(walker, types.withoutNullish(proven))) {
                bool lengthMayStandInFront = hasInlineElementAccess(walker.program().builtins().typedArrayTypeOf(classOfView)) && !walker.testsEveryPosition() && !walker.effectsOfQuietDraftsOfRow().writes.contains(AbstractHeap::TypedArrayStorage);
                if (lengthMayStandInFront && viewMayFollowSharedMemoryThatGrows(walker, name) && !isFreshInListReadsInLoops(walker, name) && !findNoteInListReadsInLoops(walker, name))
                    found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Other, false });
                continue;
            }
            if (storesByKey && !isStore)
                continue;
            if (readsLength) {
                bool staysAsMadeByLink = readsOffEntryParameterThatStaysAsMade(walker, operation) || receiverStaysAsMadeByLink(walker, operation);
                if (!staysAsMadeByLink && classOfViewInListReadsInLoops(walker, proven) && !isFreshInListReadsInLoops(walker, name) && !isListAsMade(walker, name) && !findNoteInListReadsInLoops(walker, name))
                    found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Other, false });
                continue;
            }
            TypeID list = types.withoutNullish(proven);
            if (!readTakesIndexOfProvenList(types, list, typeInLook(asksPlainVector ? operation.positionOperand() : operation.propertyOperand())))
                continue;
            if (isFreshInListReadsInLoops(walker, name))
                continue;
            if (findNoteInListReadsInLoops(walker, name))
                continue;

            if (isStore) {
                found.lists.append(ListTestedInFrontOfLoop { name, kindStoredInto, false, true });
                continue;
            }
            ArrayStorage written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, list);
            if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, input).type : TypeID())
                written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(expected));
            if (asksPlainVector) {
                if (!holdsOnlyNumbers(written))
                    found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Contiguous, false });
                continue;
            }
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

    bool turnsStoreIntoLists = found.lists.containsIf([](const ListTestedInFrontOfLoop& list) {
        return list.isWritable;
    });
    for (ListTestedInFrontOfLoop& list : found.lists)
        list.turnsStoreIntoLists = turnsStoreIntoLists;
    if (guesses.isEmpty()) {
        if (!rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), { }, { }))
            return { };
        found.areLeftAsTheyAre = true;
    }

    for (const ListTestedInFrontOfLoop& list : found.lists) {
        bool hasGuess = guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
            return guess.value == list.name;
        });
        if (hasGuess || isProvenListInListReadsInLoops(walker, list.name) || classOfProvenViewInListReadsInLoops(walker, list.name))
            continue;
        guesses.append(GuessInFrontOfLoop { list.name, types.builtinClass(BuiltinClasses::Array), { }, namesInFrontThatLoopMentions(walker, index, loop, firstValueOfLoop, list.name) });
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
        return !isProvenListInListReadsInLoops(walker, list.name) && !classOfProvenViewInListReadsInLoops(walker, list.name);
    });
    if (found.lists.isEmpty())
        return;
    if (!found.areLeftAsTheyAre && !rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), guesses, proofs))
        return;

    for (const ListTestedInFrontOfLoop& list : found.lists) {
        Draft isAsMade(Opcode::IsPristine, origin);
        walker.chooseConservativeFields(isAsMade);
        isAsMade.setValueOperand(list.name);
        BuiltinClassID classOfView = classOfProvenViewInListReadsInLoops(walker, list.name);
        isAsMade.setBuiltinClass(classOfView ? classOfView : BuiltinClassID(BuiltinClasses::Array));
        PristineAspectSet aspects { PristineAspects::Storage, PristineAspects::Names };
        bool asksCellAlone = walker.program().admitsReplacedBuiltins;
        BuiltinID lengthAskedByItsByte;
        HoldersBelowMember holdersBelowLength;
        if (classOfView && asksCellAlone) {
            PropertyKeyID key = walker.program().keyOf(BuiltinNames::length);
            TypeID views = walker.types().builtinClass(classOfView);
            BuiltinID length = findMemberOnObjectAsMade(walker, views, key);
            BeliefInMemberAbove belief = length ? beliefInMemberFoundOnObjectAsMade(walker, length, views, key, &holdersBelowLength) : BeliefInMemberAbove::RestsOnByteOfClass;
            asksCellAlone = belief != BeliefInMemberAbove::RestsOnByteOfClass;
            if (belief == BeliefInMemberAbove::IsAskedByItsByte)
                lengthAskedByItsByte = length;
        }
        if (asksCellAlone)
            aspects.add(PristineAspects::CellAlone);
        isAsMade.setAspects(aspects);
        Reference test = walker.build(WTF::move(isAsMade));
        bool isAskedOfCell = test && test.isValue() && !walker.code().isConstant(test.id());
        if (!goOnWhereTestPassesInListReadsInLoops(walker, test, closedStart))
            return;
        if (lengthAskedByItsByte) {
            if (!goOnWhereTestPassesInListReadsInLoops(walker, buildTestOfMemberAsBuilt(walker, lengthAskedByItsByte, origin), closedStart))
                return;
            noteMemberPassedTestOfBeingAsBuilt(walker, lengthAskedByItsByte);
            for (BuiltinID holder : holdersBelowLength) {
                if (!goOnWhereTestPassesInListReadsInLoops(walker, buildTestOfHolderWithoutAddedNames(walker, holder, origin), closedStart))
                    return;
                noteHolderPassedTestOfHavingNoAddedNames(walker, holder);
            }
        }
        if (classOfView && isAskedOfCell)
            noteKnown(walker, KnownNote::ViewOfFixedLength, list.name, { }, 0, LocationMasks(), 1);
        if (!hasArrayStorageForm(list.storage))
            continue;
        Draft hasKindWritten(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(hasKindWritten);
        hasKindWritten.setArrayOperand(list.name);
        hasKindWritten.setArrayStorage(list.storage);
        hasKindWritten.setAdmitsInt32Storage(list.admitsInt32Storage);
        hasKindWritten.setIsWritable(list.isWritable);
        if (!goOnWhereTestPassesInListReadsInLoops(walker, walker.build(WTF::move(hasKindWritten)), closedStart))
            return;
    }
    for (const ListTestedInFrontOfLoop& list : found.lists) {
        noteListTestedInListReadsInLoops(walker, list);
        if (classOfProvenViewInListReadsInLoops(walker, list.name))
            noteListAsMade(walker, list.name);
    }
    found.areTested = true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
