/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkListReadsInLoops.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOperandKinds.h"
#include "AOTResultRules.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include "AOTWalkBlocksThatEveryTurnPasses.h"
#include "AOTWalkKnown.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"
#include "JSString.h"
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

BuiltinClassID classOfViewBesideInstancesBelowIt(const Walker& walker, TypeID type)
{
    if (!type || !walker.types().at(type).hasOnlyClassObjects())
        return { };
    std::span<const ClassPart> parts = classPartsOf(walker.types(), walker.types().at(type));
    if (parts.size() != 2)
        return { };
    const BuiltinTable& table = walker.program().builtins();
    for (unsigned index = 0; index < 2; ++index) {
        BuiltinClassID above = parts[index].builtinClass;
        if (isTypedView(table.typedArrayTypeOf(above)) && classBelowWithTestedStructure(table, above) == parts[1 - index].builtinClass)
            return above;
    }
    return { };
}

static BuiltinClassID classOfViewInListReadsInLoops(const Walker& walker, TypeID type)
{
    const ClassPart* part = type && walker.types().at(type).isObject() ? singleBuiltinClassOf(walker.types(), walker.types().at(type)) : nullptr;
    if (!part) {
        if (BuiltinClassID above = classOfViewBesideInstancesBelowIt(walker, type))
            return above;
        return type && walker.types().at(type).hasOnlyClassObjects() ? classAboveViewsOfSeveralKinds(walker.program(), walker.types(), type) : BuiltinClassID();
    }
    return isTypedView(walker.program().builtins().typedArrayTypeOf(part->builtinClass)) ? part->builtinClass : BuiltinClassID();
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

static bool isAskedForStoresAlone(const ListTestedInFrontOfLoop& tested)
{
    return tested.isWritable && !hasArrayStorageForm(tested.storage);
}

static bool isShownToReads(const ListTestedInFrontOfLoop& tested)
{
    return !isAskedForStoresAlone(tested) && accessToElementsHasCode(Opcode::GetArrayElement, tested.storage, tested.admitsInt32Storage, false);
}

static bool holdsBoxedValuesOfEitherKind(const ListTestedInFrontOfLoop& tested)
{
    return tested.storage == ArrayStorage::Contiguous && tested.admitsInt32Storage;
}

bool leavesListsTestedInFrontOfLoopAsTheyAre(const Effects& effects)
{
    return !effects.mayRunOtherCode() && !effects.writes.containsAny(heapsUnderListsReadInLoops);
}

bool leavesListsTestedInFrontOfLoopAsTheyAre(const Walker& walker, const Effects& effects, std::span<const ListTestedInFrontOfLoop> lists, const Walker::ObjectsMadeHereThatLoopMentions& mentioned)
{
    if (effects.mayRunOtherCode())
        return false;
    if (!effects.writes.containsAny(heapsUnderListsReadInLoops))
        return true;
    return effects.writesObjectsMadeHereAlone && std::ranges::all_of(lists, [&](const ListTestedInFrontOfLoop& list) {
        std::optional<Walker::IsItselfMentioned> other = walker.isOtherThanRestOf(mentioned, list.name);
        return other && (*other == Walker::IsItselfMentioned::No || !list.isWritable);
    });
}

bool leavesListsTestedInFrontOfLoopAsTheyAre(Walker& walker, const Effects& effects, std::span<const ListTestedInFrontOfLoop> lists, BlockID inputHead)
{
    if (leavesListsTestedInFrontOfLoopAsTheyAre(effects))
        return true;
    return !effects.mayRunOtherCode() && effects.writesObjectsMadeHereAlone && leavesListsTestedInFrontOfLoopAsTheyAre(walker, effects, lists, walker.objectsMadeHereThatLoopMentions(inputHead));
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

static ValueID readOfVariableInLoopInListReadsInLoops(const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    ValueID read = valueThatEntersLoopInListReadsInLoops(index, value, firstValueOfLoop);
    if (read && read.index() >= firstValueOfLoop && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = valueThatEntersLoopInListReadsInLoops(index, index.value(read).operand(0), firstValueOfLoop);
    if (!read || read.index() < firstValueOfLoop || index.isParameter(read))
        return { };
    if (index.opcodeOf(read) == Opcode::GetModuleVariable)
        return read;
    return index.opcodeOf(read) == Opcode::GetEnvironmentField && index.value(read).environmentOperand().index() < firstValueOfLoop ? read : ValueID();
}

static VariableID variableThatFindsListInListReadsInLoops(const BodyIndex& index, ValueID input)
{
    ValueID source = index.valueBehindCasts(input);
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
        source = index.valueBehindCasts(index.value(source).operand(0));
    return source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::GetModuleVariable ? index.value(source).variable() : VariableID();
}

static WrittenListOfVariable writtenListReadInListReadsInLoops(Walker& walker, const BodyIndex& index, const BodyValue& read)
{
    VariableID variable = read.opcode() == Opcode::ElemGet ? variableThatFindsListInListReadsInLoops(index, read.objectOperand()) : VariableID();
    return variable && walker.facts().linked().nobodyChangesReceiverAt(walker.currentFunction(), read.id()) ? writtenListThatVariableHolds(walker, variable) : WrittenListOfVariable();
}

static bool textOfLoopStoresVariableInListReadsInLoops(const BodyIndex& index, const BodyValue& read)
{
    LoopID loop = index.loopOf(index.blockOf(read.id()));
    while (LoopID around = loop ? index.loop(loop).parent : LoopID())
        loop = around;
    return !loop || outermostLoopStoresWhatIsRead(index, loop, read);
}

static Reference valueThatStateHoldsOfVariableInListReadsInLoops(Walker& walker, const BodyIndex& index, const BodyValue& read)
{
    if (textOfLoopStoresVariableInListReadsInLoops(index, read))
        return { };
    if (read.opcode() == Opcode::GetModuleVariable)
        return walker.state().variable(read.variable());
    Reference environment = walker.referenceOf(read.environmentOperand());
    return environment ? walker.state().environmentField(environment, FieldReference { read.layout(), read.field() }) : Reference();
}

static Reference nameThatEntersLoopInListReadsInLoops(Walker&, const BodyIndex&, ValueID, uint32_t firstValueOfLoop);

static Reference valueThatStateHoldsInSlotReadInTurnInListReadsInLoops(Walker& walker, const BodyIndex& index, ValueID enters, uint32_t firstValueOfLoop)
{
    if (index.isParameter(enters) || index.opcodeOf(enters) != Opcode::PropGet || index.value(enters).isFreeName())
        return { };
    BodyValue read = index.value(enters);
    LoopID loop = index.loopOf(index.blockOf(enters));
    while (LoopID around = loop ? index.loop(loop).parent : LoopID())
        loop = around;
    if (!loop)
        return { };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::PropSet && !operation.isFreeName() && operation.key() == read.key())
                return { };
        }
    }
    Reference holder = nameThatEntersLoopInListReadsInLoops(walker, index, read.objectOperand(), firstValueOfLoop);
    TypeID type = holder && holder.isValue() ? walker.typeOf(holder) : TypeID();
    if (!type || !walker.types().at(type).isObject() || !walker.types().at(type).hasOnlyLayouts())
        return { };
    std::optional<FieldReference> field = findField(walker.program(), membersOf(walker.types(), walker.types().at(type))[0], read.key());
    return field ? walker.state().slot(holder, *field) : Reference();
}

static Reference nameThatEntersLoopInListReadsInLoops(Walker& walker, const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    ValueID enters = valueThatEntersLoopInListReadsInLoops(index, value, firstValueOfLoop);
    if (!enters)
        return { };
    if (enters.index() < firstValueOfLoop)
        return walker.referenceOf(enters);
    ValueID read = readOfVariableInLoopInListReadsInLoops(index, enters, firstValueOfLoop);
    if (!read)
        return valueThatStateHoldsInSlotReadInTurnInListReadsInLoops(walker, index, enters, firstValueOfLoop);
    if (Reference made = index.opcodeOf(read) == Opcode::GetModuleVariable ? objectOfVariablePutOff(walker, index.value(read).variable()) : Reference())
        return made;
    if (Reference kept = valueOfStableBinding(walker, index.value(read)))
        return kept;
    return valueThatStateHoldsOfVariableInListReadsInLoops(walker, index, index.value(read));
}

Reference nameOfListThatEntersLoopAround(Walker& walker, const BodyIndex& index, const BodyValue& access)
{
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    return loop ? nameThatEntersLoopInListReadsInLoops(walker, index, access.objectOperand(), index.block(index.loop(loop).head).firstParameter().index()) : Reference();
}

static bool lookReadsCopyOfCalleeInListReadsInLoops(Walker&, const BodyValue& call, FunctionID callee);
static void forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(Walker&, FunctionID callee, std::span<const ValueID> arguments, unsigned depth, Opcode access, const Invocable<void(ValueID argument, ValueID key, FunctionID reader, ValueID read)> auto& take);
static bool goesUnderKeyMadeInTurnInListReadsInLoops(const BodyIndex& index, const BodyValue& access)
{
    return (access.opcode() == Opcode::ElemGet || access.opcode() == Opcode::ElemSet) && !index.isParameter(index.valueBehindCasts(access.propertyOperand()));
}

static bool readGivesNoObjectInListReadsInLoops(Walker& walker, const BodyValue& read)
{
    TypeID widest = widestTypeOfOperation(walker, read);
    return widest && !walker.types().at(widest).hasObjects();
}

ValueID readOfVariableThatFindsReceiverInLoop(const BodyIndex& index, const BodyValue& access, uint32_t firstValueOfLoop)
{
    return readOfVariableInLoopInListReadsInLoops(index, access.objectOperand(), firstValueOfLoop);
}

static ValueID holderOffVariableThatNamesLeadFromInListReadsInLoops(Walker& walker, const BodyIndex& index, const BodyValue& access, uint32_t firstValueOfLoop)
{
    bool readsList = access.opcode() == Opcode::ElemGet || access.opcode() == Opcode::ElemSet || (access.opcode() == Opcode::PropGet && !access.isFreeName() && access.key() == walker.program().keyOf(BuiltinNames::length));
    if (!readsList)
        return { };
    ValueID holder = access.objectOperand();
    bool leadsThroughName = false;
    for (ValueID read = index.valueBehindCasts(holder); read && read.index() >= firstValueOfLoop && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName(); read = index.valueBehindCasts(holder)) {
        holder = index.value(read).objectOperand();
        leadsThroughName = true;
    }
    ValueID variable = leadsThroughName ? readOfVariableInLoopInListReadsInLoops(index, holder, firstValueOfLoop) : ValueID();
    TypeID widest = variable ? widestTypeOfOperation(walker, index.value(variable)) : TypeID();
    TypeID whereFilled = widest ? walker.types().withoutPrimitives(widest, Primitive::Absent | Primitive::Undefined) : TypeID();
    return whereFilled && walker.types().at(whereFilled).isObject() && walker.types().at(whereFilled).hasOnlyLayouts() ? holder : ValueID();
}

void readListsOfStableBindingsInFrontOfLoop(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::TypedOperations) || index.loop(loop).parent || (!index.opcodes().contains(Opcode::GetModuleVariable) && !index.opcodes().contains(Opcode::GetEnvironmentField)) || walker.isUnreachable())
        return;
    const Program& program = walker.program();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    std::optional<BitVector> holdersComparedWithCharacters;
    auto loopComparesElementWithCharacters = [&](const BodyValue& read) {
        if (!holdersComparedWithCharacters)
            holdersComparedWithCharacters = findHoldersOfElementsThatLoopComparesWithCharactersOrNumbers(program, index, loop);
        return readsElementUnderIndexOffOneOf(index, read, *holdersComparedWithCharacters);
    };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            Vector<ValueID, 4> lists;
            ValueID holderThatNamesLeadFrom;
            if (readsListFoundByReadFromInFront(program, index, operation, firstValueOfLoop))
                lists.append(operation.objectOperand());
            if ((holderThatNamesLeadFrom = holderOffVariableThatNamesLeadFromInListReadsInLoops(walker, index, operation, firstValueOfLoop)))
                lists.append(holderThatNamesLeadFrom);
            if (FunctionID callee = operation.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()) : FunctionID()) {
                std::span<const ValueID> arguments = operation.as<Opcode::Call>().argumentsOperands();
                bool handsVariableOn = std::ranges::any_of(arguments, [&](ValueID argument) {
                    return argument && readOfVariableInLoopInListReadsInLoops(index, argument, firstValueOfLoop);
                });
                if (handsVariableOn && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, callee)) {
                    forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, callee, arguments, 2, Opcode::ElemGet, [&](ValueID argument, ValueID, FunctionID, ValueID) {
                        if (readOfVariableInLoopInListReadsInLoops(index, argument, firstValueOfLoop) && !lists.contains(argument))
                            lists.append(argument);
                    });
                }
            }
            for (ValueID list : lists) {
            ValueID source = readOfVariableInLoopInListReadsInLoops(index, list, firstValueOfLoop);
            if (!source)
                continue;
            bool isHolder = list == holderThatNamesLeadFrom;
            BodyValue read = index.value(source);
            bool isHolderOfAnotherFile = isHolder && read.opcode() == Opcode::GetModuleVariable && program.at(read.variable()).module != program.at(walker.currentFunction()).module;
            if (read.opcode() == Opcode::GetEnvironmentField) {
                Reference environment = walker.referenceOf(read.environmentOperand());
                FieldReference field { read.layout(), read.field() };
                if (!environment || valueOfStableBinding(walker, read) || walker.state().environmentField(environment, field) || goesUnderKeyMadeInTurnInListReadsInLoops(index, operation) || readGivesNoObjectInListReadsInLoops(walker, read) || textOfLoopStoresVariableInListReadsInLoops(index, read))
                    continue;
                Draft draft = walker.draftOf(read);
                walker.chooseConservativeFields(draft);
                if (walker.state().isPastDeadZone(environment, field))
                    draft.setFollowsItsStore(true);
                Reference held = walker.build(WTF::move(draft));
                if (!held)
                    return;
                walker.holdWhatBodyReadsOfItsOwnScope(read, environment, held);
                walker.noteEnvironmentField(environment, field, held);
                noteValueOfStableBinding(walker, environment, field, held);
                walker.count(StatisticID::CapturedListsReadInFrontOfLoops);
                continue;
            }
            VariableID variable = read.variable();
            if (objectOfVariablePutOffStays(walker, variable)) {
                if (objectOfVariablePutOff(walker, variable) || walker.state().variable(variable))
                    continue;
                TypeID widest = widestTypeOfOperation(walker, read);
                if (!widest || (walker.types().at(widest).primitives & Primitive::Absent & ~walker.plan().deferredDefinitions().findDeferred(variable)->mark))
                    continue;
                Draft asWritten = walker.draftOf(read);
                walker.chooseConservativeFields(asWritten);
                Reference held = walker.build(WTF::move(asWritten));
                if (!held || !takeObjectOfDeferredVariable(walker, variable, held, walker.originOf(read)) || walker.isUnreachable())
                    return;
                walker.count(StatisticID::ConstantsOfFilesReadInFrontOfLoops);
                continue;
            }
            if (!numberReadOutOfVariableStays(walker, variable) && !walker.plan().deferredDefinitions().findDeferred(variable)) {
                if (walker.state().variable(variable) || (!isHolder && program.at(variable).module != program.at(walker.currentFunction()).module) || goesUnderKeyMadeInTurnInListReadsInLoops(index, operation) || readGivesNoObjectInListReadsInLoops(walker, read) || textOfLoopStoresVariableInListReadsInLoops(index, read))
                    continue;
                Draft draft = walker.draftOf(read);
                walker.chooseConservativeFields(draft);
                if (isHolderOfAnotherFile)
                    draft.setGivesWordAsItLies(true);
                else if (walker.state().isPastDeadZone(variable))
                    draft.setFollowsItsStore(true);
                Reference held = walker.build(WTF::move(draft));
                if (!held)
                    return;
                walker.state().noteVariable(variable, held);
                walker.count(StatisticID::CapturedListsReadInFrontOfLoops);
                continue;
            }
            if (!numberReadOutOfVariableStays(walker, variable) || valueOfStableBinding(walker, read))
                continue;
            if (!isHolder && program.at(variable).module != program.at(walker.currentFunction()).module && !walker.state().isPastDeadZone(variable))
                continue;
            TypeID widest = widestTypeOfOperation(walker, read);
            bool isHandedOver = operation.opcode() == Opcode::Call;
            if (isHandedOver && widest && !hasKind(walker.types(), walker.types().withoutNullish(widest), OperandKind::Array))
                continue;
            if (!widest || (walker.types().at(widest).primitives & Primitive::Unspecified))
                continue;
            bool mayBeInDeadZone = isHolderOfAnotherFile || (walker.types().at(widest).primitives & (isHolder ? Primitive::Absent | Primitive::Undefined : Primitive::Absent));
            if (mayBeInDeadZone && isHolder && walker.state().variable(variable))
                continue;
            if (mayBeInDeadZone && !isHolder && (isHandedOver || (!loopReckonsWithElementRead(walker, operation) && !loopComparesElementWithCharacters(operation)) || walker.state().variable(variable) || !walker.types().saysNothing(walker.types().withoutPrimitives(widest, Primitive::Absent))))
                continue;
            Draft draft = walker.draftOf(read);
            walker.chooseConservativeFields(draft);
            if (isHolderOfAnotherFile) {
                draft.setFollowsItsStore(false);
                draft.setGivesWordAsItLies(true);
            }
            Reference held = walker.build(WTF::move(draft));
            if (!held)
                return;
            if (mayBeInDeadZone)
                walker.state().noteVariable(variable, held);
            else
                noteValueOfStableBinding(walker, variable, held);
            walker.count(StatisticID::ConstantsOfFilesReadInFrontOfLoops);
            }
        }
    }
}

bool storesIntoListTestedInFrontOfLoop(Walker& walker, const BodyIndex& index, const BodyValue& store, uint32_t firstValueOfLoop, std::span<const ListTestedInFrontOfLoop> lists)
{
    Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, store.objectOperand(), firstValueOfLoop);
    return name && std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) {
        return list.isWritable && list.name == name;
    });
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
                return (isAskedForStoresAlone(*tested) ? read.opcode() == Opcode::ElemSet : isShownToReads(*tested)) ? tested : std::nullopt;
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
    std::optional<ListTestedInFrontOfLoop> tested = findNoteInListReadsInLoops(walker, list);
    return tested && isShownToReads(*tested) ? tested : std::nullopt;
}

std::optional<ListTestedInFrontOfLoop> findListTestedForStoresInFrontOfLoop(Walker& walker, Reference list)
{
    if (walker.state().answers().isEmpty() || !list || !list.isValue())
        return std::nullopt;
    std::optional<ListTestedInFrontOfLoop> tested = findNoteInListReadsInLoops(walker, list);
    return tested && tested->isWritable ? tested : std::nullopt;
}

static bool instancesBelowFindTheSameLength(Walker& walker, BuiltinClassID classOfView)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinClassID below = classBelowWithTestedStructure(table, classOfView);
    if (!below)
        return false;
    PropertyKeyID key = walker.program().keyOf(BuiltinNames::length);
    BuiltinID own = findMemberOnObjectAsMade(walker, walker.types().builtinClass(classOfView), key);
    BuiltinID ofBelow = findMemberOnObjectAsMade(walker, walker.types().builtinClass(below), key);
    return own && ofBelow && table.canonicalID(own) == table.canonicalID(ofBelow);
}

bool lengthOfViewTakesInstancesBelow(Walker& walker, TypeID typeOfView)
{
    BuiltinClassID classOfView = walker.has(Mechanism::Folding) ? classOfViewInListReadsInLoops(walker, typeOfView) : BuiltinClassID();
    return classOfView && instancesBelowFindTheSameLength(walker, classOfView);
}

bool loopTakesInstancesBelowViewForItsLength(Walker& walker, BlockID inputHead, Reference view)
{
    if (!walker.has(Mechanism::Folding) || !view || !view.isValue())
        return false;
    BuiltinClassID classOfView = classOfProvenViewInListReadsInLoops(walker, view);
    if (!classOfView || !instancesBelowFindTheSameLength(walker, classOfView))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    ValueID cell = rootOfCellInSweptCode(walker.code(), view.id());
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if ((operation.opcode() != Opcode::PropGet && operation.opcode() != Opcode::PropSet) || operation.isFreeName())
                continue;
            if (operation.opcode() == Opcode::PropGet && walker.program().nameClassOf(operation.key()) == NameClass::Length)
                continue;
            Reference holder = nameThatEntersLoopInListReadsInLoops(walker, index, operation.objectOperand(), firstValueOfLoop);
            if (holder && holder.isValue() && rootOfCellInSweptCode(walker.code(), holder.id()) == cell)
                return false;
        }
    }
    return true;
}

static Draft draftTestOfViewOrInstanceBelow(Walker& walker, Reference view, Origin origin)
{
    Draft test(Opcode::IsPristine, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(view);
    test.setBuiltinClass(classOfProvenViewInListReadsInLoops(walker, view));
    PristineAspectSet aspects { PristineAspects::NamesKeptBelow };
    if (someThreadMayGrowSharedMemory(walker))
        aspects.add(PristineAspects::Storage);
    test.setAspects(aspects);
    return test;
}

Reference buildTestOfViewOrInstanceBelow(Walker& walker, Reference view, Origin origin)
{
    return walker.build(draftTestOfViewOrInstanceBelow(walker, view, origin));
}

void askAgainWhetherLengthOfViewIsFoundAbove(Walker& walker, Reference view, Origin origin)
{
    if (!walker.checksProofs() || walker.isUnreachable() || walker.hasFailed())
        return;
    origin.isTailCall = false;
    walker.askAgainWhereAnswerIsLeanedOn(draftTestOfViewOrInstanceBelow(walker, view, origin));
}

void noteThatLengthOfViewIsFoundAbove(Walker& walker, Reference view)
{
    noteKnown(walker, KnownNote::LengthOfViewIsFoundAbove, view, { }, 0, LocationMasks(heapsUnderViewsReadInLoops), 1);
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
    if (!walker.isInLoop())
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
    if (read.opcode == Opcode::GetArrayElement && read.admitsInt32Storage()) {
        Draft kind(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(kind);
        kind.setArrayOperand(tested.name);
        kind.setArrayStorage(ArrayStorage::Double);
        BlockID whereWalkStands = walker.currentBlock();
        Reference holdsDoubles = walker.build(WTF::move(kind));
        if (holdsDoubles && holdsDoubles.isValue() && !walker.code().isConstant(holdsDoubles.id()) && walker.code().at(holdsDoubles.id()).opcode == Opcode::HasArrayStorage && walker.code().blockOf(holdsDoubles.id()) != whereWalkStands)
            read.setHoldsDoublesOperand(holdsDoubles);
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

namespace {

struct GuardedReadInListReadsInLoops {
    ValueID test;
    ValueID read;
};

} // anonymous namespace

static std::optional<GuardedReadInListReadsInLoops> findGuardedReadInListReadsInLoops(const BodyIndex& index)
{
    static constexpr OpcodeSet accessesThatTakeKindForGranted {
        Opcode::GetContiguousIndex, Opcode::GetDoubleIndex, Opcode::PutContiguousIndex, Opcode::PutDoubleIndex, Opcode::GetOwnDoubleIndex, Opcode::GetOwnInt32Index,
        Opcode::PutOwnContiguousIndex, Opcode::PutOwnDoubleIndex, Opcode::PutOwnInt32Index, Opcode::ArrayStorageKind
    };
    if (!index.opcodes().contains(Opcode::HasContiguousIndex) || !index.opcodes().contains(Opcode::GetOwnContiguousIndex) || index.holdsAnyOf(accessesThatTakeKindForGranted))
        return std::nullopt;
    GuardedReadInListReadsInLoops found;
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        for (BodyValue operation : index.block(BlockID(block)).values()) {
            bool isTest = operation.opcode() == Opcode::HasContiguousIndex;
            if (!isTest && operation.opcode() != Opcode::GetOwnContiguousIndex)
                continue;
            ValueID& place = isTest ? found.test : found.read;
            if (place)
                return std::nullopt;
            place = operation.id();
        }
    }
    BodyTerminator end = index.terminator(index.blockOf(found.test));
    if (!end.isConditionalBranch())
        return std::nullopt;
    ValueID condition = end.operand(0);
    bool isTruthOfTest = !index.isParameter(condition) && index.opcodeOf(condition) == Opcode::ToBoolean && index.value(condition).operand(0) == found.test && index.hasOneUse(condition);
    if (condition != found.test && !isTruthOfTest)
        return std::nullopt;
    BlockID taken = end.taken().block();
    if (taken == end.notTaken().block() || index.blockOf(found.read) != taken || index.numberOfIncomingEdges(taken) != 1 || index.isEnteredByHandlerEdge(taken))
        return std::nullopt;
    for (BodyValue operation : index.block(taken).values()) {
        if (operation.id() == found.read)
            break;
        if (!opcodesThatStateTypes.contains(operation.opcode()) && !primitiveConstants.contains(operation.opcode()))
            return std::nullopt;
    }
    BodyValue test = index.value(found.test);
    BodyValue read = index.value(found.read);
    bool areOfOneElement = index.valueBehindCasts(test.arrayOperand()) == index.valueBehindCasts(read.arrayOperand()) && index.valueBehindCasts(test.positionOperand()) == index.valueBehindCasts(read.positionOperand());
    return areOfOneElement ? std::optional(found) : std::nullopt;
}

static bool buildPlainVectorOfNumbersInListReadsInLoops(Walker& walker, const BodyValue& value, const std::optional<ListTestedInFrontOfLoop>& tested)
{
    bool asks = value.opcode() == Opcode::HasContiguousIndex;
    if (asks && (!tested || !holdsOnlyNumbers(tested->storage) || tested->turnsStoreIntoLists))
        return false;
    std::optional<GuardedReadInListReadsInLoops> guarded = findGuardedReadInListReadsInLoops(walker.currentBodyIndex());
    if (!guarded || value.id() != (asks ? guarded->test : guarded->read))
        return false;
    Origin origin = walker.originOf(value);
    origin.isTailCall = false;
    if (asks) {
        Reference list = walker.referenceOf(value.arrayOperand());
        Reference position = walker.referenceOf(value.positionOperand());
        if (!list || !position || !readTakesIndexOfProvenList(walker.types(), walker.typeOf(list), walker.typeOf(position)))
            return false;
        Draft isPresent(Opcode::IsElementPresent, origin);
        walker.chooseConservativeFields(isPresent);
        isPresent.inputValueGiven = value.id();
        isPresent.setArrayOperand(list);
        isPresent.setPositionOperand(position);
        isPresent.setKeyIsIntegral(true);
        nameKindOfListTestedInFrontOfLoop(isPresent, *tested);
        if (std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(list, position))
            isPresent.setIsInBounds(*inside);
        takeStorageAndLengthFromInFrontOfLoop(walker, isPresent, *tested);
        Reference test = walker.build(WTF::move(isPresent));
        if (!test || !test.isValue() || walker.code().at(test.id()).opcode != Opcode::IsElementPresent)
            return false;
        walker.bind(value.id(), test);
        return true;
    }
    Reference asked = walker.referenceBoundTo(guarded->test);
    bool isRecord = asked && asked.isValue();
    if (isRecord && walker.code().at(asked.id()).opcode == Opcode::HasContiguousIndex)
        return false;
    Draft read(Opcode::GetArrayElement, origin);
    walker.chooseConservativeFields(read);
    read.inputValueGiven = value.id();
    read.setKeyIsIntegral(true);
    read.setLevel(KnowledgeLevel::Proven);
    if (!isRecord || walker.code().at(asked.id()).opcode != Opcode::IsElementPresent) {
        read.setArrayOperand(walker.referenceOf(value.arrayOperand()));
        read.setPositionOperand(walker.referenceOf(value.positionOperand()));
        walker.bind(value.id(), walker.build(WTF::move(read)));
        return true;
    }
    auto referenceTo = [&](ValueID record) {
        return walker.code().isConstant(record) ? Reference::constant(record) : Reference::value(record);
    };
    Reference list = referenceTo(walker.code().at(asked.id()).arrayOperand());
    Reference position = referenceTo(walker.code().at(asked.id()).positionOperand());
    read.setArrayOperand(list);
    read.setPositionOperand(position);
    read.setArrayStorage(walker.code().at(asked.id()).arrayStorage());
    read.setAdmitsInt32Storage(walker.code().at(asked.id()).admitsInt32Storage());
    if (std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(list, position))
        read.setIsInBounds(*inside);
    if (std::optional<PassedTest> present = walker.showElementPresentBehindTest(list, position, walker.findPassedTest(asked)))
        read.setIsPresent(*present);
    if (tested)
        takeStorageAndLengthFromInFrontOfLoop(walker, read, *tested);
    walker.bind(value.id(), walker.build(WTF::move(read)));
    return true;
}

bool askPlainVectorBehindRowOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    std::optional<ListTestedInFrontOfLoop> tested = walker.state().answers().isEmpty() ? std::nullopt : findNoteInListReadsInLoops(walker, walker.referenceOf(value.arrayOperand()));
    if (buildPlainVectorOfNumbersInListReadsInLoops(walker, value, tested))
        return true;
    if (!tested || tested->storage != ArrayStorage::Contiguous)
        return false;
    Draft draft = walker.draftOf(value);
    Reference storage = storageOfListFromInFrontOfLoop(walker, *tested, draft.origin());
    if (!storage)
        return false;
    draft.setStorageOperand(storage);
    std::optional<PassedTest> inside = value.opcode() == Opcode::HasContiguousIndex ? walker.findWhatHoldsPositionInsideList(draft.arrayOperand(), draft.positionOperand()) : std::nullopt;
    if (inside)
        draft.setIsInBounds(*inside);
    walker.bind(value.id(), walker.build(WTF::move(draft)));
    return true;
}

const BitVector& Walker::valuesTakenForKeysInCurrentBody()
{
    const BodyIndex& index = currentBodyIndex();
    return m_valuesTakenForKeys.ensure(&index, [&] {
        static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet };
        BitVector taken(index.numberOfValues());
        if (!index.holdsAnyOf(accessesByKey))
            return taken;
        for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
            for (BodyValue operation : index.block(BlockID(block)).values()) {
                ValueID key = accessesByKey.contains(operation.opcode()) ? index.valueBehindCasts(operation.propertyOperand()) : ValueID();
                if (key)
                    taken.quickSet(key.index());
            }
        }
        return taken;
    }).iterator->value;
}

static bool turnTakesElementForKeyInListReadsInLoops(Walker& walker, ValueID element)
{
    const BodyIndex& index = walker.currentBodyIndex();
    return element && element.index() < index.numberOfValues() && (index.loopOf(index.blockOf(element)) || !index.hasLoops()) && walker.valuesTakenForKeysInCurrentBody().quickGet(element.index());
}

bool holeLeavesTurnBehindRow(Walker& walker, const BodyValue& read, const ListTestedInFrontOfLoop& tested)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!holdsOnlyNumbers(tested.storage))
        return false;
    if (tested.turnsStoreIntoLists)
        return true;
    std::optional<BodyUser> user = index.onlyUser(read.id());
    while (user && user->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(user->value)))
        user = index.onlyUser(user->value);
    return (user && user->kind == BodyUseKind::EdgeArgument) || turnTakesElementForKeyInListReadsInLoops(walker, read.id());
}

bool readLeavesTurnAtHoleBehindRow(Walker& walker, const BodyValue& read)
{
    if (!walker.standsInTurnThatLooksCountOn())
        return false;
    std::optional<ListTestedInFrontOfLoop> tested = findListTestedInFrontOfLoop(walker, read);
    return tested && holeLeavesTurnBehindRow(walker, read, *tested);
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

static ArrayStorage kindThatHoldsWhatIsWrittenInListReadsInLoops(Walker&, TypeID list);

static constexpr OpcodeSet numberConstantsInListReadsInLoops { Opcode::Double, Opcode::Integer };
static constexpr OpcodeSet operatorsThatMakeNumbersOfNumbers { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
static constexpr OpcodeSet operatorsThatReckonKeysInListReadsInLoops { Opcode::Add, Opcode::Sub, Opcode::Mul };

static TypeID expectedOfRowThatIsNotBuiltInListReadsInLoops(Walker& walker, ValueID row)
{
    Vector<ValueID, 16> remembered;
    bool forgetsItself = !walker.expectationsRememberedForLook();
    if (forgetsItself)
        walker.setExpectationsRememberedForLook(&remembered);
    TypeID expected = findExpectation(walker, row).type;
    if (!forgetsItself)
        return expected;
    walker.setExpectationsRememberedForLook(nullptr);
    for (ValueID value : remembered)
        walker.rememberedExpectationOf(value) = { };
    return expected;
}

static bool readsRowOfNumbersThatTurnFindsInListedList(Walker& walker, const BodyIndex& index, const BodyValue& operation, uint32_t firstValueOfLoop, std::span<const ListTestedInFrontOfLoop> lists, std::span<const GuessInFrontOfLoop> guesses = { })
{
    ValueID row = operation.opcode() == Opcode::ElemGet ? index.valueBehindCasts(operation.objectOperand()) : ValueID();
    if (!row || row.index() < firstValueOfLoop || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet)
        return false;
    ValueID holder = valueThatEntersLoopInListReadsInLoops(index, index.value(row).objectOperand(), firstValueOfLoop);
    Reference name = holder && holder.index() < firstValueOfLoop ? walker.referenceOf(holder) : Reference();
    bool isListed = name && std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) { return list.name == name; });
    if (!isListed && isProvenListInListReadsInLoops(walker, name) && isFreshInListReadsInLoops(walker, name)) {
        TypeID expected = expectedOfRowThatIsNotBuiltInListReadsInLoops(walker, row);
        return expected && holdsOnlyNumbers(kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, walker.types().withoutNullish(expected)));
    }
    if (!isListed)
        return false;
    TypeArena& types = walker.types();
    TypeID list = ownTypeInListReadsInLoops(walker, name);
    for (const GuessInFrontOfLoop& guess : guesses) {
        if (guess.value == name)
            list = types.intersectionOf(list, guess.tested);
    }
    const ClassPart* part = list && !holdsOnlyPrimitives(list) ? singleBuiltinClassOf(types, types.at(list)) : nullptr;
    if (!part || part->builtinClass != BuiltinClasses::Array)
        return false;
    TypeID rows = part->expectedContentsAt(walker.program().builtins(), ContentsSlot::Element).predicted();
    return rows && holdsOnlyNumbers(kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(rows)));
}

bool turnsReadRowsOfNumbersThatTheyFind(Walker& walker, BlockID inputHead, std::span<const ListTestedInFrontOfLoop> lists, std::span<const GuessInFrontOfLoop> guesses)
{
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (readsRowOfNumbersThatTurnFindsInListedList(walker, index, operation, firstValueOfLoop, lists, guesses))
                return true;
        }
    }
    return false;
}

static bool turnMayAppendToTextInListReadsInLoops(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const ListTestedInFrontOfLoop> lists, std::span<const GuessInFrontOfLoop> guesses, std::span<const Reference> proofs)
{
    if (!mayKeepTextsInStringBuilders(walker))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    BitVector numbersMadeOfNumbers;
    auto isNumberBehindTestsOfLists = [&](ValueID operand) {
        ValueID root = index.valueBehindCasts(operand);
        if (!index.isParameter(root) && numberConstantsInListReadsInLoops.contains(index.opcodeOf(root)))
            return true;
        if (root.index() >= firstValueOfLoop && numbersMadeOfNumbers.get(root.index() - firstValueOfLoop))
            return true;
        if (root.index() >= firstValueOfLoop && !index.isParameter(root) && index.opcodeOf(root) == Opcode::ElemGet) {
            ValueID entersLoop = valueThatEntersLoopInListReadsInLoops(index, index.value(root).objectOperand(), firstValueOfLoop);
            Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, entersLoop, firstValueOfLoop);
            Reference proof;
            for (size_t place = 0; name && place < guesses.size() && place < proofs.size(); ++place) {
                if (guesses[place].value == name)
                    proof = proofs[place];
            }
            if (name && std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) { return (list.name == name || (proof && list.name == proof)) && holdsOnlyNumbers(list.storage); }))
                return true;
            if (readsRowOfNumbersThatTurnFindsInListedList(walker, index, index.value(root), firstValueOfLoop, lists))
                return true;
            size_t placeOfList = entersLoop.index() - preview.firstValue;
            TypeID listInRow = entersLoop.index() >= preview.firstValue && placeOfList < preview.typesOfValues.size() ? preview.typesOfValues[placeOfList] : TypeID();
            if (listInRow && readsListFoundByReadFromInFront(walker.program(), index, index.value(root), firstValueOfLoop) && holdsOnlyNumbers(kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, walker.types().withoutNullish(listInRow))))
                return true;
        }
        ValueID step = root.index() >= firstValueOfLoop && !index.isParameter(root) && index.opcodeOf(root) == Opcode::ResultAt ? index.value(root).operand(0) : ValueID();
        if (step && !index.isParameter(step) && stepsViewByIndexAlone(walker, index, index.value(step)))
            return true;
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
            if (operatorsThatMakeNumbersOfNumbers.contains(operation.opcode()) && isNumberBehindTestsOfLists(operation.operand(0)) && isNumberBehindTestsOfLists(operation.operand(1))) {
                numbersMadeOfNumbers.set(operation.id().index() - firstValueOfLoop);
                continue;
            }
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

static constexpr OpcodeSet readsThatMayFindSameInEveryTurn { Opcode::PropGet, Opcode::ElemGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture };

static bool takesOnlyWhatReadsFromInFrontFindInListReadsInLoops(const BodyIndex& index, const BodyValue& read, uint32_t firstValueOfLoop, BitVector& found)
{
    if (read.opcode() == Opcode::PropGet && read.isFreeName())
        return false;
    if (found.get(read.id().index() - firstValueOfLoop))
        return true;
    bool takesOnlyThat = true;
    read.forEachOperand([&](ValueID operand, uint32_t) {
        if (!takesOnlyThat || operand.index() < firstValueOfLoop)
            return;
        ValueID source = index.valueBehindCasts(operand);
        if (read.opcode() == Opcode::PropGet && source && source.index() >= firstValueOfLoop && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
            source = index.valueBehindCasts(index.value(source).operand(0));
        if (!source || source.index() < firstValueOfLoop) {
            takesOnlyThat = !!source;
            return;
        }
        if (index.isParameter(source)) {
            takesOnlyThat = false;
            return;
        }
        Opcode made = index.opcodeOf(source);
        takesOnlyThat = numberConstantsInListReadsInLoops.contains(made)
            || ((readsThatMayFindSameInEveryTurn.contains(made) || operatorsThatMakeNumbersOfNumbers.contains(made)) && takesOnlyWhatReadsFromInFrontFindInListReadsInLoops(index, index.value(source), firstValueOfLoop, found));
    });
    if (takesOnlyThat)
        found.set(read.id().index() - firstValueOfLoop);
    return takesOnlyThat;
}

bool readsListFoundByReadFromInFront(const Program& program, const BodyIndex& index, const BodyValue& operation, uint32_t firstValueOfLoop)
{
    bool readsList = operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet || (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == program.keyOf(BuiltinNames::length));
    if (!readsList)
        return false;
    ValueID source = index.valueBehindCasts(operation.objectOperand());
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
        source = index.valueBehindCasts(index.value(source).operand(0));
    if (!source || source.index() < firstValueOfLoop || index.isParameter(source) || !readsThatMayFindSameInEveryTurn.contains(index.opcodeOf(source)))
        return false;
    BitVector found;
    return takesOnlyWhatReadsFromInFrontFindInListReadsInLoops(index, index.value(source), firstValueOfLoop, found);
}

static bool rowFindsListsLeftAsTheyAreInListReadsInLoops(Walker& walker, BlockID inputHead, std::span<const ListTestedInFrontOfLoop> lists, std::span<const GuessInFrontOfLoop> guesses, std::span<const Reference> proofs)
{
    if (walker.listsAreStruckInRow())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    const LoopPreview* row = walker.previewOutOfRow();
    auto typeInRow = [&](ValueID input) -> TypeID {
        TypeID known;
        if (row && input.index() >= row->firstValue && input.index() - row->firstValue < row->typesOfValues.size())
            known = row->typesOfValues[input.index() - row->firstValue];
        else if (Reference built = walker.referenceOf(input))
            known = walker.typeOf(built);
        return known ? known : TypeID(TypeTable::anything);
    };
    bool sitesMayAnswer = !walker.someCallStaysInTurnOfRow() && someReadThatAsksItsSiteIsReckonedWith(walker, walker.operationsThatRanOtherCodeOfRow(), TypesOfInputs(typeInRow));
    for (ValueID ran : walker.operationsThatRanOtherCodeOfRow()) {
        if (!ran)
            return false;
        BodyValue operation = index.value(ran);
        if (stepsViewByIndexAlone(walker, index, operation) || stepsTextByIndexAlone(walker, index, operation))
            continue;
        bool readsList = false;
        bool isStore = operation.opcode() == Opcode::ElemSet;
        if (operation.opcode() == Opcode::ElemGet || isStore || (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == walker.program().keyOf(BuiltinNames::length))) {
            ValueID entersLoop = valueThatEntersLoopInListReadsInLoops(index, operation.objectOperand(), firstValueOfLoop);
            Reference name = entersLoop ? walker.referenceOf(entersLoop) : Reference();
            if (Reference readInFront = entersLoop && entersLoop.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, entersLoop, firstValueOfLoop) : Reference())
                name = readInFront;
            Reference proof;
            for (size_t place = 0; name && place < guesses.size() && place < proofs.size(); ++place) {
                if (guesses[place].value == name)
                    proof = proofs[place];
            }
            readsList = name && std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) { return (list.name == name || (proof && list.name == proof)) && (!isStore || list.isWritable); });
        } else if (const Walker::IterationByIndex* stepped = findListSteppedByIndexAlone(walker, index, operation))
            readsList = std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) { return list.name == stepped->list; });
        else if (ValueID step = operation.opcode() == Opcode::ResultAt ? operation.as<Opcode::ResultAt>().resultsOperand() : ValueID(); step && !index.isParameter(step)) {
            if (const Walker::IterationByIndex* stepped = findListSteppedByIndexAlone(walker, index, index.value(step)))
                readsList = std::ranges::any_of(lists, [&](const ListTestedInFrontOfLoop& list) { return list.name == stepped->list; });
        }
        if (readsList || effectRuleOf(operation.opcode()) == EffectRule::Operator || readsListFoundByReadFromInFront(walker.program(), index, operation, firstValueOfLoop) || readsRowOfNumbersThatTurnFindsInListedList(walker, index, operation, firstValueOfLoop, lists))
            continue;
        if (!walker.looksAskOnlyWhetherTurnsRunOtherCode(inputHead) || !guessesMayTypeOperationInCleanTurn(walker, operation, TypesOfInputs(typeInRow), sitesMayAnswer))
            return false;
    }
    if (walker.previewOutOfRow() && turnMayAppendToTextInListReadsInLoops(walker, inputHead, *walker.previewOutOfRow(), lists, guesses, proofs))
        return false;
    return leavesListsTestedInFrontOfLoopAsTheyAre(walker, walker.effectsOfQuietDraftsOfRow(), lists, inputHead);
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

static bool onlyUserStoresWhatIsReadIntoListInListReadsInLoops(const BodyIndex& index, const BodyValue& read)
{
    ValueID name = read.id();
    ValueID user = index.onlyUsingValue(name);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    return user && index.opcodeOf(user) == Opcode::ElemSet && index.value(user).valueOperand() == name;
}

static ArrayStorage kindWrittenOrSeenByLinkInListReadsInLoops(const TypeArena& types, ArrayStorage written, TypeID seenByLink)
{
    if (written != ArrayStorage::Other || !seenByLink || types.at(seenByLink).isAnything())
        return written;
    Type seen = types.at(seenByLink);
    bool tellsStorage = seen.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined | Primitive::Absent) || !(seen.primitives & Primitive::Number);
    return tellsStorage ? arrayStorageHolding(types, seenByLink) : written;
}

static bool lookReadsCopyOfCalleeInListReadsInLoops(Walker& walker, const BodyValue& call, FunctionID callee)
{
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body)
        return false;
    Draft site(call.opcode());
    site.flags = static_cast<uint8_t>(call.flags());
    copyFields(call, site, [](ValueID operand) {
        return Reference::value(operand);
    });
    PlaceOfCall place { walker.currentFunction(), true, &walker.currentBodyIndex(), call.id() };
    if (body->hasLoops() && !sizeOfCopyThatWritesOutLoopsOverListsHandedIn(walker, callee) && !turnsOfCopyWrittenOutAt(walker, site, callee, place))
        return false;
    return mayCopyCallee(walker, site, callee, place);
}

static Reference lesserOfIntegersInListReadsInLoops(Walker& walker, Reference one, Reference other, Origin origin)
{
    Reference isLess = buildOperatorOnIntegersOfLoop(walker, Opcode::Less, one, other, origin);
    if (!isLess || !isLess.isValue() || walker.isUnreachable())
        return { };
    BlockID takesOne = walker.addBlock();
    BlockID takesOther = walker.addBlock();
    walker.branch(isLess, takesOne, takesOther);
    Vector<Walker::ArmThatMeets, 2> arms;
    arms.append(Walker::ArmThatMeets { takesOne, { one } });
    arms.append(Walker::ArmThatMeets { takesOther, { other } });
    Reference lesser;
    return walker.letArmsMeet(arms.span(), walker.addBlock(), singleElementSpan(lesser)) ? lesser : Reference();
}

void reserveRoomOfListsThatLoopFills(Walker& walker, BlockID inputHead, std::span<const Reference> entering)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::LoopInvariants))
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();

    BodyTerminator test = index.terminator(inputHead);
    if (!test.isConditionalBranch() || !index.isInLoop(test.taken().block(), loop) || index.isInLoop(test.notTaken().block(), loop))
        return;
    ValueID condition = test.operand(0);
    bool countAloneEndsLoop = true;
    if (!condition || index.isParameter(condition) || (index.opcodeOf(condition) != Opcode::Less && index.opcodeOf(condition) != Opcode::LessEq) || index.blockOf(condition) != inputHead)
        return;
    ValueID counter = index.valueBehindCasts(index.value(condition).operand(0));
    ValueID bound = index.valueBehindCasts(index.value(condition).operand(1));
    bool boundIsWritten = !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::Integer;
    Reference textWhoseLengthIsBound;
    if (bound.index() >= firstValueOfLoop && !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::PropGet && !index.value(bound).isFreeName() && index.value(bound).key() == walker.program().keyOf(BuiltinNames::length)) {
        ValueID text = index.valueBehindCasts(index.value(bound).objectOperand());
        Reference built = text.index() < firstValueOfLoop ? walker.referenceOf(text) : Reference();
        if (built && types.at(walker.typeOf(built)).hasOnlyPrimitives(Primitive::AnyString))
            textWhoseLengthIsBound = built;
    }
    if (!index.isParameter(counter) || index.blockOf(counter) != inputHead || (bound.index() >= firstValueOfLoop && !boundIsWritten && !textWhoseLengthIsBound))
        return;
    size_t placeOfCounter = counter.index() - firstValueOfLoop;
    if (placeOfCounter >= entering.size() || !entering[placeOfCounter])
        return;
    const Type& first = types.at(walker.typeOf(entering[placeOfCounter]));
    if (!first.isInteger() || first.integers.minimum < 0)
        return;
    countAloneEndsLoop &= first.integers == Range::one(0);
    Range integersThatEnter = first.integers;

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
    const BuiltinTable& table = walker.program().builtins();
    auto listWhosePushIsRead = [&](ValueID value) -> ValueID {
        if (index.isParameter(value) || index.opcodeOf(value) != Opcode::PropGet || index.value(value).isFreeName())
            return { };
        ValueID input = index.valueBehindCasts(index.value(value).objectOperand());
        Reference list = input.index() < firstValueOfLoop ? walker.referenceOf(input) : Reference();
        if (!isProvenListInListReadsInLoops(walker, list))
            return { };
        BuiltinID member = findConstantMemberReadOff(walker, list, index.value(value).key());
        return member && table.canonicalID(member) == table.canonicalID(Builtins::ArrayPrototypePush) ? input : ValueID();
    };
    constexpr uint16_t plainPrimitives = Primitive::Number | Primitive::AnyString | Primitive::Boolean | Primitive::Undefined | Primitive::Null;
    static constexpr OpcodeSet plainConstants { Opcode::Integer, Opcode::Double, Opcode::String, Opcode::Boolean, Opcode::Undefined, Opcode::Null };
    Vector<std::pair<ValueID, Reference>, 2> namesAskedInFront;
    auto readInFront = [&](ValueID read) -> Reference {
        for (const auto& [asked, built] : namesAskedInFront) {
            if (index.value(asked).variable() == index.value(read).variable())
                return built;
        }
        Draft name(Opcode::GetModuleVariable, walker.originOf(index.value(read)));
        copyFields(index.value(read), name, [](ValueID) { return Reference(); });
        namesAskedInFront.append({ read, walker.build(WTF::move(name)) });
        return namesAskedInFront.last().second;
    };
    auto nameThatIsChecked = [&](ValueID value) -> ValueID {
        if (index.isParameter(value) || index.opcodeOf(value) != Opcode::CheckInitialized)
            return { };
        ValueID read = index.valueBehindCasts(index.value(value).operand(0));
        return read.index() >= firstValueOfLoop && !index.isParameter(read) && index.opcodeOf(read) == Opcode::GetModuleVariable ? read : ValueID();
    };
    Vector<std::pair<Reference, uint64_t>, 4> textsJoined;
    uint64_t timesTextsAreJoined = 0;
    uint64_t lengthJoinedOfTheRest = 0;
    constexpr uint64_t noPrimitiveButATextIsWrittenLonger = 32;
    auto isTaken = [&](Reference built, TypeID typeWhereItIsThere, bool byOperator) -> bool {
        const Type& type = types.at(typeWhereItIsThere);
        if (!byOperator)
            return type.hasOnlyPrimitives(plainPrimitives);
        if (type.hasOnlyPrimitives(plainPrimitives & ~Primitive::AnyString)) {
            lengthJoinedOfTheRest += noPrimitiveButATextIsWrittenLonger;
            return true;
        }
        if (!type.hasOnlyPrimitives(Primitive::AnyString) || !built.isRecord() || isTextInStringBuilder(walker, built))
            return false;
        ++timesTextsAreJoined;
        size_t place = textsJoined.findIf([&](const auto& joined) { return joined.first == built; });
        if (place == notFound)
            textsJoined.append({ built, 1 });
        else
            ++textsJoined[place].second;
        return true;
    };
    auto isPlainPrimitive = [&](auto& self, ValueID value, unsigned depth, bool isTakenByOperator = false) -> bool {
        value = index.valueBehindCasts(value);
        if (value == counter) {
            lengthJoinedOfTheRest += noPrimitiveButATextIsWrittenLonger;
            return true;
        }
        if (value.index() < firstValueOfLoop) {
            Reference built = walker.referenceOf(value);
            return built && isTaken(built, walker.typeOf(built), isTakenByOperator);
        }
        if (index.isParameter(value) || !depth)
            return false;
        if (plainConstants.contains(index.opcodeOf(value))) {
            lengthJoinedOfTheRest += index.opcodeOf(value) == Opcode::String ? types.bytesOf(index.value(value).string()).size() : noPrimitiveButATextIsWrittenLonger;
            return true;
        }
        if (ValueID read = nameThatIsChecked(value)) {
            Reference built = readInFront(read);
            return built && isTaken(built, types.withoutPrimitives(walker.typeOf(built), Primitive::Absent), isTakenByOperator);
        }
        if (effectRuleOf(index.opcodeOf(value)) != EffectRule::Operator)
            return false;
        bool takesPlainPrimitives = true;
        index.value(value).forEachOperand([&](ValueID operand, auto...) {
            takesPlainPrimitives = takesPlainPrimitives && (!operand || self(self, operand, depth - 1, true));
        });
        return takesPlainPrimitives;
    };

    Vector<Reference, 2> lists;
    Vector<Reference, 2> listsPushedOnto;
    for (BlockID member : index.membersOf(loop)) {
        bool standsInEveryTurn = std::ranges::all_of(blocksThatComeRound, [&](BlockID last) { return index.dominates(member, last); });
        for (BodyValue operation : index.block(member).values()) {
            if (listWhoseLengthIsRead(operation.id()))
                continue;
            if (listWhosePushIsRead(operation.id()))
                continue;
            if (operation.opcode() == Opcode::Call) {
                auto call = operation.as<Opcode::Call>();
                ValueID input = listWhosePushIsRead(index.valueBehindCasts(call.calleeOperand()));
                if (!standsInEveryTurn || !input || index.valueBehindCasts(call.thisValueOperand()) != input || call.argumentsOperands().size() != 1) {
                    countAloneEndsLoop = false;
                    continue;
                }
                if (!isPlainPrimitive(isPlainPrimitive, call.argumentsOperands()[0], 6))
                    countAloneEndsLoop = false;
                Reference list = walker.referenceOf(input);
                if (!lists.contains(list))
                    lists.append(list);
                if (!listsPushedOnto.contains(list))
                    listsPushedOnto.append(list);
                continue;
            }
            if (operation.opcode() == Opcode::ElemSet) {
                ValueID input = index.valueBehindCasts(operation.objectOperand());
                ValueID position = index.valueBehindCasts(operation.propertyOperand());
                Reference list = input.index() < firstValueOfLoop ? walker.referenceOf(input) : Reference();
                if (!standsInEveryTurn || (position != counter && (position.index() < firstValueOfLoop || listWhoseLengthIsRead(position) != input)) || !isProvenListInListReadsInLoops(walker, list)) {
                    countAloneEndsLoop = false;
                    continue;
                }
                if (position != counter && !listsPushedOnto.contains(list))
                    listsPushedOnto.append(list);
                if (!lists.contains(list))
                    lists.append(list);
                continue;
            }
            Effects declared = declaredEffectsOfOperation(walker.program(), operation.opcode(), operation);
            if (!declared.mayThrow.byRule() && !declared.mayRunOtherCode()) {
                if (!declared.writes.containsOnly(heapsWhoseWritesNobodySees))
                    countAloneEndsLoop = false;
                continue;
            }
            if (!countAloneEndsLoop)
                continue;
            if (ValueID read = nameThatIsChecked(operation.id())) {
                if (!readInFront(read))
                    return;
                continue;
            }
            if (effectRuleOf(operation.opcode()) != EffectRule::Operator || !isPlainPrimitive(isPlainPrimitive, operation.id(), 6))
                countAloneEndsLoop = false;
        }
    }
    if (lists.isEmpty())
        return;
    if (lengthJoinedOfTheRest > JSString::MaxLength || timesTextsAreJoined > JSString::MaxLength)
        countAloneEndsLoop = false;

    Origin origin = walker.originOf(index.value(condition));
    Reference room = textWhoseLengthIsBound ? buildLengthOfString(walker, textWhoseLengthIsBound, origin) : walker.referenceOf(bound);
    if (bound.index() >= firstValueOfLoop && !textWhoseLengthIsBound) {
        Draft written(Opcode::Integer, origin);
        written.setInteger(index.value(bound).integer());
        room = walker.constant(WTF::move(written));
    }
    if (!room)
        return;
    if (!types.at(walker.typeOf(room)).isNumber())
        return;
    auto integerWritten = [&](int64_t number) {
        Draft integer(Opcode::Integer, origin);
        integer.setInteger(number);
        return walker.constant(WTF::move(integer));
    };
    if (index.opcodeOf(condition) == Opcode::LessEq) {
        if (!types.at(walker.typeOf(room)).isInteger())
            return;
        room = buildOperatorOnIntegersOfLoop(walker, Opcode::Add, room, integerWritten(1), origin);
        if (!room)
            return;
    }
    if (!countAloneEndsLoop) {
        if (!types.at(walker.typeOf(room)).isInteger())
            return;
        namesAskedInFront.shrink(0);
        textsJoined.shrink(0);
    }
    Reference roomByCount = room;
    auto roomOfLiteralFilledFromItsEnd = [&](Reference list) -> Reference {
        const Operation& born = walker.code().at(rootOfCellInSweptCode(walker.code(), list.id()));
        if (born.opcode != Opcode::NewArray || born.storage() != AllocationStorage::Heap || !born.holes().empty() || born.elementsOperands().empty())
            return { };
        int64_t written = born.elementsOperands().size();
        Reference filled = roomByCount;
        if (listsPushedOnto.contains(list)) {
            Reference turnsLeft = buildOperatorOnIntegersOfLoop(walker, Opcode::Sub, roomByCount, entering[placeOfCounter], origin);
            filled = turnsLeft ? buildOperatorOnIntegersOfLoop(walker, Opcode::Add, turnsLeft, integerWritten(written), origin) : Reference();
        } else if (integersThatEnter != Range::one(written))
            return { };
        if (!filled || !types.at(walker.typeOf(filled)).isInteger())
            return { };
        Range asked = types.at(walker.typeOf(filled)).integers;
        if (asked.maximum <= BASE_CONTIGUOUS_VECTOR_LEN_MAX)
            return filled;
        return asked.minimum > BASE_CONTIGUOUS_VECTOR_LEN_MAX ? integerWritten(BASE_CONTIGUOUS_VECTOR_LEN_MAX) : lesserOfIntegersInListReadsInLoops(walker, filled, integerWritten(BASE_CONTIGUOUS_VECTOR_LEN_MAX), origin);
    };
    auto behindForms = [&](ValueID value) {
        while (conversionsOfForm.contains(walker.code().at(value).opcode))
            value = walker.code().at(value).valueOperand();
        return value;
    };
    for (Reference list : lists) {
        if (!countAloneEndsLoop) {
            room = roomOfLiteralFilledFromItsEnd(list);
            if (walker.isUnreachable())
                return;
            if (!room)
                continue;
            walker.count(StatisticID::RoomAskedAsFarAsAProfileMayAsk);
        } else
            room = roomByCount;
        const Operation& born = walker.code().at(rootOfCellInSweptCode(walker.code(), list.id()));
        bool isBornWithLength = born.opcode == Opcode::NewArrayLike || born.opcode == Opcode::NewContiguousArray;
        if (isBornWithLength && room.isValue() && behindForms(born.lengthOperand()) == behindForms(room.id()))
            continue;
        if (Type asked = types.at(walker.typeOf(room)); isBornWithLength && asked.isInteger() && asked.integers.isSingleValue() && types.at(walker.code().at(born.lengthOperand()).type).isInteger() && types.at(walker.code().at(born.lengthOperand()).type).integers == asked.integers)
            continue;
        Draft reserve(Opcode::ReserveArrayRoom, origin);
        walker.chooseConservativeFields(reserve);
        reserve.setArrayOperand(list);
        reserve.setRoomOperand(room);
        Vector<Reference, 2> held;
        for (const auto& [asked, built] : namesAskedInFront) {
            if (types.at(walker.typeOf(built)).primitives & Primitive::Absent)
                held.append(built);
        }
        auto written = [&](uint64_t number) {
            Draft integer(Opcode::Integer, origin);
            integer.setInteger(static_cast<int64_t>(number));
            return walker.constant(WTF::move(integer));
        };
        for (const auto& [text, times] : textsJoined) {
            held.append(text);
            held.append(written(times));
        }
        if (!textsJoined.isEmpty())
            held.append(written(lengthJoinedOfTheRest));
        reserve.setVariadicOperands(held.span());
        walker.build(WTF::move(reserve));
    }
}

static void forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(Walker& walker, FunctionID callee, std::span<const ValueID> arguments, unsigned depth, Opcode access, const Invocable<void(ValueID argument, ValueID key, FunctionID reader, ValueID read)> auto& take)
{
    const BodyIndex& body = walker.facts().bodies().at(callee);
    BlockID entry = BlockID(0u);
    auto argumentBehind = [&](ValueID value) -> ValueID {
        value = body.valueBehindCasts(value);
        if (!value || !body.isParameter(value) || body.blockOf(value) != entry)
            return { };
        const Function& function = walker.program().at(callee);
        size_t place = value.index() - body.block(entry).firstParameter().index();
        if (place < function.firstParameterIndex() || place - function.firstParameterIndex() >= function.numberOfPositionalParameters())
            return { };
        place -= function.firstParameterIndex();
        return place < arguments.size() ? arguments[place] : ValueID();
    };
    for (uint32_t block = 0; block < body.numberOfBlocks(); ++block) {
        for (BodyValue operation : body.block(BlockID(block)).values()) {
            if (operation.opcode() == access) {
                ValueID key = body.valueBehindCasts(operation.propertyOperand());
                bool keyIsCounter = key && body.isParameter(key) && body.isLoopHead(body.blockOf(key));
                bool keyIsWritten = keyIsCounter || (key && !body.isParameter(key) && body.opcodeOf(key) == Opcode::Integer);
                ValueID keyHandedIn = keyIsWritten ? ValueID() : argumentBehind(key);
                if (ValueID argument = keyIsWritten || keyHandedIn ? argumentBehind(operation.objectOperand()) : ValueID())
                    take(argument, keyHandedIn, callee, operation.id());
                continue;
            }
            if (operation.opcode() != Opcode::Call || !depth)
                continue;
            FunctionID inner = walker.facts().linked().calleeAt(callee, operation.id());
            const BodyIndex* innerBody = inner ? walker.facts().bodies().find(inner) : nullptr;
            if (!innerBody || innerBody->hasLoops() || !copyOfCalleeCalledInLoopPays(walker, inner, operation.as<Opcode::Call>().argumentsOperands().size()))
                continue;
            Vector<ValueID, 4> handedOn;
            for (ValueID argument : operation.as<Opcode::Call>().argumentsOperands())
                handedOn.append(argument ? argumentBehind(argument) : ValueID());
            forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, inner, handedOn.span(), depth - 1, access, take);
        }
    }
}

static bool storesNumberInCopyOfCalleeInListReadsInLoops(Walker& walker, FunctionID writer, ValueID value, unsigned depth)
{
    const BodyIndex& body = walker.facts().bodies().at(writer);
    value = body.valueBehindCasts(value);
    if (!value || body.isParameter(value))
        return false;
    Opcode opcode = body.opcodeOf(value);
    if (numberConstantsInListReadsInLoops.contains(opcode) || operatorsThatMakeNumbers.contains(opcode))
        return true;
    if (opcode == Opcode::ElemGet) {
        TypeID seenByLink = walker.facts().linked().resultOfReadByKey(writer, value);
        return seenByLink && (walker.types().at(seenByLink).primitives & Primitive::Number) && walker.types().at(seenByLink).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    }
    BodyValue operation = body.value(value);
    return opcode == Opcode::Add && depth && storesNumberInCopyOfCalleeInListReadsInLoops(walker, writer, operation.operand(0), depth - 1) && storesNumberInCopyOfCalleeInListReadsInLoops(walker, writer, operation.operand(1), depth - 1);
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

template<typename TypeInLook>
static TypeID typeThatCalleeTakesOfElementInListReadsInLoops(Walker& walker, const BodyIndex& index, LoopID loop, const TypeInLook& typeInLook)
{
    const LinkTables& linked = walker.facts().linked();
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::Call)
                continue;
            TypeID called = typeInLook(operation.as<Opcode::Call>().calleeOperand());
            FunctionID callee = called ? singleFunctionOf(walker.types(), walker.types().at(called)) : FunctionID();
            if (!callee)
                continue;
            const Function& function = walker.program().at(callee);
            std::span<const LinkedParameter> arriving = linked.parametersOf(callee);
            auto handed = operation.as<Opcode::Call>().argumentsOperands();
            for (size_t place = 0; place < handed.size() && place < function.numberOfPositionalParameters(); ++place) {
                ValueID element = handed[place] ? index.valueBehindCasts(handed[place]) : ValueID();
                size_t entry = function.firstParameterIndex() + place;
                if (!element || index.isParameter(element) || index.opcodeOf(element) != Opcode::GetOwnContiguousIndex || entry >= arriving.size())
                    continue;
                TypeID proven = linked.function(callee).everyCallerIsKnown ? arriving[entry].proven : TypeID();
                return proven ? proven : arriving[entry].writtenOrSeenToArrive();
            }
        }
    }
    return { };
}

static bool isReadOfBodyInListReadsInLoops(const BodyIndex& index, const BodyValue& read)
{
    return read.id().index() < index.numberOfValues() && index.value(read.id()).words() == read.words();
}

static bool holdsKeysOfLoopInListReadsInLoops(Walker& walker, ValueID element, TypeID seenByLink)
{
    TypeArena& types = walker.types();
    return seenByLink && types.at(types.withoutNullish(seenByLink)).isNumberWithoutFraction() && turnTakesElementForKeyInListReadsInLoops(walker, element);
}

static ValueID valueThatStepHandsOutInListReadsInLoops(const BodyIndex& index, const BodyValue& step)
{
    for (BodyValue operation : index.block(index.blockOf(step.id())).values()) {
        if (operation.opcode() == Opcode::ResultAt && operation.as<Opcode::ResultAt>().resultsOperand() == step.id() && operation.as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Value))
            return operation.id();
    }
    return { };
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
        if (input.index() < firstValueOfLoop) {
            Reference inFrontOfLoop = walker.referenceOf(input);
            TypeID own = ownTypeInListReadsInLoops(walker, inFrontOfLoop);
            for (const GuessInFrontOfLoop& guess : guesses) {
                if (own && guess.value == inFrontOfLoop)
                    own = types.intersectionOf(own, guess.tested);
            }
            return own;
        }
        size_t place = input.index() - preview.firstValue;
        return input.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
    };

    BitVector numbersBehindListsFound;
    auto isNumberBehindListsFound = [&](ValueID value) {
        TypeID seen = typeInLook(value);
        if (seen && types.at(seen).isNumber())
            return true;
        ValueID made = index.valueBehindCasts(value);
        return made && made.index() >= firstValueOfLoop && numbersBehindListsFound.get(made.index() - firstValueOfLoop);
    };
    BitVector wholeNumbersBehindListsFound;
    auto isWholeNumberBehindListsFound = [&](ValueID value) {
        TypeID seen = typeInLook(value);
        if (seen && types.at(seen).isNumber())
            return !(types.at(seen).primitives & Primitive::FractionalNumber);
        ValueID made = index.valueBehindCasts(value);
        return made && made.index() >= firstValueOfLoop && wholeNumbersBehindListsFound.get(made.index() - firstValueOfLoop);
    };
    auto noteNumberBehindListsFound = [&](const BodyValue& operation, bool isWhole) {
        numbersBehindListsFound.set(operation.id().index() - firstValueOfLoop);
        if (isWhole)
            wholeNumbersBehindListsFound.set(operation.id().index() - firstValueOfLoop);
    };
    auto asksItsSiteAndIsReckonedWith = [&](const BodyValue& read) {
        if (read.opcode() != Opcode::PropGet || !walker.looksCountOnTurnsThatBeginAgain(inputHead))
            return false;
        TypeID holder = typeInLook(read.objectOperand());
        if (holder && !types.saysNothing(holder))
            return false;
        ValueID id = read.id();
        return readThatAsksItsSiteIsReckonedWithAsNumber(walker, id, TypesOfInputs(typeInLook));
    };
    auto isListedForReadByKey = [&](Reference name) {
        size_t place = found.lists.findIf([&](const ListTestedInFrontOfLoop& list) {
            return list.name == name;
        });
        if (place == notFound)
            return false;
        if (!holdsBoxedValuesOfEitherKind(found.lists[place]))
            return true;
        if (!found.lists[place].leavesBirthAsItIs) {
            found.lists[place].admitsInt32Storage = false;
            return true;
        }
        found.lists.removeAt(place);
        return false;
    };
    auto isNotedForReadByKey = [&](Reference name) {
        std::optional<ListTestedInFrontOfLoop> noted = findNoteInListReadsInLoops(walker, name);
        return noted && isShownToReads(*noted);
    };

    struct ReadOffListOfUnknownContents {
        ValueID read;
        Reference name;
    };
    Vector<ReadOffListOfUnknownContents, 2> readsOffListsOfUnknownContents;
    Vector<Reference, 2> listsOfUnknownContents;
    Vector<Reference, 2> listsAskedForOneStorage;
    Vector<Reference, 2> listsThatTakeMoreThanInt32s;
    auto entersLoopAsNumber = [&](ValueID value) {
        ValueID made = index.valueBehindCasts(value);
        std::optional<uint32_t> place = made ? index.block(inputHead).indexOfParameter(made) : std::nullopt;
        TypeID entered = place && *place < entering.size() && entering[*place] ? ownTypeInListReadsInLoops(walker, entering[*place]) : TypeID();
        return entered && types.at(entered).isNumber();
    };
    auto expectNumbersWhereElementIsTakenForNumber = [&](const BodyValue& operation) {
        static constexpr OpcodeSet operatorsThatTakeNumbers = operatorsThatMakeNumbers | allBitwiseOperators;
        bool takesTextsToo = operationsThatJoinTexts.contains(operation.opcode()) || allRelations.contains(operation.opcode());
        if (readsOffListsOfUnknownContents.isEmpty() || !(takesTextsToo || operatorsThatTakeNumbers.contains(operation.opcode())))
            return false;
        Reference taken;
        operation.forEachOperand([&](ValueID operand, uint32_t position) {
            ValueID made = index.valueBehindCasts(operand);
            size_t place = readsOffListsOfUnknownContents.findIf([&](const ReadOffListOfUnknownContents& kept) {
                return kept.read == made;
            });
            if (taken || place == notFound)
                return;
            ValueID other = takesTextsToo && position < 2 ? operation.operand(1 - position) : ValueID();
            bool isHeldAgainstElementOfSuchList = other && allRelations.contains(operation.opcode()) && readsOffListsOfUnknownContents.containsIf([&](const ReadOffListOfUnknownContents& kept) {
                return kept.read == index.valueBehindCasts(other);
            });
            if (takesTextsToo && !isHeldAgainstElementOfSuchList && (!other || !(isNumberBehindListsFound(other) || entersLoopAsNumber(other))))
                return;
            taken = readsOffListsOfUnknownContents[place].name;
        });
        if (!taken)
            return false;
        for (ListTestedInFrontOfLoop& list : found.lists) {
            if (list.name != taken || list.storage != ArrayStorage::Other)
                continue;
            list.storage = ArrayStorage::Double;
            list.admitsInt32Storage = true;
        }
        listsOfUnknownContents.removeAll(taken);
        readsOffListsOfUnknownContents.removeAllMatching([&](const ReadOffListOfUnknownContents& kept) {
            if (kept.name != taken)
                return false;
            numbersBehindListsFound.set(kept.read.index() - firstValueOfLoop);
            return true;
        });
        return true;
    };

    std::optional<Vector<Reference, 4>> listsThatTextOfLoopStoresInto;
    auto textOfLoopStoresIntoList = [&](Reference name) {
        if (!listsThatTextOfLoopStoresInto) {
            listsThatTextOfLoopStoresInto.emplace();
            for (BlockID member : index.membersOf(loop)) {
                for (BodyValue store : index.block(member).values()) {
                    if (store.opcode() != Opcode::ElemSet)
                        continue;
                    ValueID stored = valueThatEntersLoopInListReadsInLoops(index, store.objectOperand(), firstValueOfLoop);
                    Reference list = stored.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, stored, firstValueOfLoop) : walker.referenceOf(stored);
                    if (list && !listsThatTextOfLoopStoresInto->contains(list))
                        listsThatTextOfLoopStoresInto->append(list);
                }
            }
        }
        return listsThatTextOfLoopStoresInto->contains(name);
    };

    Vector<std::pair<Reference, WrittenListOfVariable>, 2> writtenListsRead;    auto expectBoxedValuesWhereElementIsReadByName = [&](const BodyValue& operation) {
        if (readsOffListsOfUnknownContents.isEmpty() || !siteMayAnswerReadWithoutOtherCode(walker, operation))
            return;
        size_t place = readsOffListsOfUnknownContents.findIf([&](const ReadOffListOfUnknownContents& kept) {
            return kept.read == index.valueBehindCasts(operation.objectOperand());
        });
        if (place == notFound)
            return;
        Reference taken = readsOffListsOfUnknownContents[place].name;
        for (ListTestedInFrontOfLoop& list : found.lists) {
            if (list.name == taken && list.storage == ArrayStorage::Other)
                list.storage = ArrayStorage::Contiguous;
        }
        listsOfUnknownContents.removeAll(taken);
        readsOffListsOfUnknownContents.removeAllMatching([&](const ReadOffListOfUnknownContents& kept) {
            return kept.name == taken;
        });
    };

    BlocksThatEveryTurnPasses passedByEveryTurn(index, loop);
    BitVector namesThatEveryTurnMentions;
    auto noteThatEveryTurnMentions = [&](Reference name) {
        if (name && name.isValue())
            namesThatEveryTurnMentions.set(name.id().index());
    };
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        if (block.isHandlerBlock())
            return { };
        bool isPassedByEveryTurn = passedByEveryTurn.contains(member);
        for (BodyValue operation : block.values()) {
            if (isPassedByEveryTurn) {
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    noteThatEveryTurnMentions(nameThatEntersLoopInListReadsInLoops(walker, index, operand, firstValueOfLoop));
                });
            }
            while (expectNumbersWhereElementIsTakenForNumber(operation)) { }
            expectBoxedValuesWhereElementIsReadByName(operation);
            if (numberConstantsInListReadsInLoops.contains(operation.opcode()))
                noteNumberBehindListsFound(operation, operation.opcode() == Opcode::Integer && operation.as<Opcode::Integer>().integer() == static_cast<int32_t>(operation.as<Opcode::Integer>().integer()));
            else if (operatorsThatMakeNumbersOfNumbers.contains(operation.opcode()) && isNumberBehindListsFound(operation.operand(0)) && isNumberBehindListsFound(operation.operand(1)))
                noteNumberBehindListsFound(operation, operation.opcode() != Opcode::Div && isWholeNumberBehindListsFound(operation.operand(0)) && isWholeNumberBehindListsFound(operation.operand(1)));
            else if (asksItsSiteAndIsReckonedWith(operation))
                noteNumberBehindListsFound(operation, false);
            if (surelyEndsWhatIsShownOfListsInListReadsInLoops(walker, operation, firstValueOfLoop, typeInLook, member != inputHead ? walker.looksCountOnTurnsThatBeginAgain(inputHead) && index.loopOf(member) == loop : walker.looksCountOnTurnsThatBeginAgain(inputHead)))
                return { };
            if (const Walker::IterationByIndex* stepped = findListSteppedByIndexAlone(walker, index, operation); stepped && stepped->isDeadBehindLoop && stepped->list.isValue()) {
                Reference name = stepped->list;
                if (isPassedByEveryTurn)
                    noteThatEveryTurnMentions(name);
                if (isListedForReadByKey(name) || isFreshInListReadsInLoops(walker, name) || isNotedForReadByKey(name))
                    continue;
                ArrayStorage written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(ownTypeInListReadsInLoops(walker, name)));
                if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, index.value(operation.operand(0)).as<Opcode::IteratorOpen>().iterableOperand()).type : TypeID())
                    written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(expected));
                TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id());
                written = kindWrittenOrSeenByLinkInListReadsInLoops(types, written, seenByLink);
                bool holdsNumbers = holdsOnlyNumbers(written);
                if (seenByLink && (types.at(seenByLink).isInt32OrNullish() || holdsKeysOfLoopInListReadsInLoops(walker, valueThatStepHandsOutInListReadsInLoops(index, operation), seenByLink)))
                    found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Int32, false });
                else
                    found.lists.append(ListTestedInFrontOfLoop { name, holdsNumbers ? ArrayStorage::Double : written, holdsNumbers });
                if (ValueID handedOut = found.lists.last().storage == ArrayStorage::Int32 ? valueThatStepHandsOutInListReadsInLoops(index, operation) : ValueID())
                    noteNumberBehindListsFound(index.value(handedOut), true);
                if (ValueID handedOut = written == ArrayStorage::Other && (!seenByLink || types.at(seenByLink).isAnything()) ? valueThatStepHandsOutInListReadsInLoops(index, operation) : ValueID()) {
                    listsOfUnknownContents.append(name);
                    readsOffListsOfUnknownContents.append({ handedOut, name });
                }
                continue;
            }
            FunctionID copied = operation.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()) : FunctionID();
            if (TypeID called = operation.opcode() == Opcode::Call && !copied ? typeInLook(operation.as<Opcode::Call>().calleeOperand()) : TypeID())
                copied = singleFunctionOf(types, types.at(called));
            if (copied && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, copied)) {
                forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, copied, operation.as<Opcode::Call>().argumentsOperands(), 2, Opcode::ElemGet, [&](ValueID argument, ValueID key, FunctionID reader, ValueID read) {
                    ValueID input = valueThatEntersLoopInListReadsInLoops(index, argument, firstValueOfLoop);
                    Reference name = input.index() < firstValueOfLoop ? walker.referenceOf(input) : nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop);
                    if (!name || !name.isValue() || isFreshInListReadsInLoops(walker, name) || isNotedForReadByKey(name))
                        return;
                    TypeID proven = ownTypeInListReadsInLoops(walker, name);
                    for (const GuessInFrontOfLoop& guess : guesses) {
                        if (guess.value == name)
                            proven = types.intersectionOf(proven, guess.tested);
                    }
                    TypeID list = types.withoutNullish(proven);
                    if (!hasKind(types, list, OperandKind::Array) || (key && !readTakesIndexOfProvenList(types, list, typeInLook(key))))
                        return;
                    if (isListedForReadByKey(name))
                        return;
                    TypeID seenByLink = walker.facts().linked().resultOfReadByKey(reader, read);
                    bool readerHasLoops = walker.facts().bodies().at(reader).hasLoops();
                    if (seenByLink && types.at(seenByLink).isInt32OrNullish()) {
                        found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Int32, false });
                        if (readerHasLoops)
                            walker.count(StatisticID::ListsReadInLoopsOfCopiesAskedInFrontOfLoops);
                        return;
                    }
                    ArrayStorage written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, list);
                    if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, input).type : TypeID())
                        written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(expected));
                    written = kindWrittenOrSeenByLinkInListReadsInLoops(types, written, seenByLink);
                    if (written == ArrayStorage::Other)
                        return;
                    bool holdsNumbers = holdsOnlyNumbers(written);
                    found.lists.append(ListTestedInFrontOfLoop { name, holdsNumbers ? ArrayStorage::Double : written, holdsNumbers });
                    if (readerHasLoops)
                        walker.count(StatisticID::ListsReadInLoopsOfCopiesAskedInFrontOfLoops);
                });
                if (!walker.looksCountOnTurnsThatBeginAgain(inputHead))
                    continue;
                forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, copied, operation.as<Opcode::Call>().argumentsOperands(), 2, Opcode::ElemSet, [&](ValueID argument, ValueID key, FunctionID writer, ValueID store) {
                    ValueID input = valueThatEntersLoopInListReadsInLoops(index, argument, firstValueOfLoop);
                    Reference name = input.index() < firstValueOfLoop ? walker.referenceOf(input) : nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop);
                    if (!name || !name.isValue() || isFreshInListReadsInLoops(walker, name) || !storesNumberInCopyOfCalleeInListReadsInLoops(walker, writer, walker.facts().bodies().at(writer).value(store).valueOperand(), 3))
                        return;
                    TypeID list = types.withoutNullish(ownTypeInListReadsInLoops(walker, name));
                    if (!hasKind(types, list, OperandKind::Array) || (key && !readTakesIndexOfProvenList(types, list, typeInLook(key))))
                        return;
                    size_t place = found.lists.findIf([&](const ListTestedInFrontOfLoop& listed) {
                        return listed.name == name;
                    });
                    if (place == notFound) {
                        found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Double, false, true });
                        listsAskedForOneStorage.append(name);
                        return;
                    }
                    ListTestedInFrontOfLoop& listed = found.lists[place];
                    if (!holdsOnlyNumbers(listed.storage) || (!listed.isWritable && listed.storage != ArrayStorage::Double))
                        return;
                    listed.storage = ArrayStorage::Double;
                    listed.admitsInt32Storage = false;
                    listed.isWritable = true;
                    listsAskedForOneStorage.append(name);
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
            Reference nameReadInTurn = input.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop) : Reference();
            if (input.index() >= firstValueOfLoop && !nameReadInTurn) {
                TypeID row = operation.opcode() == Opcode::ElemGet && readsListFoundByReadFromInFront(walker.program(), index, operation, firstValueOfLoop) ? typeInLook(input) : TypeID();
                TypeID key = row ? typeInLook(operation.propertyOperand()) : TypeID();
                if (key && types.at(key).isInteger() && holdsOnlyNumbers(kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(row))))
                    noteNumberBehindListsFound(operation, false);
                continue;
            }
            Reference name = nameReadInTurn ? nameReadInTurn : walker.referenceOf(input);
            if (!name || !name.isValue())
                continue;
            if (!writtenListsRead.containsIf([&](const auto& read) { return read.first == name; })) {
                if (WrittenListOfVariable written = writtenListReadInListReadsInLoops(walker, index, operation))
                    writtenListsRead.append({ name, written });
            }
            if (operation.opcode() == Opcode::ElemGet || storesByKey)
                isListedForReadByKey(name);
            size_t placeInList = found.lists.findIf([&](const ListTestedInFrontOfLoop& list) {
                return list.name == name;
            });
            bool isListed = placeInList != notFound;
            if (!isStore && !readsLength)
                listsAskedForOneStorage.append(name);
            ArrayStorage kindStoredInto = ArrayStorage::Other;
            if (isStore) {
                TypeID stored = typeInLook(operation.valueOperand());
                stored = stored ? types.withoutPrimitives(stored, Primitive::Undefined) : TypeID();
                if ((!stored || isNeverType(stored) || !types.at(stored).isNumber()) && isNumberBehindListsFound(operation.valueOperand()))
                    stored = isWholeNumberBehindListsFound(operation.valueOperand()) ? TypeTable::int32 : TypeTable::number;
                if (!stored || isNeverType(stored) || !types.at(stored).isNumber())
                    continue;
                kindStoredInto = types.at(stored).primitives & Primitive::FractionalNumber ? ArrayStorage::Double : ArrayStorage::Int32;
                if (!types.isSubset(stored, TypeTable::int32)) {
                    bool isWrittenToHoldNumbersAlone = holdsOnlyNumbers(kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(ownTypeInListReadsInLoops(walker, name))));
                    (kindStoredInto == ArrayStorage::Double && !isWrittenToHoldNumbersAlone ? listsThatTakeMoreThanInt32s : listsAskedForOneStorage).append(name);
                }
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
            if (isListed && operation.opcode() == Opcode::ElemGet && listsOfUnknownContents.contains(name))
                readsOffListsOfUnknownContents.append({ operation.id(), name });
            if (isListed) {
                found.lists[placeInList].isAskedForItsStorageAlone &= !readsLength;
                TypeID key = operation.opcode() == Opcode::ElemGet && holdsOnlyNumbers(found.lists[placeInList].storage) ? typeInLook(operation.propertyOperand()) : TypeID();
                if (key && (types.at(key).isInteger() || (types.at(key).isNumber() && walker.looksCountOnTurnsThatBeginAgain(inputHead))))
                    noteNumberBehindListsFound(operation, found.lists[placeInList].storage == ArrayStorage::Int32);
                continue;
            }

            TypeID proven = ownTypeInListReadsInLoops(walker, name);
            for (const GuessInFrontOfLoop& guess : guesses) {
                if (guess.value == name)
                    proven = types.intersectionOf(proven, guess.tested);
            }
            if (BuiltinClassID classOfView = readsLength || asksPlainVector ? BuiltinClassID() : classOfViewInListReadsInLoops(walker, types.withoutNullish(proven))) {
                bool isReadInLine = hasInlineElementAccess(walker.program().builtins().typedArrayTypeOf(classOfView)) || kindsOfViewsOfNumbersIn(walker.program(), types, types.withoutNullish(proven));
                bool lengthMayStandInFront = isReadInLine && !walker.effectsOfQuietDraftsOfRow().writes.contains(AbstractHeap::TypedArrayStorage);
                if (lengthMayStandInFront && viewMayFollowSharedMemoryThatGrows(walker, name) && !isFreshInListReadsInLoops(walker, name) && !findNoteInListReadsInLoops(walker, name)) {
                    found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Other, false });
                    found.lists.last().isAskedForItsStorageAlone = true;
                }
                continue;
            }
            if (storesByKey && !isStore)
                continue;
            if (readsLength) {
                bool staysAsMadeByLink = readsOffEntryParameterThatStaysAsMade(walker, operation) || receiverStaysAsMadeByLink(walker, operation);
                auto lengthIsFoundByNote = [&] {
                    return findKnown(walker, KnownNote::LengthOfViewIsFoundAbove, name) && loopTakesInstancesBelowViewForItsLength(walker, inputHead, name);
                };
                if (!staysAsMadeByLink && classOfViewInListReadsInLoops(walker, proven) && !isFreshInListReadsInLoops(walker, name) && !isListAsMade(walker, name) && !findNoteInListReadsInLoops(walker, name) && !lengthIsFoundByNote())
                    found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Other, false });
                continue;
            }
            TypeID list = types.withoutNullish(proven);
            TypeID keyInLook = typeInLook(asksPlainVector ? operation.positionOperand() : operation.propertyOperand());
            if (!asksPlainVector && keyInLook && !types.at(keyInLook).isNumber() && isWholeNumberBehindListsFound(operation.propertyOperand()))
                keyInLook = TypeTable::int32;
            ValueID keyAsWritten = asksPlainVector ? ValueID() : index.valueBehindCasts(operation.propertyOperand());
            bool keyIsReckonedInTurn = keyAsWritten && keyAsWritten.index() >= firstValueOfLoop && !index.isParameter(keyAsWritten) && operatorsThatReckonKeysInListReadsInLoops.contains(index.opcodeOf(keyAsWritten));
            bool keyIsAskedInTurn = keyIsReckonedInTurn && walker.looksCountOnTurnsThatBeginAgain(inputHead) && keyOfProvenListMayBeAskedForInt32(types, list, keyInLook) && textOfLoopStoresIntoList(name);
            if (!keyIsAskedInTurn && !readTakesIndexOfProvenList(types, list, keyInLook))
                continue;
            if (isFreshInListReadsInLoops(walker, name))
                continue;
            if (asksPlainVector ? !!findNoteInListReadsInLoops(walker, name) : isNotedForReadByKey(name))
                continue;

            if (isStore) {
                found.lists.append(ListTestedInFrontOfLoop { name, kindStoredInto, false, true });
                continue;
            }
            ArrayStorage written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, list);
            if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, input).type : TypeID())
                written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, types.withoutNullish(expected));
            if (walker.wasAskedForStorageOfNumbersInVain(name)) {
                found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Contiguous, false });
                continue;
            }
            if (asksPlainVector) {
                TypeID taken = written == ArrayStorage::Double || written == ArrayStorage::Other ? typeThatCalleeTakesOfElementInListReadsInLoops(walker, index, loop, typeInLook) : TypeID();
                bool takesFractions = taken && arrayStorageHolding(types, taken) == ArrayStorage::Double && (types.at(taken).primitives & Primitive::FractionalNumber);
                std::optional<GuardedReadInListReadsInLoops> guarded = takesFractions ? findGuardedReadInListReadsInLoops(index) : std::nullopt;
                if (guarded && guarded->test == operation.id())
                    found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Double, !!(types.at(taken).primitives & Primitive::NumberWithoutFraction), false, false, true });
                else
                    found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Contiguous, true, false, false, holdsOnlyNumbers(written) });
                continue;
            }
            TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id());
            written = kindWrittenOrSeenByLinkInListReadsInLoops(types, written, seenByLink);
            bool holdsNumbers = holdsOnlyNumbers(written);
            bool wholeNumbersAreOnlyStoredOn = seenByLink && types.at(types.withoutNullish(seenByLink)).isNumberWithoutFraction() && onlyUserStoresWhatIsReadIntoListInListReadsInLoops(index, operation);
            if (seenByLink && (types.at(seenByLink).isInt32OrNullish() || wholeNumbersAreOnlyStoredOn || holdsKeysOfLoopInListReadsInLoops(walker, operation.id(), seenByLink))) {
                found.lists.append(ListTestedInFrontOfLoop { name, ArrayStorage::Int32, false });
                noteNumberBehindListsFound(operation, true);
                continue;
            }
            found.lists.append(ListTestedInFrontOfLoop { name, holdsNumbers ? ArrayStorage::Double : written, holdsNumbers });
            if (holdsNumbers)
                noteNumberBehindListsFound(operation, false);
            else if (written == ArrayStorage::Other && (!seenByLink || types.at(seenByLink).isAnything())) {
                listsOfUnknownContents.append(name);
                readsOffListsOfUnknownContents.append({ operation.id(), name });
            }
        }
    }
    bool turnsStoreIntoLists = found.lists.containsIf([](const ListTestedInFrontOfLoop& list) {
        return list.isWritable;
    });
    if (found.lists.isEmpty())
        return found;

    for (ListTestedInFrontOfLoop& list : found.lists) {
        list.turnsStoreIntoLists = turnsStoreIntoLists;
        if (list.isWritable && holdsOnlyNumbers(list.storage) && !listsAskedForOneStorage.contains(list.name)) {
            list.storage = ArrayStorage::Other;
            list.admitsInt32Storage = !listsThatTakeMoreThanInt32s.contains(list.name);
        }
        size_t written = list.isWritable ? notFound : writtenListsRead.findIf([&](const auto& read) { return read.first == list.name; });
        if (written == notFound)
            continue;
        list.written = writtenListsRead[written].second;
        list.storage = list.written.storage;
        list.admitsInt32Storage = false;
    }
    if (guesses.isEmpty()) {
        if (!rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), { }, { }))
            return { };
        found.areLeftAsTheyAre = true;
    }
    found.lists.removeAllMatching([&](const ListTestedInFrontOfLoop& list) {
        bool canFailByKindOfStorageAlone = !list.isWritable && walker.types().isSubset(walker.typeOf(list.name), walker.types().builtinClass(BuiltinClasses::Array));
        bool isAskedInFrontAnyway = guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
            return guess.value == list.name;
        });
        return !namesThatEveryTurnMentions.get(list.name.id().index()) && !canFailByKindOfStorageAlone && !isAskedInFrontAnyway;
    });
    if (found.lists.isEmpty())
        return { };

    for (const ListTestedInFrontOfLoop& list : found.lists) {
        bool hasGuess = guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
            return guess.value == list.name;
        });
        if (hasGuess || isProvenListInListReadsInLoops(walker, list.name) || classOfProvenViewInListReadsInLoops(walker, list.name))
            continue;
        TypeID ownType = types.withoutNullish(ownTypeInListReadsInLoops(walker, list.name));
        BuiltinClassID classOfView = classOfViewInListReadsInLoops(walker, ownType);
        TypeID asked = kindsOfViewsOfNumbersIn(walker.program(), types, ownType) ? ownType : types.builtinClass(classOfView ? classOfView : BuiltinClassID(BuiltinClasses::Array));
        guesses.append(GuessInFrontOfLoop { list.name, asked, { }, namesInFrontThatLoopMentions(walker, index, loop, firstValueOfLoop, list.name) });
    }
    return found;
}

static Draft questionWhetherListIsAsMadeInListReadsInLoops(Walker& walker, Reference list, Origin origin)
{
    Draft isAsMade(Opcode::IsPristine, origin);
    walker.chooseConservativeFields(isAsMade);
    isAsMade.setValueOperand(list);
    isAsMade.setBuiltinClass(BuiltinClassID(BuiltinClasses::Array));
    PristineAspectSet aspects { PristineAspects::Storage, PristineAspects::Names };
    if (walker.program().admitsReplacedBuiltins)
        aspects.add(PristineAspects::CellAlone);
    isAsMade.setAspects(aspects);
    return isAsMade;
}

static Draft questionForKindOfStorageInListReadsInLoops(Walker& walker, const ListTestedInFrontOfLoop& tested, Origin origin)
{
    Draft hasKindWritten(Opcode::HasArrayStorage, origin);
    walker.chooseConservativeFields(hasKindWritten);
    hasKindWritten.setArrayOperand(tested.name);
    hasKindWritten.setArrayStorage(tested.storage);
    hasKindWritten.setAdmitsInt32Storage(tested.admitsInt32Storage);
    hasKindWritten.setIsWritable(tested.isWritable);
    hasKindWritten.setData(tested.written.data);
    return hasKindWritten;
}

template<typename Passes>
static bool askWhatListIsAskedInListReadsInLoops(Walker& walker, const ListTestedInFrontOfLoop& tested, Origin origin, const Passes& passes)
{
    if (!passes(questionWhetherListIsAsMadeInListReadsInLoops(walker, tested.name, origin)))
        return false;
    return (!hasArrayStorageForm(tested.storage) && !tested.isWritable) || passes(questionForKindOfStorageInListReadsInLoops(walker, tested, origin));
}

static bool isFoundByReadAtSiteInListReadsInLoops(Walker& walker, Reference list)
{
    const Code& code = walker.code();
    ValueID held = list.id();
    while (opcodesThatGiveSameValue.contains(code.at(held).opcode))
        held = code.at(held).operand(0);
    return code.at(held).opcode == Opcode::GetValueAtSiteOrAbsent;
}

std::optional<ListTestedInFrontOfLoop> askListInFrontOfTypedCopy(Walker& walker, const BodyValue& read, Reference list, ArrayStorage kindStoredInto, bool goesByKindOfStorage)
{
    if (!walker.has(Mechanism::Folding) || !list || !list.isValue() || !walker.standsInFrontOfTypedCopy(list) || !walker.turnsOfTypedCopyLeaveListsAsTheyAre(list))
        return std::nullopt;
    if (!isProvenListInListReadsInLoops(walker, list) || isFreshInListReadsInLoops(walker, list))
        return std::nullopt;
    ArrayStorage written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, ownTypeInListReadsInLoops(walker, list));
    if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, read.objectOperand()).type : TypeID())
        written = kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, walker.types().withoutNullish(expected));
    TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
    if (written == ArrayStorage::Other && seenByLink && !walker.types().at(seenByLink).isAnything()) {
        ArrayStorage seen = arrayStorageHolding(walker.types(), seenByLink);
        if (holdsOnlyNumbers(seen) || !(walker.types().at(seenByLink).primitives & Primitive::Number))
            written = seen;
    }
    auto typeAsBuilt = [&](ValueID input) {
        Reference built = walker.referenceOf(input);
        return built ? walker.typeOf(built) : TypeID();
    };
    if (written == ArrayStorage::Other && kindStoredInto == ArrayStorage::Other && read.opcode() == Opcode::ElemGet && isFoundByReadAtSiteInListReadsInLoops(walker, list) && valueIsOnlyReckonedWithAsNumber(walker, read.id(), TypesOfInputs(typeAsBuilt)))
        written = ArrayStorage::Double;
    bool holdsNumbers = holdsOnlyNumbers(written);
    ListTestedInFrontOfLoop tested { list, holdsNumbers ? ArrayStorage::Double : written, holdsNumbers };
    if (seenByLink && (walker.types().at(seenByLink).isInt32OrNullish() || (isReadOfBodyInListReadsInLoops(walker.currentBodyIndex(), read) && holdsKeysOfLoopInListReadsInLoops(walker, read.id(), seenByLink))))
        tested = ListTestedInFrontOfLoop { list, ArrayStorage::Int32, false };
    if (kindStoredInto != ArrayStorage::Other)
        tested = goesByKindOfStorage && (kindStoredInto == ArrayStorage::Int32 || !holdsNumbers) ? ListTestedInFrontOfLoop { list, ArrayStorage::Other, kindStoredInto == ArrayStorage::Int32, true, true } : ListTestedInFrontOfLoop { list, kindStoredInto, false, true, true };
    else if (WrittenListOfVariable written = writtenListReadInListReadsInLoops(walker, walker.currentBodyIndex(), read)) {
        tested = ListTestedInFrontOfLoop { list, written.storage, false };
        tested.written = written;
    }
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    auto passesInFront = [&](Draft&& draft) {
        Reference test;
        {
            Walker::InFrontOfTypedCopy inFront(walker, true);
            test = walker.build(WTF::move(draft));
        }
        return test && walker.branchInFrontOfTypedCopy(test, true, Walker::TurnOfTypedCopy::HasDoneNothing, kindStoredInto == ArrayStorage::Other ? Walker::TurnsThatAsk::Some : Walker::TurnsThatAsk::All);
    };
    if (!askWhatListIsAskedInListReadsInLoops(walker, tested, origin, passesInFront))
        return std::nullopt;
    walker.noteGuessOpened();
    walker.noteListAskedInFrontOfTypedCopy();
    noteListTestedInListReadsInLoops(walker, tested);
    if (tested.written)
        noteStorageOfWrittenList(walker, list, tested.written);
    return tested;
}

bool isExpectedToBeListOfNumbers(Walker& walker, TypeID expected)
{
    return holdsOnlyNumbers(kindThatHoldsWhatIsWrittenInListReadsInLoops(walker, expected));
}

bool turnOfTypedCopyMayAskList(Walker& walker, Reference list)
{
    return walker.has(Mechanism::Folding) && list && list.isValue() && walker.standsInTurnThatLooksCountOn() && walker.mayBeginTurnAgainInClosedCopy() && walker.turnsOfTypedCopyLeaveListsAsTheyAre(list) && walker.turnsOfTypedCopyStoreIntoNoList() && !isFreshInListReadsInLoops(walker, list);
}

static bool goOnWhereTestPassesOrBeginTurnAgainInListReadsInLoops(Walker& walker, Draft&& draft)
{
    Reference test = walker.build(WTF::move(draft));
    if (!test)
        return false;
    BlockID passed = walker.addBlock();
    BlockID failed = walker.addBlock(BlockFrequency::Rare);
    walker.branch(test, passed, failed);
    walker.continueIn(failed);
    if (!walker.isUnreachable())
        walker.beginTurnAgainInClosedCopy();
    walker.continueIn(passed);
    return !walker.isUnreachable() && !walker.hasFailed();
}

bool goOnWhereListIsAsMadeOrBeginTurnAgain(Walker& walker, Reference list, Origin origin)
{
    return goOnWhereTestPassesOrBeginTurnAgainInListReadsInLoops(walker, questionWhetherListIsAsMadeInListReadsInLoops(walker, list, origin));
}

Reference askWhetherListIsAsMade(Walker& walker, Reference list, Origin origin)
{
    return walker.build(questionWhetherListIsAsMadeInListReadsInLoops(walker, list, origin));
}

Reference askWhetherStorageOfListIsOfKind(Walker& walker, Reference list, ArrayStorage kind, Origin origin, bool isWritable)
{
    return walker.build(questionForKindOfStorageInListReadsInLoops(walker, ListTestedInFrontOfLoop { list, kind, false, isWritable }, origin));
}

void noteKindOfStorageThatTurnFound(Walker& walker, Reference list, ArrayStorage kind, bool isWritable)
{
    noteListTestedInListReadsInLoops(walker, ListTestedInFrontOfLoop { list, kind, false, isWritable });
}

std::optional<ListTestedInFrontOfLoop> askListOfNumbersInTurnOfTypedCopy(Walker& walker, const BodyValue& read, Reference list)
{
    if (!turnOfTypedCopyMayAskList(walker, list) || !isProvenListInListReadsInLoops(walker, list) || !isExpectedToBeListOfNumbers(walker, ownTypeInListReadsInLoops(walker, list)))
        return std::nullopt;
    ListTestedInFrontOfLoop tested { list, ArrayStorage::Double, true };
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    auto passesInTurn = [&](Draft&& draft) {
        return goOnWhereTestPassesOrBeginTurnAgainInListReadsInLoops(walker, WTF::move(draft));
    };
    if (!askWhatListIsAskedInListReadsInLoops(walker, tested, origin, passesInTurn))
        return std::nullopt;
    walker.noteGuessOpened();
    walker.count(StatisticID::ListsAskedInTurnsOfTypedCopies);
    noteListTestedInListReadsInLoops(walker, tested);
    return tested;
}

std::optional<PassedTest> askElementInFrontOfTypedCopy(Walker& walker, const BodyValue& read, const ListTestedInFrontOfLoop& tested, Reference list, Reference position)
{
    bool isNumber = holdsOnlyNumbers(tested.storage);
    if (!walker.has(Mechanism::Folding) || walker.listsAreStruckInRow() || (tested.storage != ArrayStorage::Contiguous && !isNumber) || tested.isWritable || (isNumber && tested.turnsStoreIntoLists))
        return std::nullopt;
    if (!walker.standsInFrontOfTypedCopy(list) || !walker.standsInFrontOfTypedCopy(position) || !walker.turnsOfTypedCopyLeaveListsAsTheyAre(list) || !(isNumber ? walker.turnsOfTypedCopyStoreIntoNoList() : walker.turnsOfTypedCopyStoreOnlyRawDoublesIntoLists()))
        return std::nullopt;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    Reference isThere;
    {
        Walker::InFrontOfTypedCopy inFront(walker, true);
        Draft isPresent(Opcode::IsElementPresent, origin);
        walker.chooseConservativeFields(isPresent);
        isPresent.setArrayOperand(list);
        isPresent.setPositionOperand(position);
        isPresent.setArrayStorage(tested.storage);
        isPresent.setAdmitsInt32Storage(tested.admitsInt32Storage);
        isPresent.setKeyIsIntegral(true);
        isThere = walker.build(WTF::move(isPresent));
    }
    std::optional<PassedTest> present = isThere ? walker.showElementPresentBehindTest(list, position, walker.goOnInTypedCopyWhereTestInFrontPasses(isThere, true)) : std::nullopt;
    if (!present)
        return std::nullopt;
    walker.noteGuessOpened();
    if (isNumber) {
        walker.noteNumberReadInFrontOfTypedCopy();
        walker.count(StatisticID::NumbersOfListsReadInFrontOfTypedCopies);
    } else
        walker.noteElementAskedInFrontOfTypedCopy();
    return present;
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

static bool isBornToHoldDoublesInListReadsInLoops(Walker& walker, Reference list)
{
    const Operation& born = walker.code().at(rootOfCellInSweptCode(walker.code(), list.id()));
    return born.opcode == Opcode::NewArrayLike && !born.sourceOperand() && born.arrayStorage() == ArrayStorage::Double;
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

    for (ListTestedInFrontOfLoop& list : found.lists) {
        Draft isAsMade(Opcode::IsPristine, origin);
        walker.chooseConservativeFields(isAsMade);
        isAsMade.setValueOperand(list.name);
        BuiltinClassID classOfView = classOfProvenViewInListReadsInLoops(walker, list.name);
        isAsMade.setBuiltinClass(classOfView ? classOfView : BuiltinClassID(BuiltinClasses::Array));
        bool isToldByItsStructure = classOfView && pristineTestOf(walker.program().builtins(), classOfView) == PristineTest::InstanceStructure;
        PristineAspectSet aspects = isToldByItsStructure ? PristineAspectSet { PristineAspects::Names } : PristineAspectSet { PristineAspects::Storage, PristineAspects::Names };
        list.isAskedForItsStorageAlone &= classOfView && !isToldByItsStructure;
        if (list.isAskedForItsStorageAlone)
            aspects.remove(PristineAspects::Names);
        list.takesInstancesBelow = classOfView && !isToldByItsStructure && !list.isAskedForItsStorageAlone && loopTakesInstancesBelowViewForItsLength(walker, inputHead, list.name);
        if (list.takesInstancesBelow) {
            aspects.remove(PristineAspects::Names);
            aspects.add(PristineAspects::NamesKeptBelow);
            walker.count(StatisticID::ViewsThatTakeInstancesBelowForTheirLength);
        }
        if (classOfView && !someThreadMayGrowSharedMemory(walker))
            aspects.remove(PristineAspects::Storage);
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
        if (classOfView && isAskedOfCell && (isToldByItsStructure || aspects.contains(PristineAspects::Storage)))
            noteKnown(walker, KnownNote::ViewOverMemoryThatNoOtherThreadGrows, list.name, { }, 0, LocationMasks(), 1);
    }
    for (const ListTestedInFrontOfLoop& list : found.lists) {
        if ((list.storage != ArrayStorage::Double && hasArrayStorageForm(list.storage)) || !list.isWritable || list.admitsInt32Storage)
            continue;
        Draft convert(Opcode::ConvertToDoubleStorage, origin);
        walker.chooseConservativeFields(convert);
        convert.setArrayOperand(list.name);
        walker.build(WTF::move(convert));
        walker.count(StatisticID::ListsConvertedInFrontOfTypedCopies);
    }
    for (ListTestedInFrontOfLoop& list : found.lists) {
        if (!hasArrayStorageForm(list.storage) && !list.isWritable)
            continue;
        if (list.written) {
            Draft isStillWritten(Opcode::HasArrayStorage, origin);
            walker.chooseConservativeFields(isStillWritten);
            isStillWritten.setArrayOperand(list.name);
            isStillWritten.setArrayStorage(list.storage);
            isStillWritten.setData(list.written.data);
            if (!goOnWhereTestPassesInListReadsInLoops(walker, walker.build(WTF::move(isStillWritten)), closedStart))
                return;
            noteStorageOfWrittenList(walker, list.name, list.written);
            continue;
        }
        if (!list.leavesBirthAsItIs)
            askBirthOfListForStorage(walker, list.name, hasArrayStorageForm(list.storage) ? list.storage : list.admitsInt32Storage ? ArrayStorage::Int32 : ArrayStorage::Double);
        if (list.storage == ArrayStorage::Double && list.admitsInt32Storage && isBornToHoldDoublesInListReadsInLoops(walker, list.name)) {
            list.admitsInt32Storage = false;
            walker.count(StatisticID::ListsBornToHoldDoublesAskedForDoublesAlone);
        }
        Draft hasKindWritten(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(hasKindWritten);
        hasKindWritten.setArrayOperand(list.name);
        hasKindWritten.setArrayStorage(list.storage);
        hasKindWritten.setAdmitsInt32Storage(list.admitsInt32Storage);
        hasKindWritten.setIsWritable(list.isWritable);
        Reference holdsKindWritten = walker.build(WTF::move(hasKindWritten));
        if (!goOnWhereTestPassesInListReadsInLoops(walker, holdsKindWritten, closedStart))
            return;
    }
    for (const ListTestedInFrontOfLoop& list : found.lists) {
        if (list.isAskedForItsStorageAlone)
            continue;
        noteListTestedInListReadsInLoops(walker, list);
        if (list.takesInstancesBelow)
            noteThatLengthOfViewIsFoundAbove(walker, list.name);
        else if (classOfProvenViewInListReadsInLoops(walker, list.name))
            noteListAsMade(walker, list.name);
    }
    found.areTested = true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
