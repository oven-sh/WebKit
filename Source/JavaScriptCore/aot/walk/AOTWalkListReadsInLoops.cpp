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
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOperandKinds.h"
#include "AOTResultRules.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include "AOTWalkBlocksThatEveryTurnPasses.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkKnown.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"
#include "JSString.h"
#include <array>
#include <wtf/SetForScope.h>
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

static void noteListTestedInListReadsInLoops(Walker& walker, const HoistedArrayGuard& tested)
{
    uint64_t noted = tested.name.bits()
        | static_cast<uint64_t>(tested.storage) << shiftOfStorageInListReadsInLoops
        | static_cast<uint64_t>(tested.admitsInt32Storage) << shiftOfSecondKindInListReadsInLoops
        | static_cast<uint64_t>(tested.isWritable) << shiftOfWritableInListReadsInLoops
        | static_cast<uint64_t>(tested.turnsStoreIntoLists) << shiftOfStoresInListReadsInLoops;
    noteKnown(walker, KnownNote::HoistedArrayGuard, tested.name, { }, 0, LocationMasks(classOfProvenViewInListReadsInLoops(walker, tested.name) ? heapsUnderViewsReadInLoops : heapsUnderListsReadInLoops), noted);
}

static std::optional<HoistedArrayGuard> findNoteInListReadsInLoops(Walker& walker, Reference list)
{
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::HoistedArrayGuard, list);
    if (!noted)
        return std::nullopt;
    return HoistedArrayGuard {
        Reference::fromBits(static_cast<uint32_t>(*noted)),
        static_cast<ArrayStorage>(*noted >> shiftOfStorageInListReadsInLoops & 0xff),
        !!(*noted >> shiftOfSecondKindInListReadsInLoops & 1),
        !!(*noted >> shiftOfWritableInListReadsInLoops & 1),
        !!(*noted >> shiftOfStoresInListReadsInLoops & 1)
    };
}

static bool isAskedForStoresAlone(const HoistedArrayGuard& tested)
{
    return tested.isWritable && !hasArrayStorageForm(tested.storage);
}

static bool isShownToReads(const HoistedArrayGuard& tested)
{
    return !isAskedForStoresAlone(tested) && accessToElementsHasCode(Opcode::GetArrayElement, tested.storage, tested.admitsInt32Storage, false);
}

static bool holdsBoxedValuesOfEitherKind(const HoistedArrayGuard& tested)
{
    return tested.storage == ArrayStorage::Contiguous && tested.admitsInt32Storage;
}

static bool writesWhatLiesUnderListsReadInLoops(const Walker& walker, const Effects& effects)
{
    if (walker.shouldAssumeInvariantArrayStorage())
        return effects.writes.containsAny(heapsUnderListsReadInLoops - AbstractHeapSet { AbstractHeap::ArrayStorage });
    return effects.writes.containsAny(heapsUnderListsReadInLoops);
}

bool preservesHoistedArrayGuards(const Walker& walker, const Effects& effects)
{
    return !effects.mayRunOtherCode() && !writesWhatLiesUnderListsReadInLoops(walker, effects);
}

bool preservesHoistedArrayGuards(const Walker& walker, const Effects& effects, std::span<const HoistedArrayGuard> lists, const Walker::AllocationsUsedInLoop& mentioned)
{
    if (effects.mayRunOtherCode())
        return false;
    if (!writesWhatLiesUnderListsReadInLoops(walker, effects))
        return true;
    return effects.writesObjectsMadeHereAlone && std::ranges::all_of(lists, [&](const HoistedArrayGuard& list) {
        std::optional<Walker::IsListedAllocation> other = walker.classifyAgainstAllocations(mentioned, list.name);
        return other && (*other == Walker::IsListedAllocation::No || !list.isWritable);
    });
}

bool preservesHoistedArrayGuards(Walker& walker, const Effects& effects, std::span<const HoistedArrayGuard> lists, BlockID inputHead)
{
    if (preservesHoistedArrayGuards(walker, effects))
        return true;
    return !effects.mayRunOtherCode() && effects.writesObjectsMadeHereAlone && preservesHoistedArrayGuards(walker, effects, lists, walker.allocationsUsedInLoop(inputHead));
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
    ValueID source = index.stripCasts(input);
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
        source = index.stripCasts(index.value(source).operand(0));
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
    if (index.isParameter(enters) || (index.opcodeOf(enters) != Opcode::PrivateGet && (index.opcodeOf(enters) != Opcode::PropGet || index.value(enters).isFreeName())))
        return { };
    BodyValue read = index.value(enters);
    PropertyKeyID key = keyOfAccessToSlot(walker.program(), read);
    LoopID loop = index.loopOf(index.blockOf(enters));
    while (LoopID around = loop ? index.loop(loop).parent : LoopID())
        loop = around;
    if (!loop || walker.textOfOutermostLoopStoresUnderNameInCurrentBody(loop, key))
        return { };
    Reference holder = nameThatEntersLoopInListReadsInLoops(walker, index, read.objectOperand(), firstValueOfLoop);
    TypeID type = holder && holder.isValue() ? walker.typeOf(holder) : TypeID();
    if (!type || !walker.types().at(type).isObject() || !walker.types().at(type).hasOnlyLayouts())
        return { };
    std::optional<FieldReference> field = findField(walker.program(), membersOf(walker.types(), walker.types().at(type))[0], key);
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
    if (!read && !index.isParameter(enters) && index.opcodeOf(enters) == Opcode::ElemGet && walker.standsInCopyOfLoopBuiltForEachMember())
        return rowFoundUnderKeyInFrontOfLoop(walker, index.value(enters));
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
    return (access.opcode() == Opcode::ElemGet || access.opcode() == Opcode::ElemSet) && !index.isParameter(index.stripCasts(access.propertyOperand()));
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
    for (ValueID read = index.stripCasts(holder); read && read.index() >= firstValueOfLoop && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName(); read = index.stripCasts(holder)) {
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
    WhatReadsFromInFrontOfLoopFind foundFromInFront;
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
            if (readsListFoundByReadFromInFront(program, index, operation, firstValueOfLoop, foundFromInFront))
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
                    draft.setIsKnownInitialized(true);
                Reference held = walker.build(WTF::move(draft));
                if (!held)
                    return;
                walker.holdWhatBodyReadsOfItsOwnScope(read, environment, held);
                walker.noteEnvironmentField(environment, field, held);
                noteValueOfStableBinding(walker, environment, field, held);
                walker.count(StatisticID::CapturedArrayReadsHoisted);
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
                walker.count(StatisticID::ModuleConstantReadsHoisted);
                continue;
            }
            if (!numberReadOutOfVariableStays(walker, variable) && !walker.plan().deferredDefinitions().findDeferred(variable)) {
                if (walker.state().variable(variable) || (!isHolder && program.at(variable).module != program.at(walker.currentFunction()).module) || goesUnderKeyMadeInTurnInListReadsInLoops(index, operation) || readGivesNoObjectInListReadsInLoops(walker, read) || textOfLoopStoresVariableInListReadsInLoops(index, read))
                    continue;
                Draft draft = walker.draftOf(read);
                walker.chooseConservativeFields(draft);
                if (isHolderOfAnotherFile)
                    draft.setReturnsRawSlot(true);
                else if (walker.state().isPastDeadZone(variable))
                    draft.setIsKnownInitialized(true);
                Reference held = walker.build(WTF::move(draft));
                if (!held)
                    return;
                walker.state().noteVariable(variable, held);
                walker.count(StatisticID::CapturedArrayReadsHoisted);
                continue;
            }
            if (!numberReadOutOfVariableStays(walker, variable) || valueOfStableBinding(walker, read) || (program.at(variable).initialization != VariableInitialization::Uninitialized && walker.state().variable(variable)))
                continue;
            if (!isHolder && program.at(variable).module != program.at(walker.currentFunction()).module && !walker.state().isPastDeadZone(variable))
                continue;
            TypeID widest = widestTypeOfOperation(walker, read);
            bool isCallArgument = operation.opcode() == Opcode::Call;
            if (isCallArgument && widest && (isNeverType(walker.types().withoutNullish(widest)) || !hasKind(walker.types(), walker.types().withoutNullish(widest), OperandKind::Array)))
                continue;
            if (!widest || (walker.types().at(widest).primitives & Primitive::Unspecified))
                continue;
            bool mayBeInDeadZone = isHolderOfAnotherFile || (walker.types().at(widest).primitives & (isHolder ? Primitive::Absent | Primitive::Undefined : Primitive::Absent));
            if (mayBeInDeadZone && isHolder && walker.state().variable(variable))
                continue;
            if (mayBeInDeadZone && !isHolder && (isCallArgument || (!isElementUsedInArithmetic(walker, operation) && !loopComparesElementWithCharacters(operation) && !(member == inputHead && operation.opcode() == Opcode::PropGet && readsOnlyVariableLength(program, index, variable))) || walker.state().variable(variable) || !walker.types().isUninformative(walker.types().withoutPrimitives(widest, Primitive::Absent))))
                continue;
            Draft draft = walker.draftOf(read);
            walker.chooseConservativeFields(draft);
            if (isHolderOfAnotherFile) {
                draft.setIsKnownInitialized(false);
                draft.setReturnsRawSlot(true);
            }
            Reference held = walker.build(WTF::move(draft));
            if (!held)
                return;
            bool mayHoldUndefinedStill = program.at(variable).initialization != VariableInitialization::Uninitialized && (walker.types().at(widest).primitives & Primitive::Undefined);
            if (mayBeInDeadZone || mayHoldUndefinedStill)
                walker.state().noteVariable(variable, held);
            else
                noteValueOfStableBinding(walker, variable, held);
            walker.count(StatisticID::ModuleConstantReadsHoisted);
            }
        }
    }
}

static Reference readFieldInFrontInListReadsInLoops(Walker& walker, const BodyIndex& index, ValueID value, LoopID outermost, uint32_t firstValueOfLoop)
{
    ValueID found = valueThatEntersLoopInListReadsInLoops(index, value, firstValueOfLoop);
    if (!found || found.index() < firstValueOfLoop || index.isParameter(found))
        return nameThatEntersLoopInListReadsInLoops(walker, index, value, firstValueOfLoop);
    BodyValue read = index.value(found);
    if (read.opcode() != Opcode::PrivateGet && (read.opcode() != Opcode::PropGet || read.isFreeName()))
        return nameThatEntersLoopInListReadsInLoops(walker, index, value, firstValueOfLoop);
    PropertyKeyID key = keyOfAccessToSlot(walker.program(), read);
    if (walker.textOfOutermostLoopStoresUnderNameInCurrentBody(outermost, key))
        return { };
    Reference holder = readFieldInFrontInListReadsInLoops(walker, index, read.objectOperand(), outermost, firstValueOfLoop);
    return readFilledFieldInFrontOfTurns(walker, read, holder);
}

static bool loadsViewInListReadsInLoops(Walker& walker, const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    ValueID loaded = valueThatEntersLoopInListReadsInLoops(index, value, firstValueOfLoop);
    if (!loaded || loaded.index() < firstValueOfLoop || index.isParameter(loaded))
        return false;
    TypeID widest = widestTypeOfOperation(walker, index.value(loaded));
    return widest && isViewOfNumbersReadInLine(walker, walker.types().withoutNullish(widest));
}

void readListsOfFieldsInFrontOfLoop(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::TypedOperations) || !index.holdsAnyOf(OpcodeSet { Opcode::ElemGet, Opcode::ElemSet }))
        return;
    bool storesElements = index.opcodes().contains(Opcode::ElemSet);
    bool isToldApart = walker.standsInCopyOfLoopBuiltForEachMember();
    auto readsWhatNobodyKnows = [&](const BodyValue& access) {
        if (!isToldApart)
            return false;
        TypeID seenByLink = access.opcode() == Opcode::ElemGet ? walker.facts().linked().resultOfReadByKey(walker.currentFunction(), access.id()) : TypeID(TypeTable::never);
        return !seenByLink || walker.types().at(seenByLink).isAnything();
    };
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    LoopID loop = index.loopOf(inputHead);
    LoopID outermost = loop;
    while (LoopID around = index.loop(outermost).parent)
        outermost = around;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (walker.isUnreachable())
                return;
            if ((operation.opcode() != Opcode::ElemSet && operation.opcode() != Opcode::ElemGet) || goesUnderKeyMadeInTurnInListReadsInLoops(index, operation))
                continue;
            if (storesElements || readsWhatNobodyKnows(operation) || loadsViewInListReadsInLoops(walker, index, operation.objectOperand(), firstValueOfLoop))
                readFieldInFrontInListReadsInLoops(walker, index, operation.objectOperand(), outermost, firstValueOfLoop);
        }
    }
}

void forEachElementAccessInInlinedCallee(Walker& walker, const BodyValue& call, NOESCAPE const ScopedLambda<void(ValueID argument, ValueID key)>& take)
{
    FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id());
    if (!callee || !lookReadsCopyOfCalleeInListReadsInLoops(walker, call, callee))
        return;
    for (Opcode access : { Opcode::ElemGet, Opcode::ElemSet }) {
        forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, callee, call.as<Opcode::Call>().argumentsOperands(), 2, access, [&](ValueID argument, ValueID key, FunctionID, ValueID) {
            take(argument, key);
        });
    }
}

bool storesIntoListTestedInFrontOfLoop(Walker& walker, const BodyIndex& index, const BodyValue& store, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists)
{
    Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, store.objectOperand(), firstValueOfLoop);
    return name && std::ranges::any_of(lists, [&](const HoistedArrayGuard& list) {
        return list.isWritable && list.name == name;
    });
}

Reference literalThatCallPushesOntoInEveryTurn(Walker& walker, const BodyValue& call, BlockID inputHead)
{
    if (call.opcode() != Opcode::Call || !call.as<Opcode::Call>().thisValueOperand() || call.as<Opcode::Call>().argumentsOperands().size() != 1 || walker.state().answers().isEmpty())
        return { };
    Reference list = walker.referenceOf(walker.currentBodyIndex().stripCasts(call.as<Opcode::Call>().thisValueOperand()));
    if (!list || !list.isValue())
        return { };
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::PushAppendsToLiteral, list, { }, inputHead.index());
    return noted && *noted == (call.id().index() | static_cast<uint64_t>(walker.currentFunction().index()) << 32) ? list : Reference();
}

std::optional<HoistedArrayGuard> findHoistedArrayGuard(Walker& walker, const BodyValue& read)
{
    if (walker.state().answers().isEmpty())
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID outermost = index.loopOf(index.blockOf(read.id()));
    if (!outermost && !walker.isInLoop() && !walker.guardedListOfReads())
        return std::nullopt;
    while (LoopID around = outermost ? index.loop(outermost).parent : LoopID())
        outermost = around;
    uint32_t firstValueOfLoops = outermost ? index.block(index.loop(outermost).head).firstParameter().index() : 0;
    for (ValueID input = read.objectOperand(); input; input = index.value(input).operand(0)) {
        if (index.isParameter(input) && index.blockOf(input) == walker.inputBlockBeingWalked() && index.isLoopHead(index.blockOf(input)))
            return std::nullopt;
        if (Reference list = walker.referenceOf(input); list && list.isValue()) {
            if (std::optional<HoistedArrayGuard> tested = findNoteInListReadsInLoops(walker, list))
                return (isAskedForStoresAlone(*tested) ? read.opcode() == Opcode::ElemSet : isShownToReads(*tested)) ? tested : std::nullopt;
        }
        if (!isCastInLoopsInListReadsInLoops(index, input, firstValueOfLoops))
            break;
    }
    return std::nullopt;
}

std::optional<HoistedArrayGuard> findHoistedArrayGuard(Walker& walker, Reference list)
{
    if (walker.state().answers().isEmpty() || !list || !list.isValue())
        return std::nullopt;
    std::optional<HoistedArrayGuard> tested = findNoteInListReadsInLoops(walker, list);
    return tested && isShownToReads(*tested) ? tested : std::nullopt;
}

std::optional<HoistedArrayGuard> findListTestedForStoresInFrontOfLoop(Walker& walker, Reference list)
{
    if (walker.state().answers().isEmpty() || !list || !list.isValue())
        return std::nullopt;
    std::optional<HoistedArrayGuard> tested = findNoteInListReadsInLoops(walker, list);
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
    ValueID cell = underlyingCellOf(walker.code(), view.id());
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if ((operation.opcode() != Opcode::PropGet && operation.opcode() != Opcode::PropSet) || operation.isFreeName())
                continue;
            if (operation.opcode() == Opcode::PropGet && walker.program().nameClassOf(operation.key()) == NameClass::Length)
                continue;
            Reference holder = nameThatEntersLoopInListReadsInLoops(walker, index, operation.objectOperand(), firstValueOfLoop);
            if (holder && holder.isValue() && underlyingCellOf(walker.code(), holder.id()) == cell)
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
    walker.verifyRememberedTest(draftTestOfViewOrInstanceBelow(walker, view, origin));
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
    std::optional<HoistedArrayGuard> tested = findHoistedArrayGuard(walker, read);
    return tested && classOfProvenViewInListReadsInLoops(walker, tested->name);
}

void nameKindOfListTestedInFrontOfLoop(Draft& read, const HoistedArrayGuard& tested)
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

void takeStorageAndLengthFromInFrontOfLoop(Walker& walker, Draft& read, const HoistedArrayGuard& tested)
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
    if ((read.opcode == Opcode::GetArrayElement || read.opcode == Opcode::IsElementPresent) && read.admitsInt32Storage() && read.arrayStorage() == ArrayStorage::Double) {
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

Reference storageOfListFromInFrontOfLoop(Walker& walker, const HoistedArrayGuard& tested, Origin origin)
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
    bool areOfOneElement = index.stripCasts(test.arrayOperand()) == index.stripCasts(read.arrayOperand()) && index.stripCasts(test.positionOperand()) == index.stripCasts(read.positionOperand());
    return areOfOneElement ? std::optional(found) : std::nullopt;
}

static bool buildPlainVectorOfNumbersInListReadsInLoops(Walker& walker, const BodyValue& value, const std::optional<HoistedArrayGuard>& tested)
{
    bool asks = value.opcode() == Opcode::HasContiguousIndex;
    if (asks && (!tested || !isNumericStorage(tested->storage) || tested->turnsStoreIntoLists))
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

static Draft questionForKindOfStorageInListReadsInLoops(Walker&, const HoistedArrayGuard&, Origin);
OperatorForm operatorFormFor(Walker&, const Draft&);

static bool tryStoreWithHoistedStorageGuard(Walker& walker, const BodyValue& value)
{
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnly;
    if (!walker.has(Mechanism::Folding) || !walker.isInLoop() || walker.state().answers().isEmpty())
        return false;
    Draft written = walker.draftOf(value);
    ValueID birth = written.arrayOperand() && written.arrayOperand().isValue() ? underlyingCellOf(walker.code(), written.arrayOperand().id()) : ValueID();
    bool remembered = birth && walker.code().at(birth).opcode == Opcode::NewArrayLike && walker.code().at(birth).remembersStorageAtSite();
    walker.chooseConservativeFields(written);
    if (written.opcode != Opcode::TryPutByValDirect)
        return false;
    Reference list = written.arrayOperand();
    Reference position = written.positionOperand();
    Reference stored = written.valueOperand();
    bool goesByKind = written.arrayStorage() == ArrayStorage::Double && stored && walker.typeOf(stored) && walker.types().at(walker.typeOf(stored)).isNumber();
    if (written.arrayStorage() != ArrayStorage::Contiguous && !goesByKind)
        return false;
    auto staysTyped = [&] {
        if (goesByKind && remembered && walker.code().at(birth).remembersStorageAtSite())
            walker.code().letListForgetStorageAtSite(birth);
        return false;
    };
    bool mayBeNaN = goesByKind && !canStoreWithoutConversion(walker.types(), ArrayStorage::Double, walker.typeOf(stored));
    if (mayBeNaN && !walker.canRestartIteration())
        return staysTyped();
    ArrayStorage kindAsked = goesByKind ? ArrayStorage::Other : ArrayStorage::Contiguous;
    if (!list || !list.isValue() || !position || !stored || !walker.isInvariantInSpecializedVersion(list, purity) || !walker.iterationsPreserveArrayGuards(list, purity))
        return staysTyped();
    std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(list, position);
    if (!inside)
        return staysTyped();
    Origin origin = written.origin();
    origin.isTailCall = false;
    std::optional<HoistedArrayGuard> tested = findNoteInListReadsInLoops(walker, list);
    if (!tested) {
        HoistedArrayGuard asked { list, kindAsked, false, true, true };
        Reference test;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
            test = walker.build(questionForKindOfStorageInListReadsInLoops(walker, asked, origin));
        }
        if (!test || !walker.tryBranchInPreheader(test, true, purity))
            return staysTyped();
        walker.recordSpeculation();
        walker.recordHoistedArrayGuard();
        noteListTestedInListReadsInLoops(walker, asked);
        tested = asked;
    }
    if (!tested->isWritable || tested->storage != kindAsked || tested->admitsInt32Storage)
        return staysTyped();
    if (mayBeNaN) {
        Draft isItself(Opcode::StrictEq, origin);
        walker.chooseConservativeFields(isItself);
        isItself.setLeftOperand(stored);
        isItself.setRightOperand(stored);
        isItself.setForm(operatorFormFor(walker, isItself));
        Reference test = walker.build(WTF::move(isItself));
        if (!test)
            return true;
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
        walker.branch(test, passed, failed);
        walker.continueIn(failed);
        if (!walker.isUnreachable())
            walker.restartIteration();
        walker.continueIn(passed);
        walker.recordSpeculation();
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
    }
    Draft store(Opcode::PutContiguousIndex, origin);
    walker.chooseConservativeFields(store);
    store.setArrayOperand(list);
    store.setPositionOperand(position);
    store.setValueOperand(stored);
    store.setIsInBounds(*inside);
    if (goesByKind) {
        store.setGoesByKindOfStorage(true);
        walker.count(StatisticID::NumberStoresDispatchedByIndexingType);
        if (remembered && !walker.code().at(birth).remembersStorageAtSite())
            walker.code().letListRememberStorageAtSite(birth);
    }
    if (Reference storage = storageOfListFromInFrontOfLoop(walker, *tested, origin))
        store.setStorageOperand(storage);
    walker.build(WTF::move(store));
    Draft isStored(Opcode::Boolean, origin);
    isStored.setBoolean(true);
    walker.bind(value.id(), walker.constant(WTF::move(isStored)));
    return true;
}

static Reference storageOfListThatTurnsLeaveAsItIsInListReadsInLoops(Walker& walker, const HoistedArrayGuard& tested, Origin origin)
{
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnly;
    if (!walker.isInvariantInSpecializedVersion(tested.name, purity) || !walker.iterationsPreserveArrayGuards(tested.name, purity))
        return { };
    Draft storage(Opcode::GetArrayStorage, origin);
    walker.chooseConservativeFields(storage);
    storage.setArrayOperand(tested.name);
    walker.count(StatisticID::ArrayStorageLoadsHoisted);
    Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
    return walker.build(WTF::move(storage));
}

bool askPlainVectorBehindRowOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    if (value.opcode() == Opcode::TryPutByValDirect)
        return tryStoreWithHoistedStorageGuard(walker, value);
    std::optional<HoistedArrayGuard> tested = walker.state().answers().isEmpty() ? std::nullopt : findNoteInListReadsInLoops(walker, walker.referenceOf(value.arrayOperand()));
    if (buildPlainVectorOfNumbersInListReadsInLoops(walker, value, tested))
        return true;
    if (!tested || tested->storage != ArrayStorage::Contiguous)
        return false;
    Draft draft = walker.draftOf(value);
    Reference storage = storageOfListFromInFrontOfLoop(walker, *tested, draft.origin());
    if (!storage)
        storage = storageOfListThatTurnsLeaveAsItIsInListReadsInLoops(walker, *tested, draft.origin());
    if (!storage)
        return false;
    draft.setStorageOperand(storage);
    std::optional<PassedTest> inside = value.opcode() == Opcode::HasContiguousIndex ? walker.findWhatHoldsPositionInsideList(draft.arrayOperand(), draft.positionOperand()) : std::nullopt;
    if (inside)
        draft.setIsInBounds(*inside);
    walker.bind(value.id(), walker.build(WTF::move(draft)));
    return true;
}

bool Walker::textOfOutermostLoopStoresUnderNameInCurrentBody(LoopID outermost, PropertyKeyID name)
{
    const BodyIndex& index = currentBodyIndex();
    const Vector<uint64_t>& stored = m_namesStoredInOutermostLoops.ensure(&index, [&] {
        Vector<uint64_t> found;
        if (!index.holdsAnyOf(OpcodeSet { Opcode::PropSet, Opcode::PrivateSet }))
            return found;
        for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
            LoopID loop = index.loopOf(BlockID(block));
            while (LoopID around = loop ? index.loop(loop).parent : LoopID())
                loop = around;
            if (!loop)
                continue;
            for (BodyValue operation : index.block(BlockID(block)).values()) {
                if (operation.opcode() == Opcode::PrivateSet || (operation.opcode() == Opcode::PropSet && !operation.isFreeName()))
                    found.append(static_cast<uint64_t>(loop.index()) << 32 | keyOfAccessToSlot(program(), operation).index());
            }
        }
        std::ranges::sort(found);
        return found;
    }).iterator->value;
    return std::ranges::binary_search(stored, static_cast<uint64_t>(outermost.index()) << 32 | name.index());
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
                ValueID key = accessesByKey.contains(operation.opcode()) ? index.stripCasts(operation.propertyOperand()) : ValueID();
                if (key)
                    taken.quickSet(key.index());
            }
        }
        return taken;
    }).iterator->value;
}

bool turnTakesElementForKeyInListReadsInLoops(Walker& walker, ValueID element)
{
    const BodyIndex& index = walker.currentBodyIndex();
    return element && element.index() < index.numberOfValues() && (index.loopOf(index.blockOf(element)) || !index.hasLoops()) && walker.valuesTakenForKeysInCurrentBody().quickGet(element.index());
}

bool holeLeavesTurnBehindRow(Walker& walker, const BodyValue& read, const HoistedArrayGuard& tested)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!isNumericStorage(tested.storage))
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
    if (!walker.canBuildIterationGuard())
        return false;
    std::optional<HoistedArrayGuard> tested = findHoistedArrayGuard(walker, read);
    return tested && holeLeavesTurnBehindRow(walker, read, *tested);
}

static bool isProvenArray(const Walker& walker, Reference name)
{
    return name && name.isValue() && hasKind(walker.types(), walker.ownTypeOf(name), OperandKind::Array);
}

bool isFreshAllocation(Walker& walker, Reference name)
{
    if (!name || !name.isValue())
        return false;
    ValueID cell = underlyingCellOf(walker.code(), name.id());
    return !walker.code().isConstant(cell) && walker.state().isFresh(Reference::value(cell));
}

static bool isWrittenEmptyInListReadsInLoops(Walker& walker, const BodyIndex& index, ValueID list)
{
    ValueID written = list ? index.stripCasts(list) : ValueID();
    if (!written || index.isParameter(written))
        return false;
    if (index.opcodeOf(written) == Opcode::NewArray)
        return index.value(written).as<Opcode::NewArray>().holes().empty() && index.value(written).as<Opcode::NewArray>().elementsOperands().empty();
    if (index.opcodeOf(written) != Opcode::Data)
        return false;
    const auto* data = walker.program().at(index.value(written).as<Opcode::Data>().data()).root.tryAs<DataNode::Array>();
    return data && !data->elements.size();
}

static bool isBornEmptyInListReadsInLoops(Walker& walker, const BodyIndex& index, ValueID list, Reference name)
{
    if (isWrittenEmptyInListReadsInLoops(walker, index, list))
        return true;
    if (!name.isValue())
        return false;
    const auto& born = walker.code().at(underlyingCellOf(walker.code(), name.id()));
    return born.opcode == Opcode::NewArray && born.holes().empty() && born.elementsOperands().empty();
}

static ArrayStorage predictedArrayStorage(Walker&, TypeID list);

static constexpr OpcodeSet numberConstantOpcodes { Opcode::Double, Opcode::Integer };
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

bool isExpectedToBeView(Walker& walker, ValueID unbuiltList)
{
    TypeID expected = expectedOfRowThatIsNotBuiltInListReadsInLoops(walker, unbuiltList);
    return expected && !!classOfViewInListReadsInLoops(walker, walker.types().withoutNullish(expected));
}

namespace {

template<typename Entry>
class EntriesByName {
public:
    static constexpr size_t linearSearchLimit = 8;

    EntriesByName(std::span<const Entry> entries, Reference Entry::* name)
        : m_entries(entries)
        , m_name(name)
    {
        if (entries.size() <= linearSearchLimit) [[likely]]
            return;
        m_namesAndPlaces = Vector<uint64_t>(entries.size(), [&](size_t place) { return static_cast<uint64_t>((entries[place].*name).bits()) << 32 | place; });
        std::ranges::sort(m_namesAndPlaces);
    }

    template<typename Functor>
    bool anyOf(Reference name, const Functor& functor) const
    {
        if (m_namesAndPlaces.isEmpty()) [[likely]] {
            for (size_t place = 0; place < m_entries.size(); ++place) {
                if (m_entries[place].*m_name == name && functor(place))
                    return true;
            }
            return false;
        }
        for (uint64_t listed : std::ranges::equal_range(m_namesAndPlaces, name.bits(), { }, [](uint64_t listed) { return static_cast<uint32_t>(listed >> 32); })) {
            if (functor(static_cast<size_t>(static_cast<uint32_t>(listed))))
                return true;
        }
        return false;
    }

private:
    std::span<const Entry> m_entries;
    Reference Entry::* m_name;
    Vector<uint64_t> m_namesAndPlaces;
};

} // anonymous namespace

static ALWAYS_INLINE bool readsRowOfNumbersThatTurnFindsInListedList(Walker& walker, const BodyIndex& index, const BodyValue& operation, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses = { })
{
    ValueID row = operation.opcode() == Opcode::ElemGet ? index.stripCasts(operation.objectOperand()) : ValueID();
    if (!row || row.index() < firstValueOfLoop || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet)
        return false;
    ValueID holder = valueThatEntersLoopInListReadsInLoops(index, index.value(row).objectOperand(), firstValueOfLoop);
    Reference name = holder ? nameThatEntersLoopInListReadsInLoops(walker, index, holder, firstValueOfLoop) : Reference();
    bool isListed = name && std::ranges::any_of(lists, [&](const HoistedArrayGuard& list) { return list.name == name; });
    if (!isListed && isProvenArray(walker, name) && isFreshAllocation(walker, name)) {
        TypeID expected = expectedOfRowThatIsNotBuiltInListReadsInLoops(walker, row);
        return expected && isNumericStorage(predictedArrayStorage(walker, walker.types().withoutNullish(expected)));
    }
    if (!isListed)
        return false;
    TypeArena& types = walker.types();
    TypeID list = walker.ownTypeOf(name);
    for (const HoistedSpeculation& guess : guesses) {
        if (guess.value == name)
            list = types.intersectionOf(list, guess.tested);
    }
    const ClassPart* part = list && !holdsOnlyPrimitives(list) ? singleBuiltinClassOf(types, types.at(list)) : nullptr;
    if (!part || part->builtinClass != BuiltinClasses::Array)
        return false;
    TypeID rows = part->expectedContentsAt(walker.program().builtins(), ContentsSlot::Element).predicted();
    if (rows && isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(rows))))
        return true;
    TypeID expected = expectedOfRowThatIsNotBuiltInListReadsInLoops(walker, row);
    return expected && isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(expected)));
}

bool turnsReadRowsOfNumbersThatTheyFind(Walker& walker, BlockID inputHead, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses)
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

static bool turnMayAppendToTextInListReadsInLoops(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses, std::span<const Reference> proofs)
{
    if (!canKeepStringsInBuilders(walker))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    WhatReadsFromInFrontOfLoopFind foundFromInFront;
    BitVector numbersMadeOfNumbers;
    EntriesByName<HoistedSpeculation> provenSpeculations(guesses.first(std::min(guesses.size(), proofs.size())), &HoistedSpeculation::value);
    EntriesByName<HoistedArrayGuard> listsByName(lists, &HoistedArrayGuard::name);
    auto isNumericArrayAt = [&](size_t place) { return isNumericStorage(lists[place].storage); };
    auto isNumberBehindTestsOfLists = [&](ValueID operand) {
        ValueID root = index.stripCasts(operand);
        if (!index.isParameter(root) && numberConstantOpcodes.contains(index.opcodeOf(root)))
            return true;
        if (root.index() >= firstValueOfLoop && numbersMadeOfNumbers.get(root.index() - firstValueOfLoop))
            return true;
        if (root.index() >= firstValueOfLoop && !index.isParameter(root) && index.opcodeOf(root) == Opcode::ElemGet) {
            ValueID entersLoop = valueThatEntersLoopInListReadsInLoops(index, index.value(root).objectOperand(), firstValueOfLoop);
            Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, entersLoop, firstValueOfLoop);
            Reference proof;
            if (name) {
                provenSpeculations.anyOf(name, [&](size_t place) {
                    proof = proofs[place];
                    return false;
                });
            }
            if (name && (listsByName.anyOf(name, isNumericArrayAt) || (proof && listsByName.anyOf(proof, isNumericArrayAt))))
                return true;
            TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), root);
            if (seenByLink && !isNeverType(seenByLink) && walker.types().at(seenByLink).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined))
                return true;
            if (readsRowOfNumbersThatTurnFindsInListedList(walker, index, index.value(root), firstValueOfLoop, lists))
                return true;
            size_t placeOfList = entersLoop.index() - preview.firstValue;
            TypeID listInRow = entersLoop.index() >= preview.firstValue && placeOfList < preview.typesOfValues.size() ? preview.typesOfValues[placeOfList] : TypeID();
            if (listInRow && readsListFoundByReadFromInFront(walker.program(), index, index.value(root), firstValueOfLoop, foundFromInFront) && isNumericStorage(predictedArrayStorage(walker, walker.types().withoutNullish(listInRow))))
                return true;
        }
        ValueID step = root.index() >= firstValueOfLoop && !index.isParameter(root) && index.opcodeOf(root) == Opcode::ResultAt ? index.value(root).operand(0) : ValueID();
        if (step && !index.isParameter(step) && stepsViewByIndexAlone(walker, index, index.value(step)))
            return true;
        if (const Walker::IndexedIteration* stepped = step && !index.isParameter(step) ? findIndexedArrayIteration(walker, index, index.value(step)) : nullptr) {
            if (listsByName.anyOf(stepped->list, isNumericArrayAt))
                return true;
        }
        size_t place = operand.index() - preview.firstValue;
        TypeID known = operand.index() < firstValueOfLoop ? walker.ownTypeOf(walker.referenceOf(operand)) : operand.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
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

static constexpr OpcodeSet readsThatMayFindSameInEveryTurn { Opcode::PropGet, Opcode::ElemGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture, Opcode::PrivateGet, Opcode::Call };

static bool takesOnlyWhatReadsFromInFrontFindInListReadsInLoops(const BodyIndex& index, const BodyValue& read, uint32_t firstValueOfLoop, WhatReadsFromInFrontOfLoopFind& known)
{
    if (read.opcode() == Opcode::PropGet && read.isFreeName())
        return false;
    if (known.found.get(read.id().index() - firstValueOfLoop))
        return true;
    if (known.refused.get(read.id().index() - firstValueOfLoop))
        return false;
    bool takesOnlyThat = true;
    read.forEachOperand([&](ValueID operand, uint32_t) {
        if (!takesOnlyThat || operand.index() < firstValueOfLoop)
            return;
        ValueID source = index.stripCasts(operand);
        if ((read.opcode() == Opcode::PropGet || read.opcode() == Opcode::ElemGet || read.opcode() == Opcode::PrivateGet || read.opcode() == Opcode::Call) && source && source.index() >= firstValueOfLoop && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
            source = index.stripCasts(index.value(source).operand(0));
        if (!source || source.index() < firstValueOfLoop) {
            takesOnlyThat = !!source;
            return;
        }
        if (index.isParameter(source)) {
            takesOnlyThat = false;
            return;
        }
        Opcode made = index.opcodeOf(source);
        takesOnlyThat = numberConstantOpcodes.contains(made) || made == Opcode::String
            || ((readsThatMayFindSameInEveryTurn.contains(made) || operatorsThatMakeNumbersOfNumbers.contains(made)) && takesOnlyWhatReadsFromInFrontFindInListReadsInLoops(index, index.value(source), firstValueOfLoop, known));
    });
    (takesOnlyThat ? known.found : known.refused).set(read.id().index() - firstValueOfLoop);
    return takesOnlyThat;
}

bool readsListFoundByReadFromInFront(const Program& program, const BodyIndex& index, const BodyValue& operation, uint32_t firstValueOfLoop, WhatReadsFromInFrontOfLoopFind& known)
{
    bool readsList = operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet || (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == program.keyOf(BuiltinNames::length));
    if (!readsList)
        return false;
    ValueID source = index.stripCasts(operation.objectOperand());
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
        source = index.stripCasts(index.value(source).operand(0));
    if (!source || source.index() < firstValueOfLoop || index.isParameter(source) || !readsThatMayFindSameInEveryTurn.contains(index.opcodeOf(source)))
        return false;
    return takesOnlyWhatReadsFromInFrontFindInListReadsInLoops(index, index.value(source), firstValueOfLoop, known);
}

bool hasOnlyOtherArrayReads(Walker& walker, BlockID inputHead, ValueID made)
{
    const Code& code = walker.code();
    bool readsAnother = false;
    for (uint64_t pair : walker.holdersFromInFrontThatLoopReadsUnderIndexes(inputHead)) {
        Reference read = walker.referenceOf(ValueID(static_cast<uint32_t>(pair)));
        if (!read || !read.isValue())
            continue;
        if (underlyingCellOf(code, read.id()) == made)
            return false;
        readsAnother = true;
    }
    return readsAnother;
}

static bool someListedListIsReceiverOfMethodInListReadsInLoops(Walker& walker, BlockID inputHead, std::span<const HoistedArrayGuard> lists)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Code& code = walker.code();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::PropGet || operation.isFreeName() || operation.key() == walker.program().keyOf(BuiltinNames::length))
                continue;
            ValueID input = index.stripCasts(operation.objectOperand());
            Reference receiver = input.index() < firstValueOfLoop ? walker.referenceOf(input) : Reference();
            if (!receiver || !receiver.isValue())
                continue;
            ValueID root = underlyingCellOf(code, receiver.id());
            if (std::ranges::any_of(lists, [&](const HoistedArrayGuard& list) { return list.name.isValue() && underlyingCellOf(code, list.name.id()) == root; }))
                return true;
        }
    }
    return false;
}

template<typename TypeInLook>
static const LinkedEffectsGivenArguments* findEffectsGivenArgumentsInListReadsInLoops(Walker& walker, const BodyValue& operation, FunctionID callee, const TypeInLook& typeInLook)
{
    const LinkTables& linked = walker.facts().linked();
    const LinkedEffectsGivenArguments* given = linked.known.findEffectsGivenArguments(callee);
    if (!given || given->parametersCalled)
        return nullptr;
    TypeArena& types = walker.types();
    auto call = operation.as<Opcode::Call>();
    auto arguments = call.argumentsOperands();
    const Function& function = walker.program().at(callee);
    std::span<const LinkedParameter> parameters = linked.parametersOf(callee);
    for (uint32_t parameter = 0; parameter < 32; ++parameter) {
        if (!(given->parametersTakenAsExpected >> parameter & 1))
            continue;
        TypeID expected = parameter < parameters.size() ? parameters[parameter].writtenOrSeenToArrive() : TypeID();
        if (!expected)
            return nullptr;
        ValueID argument;
        EntryParameter kind = function.entryParameterKind(parameter);
        if (kind == EntryParameter::This)
            argument = call.thisValueOperand();
        else if (uint32_t position = parameter - function.firstParameterIndex(); kind == EntryParameter::Parameter && position < arguments.size())
            argument = arguments[position];
        if (!argument)
            return nullptr;
        TypeID arriving = typeInLook(argument);
        if (arriving && !types.overlap(arriving, expected))
            return nullptr;
    }
    return given;
}

template<typename TypeInLook>
static bool callLeavesListsAsTheyAreGivenArguments(Walker& walker, const BodyValue& operation, const TypeInLook& typeInLook)
{
    if (operation.opcode() != Opcode::Call)
        return false;
    FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), operation.id());
    const LinkedEffectsGivenArguments* given = callee ? findEffectsGivenArgumentsInListReadsInLoops(walker, operation, callee, typeInLook) : nullptr;
    return given && preservesHoistedArrayGuards(walker, given->onReturn) && preservesHoistedArrayGuards(walker, given->onThrow);
}

static bool rowFindsListsLeftAsTheyAreInListReadsInLoops(Walker& walker, BlockID inputHead, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses, std::span<const Reference> proofs)
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
    WhatReadsFromInFrontOfLoopFind foundFromInFront;
    EntriesByName<HoistedSpeculation> provenSpeculations(guesses.first(std::min(guesses.size(), proofs.size())), &HoistedSpeculation::value);
    EntriesByName<HoistedArrayGuard> listsByName(lists, &HoistedArrayGuard::name);
    auto isThere = [](size_t) { return true; };
    bool sitesMayAnswer = !walker.summaryHasUninlinedCall() && someReadThatAsksItsSiteIsReckonedWith(walker, walker.operationsThatRanOtherCodeOfRow(), TypesOfInputs(typeInRow));
    for (ValueID ran : walker.operationsThatRanOtherCodeOfRow()) {
        if (!ran)
            return false;
        BodyValue operation = index.value(ran);
        if (stepsViewByIndexAlone(walker, index, operation) || stepsTextByIndexAlone(walker, index, operation) || callLeavesListsAsTheyAreGivenArguments(walker, operation, typeInRow))
            continue;
        if (operation.opcode() == Opcode::ElemGet && walker.standsInCopyOfLoopBuiltForEachMember() && rowFoundUnderKeyInFrontOfLoop(walker, operation))
            continue;
        bool readsList = false;
        bool isStore = operation.opcode() == Opcode::ElemSet;
        if (operation.opcode() == Opcode::ElemGet || isStore || (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == walker.program().keyOf(BuiltinNames::length))) {
            ValueID entersLoop = valueThatEntersLoopInListReadsInLoops(index, operation.objectOperand(), firstValueOfLoop);
            Reference name = entersLoop ? walker.referenceOf(entersLoop) : Reference();
            if (Reference readInFront = entersLoop && entersLoop.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, entersLoop, firstValueOfLoop) : Reference())
                name = readInFront;
            Reference proof;
            if (name) {
                provenSpeculations.anyOf(name, [&](size_t place) {
                    proof = proofs[place];
                    return false;
                });
            }
            auto serves = [&](size_t place) { return !isStore || lists[place].isWritable; };
            readsList = name && (listsByName.anyOf(name, serves) || (proof && listsByName.anyOf(proof, serves)));
        } else if (const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, operation, true))
            readsList = listsByName.anyOf(stepped->list, isThere);
        else if (const Walker::IndexedIteration* stepped = findDestructuredEntriesIteration(walker, index, operation))
            readsList = listsByName.anyOf(stepped->list, isThere);
        else if (ValueID step = operation.opcode() == Opcode::ResultAt ? operation.as<Opcode::ResultAt>().resultsOperand() : ValueID(); step && !index.isParameter(step)) {
            if (const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, index.value(step), true))
                readsList = listsByName.anyOf(stepped->list, isThere);
        }
        if (readsList || effectRuleOf(operation.opcode()) == EffectRule::Operator || readsListFoundByReadFromInFront(walker.program(), index, operation, firstValueOfLoop, foundFromInFront) || readsRowOfNumbersThatTurnFindsInListedList(walker, index, operation, firstValueOfLoop, lists))
            continue;
        if (!walker.looksAskOnlyWhetherTurnsRunOtherCode(inputHead) || !guessesMayTypeOperationInCleanTurn(walker, operation, TypesOfInputs(typeInRow), sitesMayAnswer))
            return false;
    }
    if (walker.previewOutOfRow() && turnMayAppendToTextInListReadsInLoops(walker, inputHead, *walker.previewOutOfRow(), lists, guesses, proofs))
        return false;
    if (!preservesHoistedArrayGuards(walker, walker.effectsOfQuietDraftsOfRow()) && someListedListIsReceiverOfMethodInListReadsInLoops(walker, inputHead, lists))
        return false;
    return preservesHoistedArrayGuards(walker, walker.effectsOfQuietDraftsOfRow(), lists, inputHead);
}

void NamesBeforeLoop::collectNames()
{
    const BodyIndex& index = m_walker.currentBodyIndex();
    uint32_t firstValueOfLoop = index.block(m_inputHead).firstParameter().index();
    BitVector isCollected;
    auto mention = [&](ValueID operand) {
        if (!operand || operand.index() >= firstValueOfLoop || isCollected.get(operand.index()))
            return;
        isCollected.set(operand.index());
        if (Reference value = m_walker.referenceOf(operand))
            m_names.append({ value.bits(), operand });
    };
    for (BlockID member : index.membersOf(index.loopOf(m_inputHead))) {
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
    std::ranges::stable_sort(m_names, { }, &Name::value);
}

Vector<ValueID, 2> NamesBeforeLoop::findNamesOf(Reference value)
{
    if (!std::exchange(m_areNamesCollected, true))
        collectNames();
    Vector<ValueID, 2> names;
    for (const Name& name : std::ranges::equal_range(m_names, value.bits(), { }, &Name::value)) {
        if (m_walker.referenceOf(name.id) == value)
            names.append(name.id);
    }
    return names;
}

static ArrayStorage predictedArrayStorage(Walker& walker, TypeID list)
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

static bool elementIsOnlyReadByKeyInListReadsInLoops(const BodyIndex& index, const BodyValue& read)
{
    if (read.opcode() != Opcode::ElemGet)
        return false;
    ValueID user = index.onlyUsingValue(read.id());
    return user && index.opcodeOf(user) == Opcode::ElemGet && index.value(user).objectOperand() == read.id();
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
    if (body->hasLoops() && !sizeOfCopyThatWritesOutLoopsOverListsHandedIn(walker, callee) && !turnsOfCopyWrittenOutAt(walker, site, callee, place) && !canInlineSingleCallLoop(walker, callee, place, nullptr))
        return false;
    return mayCopyCallee(walker, site, callee, place);
}

static Reference buildIntegerMinimum(Walker& walker, Reference one, Reference other, Origin origin)
{
    Reference isLess = buildIntegerOperator(walker, Opcode::Less, one, other, origin);
    if (!isLess || !isLess.isValue() || walker.isUnreachable())
        return { };
    BlockID takesOne = walker.addBlock();
    BlockID takesOther = walker.addBlock();
    walker.branch(isLess, takesOne, takesOther);
    Vector<Walker::JoinArm, 2> arms;
    arms.append(Walker::JoinArm { takesOne, { one } });
    arms.append(Walker::JoinArm { takesOther, { other } });
    Reference lesser;
    return walker.joinArms(arms.span(), walker.addBlock(), singleElementSpan(lesser)) ? lesser : Reference();
}

bool isResultUnused(const BodyIndex& index, ValueID result)
{
    while (!index.isUnused(result)) {
        ValueID user = index.onlyUsingValue(result);
        if (!user || !opcodesThatStateTypes.contains(index.opcodeOf(user)))
            return false;
        result = user;
    }
    return true;
}

static Reference bearLiteralAgainBehindItsRoom(Walker& walker, const BodyIndex& index, ValueID written, Reference list, BlockID inputHead)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !written || index.isParameter(written) || index.opcodeOf(written) != Opcode::NewArray || !list.isValue() || walker.referenceBoundTo(written) != list)
        return { };
    const Operation& born = walker.code().at(list.id());
    if (born.opcode != Opcode::NewArray || born.storage() != AllocationStorage::Heap || born.literalStorage() != ArrayLiteralStorage::Fresh || !born.holes().empty() || born.elementsOperands().empty())
        return { };
    BlockID home = index.blockOf(written);
    BodyTerminator end = index.terminator(home);
    if (home.index() >= inputHead.index() || end.kind() != TerminatorKind::Jump || end.targets()[0].block() != inputHead)
        return { };
    Vector<ValueID, 2> casts;
    ValueID named = written;
    std::optional<BodyUser> first = index.firstUser(named);
    while (first && first->kind == BodyUseKind::Operand && index.hasOneUse(named) && index.blockOf(first->value) == home && opcodesThatStateTypes.contains(index.opcodeOf(first->value)) && walker.referenceBoundTo(first->value) == list) {
        named = first->value;
        casts.append(named);
        first = index.firstUser(named);
    }
    if (!first || (first->kind == BodyUseKind::Operand ? index.blockOf(first->value).index() : first->block.index()) < inputHead.index())
        return { };
    buildFreshObject(walker, index.value(written));
    Reference again = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(written);
    if (!again || !again.isValue() || again == list || walker.code().at(again.id()).opcode != Opcode::NewArray)
        return { };
    for (ValueID cast : casts)
        walker.bind(cast, again);
    walker.count(StatisticID::ArrayLiteralAllocationsSunk);
    return again;
}

static bool isMergeOfComputedNumbers(const BodyIndex& index, LoopID loop, BlockID inputHead, ValueID counter, ValueID stored)
{
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    BitVector reckoned;
    BitVector meetsSomethingElse;
    Vector<std::pair<BlockID, uint32_t>, 8> edgesSeenInto;
    auto isReckoned = [&](ValueID value) {
        value = value ? index.stripCasts(value) : ValueID();
        if (!value)
            return false;
        if (value.index() < firstValueOfLoop)
            return !index.isParameter(value) && numberConstantOpcodes.contains(index.opcodeOf(value));
        return value == counter || reckoned.get(value.index() - firstValueOfLoop);
    };
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        size_t seen = edgesSeenInto.findIf([&](const auto& known) { return known.first == member; });
        bool everyEdgeWasSeen = seen != notFound && !index.isLoopHead(member) && !index.isEnteredByHandlerEdge(member) && edgesSeenInto[seen].second == index.numberOfIncomingEdges(member);
        for (uint32_t position = 0; everyEdgeWasSeen && position < block.numberOfParameters(); ++position) {
            if (!meetsSomethingElse.get(block.parameter(position).index() - firstValueOfLoop))
                reckoned.set(block.parameter(position).index() - firstValueOfLoop);
        }
        for (BodyValue operation : block.values()) {
            Opcode opcode = operation.opcode();
            if (numberConstantOpcodes.contains(opcode) || numberProducingOperators.contains(opcode) || allBitwiseOperators.contains(opcode) || (opcode == Opcode::Add && (isReckoned(operation.operand(0)) || isReckoned(operation.operand(1)))))
                reckoned.set(operation.id().index() - firstValueOfLoop);
        }
        for (BodyTarget target : index.terminator(member).targets()) {
            if (target.block().index() <= member.index() || !index.isInLoop(target.block(), loop))
                continue;
            size_t place = edgesSeenInto.findIf([&](const auto& known) { return known.first == target.block(); });
            if (place == notFound)
                edgesSeenInto.append({ target.block(), 1 });
            else
                ++edgesSeenInto[place].second;
            BodyBlock reached = index.block(target.block());
            for (uint32_t position = 0; position < reached.numberOfParameters(); ++position) {
                if (position >= target.arguments().size() || !isReckoned(target.arguments()[position]))
                    meetsSomethingElse.set(reached.parameter(position).index() - firstValueOfLoop);
            }
        }
    }
    stored = index.stripCasts(stored);
    return stored && index.isParameter(stored) && stored != counter && isReckoned(stored);
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
    bool hasExactTripCount = true;
    if (!condition || index.isParameter(condition) || (index.opcodeOf(condition) != Opcode::Less && index.opcodeOf(condition) != Opcode::LessEq) || index.blockOf(condition) != inputHead)
        return;
    ValueID counter = index.stripCasts(index.value(condition).operand(0));
    ValueID bound = index.stripCasts(index.value(condition).operand(1));
    bool boundIsWritten = !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::Integer;
    Reference textWhoseLengthIsBound;
    if (bound.index() >= firstValueOfLoop && !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::PropGet && !index.value(bound).isFreeName() && index.value(bound).key() == walker.program().keyOf(BuiltinNames::length)) {
        ValueID text = index.stripCasts(index.value(bound).objectOperand());
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
    hasExactTripCount &= first.integers == Range::one(0);
    Range initialCounterRange = first.integers;

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
            ValueID next = placeOfCounter < target.arguments().size() ? index.stripCasts(target.arguments()[placeOfCounter]) : ValueID();
            if (!next || index.isParameter(next) || index.opcodeOf(next) != Opcode::Increment || index.stripCasts(index.value(next).operand(0)) != counter)
                return;
            blocksThatComeRound.append(member);
        }
    }

    auto listWhoseLengthIsRead = [&](ValueID value) -> ValueID {
        if (index.isParameter(value) || index.opcodeOf(value) != Opcode::PropGet || index.value(value).isFreeName() || index.value(value).key() != walker.program().keyOf(BuiltinNames::length))
            return { };
        ValueID input = index.stripCasts(index.value(value).objectOperand());
        return input.index() < firstValueOfLoop && isProvenArray(walker, walker.referenceOf(input)) ? input : ValueID();
    };
    const BuiltinTable& table = walker.program().builtins();
    auto listWhosePushIsRead = [&](ValueID value) -> ValueID {
        if (index.isParameter(value) || index.opcodeOf(value) != Opcode::PropGet || index.value(value).isFreeName())
            return { };
        ValueID input = index.stripCasts(index.value(value).objectOperand());
        Reference list = input.index() < firstValueOfLoop ? walker.referenceOf(input) : Reference();
        if (!isProvenArray(walker, list))
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
        ValueID read = index.stripCasts(index.value(value).operand(0));
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
        if (!type.hasOnlyPrimitives(Primitive::AnyString) || !built.isRecord() || isStringInBuilder(walker, built))
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
        value = index.stripCasts(value);
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
    Vector<ValueID, 2> namesOfLists;
    Vector<Reference, 2> listsFilledTwiceInATurn;
    Vector<Reference, 2> listsPushedOnto;
    Vector<std::pair<Reference, ValueID>, 2> pushesWithUnusedResult;
    Vector<std::pair<ValueID, ValueID>, 2> invariantArrayReads;
    Vector<std::pair<Reference, ValueID>, 2> parametersStoredUnderCounter;
    ValueID listReadUnderCounter;
    bool everyStoreStandsBehindTestOfHead = true;
    for (BlockID member : index.membersOf(loop)) {
        bool dominatesBackEdges = std::ranges::all_of(blocksThatComeRound, [&](BlockID last) { return index.dominates(member, last); });
        bool standsBehindTestOfHead = member != inputHead && test.taken().block() != inputHead && index.dominates(test.taken().block(), member);
        for (BodyValue operation : index.block(member).values()) {
            if (listWhoseLengthIsRead(operation.id()))
                continue;
            if (listWhosePushIsRead(operation.id()))
                continue;
            if (operation.opcode() == Opcode::Call) {
                auto call = operation.as<Opcode::Call>();
                ValueID input = listWhosePushIsRead(index.stripCasts(call.calleeOperand()));
                if (!dominatesBackEdges || !input || index.stripCasts(call.thisValueOperand()) != input || call.argumentsOperands().size() != 1) {
                    hasExactTripCount = false;
                    continue;
                }
                if (!isPlainPrimitive(isPlainPrimitive, call.argumentsOperands()[0], 6))
                    hasExactTripCount = false;
                everyStoreStandsBehindTestOfHead &= standsBehindTestOfHead;
                Reference list = walker.referenceOf(input);
                if (!lists.contains(list)) {
                    lists.append(list);
                    namesOfLists.append(input);
                } else
                    listsFilledTwiceInATurn.append(list);
                if (!listsPushedOnto.contains(list))
                    listsPushedOnto.append(list);
                if (isResultUnused(index, operation.id()) && standsBehindTestOfHead && index.loopOf(member) == loop)
                    pushesWithUnusedResult.append({ list, operation.id() });
                continue;
            }
            if (operation.opcode() == Opcode::ElemSet) {
                ValueID input = index.stripCasts(operation.objectOperand());
                ValueID position = index.stripCasts(operation.propertyOperand());
                Reference list = input.index() < firstValueOfLoop ? walker.referenceOf(input) : Reference();
                if (!dominatesBackEdges || (position != counter && (position.index() < firstValueOfLoop || listWhoseLengthIsRead(position) != input)) || !isProvenArray(walker, list)) {
                    hasExactTripCount = false;
                    continue;
                }
                everyStoreStandsBehindTestOfHead &= standsBehindTestOfHead;
                if (position != counter && !listsPushedOnto.contains(list))
                    listsPushedOnto.append(list);
                if (position == counter && index.isParameter(index.stripCasts(operation.valueOperand())))
                    parametersStoredUnderCounter.append({ list, operation.valueOperand() });
                if (!lists.contains(list)) {
                    lists.append(list);
                    namesOfLists.append(input);
                } else
                    listsFilledTwiceInATurn.append(list);
                continue;
            }
            if (operation.opcode() == Opcode::ElemGet && dominatesBackEdges && !listReadUnderCounter && index.stripCasts(operation.propertyOperand()) == counter && index.stripCasts(operation.objectOperand()).index() < firstValueOfLoop)
                listReadUnderCounter = index.stripCasts(operation.objectOperand());
            if (ValueID read = operation.opcode() == Opcode::ElemGet ? index.stripCasts(operation.objectOperand()) : ValueID(); read && read.index() < firstValueOfLoop && !invariantArrayReads.containsIf([&](const auto& known) { return known.first == read; }))
                invariantArrayReads.append({ read, operation.id() });
            Effects declared = declaredEffectsOfOperation(walker.program(), operation.opcode(), operation);
            if (!declared.mayThrow.byRule() && !declared.mayRunOtherCode() && !operandsMayRunCode(operation.opcode())) {
                if (!declared.writes.containsOnly(unobservableWriteHeaps))
                    hasExactTripCount = false;
                continue;
            }
            if (!hasExactTripCount)
                continue;
            if (ValueID read = nameThatIsChecked(operation.id())) {
                if (!readInFront(read))
                    return;
                continue;
            }
            if (effectRuleOf(operation.opcode()) != EffectRule::Operator || !isPlainPrimitive(isPlainPrimitive, operation.id(), 6))
                hasExactTripCount = false;
        }
    }
    if (lists.isEmpty())
        return;
    if (lengthJoinedOfTheRest > JSString::MaxLength || timesTextsAreJoined > JSString::MaxLength)
        hasExactTripCount = false;

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
        room = buildIntegerOperator(walker, Opcode::Add, room, integerWritten(1), origin);
        if (!room)
            return;
    }
    if (!hasExactTripCount) {
        if (!types.at(walker.typeOf(room)).isInteger())
            return;
        namesAskedInFront.shrink(0);
        textsJoined.shrink(0);
    }
    Reference roomByCount = room;
    auto roomOfLiteralFilledFromItsEnd = [&](Reference list) -> Reference {
        const Operation& born = walker.code().at(underlyingCellOf(walker.code(), list.id()));
        if (born.opcode != Opcode::NewArray || born.storage() != AllocationStorage::Heap || !born.holes().empty() || born.elementsOperands().empty())
            return { };
        int64_t written = born.elementsOperands().size();
        Reference filled = roomByCount;
        if (listsPushedOnto.contains(list)) {
            Reference turnsLeft = buildIntegerOperator(walker, Opcode::Sub, roomByCount, entering[placeOfCounter], origin);
            filled = turnsLeft ? buildIntegerOperator(walker, Opcode::Add, turnsLeft, integerWritten(written), origin) : Reference();
        } else if (initialCounterRange != Range::one(written))
            return { };
        if (!filled || !types.at(walker.typeOf(filled)).isInteger())
            return { };
        Range asked = types.at(walker.typeOf(filled)).integers;
        if (asked.maximum <= BASE_CONTIGUOUS_VECTOR_LEN_MAX)
            return filled;
        return asked.minimum > BASE_CONTIGUOUS_VECTOR_LEN_MAX ? integerWritten(BASE_CONTIGUOUS_VECTOR_LEN_MAX) : buildIntegerMinimum(walker, filled, integerWritten(BASE_CONTIGUOUS_VECTOR_LEN_MAX), origin);
    };
    auto behindForms = [&](ValueID value) {
        while (conversionsOfForm.contains(walker.code().at(value).opcode))
            value = walker.code().at(value).valueOperand();
        return value;
    };
    for (size_t place = 0; place < lists.size(); ++place) {
        Reference list = lists[place];
        if (!hasExactTripCount) {
            room = roomOfLiteralFilledFromItsEnd(list);
            if (walker.isUnreachable())
                return;
            if (!room)
                continue;
            walker.count(StatisticID::ClampedCapacitiesReserved);
        } else
            room = roomByCount;
        const Operation& born = walker.code().at(underlyingCellOf(walker.code(), list.id()));
        bool isBornWithLength = born.opcode == Opcode::NewArrayLike || born.opcode == Opcode::NewContiguousArray;
        if (isBornWithLength && room.isValue() && behindForms(born.lengthOperand()) == behindForms(room.id()))
            continue;
        if (Type asked = types.at(walker.typeOf(room)); isBornWithLength && asked.isInteger() && asked.integers.isSingleValue() && types.at(walker.code().at(born.lengthOperand()).type).isInteger() && types.at(walker.code().at(born.lengthOperand()).type).integers == asked.integers)
            continue;
        bool hasMergedNumberStore = false;
        bool hasPushesAndArithmeticUsers = false;
        bool hasCapacityForSpecializedVersion = hasExactTripCount || types.at(walker.typeOf(roomByCount)).integers.maximum <= BASE_CONTIGUOUS_VECTOR_LEN_MAX || (listReadUnderCounter && listReadUnderCounter != namesOfLists[place]);
        if (namesAskedInFront.isEmpty() && textsJoined.isEmpty() && types.at(walker.typeOf(room)).isInteger() && !listsFilledTwiceInATurn.contains(list)) {
            if (hasExactTripCount && listsPushedOnto.contains(list) && born.opcode == Opcode::NewArray) {
                Reference turnsLeft = buildIntegerOperator(walker, Opcode::Sub, room, entering[placeOfCounter], origin);
                Reference filled = turnsLeft ? buildIntegerOperator(walker, Opcode::Add, turnsLeft, integerWritten(static_cast<int64_t>(born.elementsOperands().size())), origin) : Reference();
                if (walker.isUnreachable())
                    return;
                if (filled && types.at(walker.typeOf(filled)).isInteger())
                    room = filled;
            }
            size_t placeOfRead = invariantArrayReads.findIf([&](const auto& known) { return index.stripCasts(known.first) == index.stripCasts(namesOfLists[place]); });
            TypeID seenByLink = placeOfRead != notFound && !listsPushedOnto.contains(list) && born.opcode == Opcode::NewArray && initialCounterRange == Range::one(static_cast<int64_t>(born.elementsOperands().size())) ? walker.facts().linked().resultOfReadByKey(walker.currentFunction(), invariantArrayReads[placeOfRead].second) : TypeID();
            bool isNumeric = seenByLink && (types.at(seenByLink).primitives & Primitive::Number) && types.at(seenByLink).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined | Primitive::Absent);
            size_t placeOfChoice = parametersStoredUnderCounter.findIf([&](const auto& known) { return known.first == lists[place]; });
            hasMergedNumberStore = !isNumeric && hasCapacityForSpecializedVersion && placeOfChoice != notFound && !listsPushedOnto.contains(list) && born.opcode == Opcode::NewArray && initialCounterRange == Range::one(static_cast<int64_t>(born.elementsOperands().size())) && isMergeOfComputedNumbers(index, loop, inputHead, counter, parametersStoredUnderCounter[placeOfChoice].second);
            hasPushesAndArithmeticUsers = !isNumeric && hasCapacityForSpecializedVersion && placeOfRead != notFound && born.opcode == Opcode::NewArray && initialCounterRange == Range::one(static_cast<int64_t>(born.elementsOperands().size()))
                && pushesWithUnusedResult.containsIf([&](const auto& push) { return push.first == lists[place]; }) && isElementUsedInArithmetic(walker, index.value(invariantArrayReads[placeOfRead].second))
                && std::ranges::all_of(invariantArrayReads, [&](const auto& known) {
                       Reference holder = walker.referenceOf(known.first);
                       return isProvenArray(walker, holder) || types.at(walker.typeOf(holder)).hasOnlyPrimitives(Primitive::AnyString);
                   });
            SetForScope requestedLiteralStorageScope(walker.requestedLiteralStorage(), hasMergedNumberStore || hasPushesAndArithmeticUsers ? ArrayStorage::Double : !isNumeric ? ArrayStorage::Other : types.at(seenByLink).isInt32OrNullish() ? ArrayStorage::Int32 : types.at(seenByLink).primitives & Primitive::FractionalNumber ? ArrayStorage::Double : ArrayStorage::Other);
            if (Reference again = bearLiteralAgainBehindItsRoom(walker, index, namesOfLists[place], list, inputHead))
                list = again;
            if (walker.isUnreachable())
                return;
        }
        Draft reserve(Opcode::ReserveArrayCapacity, origin);
        walker.chooseConservativeFields(reserve);
        reserve.setArrayOperand(list);
        reserve.setCapacityOperand(room);
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
        ValueID literal = namesOfLists[place];
        bool isBornForThisEntry = !index.isParameter(literal) && (index.opcodeOf(literal) == Opcode::NewArray || index.opcodeOf(literal) == Opcode::Data) && index.loopOf(index.blockOf(literal)) == index.loop(loop).parent;
        const Operation& filled = walker.code().at(underlyingCellOf(walker.code(), list.id()));
        bool startsAtLiteralLength = filled.opcode == Opcode::NewArray && filled.holes().empty() && initialCounterRange == Range::one(static_cast<int64_t>(filled.elementsOperands().size()));
        size_t placeOfPush = pushesWithUnusedResult.findIf([&](const auto& push) { return push.first == lists[place] && (hasCapacityForSpecializedVersion || index.isUnused(push.second)); });
        bool appendsToLiteral = !walker.isUnreachable() && isBornForThisEntry && startsAtLiteralLength && (!listsPushedOnto.contains(lists[place]) || placeOfPush != notFound) && !listsFilledTwiceInATurn.contains(lists[place]);
        if (appendsToLiteral && placeOfPush != notFound) {
            noteKnown(walker, KnownNote::PushAppendsToLiteral, list, { }, inputHead.index(), LocationMasks(), pushesWithUnusedResult[placeOfPush].second.index() | static_cast<uint64_t>(walker.currentFunction().index()) << 32);
            walker.noteLoopWhosePushFillsLiteral(inputHead);
        }
        if (appendsToLiteral)
            noteKnown(walker, KnownNote::LoopAppendsToLiteral, list, { }, inputHead.index(), LocationMasks(), (placeOfCounter + 1) | static_cast<uint64_t>(walker.currentFunction().index()) << 32);
        if (appendsToLiteral && hasExactTripCount && everyStoreStandsBehindTestOfHead && types.at(walker.typeOf(roomByCount)).isInteger())
            noteKnown(walker, KnownNote::FinalLiteralLength, list, { }, inputHead.index(), LocationMasks(), roomByCount.bits());
        if (appendsToLiteral && ((hasMergedNumberStore && !listsPushedOnto.contains(lists[place])) || hasPushesAndArithmeticUsers) && !hasExactTripCount && listReadUnderCounter && listReadUnderCounter != namesOfLists[place] && types.at(walker.typeOf(roomByCount)).isInteger() && types.at(walker.typeOf(roomByCount)).integers.maximum > BASE_CONTIGUOUS_VECTOR_LEN_MAX)
            noteKnown(walker, KnownNote::CapacityForRemainingIterations, list, { }, inputHead.index(), LocationMasks(), roomByCount.bits() | static_cast<uint64_t>(listReadUnderCounter.index()) << 32);
    }
}

static void forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(Walker& walker, FunctionID callee, std::span<const ValueID> arguments, unsigned depth, Opcode access, const Invocable<void(ValueID argument, ValueID key, FunctionID reader, ValueID read)> auto& take)
{
    const BodyIndex& body = walker.facts().bodies().at(callee);
    BlockID entry = BlockID(0u);
    auto argumentBehind = [&](ValueID value) -> ValueID {
        value = body.stripCasts(value);
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
                ValueID key = body.stripCasts(operation.propertyOperand());
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

bool copyOfCalleeReadsUnderKeyWhatNobodyKnows(Walker& walker, const BodyValue& call, uint32_t firstValueOfLoop)
{
    const BodyIndex& index = walker.currentBodyIndex();
    FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id());
    if (!callee || !lookReadsCopyOfCalleeInListReadsInLoops(walker, call, callee))
        return false;
    bool readsIt = false;
    forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, callee, call.as<Opcode::Call>().argumentsOperands(), 2, Opcode::ElemGet, [&](ValueID argument, ValueID, FunctionID reader, ValueID read) {
        if (reader != callee || walker.facts().bodies().at(reader).blockOf(read) != BlockID(0u))
            return;
        ValueID variable = argument.index() < firstValueOfLoop ? ValueID() : readOfVariableInLoopInListReadsInLoops(index, argument, firstValueOfLoop);
        Reference inFront = argument.index() < firstValueOfLoop ? walker.referenceOf(argument) : Reference();
        TypeID known = inFront ? walker.typeOf(inFront) : variable ? widestTypeOfOperation(walker, index.value(variable)) : TypeID();
        readsIt |= known && walker.types().isUninformative(known);
    });
    return readsIt;
}

static bool storesNumberInCopyOfCalleeInListReadsInLoops(Walker& walker, FunctionID writer, ValueID value, unsigned depth)
{
    const BodyIndex& body = walker.facts().bodies().at(writer);
    value = body.stripCasts(value);
    if (!value || body.isParameter(value))
        return false;
    Opcode opcode = body.opcodeOf(value);
    if (numberConstantOpcodes.contains(opcode) || numberProducingOperators.contains(opcode))
        return true;
    if (opcode == Opcode::ElemGet) {
        TypeID seenByLink = walker.facts().linked().resultOfReadByKey(writer, value);
        return seenByLink && (walker.types().at(seenByLink).primitives & Primitive::Number) && walker.types().at(seenByLink).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    }
    BodyValue operation = body.value(value);
    return opcode == Opcode::Add && depth && storesNumberInCopyOfCalleeInListReadsInLoops(walker, writer, operation.operand(0), depth - 1) && storesNumberInCopyOfCalleeInListReadsInLoops(walker, writer, operation.operand(1), depth - 1);
}

template<typename TypeInLook>
static bool surelyEndsWhatIsShownOfListsInListReadsInLoops(Walker& walker, const BodyValue& operation, uint32_t firstValueOfLoop, const TypeInLook& typeInLook, bool usesIterationGuards)
{
    TypeArena& types = walker.types();
    if (operation.opcode() == Opcode::Call) {
        if (walker.previewOutOfRow() && std::ranges::find(walker.operationsThatRanOtherCodeOfRow(), operation.id()) == walker.operationsThatRanOtherCodeOfRow().end())
            return false;
        const LinkTables& linked = walker.facts().linked();
        FunctionID known = linked.calleeAt(walker.currentFunction(), operation.id());
        if (TypeID called = known ? TypeID() : typeInLook(operation.as<Opcode::Call>().calleeOperand()))
            known = singleFunctionOf(types, types.at(called));
        if (known && usesIterationGuards && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, known))
            return false;
        if (FunctionID callee = known) {
            if (const LinkedEffectsGivenArguments* given = findEffectsGivenArgumentsInListReadsInLoops(walker, operation, callee, typeInLook))
                return !preservesHoistedArrayGuards(walker, given->onReturn) || !preservesHoistedArrayGuards(walker, given->onThrow);
            return !preservesHoistedArrayGuards(walker, linked.function(callee).effectsOnReturn()) || !preservesHoistedArrayGuards(walker, linked.function(callee).effectsOnThrow());
        }
        TypeID callee = typeInLook(operation.as<Opcode::Call>().calleeOperand());
        return !callee || !singleBuiltinOf(types, types.at(callee));
    }
    if (operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.objectOperand().index() >= firstValueOfLoop && !usesIterationGuards) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID stepValue = index.stripCasts(operation.objectOperand());
        ValueID step = !index.isParameter(stepValue) && index.opcodeOf(stepValue) == Opcode::ResultAt ? index.value(stepValue).operand(0) : ValueID();
        if (step && !index.isParameter(step) && findIndexedArrayIteration(walker, index, index.value(step)))
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
                ValueID element = handed[place] ? index.stripCasts(handed[place]) : ValueID();
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

static TypeID elementsThatLinkProvesOfReceiverInListReadsInLoops(Walker& walker, const BodyIndex& index, Reference list)
{
    auto [caller, call] = walker.callThatFrameAtWorkCopies();
    std::optional<unsigned> ofThis = walker.program().at(walker.currentFunction()).thisParameterIndex();
    if (!call || !ofThis || !list || !list.isValue() || *ofThis >= index.block(entryBlock).numberOfParameters())
        return { };
    Reference receiver = walker.referenceOf(index.block(entryBlock).parameter(*ofThis));
    if (!receiver || !receiver.isValue() || underlyingCellOf(walker.code(), receiver.id()) != underlyingCellOf(walker.code(), list.id()))
        return { };
    const LinkedContentsAtOperation* contents = walker.facts().linked().contentsAtOperation(caller, call);
    return contents ? contents->value : TypeID();
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

static ValueID findStepValue(const BodyIndex& index, const BodyValue& step)
{
    for (BodyValue operation : index.block(index.blockOf(step.id())).values()) {
        if (operation.opcode() == Opcode::ResultAt && operation.as<Opcode::ResultAt>().resultsOperand() == step.id() && operation.as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Value))
            return operation.id();
    }
    return { };
}

static bool isMathCall(const Walker& walker, const BodyIndex& index, const BodyValue& operation)
{
    if (operation.opcode() != Opcode::Call)
        return false;
    ValueID callee = operation.as<Opcode::Call>().calleeOperand();
    callee = callee ? index.stripCasts(callee) : ValueID();
    if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    return std::ranges::any_of(table.overloadsOf(table.canonicalID(index.value(callee).as<Opcode::Builtin>().builtin())), [](const BuiltinOverload& overload) {
        std::optional<InlineBuiltin> inLine = overload.mode == BuiltinCallMode::Call ? overload.code.implementation.inlineBuiltin : std::nullopt;
        if (!inLine)
            return false;
        switch (*inLine) {
        case InlineBuiltin::MathAbs:
        case InlineBuiltin::MathFloor:
        case InlineBuiltin::MathCeil:
        case InlineBuiltin::MathTrunc:
        case InlineBuiltin::MathRound:
        case InlineBuiltin::MathFround:
        case InlineBuiltin::MathF16Round:
        case InlineBuiltin::MathSqrt:
        case InlineBuiltin::MathPow:
        case InlineBuiltin::MathMin:
        case InlineBuiltin::MathMax:
        case InlineBuiltin::MathImul:
        case InlineBuiltin::MathClz32:
        case InlineBuiltin::MathSign:
        case InlineBuiltin::MathFunction:
        case InlineBuiltin::MathATan2:
        case InlineBuiltin::MathHypot:
            return true;
        default:
            return false;
        }
    });
}

static ListsReadInLoop findListsReadInLoopInListReadsInLoops(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses, bool keysOfReadsTakeNotes, bool& someReadHasNotedKey, std::optional<BitVector>& namesOfCountersAskedInFront)
{
    ListsReadInLoop found;
    ArrayLookupByName foundArrayLookup;
    Vector<uint32_t> speculationsByName;
    if (guesses.size() > ArrayLookupByName::linearSearchLimit) {
        speculationsByName = Vector<uint32_t>(guesses.size(), [](size_t place) { return static_cast<uint32_t>(place); });
        std::ranges::stable_sort(speculationsByName, { }, [&](uint32_t place) { return guesses[place].value.bits(); });
    }
    WhatReadsFromInFrontOfLoopFind foundFromInFront;
    bool someListIsTakenForNotedKey = false;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Folding))
        return found;

    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto typeInLook = [&](ValueID input) -> TypeID {
        if (input.index() < firstValueOfLoop) {
            Reference inFrontOfLoop = walker.referenceOf(input);
            TypeID own = walker.ownTypeOf(inFrontOfLoop);
            for (const HoistedSpeculation& guess : guesses) {
                if (own && guess.value == inFrontOfLoop)
                    own = types.intersectionOf(own, guess.tested);
            }
            return own;
        }
        size_t place = input.index() - preview.firstValue;
        return input.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
    };

    auto typeOfKeyInLook = [&](ValueID key) -> TypeID {
        TypeID seen = typeInLook(key);
        if (!seen || types.at(seen).isInteger() || !(types.at(seen).primitives & Primitive::Integer) || key.index() < firstValueOfLoop || walker.keysAreReadOffLookInRow())
            return seen;
        if (!namesOfCountersAskedInFront)
            namesOfCountersAskedInFront = namesOfCountersAskedForInt32InFrontOfLoop(walker, inputHead, entering);
        if (!namesOfCountersAskedInFront->get(key.index() - firstValueOfLoop))
            return seen;
        walker.noteKeyTakenForCounterAskedInFront();
        return TypeTable::int32;
    };
    std::optional<Vector<ValueID, 4>> elementCarriedIndexes;
    auto isCarriedIndex = [&](ValueID read) {
        if (!elementCarriedIndexes)
            elementCarriedIndexes = findElementCarriedIndexes(walker, inputHead);
        return elementCarriedIndexes->contains(read);
    };
    BitVector numbersThatEnterInnerLoops;
    auto entersLoopAsNumber = [&](ValueID value) {
        ValueID made = index.stripCasts(value);
        if (made && made.index() >= firstValueOfLoop && numbersThatEnterInnerLoops.get(made.index() - firstValueOfLoop))
            return true;
        std::optional<uint32_t> place = made ? index.block(inputHead).indexOfParameter(made) : std::nullopt;
        TypeID entered = place && *place < entering.size() && entering[*place] ? walker.ownTypeOf(entering[*place]) : TypeID();
        if (!entered || types.at(entered).isNumber())
            return !!entered;
        return !walker.sumsAreNotAskedInRow() && *place < preview.typesOfParameters.size() && isNumberCarriedBesideTextOrBigInt(walker, entering[*place], preview.typesOfParameters[*place]);
    };
    BitVector numbersBehindListsFound;
    auto isNumberBehindListsFound = [&](ValueID value) {
        TypeID seen = typeInLook(value);
        if (seen && types.at(seen).isNumber())
            return true;
        ValueID made = index.stripCasts(value);
        return (made && made.index() >= firstValueOfLoop && numbersBehindListsFound.get(made.index() - firstValueOfLoop)) || entersLoopAsNumber(value);
    };
    BitVector wholeNumbersBehindListsFound;
    auto isWholeNumberBehindListsFound = [&](ValueID value) {
        TypeID seen = typeInLook(value);
        if (seen && types.at(seen).isNumber())
            return !(types.at(seen).primitives & Primitive::FractionalNumber);
        ValueID made = index.stripCasts(value);
        return made && made.index() >= firstValueOfLoop && wholeNumbersBehindListsFound.get(made.index() - firstValueOfLoop);
    };
    struct JoinAwaited {
        BlockID block;
        uint32_t edgesSeen;
    };
    Vector<JoinAwaited, 8> joinsAwaited;
    auto placeOfJoinAwaited = [&](BlockID block) {
        for (size_t place = joinsAwaited.size(); place--;) {
            if (joinsAwaited[place].block == block)
                return place;
        }
        return notFound;
    };
    BitVector parametersThatMeetWhatIsNoNumber;
    BitVector parametersThatMeetWhatIsNoWholeNumber;
    auto noteWhatEdgesHandOn = [&](BlockID from) {
        for (BodyTarget target : index.terminator(from).targets()) {
            if ((target.arguments().empty() && target.proofs().empty()) || target.block().index() <= from.index() || !index.isInLoop(target.block(), loop))
                continue;
            BodyBlock reached = index.block(target.block());
            if (!reached.numberOfParameters() || reached.firstParameter().index() < firstValueOfLoop)
                continue;
            if (size_t place = placeOfJoinAwaited(target.block()); place != notFound)
                ++joinsAwaited[place].edgesSeen;
            else
                joinsAwaited.append({ target.block(), 1 });
            for (uint32_t position = 0; position < reached.numberOfParameters(); ++position) {
                uint32_t place = reached.parameter(position).index() - firstValueOfLoop;
                ValueID handedOn = position < target.arguments().size() ? target.arguments()[position] : index.valueNarrowedInto(reached.parameter(position));
                if (!handedOn || !isNumberBehindListsFound(handedOn))
                    parametersThatMeetWhatIsNoNumber.set(place);
                else if (!isWholeNumberBehindListsFound(handedOn))
                    parametersThatMeetWhatIsNoWholeNumber.set(place);
            }
        }
    };
    auto noteNumbersThatMeetInParametersOf = [&](BlockID join) {
        size_t awaited = joinsAwaited.isEmpty() ? notFound : placeOfJoinAwaited(join);
        if (awaited == notFound)
            return;
        uint32_t edgesSeen = joinsAwaited[awaited].edgesSeen;
        joinsAwaited[awaited] = joinsAwaited.last();
        joinsAwaited.removeLast();
        bool isInnerHead = index.isLoopHead(join);
        if (index.isEnteredByHandlerEdge(join) || (!isInnerHead && edgesSeen != index.numberOfIncomingEdges(join)) || index.numberOfIncomingEdges(join) >= UINT16_MAX)
            return;
        BodyBlock block = index.block(join);
        for (uint32_t position = 0; position < block.numberOfParameters(); ++position) {
            uint32_t place = block.parameter(position).index() - firstValueOfLoop;
            if (parametersThatMeetWhatIsNoNumber.get(place))
                continue;
            if (isInnerHead) {
                numbersThatEnterInnerLoops.set(place);
                continue;
            }
            numbersBehindListsFound.set(place);
            if (!parametersThatMeetWhatIsNoWholeNumber.get(place))
                wholeNumbersBehindListsFound.set(place);
        }
    };
    auto noteNumberBehindListsFound = [&](const BodyValue& operation, bool isWhole) {
        numbersBehindListsFound.set(operation.id().index() - firstValueOfLoop);
        if (isWhole)
            wholeNumbersBehindListsFound.set(operation.id().index() - firstValueOfLoop);
    };
    auto asksItsSiteAndIsReckonedWith = [&](const BodyValue& read) {
        if (read.opcode() != Opcode::PropGet || !walker.canUseIterationGuards(inputHead))
            return false;
        TypeID holder = typeInLook(read.objectOperand());
        if (holder && !types.isUninformative(holder))
            return false;
        ValueID id = read.id();
        return readThatAsksItsSiteIsReckonedWithAsNumber(walker, id, TypesOfInputs(typeInLook));
    };
    BitVector wholeNumbersThatKeysOfReadsTake;
    auto isWholeNumberThatKeyOfReadTakes = [&](ValueID value) {
        ValueID made = index.stripCasts(value);
        return made && made.index() >= firstValueOfLoop && wholeNumbersThatKeysOfReadsTake.get(made.index() - firstValueOfLoop);
    };
    auto isWholeNumberForKeyOfRead = [&](ValueID value) {
        return isWholeNumberThatKeyOfReadTakes(value) || isWholeNumberBehindListsFound(value);
    };
    auto noteWholeNumberThatKeyOfReadTakes = [&](const BodyValue& operation) {
        wholeNumbersThatKeysOfReadsTake.set(operation.id().index() - firstValueOfLoop);
    };
    BitVector numbersThatComeOfLengthsAndRemainders;
    auto comesOfLengthOrRemainder = [&](ValueID value) {
        ValueID made = index.stripCasts(value);
        return made && made.index() >= firstValueOfLoop && numbersThatComeOfLengthsAndRemainders.get(made.index() - firstValueOfLoop);
    };
    auto noteThatComesOfLengthOrRemainder = [&](const BodyValue& operation) {
        noteWholeNumberThatKeyOfReadTakes(operation);
        numbersThatComeOfLengthsAndRemainders.set(operation.id().index() - firstValueOfLoop);
    };
    bool nothingElseIsGuessedInFront = guesses.isEmpty();
    auto tryReuseForElementAccess = [&](Reference name) {
        size_t place = foundArrayLookup.find(found.lists.span(), name);
        if (place == notFound)
            return false;
        if (!holdsBoxedValuesOfEitherKind(found.lists[place]))
            return true;
        if (!found.lists[place].leavesBirthAsItIs) {
            found.lists[place].admitsInt32Storage = false;
            return true;
        }
        found.lists.removeAt(place);
        foundArrayLookup.forget();
        return false;
    };
    auto isAlreadyGuardedForReads = [&](Reference name) {
        std::optional<HoistedArrayGuard> noted = findNoteInListReadsInLoops(walker, name);
        return noted && isShownToReads(*noted);
    };

    struct ReadOffListOfUnknownContents {
        ValueID read;
        Reference name;
    };
    Vector<ReadOffListOfUnknownContents, 2> readsOffListsOfUnknownContents;
    Vector<Reference, 2> listsOfUnknownContents;
    Vector<Reference, 2> arraysNeedingExactStorage;
    Vector<Reference, 2> listsThatTakeMoreThanInt32s;
    Vector<Reference, 2> listsThatCopiesOfCalleesStoreIntoUncounted;
    Vector<Reference, 2> listsTakenUnderCountersAskedInFront;
    Vector<Reference, 2> listsReachedUnderCountersAskedInFront;
    static constexpr OpcodeSet stepsOfNumbers { Opcode::ToNumeric, Opcode::Increment, Opcode::Decrement };
    auto expectDoublesWhereElementIsMoved = [&](const BodyValue& operation) {
        if (operation.opcode() != Opcode::ElemSet || readsOffListsOfUnknownContents.isEmpty() || index.loop(loop).numberOfLoopsWithin != 1 || walker.copyForInt32sIsStruck() || !walker.canUseIterationGuards(inputHead))
            return false;
        ValueID made = index.stripCasts(operation.valueOperand());
        size_t place = readsOffListsOfUnknownContents.findIf([&](const ReadOffListOfUnknownContents& kept) {
            return kept.read == made;
        });
        if (place == notFound)
            return false;
        Reference taken = readsOffListsOfUnknownContents[place].name;
        static constexpr OpcodeSet makersInSight { Opcode::NewArray, Opcode::Data };
        if (makersInSight.contains(walker.code().at(underlyingCellOf(walker.code(), taken.id())).opcode))
            return false;
        for (HoistedArrayGuard& list : found.lists) {
            if (list.name != taken || list.storage != ArrayStorage::Other)
                continue;
            list.storage = ArrayStorage::Double;
            list.leavesBirthAsItIs = true;
            list.hasAlternateStorage = true;
            list.alternateStorage = ArrayStorage::Contiguous;
        }
        listsOfUnknownContents.removeAll(taken);
        readsOffListsOfUnknownContents.removeAllMatching([&](const ReadOffListOfUnknownContents& kept) {
            if (kept.name == taken)
                numbersBehindListsFound.set(kept.read.index() - firstValueOfLoop);
            return kept.name == taken;
        });
        walker.noteCopyForInt32s();
        return true;
    };
    auto expectNumbersWhereElementIsTakenForNumber = [&](const BodyValue& operation) {
        static constexpr OpcodeSet operatorsThatTakeNumbers = numberProducingOperators | allBitwiseOperators | stepsOfNumbers;
        bool isEqualityInLoopThatStandsInNoOther = allEqualities.contains(operation.opcode()) && !index.loop(index.loopOf(inputHead)).parent;
        bool takesTextsToo = operationsThatJoinTexts.contains(operation.opcode()) || allRelations.contains(operation.opcode()) || isEqualityInLoopThatStandsInNoOther;
        if (readsOffListsOfUnknownContents.isEmpty() || !(takesTextsToo || operatorsThatTakeNumbers.contains(operation.opcode()) || isMathCall(walker, index, operation)))
            return false;
        Reference taken;
        operation.forEachOperand([&](ValueID operand, uint32_t position) {
            ValueID made = index.stripCasts(operand);
            size_t place = readsOffListsOfUnknownContents.findIf([&](const ReadOffListOfUnknownContents& kept) {
                return kept.read == made;
            });
            if (taken || place == notFound)
                return;
            ValueID other = takesTextsToo && position < 2 ? operation.operand(1 - position) : ValueID();
            bool isHeldAgainstElementOfSuchList = other && (allRelations.contains(operation.opcode()) || operation.opcode() == Opcode::Add) && readsOffListsOfUnknownContents.containsIf([&](const ReadOffListOfUnknownContents& kept) {
                return kept.read == index.stripCasts(other);
            });
            if (takesTextsToo && !isHeldAgainstElementOfSuchList && (!other || !isNumberBehindListsFound(other)))
                return;
            taken = readsOffListsOfUnknownContents[place].name;
        });
        if (!taken)
            return false;
        for (HoistedArrayGuard& list : found.lists) {
            if (list.name != taken || list.storage != ArrayStorage::Other)
                continue;
            list.storage = ArrayStorage::Double;
            list.admitsInt32Storage = true;
            list.hasAlternateStorage = isEqualityInLoopThatStandsInNoOther;
            if (isEqualityInLoopThatStandsInNoOther)
                list.alternateStorage = ArrayStorage::Other;
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
            return kept.read == index.stripCasts(operation.objectOperand());
        });
        if (place == notFound)
            return;
        Reference taken = readsOffListsOfUnknownContents[place].name;
        for (HoistedArrayGuard& list : found.lists) {
            if (list.name == taken && list.storage == ArrayStorage::Other)
                list.storage = ArrayStorage::Contiguous;
        }
        listsOfUnknownContents.removeAll(taken);
        readsOffListsOfUnknownContents.removeAllMatching([&](const ReadOffListOfUnknownContents& kept) {
            return kept.name == taken;
        });
    };

    BackEdgeDominators backEdgeDominators(index, loop);
    Vector<uint32_t, 32> namesUsedEveryIteration;
    auto markUsedEveryIteration = [&](Reference name) {
        if (name && name.isValue() && (namesUsedEveryIteration.isEmpty() || namesUsedEveryIteration.last() != name.id().index()))
            namesUsedEveryIteration.append(name.id().index());
    };
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        if (block.isHandlerBlock())
            return { };
        noteNumbersThatMeetInParametersOf(member);
        bool dominatesBackEdges = backEdgeDominators.contains(member);
        for (BodyValue operation : block.values()) {
            if (dominatesBackEdges) {
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    markUsedEveryIteration(nameThatEntersLoopInListReadsInLoops(walker, index, operand, firstValueOfLoop));
                });
            }
            while (expectNumbersWhereElementIsTakenForNumber(operation)) { }
            bool movesElement = expectDoublesWhereElementIsMoved(operation);
            expectBoxedValuesWhereElementIsReadByName(operation);
            if (numberConstantOpcodes.contains(operation.opcode()))
                noteNumberBehindListsFound(operation, operation.opcode() == Opcode::Integer && operation.as<Opcode::Integer>().integer() == static_cast<int32_t>(operation.as<Opcode::Integer>().integer()));
            else if (operatorsThatMakeNumbersOfNumbers.contains(operation.opcode()) && isNumberBehindListsFound(operation.operand(0)) && isNumberBehindListsFound(operation.operand(1)))
                noteNumberBehindListsFound(operation, operation.opcode() != Opcode::Div && isWholeNumberBehindListsFound(operation.operand(0)) && isWholeNumberBehindListsFound(operation.operand(1)));
            else if (stepsOfNumbers.contains(operation.opcode()) && isNumberBehindListsFound(operation.operand(0)))
                noteNumberBehindListsFound(operation, isWholeNumberBehindListsFound(operation.operand(0)));
            else if (operation.opcode() == Opcode::Negate && isNumberBehindListsFound(operation.operand(0)))
                noteNumberBehindListsFound(operation, false);
            else if (asksItsSiteAndIsReckonedWith(operation))
                noteNumberBehindListsFound(operation, false);
            if ((operatorsThatMakeNumbersOfNumbers.contains(operation.opcode()) || operation.opcode() == Opcode::Mod) && operation.opcode() != Opcode::Div && isWholeNumberForKeyOfRead(operation.operand(0)) && isWholeNumberForKeyOfRead(operation.operand(1))) {
                if (operation.opcode() == Opcode::Mod || comesOfLengthOrRemainder(operation.operand(0)) || comesOfLengthOrRemainder(operation.operand(1)))
                    noteThatComesOfLengthOrRemainder(operation);
                else
                    noteWholeNumberThatKeyOfReadTakes(operation);
            }
            if (Reference pushedOnto = literalThatCallPushesOntoInEveryTurn(walker, operation, inputHead); pushedOnto && walker.canUseIterationGuards(inputHead)) {
                TypeID pushed = typeInLook(operation.as<Opcode::Call>().argumentsOperands()[0]);
                bool isListed = foundArrayLookup.find(found.lists.span(), pushedOnto) != notFound;
                if (pushed && !isNeverType(pushed) && types.at(pushed).isNumber() && !isListed && !isFreshAllocation(walker, pushedOnto)) {
                    ArrayStorage kindPushedOnto = types.at(pushed).primitives & Primitive::FractionalNumber ? ArrayStorage::Double : ArrayStorage::Int32;
                    if (!types.isSubset(pushed, TypeTable::int32)) {
                        bool isWrittenToHoldNumbersAlone = isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(pushedOnto))));
                        (kindPushedOnto == ArrayStorage::Double && !isWrittenToHoldNumbersAlone ? listsThatTakeMoreThanInt32s : arraysNeedingExactStorage).append(pushedOnto);
                    }
                    found.lists.append(HoistedArrayGuard { pushedOnto, kindPushedOnto, false, true });
                } else if (size_t place = foundArrayLookup.find(found.lists.span(), pushedOnto); place != notFound && pushed && !isNeverType(pushed) && types.at(pushed).isNumber() && isNumericStorage(found.lists[place].storage)) {
                    found.lists[place].isWritable = true;
                    const Operation& born = walker.code().at(underlyingCellOf(walker.code(), pushedOnto.id()));
                    bool isAllocatedAsDouble = born.opcode == Opcode::NewArray && born.arrayStorage() == ArrayStorage::Double;
                    found.lists[place].storage = isAllocatedAsDouble || (types.at(pushed).primitives & Primitive::FractionalNumber) ? ArrayStorage::Double : ArrayStorage::Int32;
                    found.lists[place].admitsInt32Storage = false;
                }
                continue;
            }
            if (surelyEndsWhatIsShownOfListsInListReadsInLoops(walker, operation, firstValueOfLoop, typeInLook, member != inputHead ? walker.canUseIterationGuards(inputHead) && index.loopOf(member) == loop : walker.canUseIterationGuards(inputHead)))
                return { };
            if (const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, operation, true); stepped && stepped->kind != JSC::IterationKind::Values && stepped->isDeadAfterLoop && stepped->list.isValue()) {
                Reference name = stepped->list;
                if (dominatesBackEdges)
                    markUsedEveryIteration(name);
                if (tryReuseForElementAccess(name) || isFreshAllocation(walker, name) || isAlreadyGuardedForReads(name))
                    continue;
                ArrayStorage written = stepped->kind == JSC::IterationKind::Entries ? predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(name))) : ArrayStorage::Other;
                ValueID opened = index.stripCasts(index.value(operation.operand(0)).as<Opcode::IteratorOpen>().iterableOperand());
                ValueID receiver = stepped->kind == JSC::IterationKind::Entries && opened && !index.isParameter(opened) && index.opcodeOf(opened) == Opcode::Call ? index.value(opened).thisValueOperand() : ValueID();
                if (TypeID expected = written == ArrayStorage::Other && receiver ? findExpectation(walker, receiver).type : TypeID())
                    written = predictedArrayStorage(walker, types.withoutNullish(expected));
                bool isNumeric = isNumericStorage(written);
                found.lists.append(HoistedArrayGuard { name, isNumeric ? ArrayStorage::Double : written, isNumeric });
                continue;
            }
            if (const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, operation); stepped && stepped->isDeadAfterLoop && stepped->list.isValue()) {
                Reference name = stepped->list;
                if (dominatesBackEdges)
                    markUsedEveryIteration(name);
                if (tryReuseForElementAccess(name) || isFreshAllocation(walker, name) || isAlreadyGuardedForReads(name))
                    continue;
                ArrayStorage written = predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(name)));
                if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, index.value(operation.operand(0)).as<Opcode::IteratorOpen>().iterableOperand()).type : TypeID())
                    written = predictedArrayStorage(walker, types.withoutNullish(expected));
                TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id());
                written = kindWrittenOrSeenByLinkInListReadsInLoops(types, written, seenByLink);
                bool isNumeric = isNumericStorage(written);
                if (seenByLink && (types.at(seenByLink).isInt32OrNullish() || holdsKeysOfLoopInListReadsInLoops(walker, findStepValue(index, operation), seenByLink)))
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                else
                    found.lists.append(HoistedArrayGuard { name, isNumeric ? ArrayStorage::Double : written, isNumeric });
                if (ValueID stepValue = isNumericStorage(found.lists.last().storage) ? findStepValue(index, operation) : ValueID())
                    noteNumberBehindListsFound(index.value(stepValue), found.lists.last().storage == ArrayStorage::Int32);
                if (ValueID stepValue = written == ArrayStorage::Other && (!seenByLink || types.at(seenByLink).isAnything()) ? findStepValue(index, operation) : ValueID()) {
                    listsOfUnknownContents.append(name);
                    readsOffListsOfUnknownContents.append({ stepValue, name });
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
                    if (!name || !name.isValue() || isFreshAllocation(walker, name) || isAlreadyGuardedForReads(name) || listsThatCopiesOfCalleesStoreIntoUncounted.contains(name))
                        return;
                    TypeID proven = walker.ownTypeOf(name);
                    for (const HoistedSpeculation& guess : guesses) {
                        if (guess.value == name)
                            proven = types.intersectionOf(proven, guess.tested);
                    }
                    TypeID list = types.withoutNullish(proven);
                    if (!hasKind(types, list, OperandKind::Array) || (key && !readTakesIndexOfProvenList(types, list, typeOfKeyInLook(key))))
                        return;
                    if (tryReuseForElementAccess(name))
                        return;
                    TypeID seenByLink = walker.facts().linked().resultOfReadByKey(reader, read);
                    bool readerHasLoops = walker.facts().bodies().at(reader).hasLoops();
                    if (seenByLink && types.at(seenByLink).isInt32OrNullish()) {
                        found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                        if (readerHasLoops)
                            walker.count(StatisticID::InlineeArrayGuardsHoisted);
                        return;
                    }
                    ArrayStorage written = predictedArrayStorage(walker, list);
                    if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, input).type : TypeID())
                        written = predictedArrayStorage(walker, types.withoutNullish(expected));
                    written = kindWrittenOrSeenByLinkInListReadsInLoops(types, written, seenByLink);
                    if (written == ArrayStorage::Other)
                        return;
                    bool isNumeric = isNumericStorage(written);
                    if (!isNumeric && key && typeOfKeyInLook(key) != typeInLook(key))
                        return;
                    if (key && typeOfKeyInLook(key) != typeInLook(key) && !listsReachedUnderCountersAskedInFront.contains(name))
                        listsReachedUnderCountersAskedInFront.append(name);
                    found.lists.append(HoistedArrayGuard { name, isNumeric ? ArrayStorage::Double : written, isNumeric });
                    if (readerHasLoops)
                        walker.count(StatisticID::InlineeArrayGuardsHoisted);
                });
                if (!walker.canUseIterationGuards(inputHead))
                    continue;
                forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, copied, operation.as<Opcode::Call>().argumentsOperands(), 2, Opcode::ElemSet, [&](ValueID argument, ValueID key, FunctionID writer, ValueID store) {
                    ValueID input = valueThatEntersLoopInListReadsInLoops(index, argument, firstValueOfLoop);
                    Reference name = input.index() < firstValueOfLoop ? walker.referenceOf(input) : nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop);
                    if (!name || !name.isValue() || isFreshAllocation(walker, name))
                        return;
                    if (!storesNumberInCopyOfCalleeInListReadsInLoops(walker, writer, walker.facts().bodies().at(writer).value(store).valueOperand(), 3)) {
                        if (!listsThatCopiesOfCalleesStoreIntoUncounted.contains(name))
                            listsThatCopiesOfCalleesStoreIntoUncounted.append(name);
                        if (found.lists.removeAllMatching([&](const HoistedArrayGuard& listed) { return listed.name == name && !listed.isWritable; })) {
                            foundArrayLookup.forget();
                            walker.count(StatisticID::InlineeStoredArraysNotGuarded);
                        }
                        return;
                    }
                    TypeID list = types.withoutNullish(walker.ownTypeOf(name));
                    if (!hasKind(types, list, OperandKind::Array) || (key && !readTakesIndexOfProvenList(types, list, typeOfKeyInLook(key))))
                        return;
                    if (key && typeOfKeyInLook(key) != typeInLook(key) && isBornEmptyInListReadsInLoops(walker, index, input, name))
                        return;
                    if (key && typeOfKeyInLook(key) != typeInLook(key) && !listsReachedUnderCountersAskedInFront.contains(name))
                        listsReachedUnderCountersAskedInFront.append(name);
                    size_t place = found.lists.findIf([&](const HoistedArrayGuard& listed) {
                        return listed.name == name;
                    });
                    if (place == notFound) {
                        found.lists.append(HoistedArrayGuard { name, ArrayStorage::Double, false, true });
                        arraysNeedingExactStorage.append(name);
                        return;
                    }
                    HoistedArrayGuard& listed = found.lists[place];
                    if (!isNumericStorage(listed.storage) || (!listed.isWritable && listed.storage != ArrayStorage::Double))
                        return;
                    listed.storage = ArrayStorage::Double;
                    listed.admitsInt32Storage = false;
                    listed.isWritable = true;
                    arraysNeedingExactStorage.append(name);
                });
                continue;
            }
            bool isStore = operation.opcode() == Opcode::ElemSet && walker.canUseIterationGuards(inputHead);
            bool readsLength = operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() == walker.program().keyOf(BuiltinNames::length);
            bool asksPlainVector = operation.opcode() == Opcode::HasContiguousIndex;
            bool storesByKey = operation.opcode() == Opcode::ElemSet;
            if (operation.opcode() != Opcode::ElemGet && !storesByKey && !readsLength && !asksPlainVector)
                continue;
            if (operation.opcode() == Opcode::ElemGet && walker.standsInCopyOfLoopBuiltForEachMember() && rowFoundUnderKeyInFrontOfLoop(walker, operation))
                continue;
            ValueID input = valueThatEntersLoopInListReadsInLoops(index, asksPlainVector ? operation.arrayOperand() : operation.objectOperand(), firstValueOfLoop);
            Reference nameReadInTurn = input.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop) : Reference();
            if (input.index() >= firstValueOfLoop && !nameReadInTurn) {
                TypeID row = operation.opcode() == Opcode::ElemGet && readsListFoundByReadFromInFront(walker.program(), index, operation, firstValueOfLoop, foundFromInFront) ? typeInLook(input) : TypeID();
                TypeID key = row ? typeInLook(operation.propertyOperand()) : TypeID();
                if (key && types.at(key).isInteger() && isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(row))))
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
                tryReuseForElementAccess(name);
            size_t placeInList = foundArrayLookup.find(found.lists.span(), name);
            bool isListed = placeInList != notFound;
            if (!isStore && !readsLength)
                arraysNeedingExactStorage.append(name);
            ArrayStorage kindStoredInto = ArrayStorage::Other;
            if (isStore) {
                TypeID keyOfStore = typeInLook(operation.propertyOperand());
                ValueID written = index.stripCasts(operation.objectOperand());
                bool isLiteralBeforeLoop = written && !index.isParameter(written) && written.index() < firstValueOfLoop && index.opcodeOf(written) == Opcode::NewArray && !index.value(written).as<Opcode::NewArray>().elementsOperands().empty();
                const Operation* allocationBeforeLoop = isLiteralBeforeLoop && name.isValue() ? &walker.code().at(underlyingCellOf(walker.code(), name.id())) : nullptr;
                bool isAllocatedAsNumeric = allocationBeforeLoop && allocationBeforeLoop->opcode == Opcode::NewArray && isNumericStorage(allocationBeforeLoop->arrayStorage());
                if (isLiteralBeforeLoop && !isAllocatedAsNumeric && index.isParameter(index.stripCasts(operation.valueOperand())) && keyOfStore && types.at(keyOfStore).isInteger() && types.at(keyOfStore).integers.minimum >= static_cast<int64_t>(index.value(written).as<Opcode::NewArray>().elementsOperands().size()))
                    continue;
                TypeID stored = typeInLook(operation.valueOperand());
                stored = stored ? types.withoutPrimitives(stored, Primitive::Undefined) : TypeID();
                if ((!stored || isNeverType(stored) || !types.at(stored).isNumber()) && isNumberBehindListsFound(operation.valueOperand()))
                    stored = isWholeNumberBehindListsFound(operation.valueOperand()) ? TypeTable::int32 : TypeTable::number;
                bool storesNumber = stored && !isNeverType(stored) && types.at(stored).isNumber();
                bool isReadForBoxedValues = isListed && found.lists[placeInList].storage == ArrayStorage::Contiguous && !found.lists[placeInList].admitsInt32Storage;
                if (!storesNumber && (!stored || isNeverType(stored) || (!isReadForBoxedValues && predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(name))) != ArrayStorage::Contiguous)))
                    continue;
                bool takesDoublesInCopyForThem = storesNumber && walker.walksCopyForAnotherKindOfStorage() && !types.isSubset(stored, TypeTable::int32);
                kindStoredInto = !storesNumber ? ArrayStorage::Contiguous : (types.at(stored).primitives & Primitive::FractionalNumber) || takesDoublesInCopyForThem ? ArrayStorage::Double : ArrayStorage::Int32;
                if (storesNumber && !types.isSubset(stored, TypeTable::int32)) {
                    bool isWrittenToHoldNumbersAlone = isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(name))));
                    (kindStoredInto == ArrayStorage::Double && !isWrittenToHoldNumbersAlone && !walker.walksCopyForAnotherKindOfStorage() ? listsThatTakeMoreThanInt32s : arraysNeedingExactStorage).append(name);
                }
                if (storesNumber && isAllocatedAsNumeric && allocationBeforeLoop->arrayStorage() == ArrayStorage::Double) {
                    kindStoredInto = ArrayStorage::Double;
                    arraysNeedingExactStorage.append(name);
                }
                if (movesElement)
                    arraysNeedingExactStorage.append(name);
            }
            if (isStore && isListed) {
                HoistedArrayGuard& list = found.lists[placeInList];
                bool isOfEitherKind = list.storage == ArrayStorage::Double && list.admitsInt32Storage;
                if (list.isWritable && list.storage != kindStoredInto && (list.storage == ArrayStorage::Contiguous || kindStoredInto == ArrayStorage::Contiguous))
                    continue;
                if (list.isWritable && list.storage != kindStoredInto)
                    kindStoredInto = ArrayStorage::Double;
                else if (!list.isWritable && !isOfEitherKind && list.storage != kindStoredInto)
                    continue;
                list.storage = kindStoredInto;
                list.admitsInt32Storage = false;
                list.isWritable = true;
                if (movesElement) {
                    list.leavesBirthAsItIs = true;
                    list.hasAlternateStorage = true;
                    list.alternateStorage = ArrayStorage::Contiguous;
                }
            }
            if (isListed && operation.opcode() == Opcode::ElemGet && listsOfUnknownContents.contains(name))
                readsOffListsOfUnknownContents.append({ operation.id(), name });
            if (isListed) {
                if (TypeID ownKey = operation.opcode() == Opcode::ElemGet ? typeInLook(operation.propertyOperand()) : TypeID(); ownKey && types.at(ownKey).isInteger())
                    listsTakenUnderCountersAskedInFront.removeAll(name);
                found.lists[placeInList].isAskedForItsStorageAlone &= !readsLength;
                if (readsLength)
                    noteThatComesOfLengthOrRemainder(operation);
                TypeID key = operation.opcode() == Opcode::ElemGet && isNumericStorage(found.lists[placeInList].storage) ? typeInLook(operation.propertyOperand()) : TypeID();
                if (key && (types.at(key).isInteger() || (types.at(key).isNumber() && walker.canUseIterationGuards(inputHead))))
                    noteNumberBehindListsFound(operation, found.lists[placeInList].storage == ArrayStorage::Int32);
                continue;
            }

            TypeID proven = walker.ownTypeOf(name);
            if (speculationsByName.isEmpty()) [[likely]] {
                for (const HoistedSpeculation& guess : guesses) {
                    if (guess.value == name)
                        proven = types.intersectionOf(proven, guess.tested);
                }
            } else {
                for (uint32_t place : std::ranges::equal_range(speculationsByName, name.bits(), { }, [&](uint32_t place) { return guesses[place].value.bits(); }))
                    proven = types.intersectionOf(proven, guesses[place].tested);
            }
            if (BuiltinClassID classOfView = readsLength || asksPlainVector ? BuiltinClassID() : classOfViewInListReadsInLoops(walker, types.withoutNullish(proven))) {
                bool isReadInLine = hasInlineElementAccess(walker.program().builtins().typedArrayTypeOf(classOfView)) || kindsOfViewsOfNumbersIn(walker.program(), types, types.withoutNullish(proven));
                bool lengthMayStandInFront = isReadInLine && !walker.effectsOfQuietDraftsOfRow().writes.contains(AbstractHeap::TypedArrayStorage);
                if (lengthMayStandInFront && (viewMayFollowSharedMemoryThatGrows(walker, name) || proven != types.withoutNullish(proven)) && !isFreshAllocation(walker, name) && !findNoteInListReadsInLoops(walker, name)) {
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Other, false });
                    found.lists.last().isAskedForItsStorageAlone = true;
                }
                continue;
            }
            if (storesByKey && !isStore)
                continue;
            if (readsLength) {
                if (isProvenToBeList(types, types.withoutNullish(proven)))
                    noteThatComesOfLengthOrRemainder(operation);
                bool staysAsMadeByLink = readsOffEntryParameterThatStaysAsMade(walker, operation) || receiverStaysAsMadeByLink(walker, operation);
                auto lengthIsFoundByNote = [&] {
                    return findKnown(walker, KnownNote::LengthOfViewIsFoundAbove, name) && loopTakesInstancesBelowViewForItsLength(walker, inputHead, name);
                };
                if (!staysAsMadeByLink && classOfViewInListReadsInLoops(walker, proven) && !isFreshAllocation(walker, name) && !isKnownPristine(walker, name) && !findNoteInListReadsInLoops(walker, name) && !lengthIsFoundByNote())
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Other, false });
                continue;
            }
            TypeID list = types.withoutNullish(proven);
            TypeID keyInLook = asksPlainVector ? typeInLook(operation.positionOperand()) : typeOfKeyInLook(operation.propertyOperand());
            bool keyIsCounterAskedInFront = !asksPlainVector && keyInLook != typeInLook(operation.propertyOperand());
            if (!asksPlainVector && keyInLook && !types.at(keyInLook).isNumber() && isWholeNumberBehindListsFound(operation.propertyOperand()))
                keyInLook = TypeTable::int32;
            bool keyIsNoted = operation.opcode() == Opcode::ElemGet && !(keyInLook && types.at(keyInLook).isInteger()) && (nothingElseIsGuessedInFront ? isWholeNumberThatKeyOfReadTakes(operation.propertyOperand()) : comesOfLengthOrRemainder(operation.propertyOperand()));
            someReadHasNotedKey |= keyIsNoted;
            bool keyTakesNote = keysOfReadsTakeNotes && keyIsNoted;
            if (keyTakesNote)
                keyInLook = TypeTable::int32;
            ValueID keyAsWritten = asksPlainVector ? ValueID() : index.stripCasts(operation.propertyOperand());
            bool keyIsReckonedInTurn = keyAsWritten && keyAsWritten.index() >= firstValueOfLoop && !index.isParameter(keyAsWritten) && operatorsThatReckonKeysInListReadsInLoops.contains(index.opcodeOf(keyAsWritten));
            bool keyIsAskedInTurn = keyIsReckonedInTurn && walker.canUseIterationGuards(inputHead) && keyOfProvenListMayBeAskedForInt32(types, list, keyInLook) && textOfLoopStoresIntoList(name);
            if (!keyIsAskedInTurn && !readTakesIndexOfProvenList(types, list, keyInLook))
                continue;
            if (keyIsCounterAskedInFront && operation.opcode() == Opcode::ElemSet && isBornEmptyInListReadsInLoops(walker, index, input, name))
                continue;
            if (keyIsCounterAskedInFront && !listsReachedUnderCountersAskedInFront.contains(name))
                listsReachedUnderCountersAskedInFront.append(name);
            if (isFreshAllocation(walker, name))
                continue;
            if ((asksPlainVector ? !!findNoteInListReadsInLoops(walker, name) : isAlreadyGuardedForReads(name)) && !walker.isArrayGuardedAtHeader(name))
                continue;
            if (listsThatCopiesOfCalleesStoreIntoUncounted.contains(name)) {
                walker.count(StatisticID::InlineeStoredArraysNotGuarded);
                continue;
            }
            someListIsTakenForNotedKey |= keyTakesNote;

            if (isStore) {
                found.lists.append(HoistedArrayGuard { name, kindStoredInto, false, true });
                static constexpr OpcodeSet makersInSight { Opcode::NewArray, Opcode::NewArrayLike, Opcode::Data };
                bool isMadeInSight = makersInSight.contains(walker.code().at(underlyingCellOf(walker.code(), name.id())).opcode);
                if (kindStoredInto == ArrayStorage::Int32 && !isMadeInSight && index.loop(loop).numberOfLoopsWithin == 1 && arraysNeedingExactStorage.contains(name) && !walker.walksCopyForAnotherKindOfStorage() && !walker.copyForInt32sIsStruck()) {
                    found.lists.last().hasAlternateStorage = true;
                    found.lists.last().alternateStorage = ArrayStorage::Other;
                    walker.noteCopyForInt32s();
                    walker.count(StatisticID::WholeNumberListsAskedTwice);
                }
                if (movesElement) {
                    found.lists.last().leavesBirthAsItIs = true;
                    found.lists.last().hasAlternateStorage = true;
                    found.lists.last().alternateStorage = ArrayStorage::Contiguous;
                }
                continue;
            }
            ArrayStorage written = predictedArrayStorage(walker, list);
            if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, input).type : TypeID())
                written = predictedArrayStorage(walker, types.withoutNullish(expected));
            if (walker.wasAskedForStorageOfNumbersInVain(name)) {
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Contiguous, false });
                continue;
            }
            if (asksPlainVector) {
                TypeID taken = written == ArrayStorage::Double || written == ArrayStorage::Other ? typeThatCalleeTakesOfElementInListReadsInLoops(walker, index, loop, typeInLook) : TypeID();
                bool takesFractions = taken && arrayStorageHolding(types, taken) == ArrayStorage::Double && (types.at(taken).primitives & Primitive::FractionalNumber);
                TypeID elements = elementsThatLinkProvesOfReceiverInListReadsInLoops(walker, index, name);
                bool takesWholeNumbers = !takesFractions && elements && !(types.at(elements).primitives & (Primitive::UndefinedOrNull | Primitive::FractionalNumber)) && isNumericStorage(arrayStorageHolding(types, elements)) && !index.holdsAnyOf(OpcodeSet { Opcode::TryPutIndex, Opcode::TryPutByValDirect });
                TypeID besideNullish = elements ? types.withoutNullish(elements) : taken ? types.withoutNullish(taken) : TypeID();
                bool mayTakeWholeNumbers = !walker.walksCopyForAnotherKindOfStorage() && !takesFractions && !takesWholeNumbers && besideNullish && types.at(besideNullish).isNumberWithoutFraction() && !index.holdsAnyOf(OpcodeSet { Opcode::TryPutIndex, Opcode::TryPutByValDirect });
                if (takesWholeNumbers)
                    taken = elements;
                std::optional<GuardedReadInListReadsInLoops> guarded = takesFractions || takesWholeNumbers || mayTakeWholeNumbers ? findGuardedReadInListReadsInLoops(index) : std::nullopt;
                bool goesByRowOfNumbers = guarded && guarded->test == operation.id();
                bool mayTakeAnyNumber = takesFractions && (types.at(taken).primitives & Primitive::NumberWithoutFraction);
                bool takesAnyNumber = mayTakeAnyNumber && !walker.copyForInt32sIsStruck();
                bool asksForInt32sFirst = goesByRowOfNumbers && (mayTakeWholeNumbers || (!walker.walksCopyForAnotherKindOfStorage() && (takesAnyNumber || (takesWholeNumbers && arrayStorageHolding(types, taken) != ArrayStorage::Int32))));
                if (goesByRowOfNumbers && (asksForInt32sFirst || (takesWholeNumbers && arrayStorageHolding(types, taken) == ArrayStorage::Int32)))
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false, false, false, true });
                else if (goesByRowOfNumbers)
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Double, !!(types.at(taken).primitives & Primitive::NumberWithoutFraction), false, false, true });
                else
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Contiguous, true, false, false, isNumericStorage(written) });
                if (asksForInt32sFirst && takesAnyNumber)
                    walker.noteCopyForInt32s();
                else if (goesByRowOfNumbers && mayTakeAnyNumber && !takesAnyNumber)
                    walker.count(StatisticID::CopiesForInt32sStruck);
                found.lists.last().hasAlternateStorage = asksForInt32sFirst;
                found.lists.last().alternateStorage = mayTakeWholeNumbers ? ArrayStorage::Contiguous : ArrayStorage::Double;
                continue;
            }
            TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id());
            written = kindWrittenOrSeenByLinkInListReadsInLoops(types, written, seenByLink);
            if (written == ArrayStorage::Other && elementIsOnlyReadByKeyInListReadsInLoops(index, operation))
                written = ArrayStorage::Contiguous;
            bool isNumeric = isNumericStorage(written);
            if (keyIsCounterAskedInFront && !isNumeric && written != ArrayStorage::Other)
                continue;
            if (keyIsCounterAskedInFront && !listsTakenUnderCountersAskedInFront.contains(name))
                listsTakenUnderCountersAskedInFront.append(name);
            bool wholeNumbersAreOnlyStoredOn = seenByLink && types.at(types.withoutNullish(seenByLink)).isNumberWithoutFraction() && onlyUserStoresWhatIsReadIntoListInListReadsInLoops(index, operation);
            bool holdsInt32sByLink = seenByLink && (types.at(seenByLink).isInt32OrNullish() || holdsKeysOfLoopInListReadsInLoops(walker, operation.id(), seenByLink));
            bool hasCarriedIndexes = !holdsInt32sByLink && (isNumeric || written == ArrayStorage::Other) && !walker.walksCopyForAnotherKindOfStorage() && isCarriedIndex(operation.id());
            if (holdsInt32sByLink || hasCarriedIndexes || (wholeNumbersAreOnlyStoredOn && !walker.walksCopyForAnotherKindOfStorage())) {
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                found.lists.last().hasAlternateStorage = !holdsInt32sByLink && !index.loop(index.loopOf(inputHead)).parent;
                found.lists.last().alternateStorage = ArrayStorage::Double;
                found.lists.last().keepsStorageWithoutAlternate = true;
                noteNumberBehindListsFound(operation, true);
                continue;
            }
            bool asksForDoublesFirst = isNumeric && !walker.walksCopyForAnotherKindOfStorage() && !walker.copyForInt32sIsStruck() && index.loop(loop).numberOfLoopsWithin > 1 && walker.hasUnrolledInnerLoops(inputHead);
            found.lists.append(HoistedArrayGuard { name, isNumeric ? ArrayStorage::Double : written, isNumeric && !asksForDoublesFirst });
            if (asksForDoublesFirst) {
                found.lists.last().hasAlternateStorage = true;
                found.lists.last().alternateStorage = ArrayStorage::Double;
                walker.noteCopyForInt32s();
            }
            if (isNumeric)
                noteNumberBehindListsFound(operation, false);
            else if (written == ArrayStorage::Other) {
                listsOfUnknownContents.append(name);
                readsOffListsOfUnknownContents.append({ operation.id(), name });
            }
        }
        noteWhatEdgesHandOn(member);
    }
    if (someListIsTakenForNotedKey && !rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), guesses.span(), { }))
        return findListsReadInLoopInListReadsInLoops(walker, inputHead, preview, entering, guesses, false, someReadHasNotedKey, namesOfCountersAskedInFront);
    found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
        return !isNumericStorage(list.storage) && listsTakenUnderCountersAskedInFront.contains(list.name);
    });
    bool someListIsTakenUnderCounter = found.lists.containsIf([&](const HoistedArrayGuard& list) {
        return listsTakenUnderCountersAskedInFront.contains(list.name);
    });
    if (someListIsTakenUnderCounter && !rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), guesses.span(), { })) {
        found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
            return listsTakenUnderCountersAskedInFront.contains(list.name);
        });
    }
    for (Reference name : listsReachedUnderCountersAskedInFront) {
        if (!listsTakenUnderCountersAskedInFront.contains(name))
            listsTakenUnderCountersAskedInFront.append(name);
    }
    for (Reference name : listsTakenUnderCountersAskedInFront) {
        if (!found.lists.containsIf([&](const HoistedArrayGuard& list) { return list.name == name; }))
            continue;
        walker.count(StatisticID::ArraysGuardedUnderSpeculatedCounters);
        found.someListIsTakenUnderCounterAskedInFront = true;
    }
    if (found.lists.isEmpty())
        return found;
    auto takesRowOfListedList = [&](BodyValue access, Opcode opcode) {
        ValueID row = access.opcode() == opcode ? index.stripCasts(access.objectOperand()) : ValueID();
        if (!row || row.index() < firstValueOfLoop || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet)
            return false;
        Reference holder = nameThatEntersLoopInListReadsInLoops(walker, index, index.value(row).objectOperand(), firstValueOfLoop);
        return holder && foundArrayLookup.find(found.lists.span(), holder) != notFound;
    };
    auto turnsTakeRows = [&](Opcode opcode) {
        return index.opcodes().contains(opcode) && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::any_of(index.block(member).values(), [&](BodyValue access) { return takesRowOfListedList(access, opcode); });
        });
    };
    bool storesIntoListsInFront = found.lists.containsIf([](const HoistedArrayGuard& list) { return list.isWritable; });
    bool turnsStoreIntoLists = storesIntoListsInFront || turnsTakeRows(Opcode::ElemSet);
    bool turnsReadRows = storesIntoListsInFront && turnsTakeRows(Opcode::ElemGet);
    bool holdsDoublesAlone = found.lists.containsIf([](const HoistedArrayGuard& list) {
        return !list.isWritable && list.hasAlternateStorage && list.storage == ArrayStorage::Double && list.alternateStorage == ArrayStorage::Double;
    });
    for (HoistedArrayGuard& list : found.lists) {
        list.turnsStoreIntoLists = turnsStoreIntoLists;
        list.leavesBirthAsItIs |= walker.walksCopyForAnotherKindOfStorage();
        if (list.isWritable && isNumericStorage(list.storage) && !turnsReadRows && !arraysNeedingExactStorage.contains(list.name)) {
            if (holdsDoublesAlone && list.storage == ArrayStorage::Double && listsThatTakeMoreThanInt32s.contains(list.name)) {
                list.hasAlternateStorage = true;
                list.alternateStorage = ArrayStorage::Other;
                continue;
            }
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
    SpeculationLookupByName speculationLookup;
    bool didSortNames = namesUsedEveryIteration.size() > ArrayLookupByName::linearSearchLimit;
    if (didSortNames)
        std::ranges::sort(namesUsedEveryIteration);
    found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
        bool needsOnlyStorageCheck = !list.isWritable && walker.types().isSubset(walker.typeOf(list.name), walker.types().builtinClass(BuiltinClasses::Array));
        bool isStoredIntoAnyway = list.isWritable && hasKind(types, types.withoutNullish(walker.ownTypeOf(list.name)), OperandKind::Array);
        bool hasSpeculation = speculationLookup.find(guesses.span(), list.name) != notFound;
        bool isUsedEveryIteration = didSortNames ? std::ranges::binary_search(namesUsedEveryIteration, list.name.id().index()) : namesUsedEveryIteration.contains(list.name.id().index());
        return !isUsedEveryIteration && !needsOnlyStorageCheck && !hasSpeculation && !isStoredIntoAnyway;
    });
    if (found.lists.isEmpty())
        return { };

    NamesBeforeLoop namesBeforeLoop(walker, inputHead);
    for (const HoistedArrayGuard& list : found.lists) {
        bool hasSpeculation = speculationLookup.find(guesses.span(), list.name) != notFound;
        if (hasSpeculation || isProvenArray(walker, list.name) || classOfProvenViewInListReadsInLoops(walker, list.name))
            continue;
        TypeID ownType = types.withoutNullish(walker.ownTypeOf(list.name));
        BuiltinClassID classOfView = classOfViewInListReadsInLoops(walker, ownType);
        TypeID asked = kindsOfViewsOfNumbersIn(walker.program(), types, ownType) ? ownType : types.builtinClass(classOfView ? classOfView : BuiltinClassID(BuiltinClasses::Array));
        guesses.append(HoistedSpeculation { list.name, asked, { }, namesBeforeLoop.findNamesOf(list.name) });
    }
    return found;
}

ListsReadInLoop findListsReadInLoop(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses)
{
    bool someReadHasNotedKey = false;
    std::optional<BitVector> namesOfCountersAskedInFront;
    ListsReadInLoop found = findListsReadInLoopInListReadsInLoops(walker, inputHead, preview, entering, guesses, false, someReadHasNotedKey, namesOfCountersAskedInFront);
    if (!found.lists.isEmpty() || !someReadHasNotedKey)
        return found;
    return findListsReadInLoopInListReadsInLoops(walker, inputHead, preview, entering, guesses, true, someReadHasNotedKey, namesOfCountersAskedInFront);
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

static Draft questionForKindOfStorageInListReadsInLoops(Walker& walker, const HoistedArrayGuard& tested, Origin origin)
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
static bool askWhatListIsAskedInListReadsInLoops(Walker& walker, const HoistedArrayGuard& tested, Origin origin, const Passes& passes)
{
    if (!passes(questionWhetherListIsAsMadeInListReadsInLoops(walker, tested.name, origin)))
        return false;
    return (!hasArrayStorageForm(tested.storage) && !tested.isWritable) || passes(questionForKindOfStorageInListReadsInLoops(walker, tested, origin));
}

static bool isFoundByReadAtSiteInListReadsInLoops(Walker& walker, Reference list)
{
    const Code& code = walker.code();
    ValueID held = list.id();
    while (valuePreservingOpcodes.contains(code.at(held).opcode))
        held = code.at(held).operand(0);
    return code.at(held).opcode == Opcode::GetValueAtSiteOrAbsent || code.at(held).opcode == Opcode::GetOwnValueUnderKeyOrAbsent || walker.types().isUninformative(code.at(held).type);
}

static std::optional<HoistedArrayGuard> tryHoistArrayGuard(Walker& walker, const HoistedArrayGuard& tested, Origin origin, Walker::IterationPurity purity, Walker::GuardedIterations guardedIterations)
{
    origin.isTailCall = false;
    auto passesInFront = [&](Draft&& draft) {
        Reference test;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
            test = walker.build(WTF::move(draft));
        }
        return test && walker.tryBranchInPreheader(test, true, purity, guardedIterations);
    };
    if (!askWhatListIsAskedInListReadsInLoops(walker, tested, origin, passesInFront))
        return std::nullopt;
    walker.recordSpeculation();
    walker.recordHoistedArrayGuard();
    if (purity != Walker::IterationPurity::EffectFree)
        walker.count(StatisticID::ArrayGuardsHoistedPastStores);
    noteListTestedInListReadsInLoops(walker, tested);
    if (tested.written)
        noteStorageOfWrittenList(walker, tested.name, tested.written);
    return tested;
}

static ArrayStorage kindThatTakersOfPatternSpeakForInListReadsInLoops(Walker& walker, const BodyValue& open)
{
    const BodyIndex& index = walker.currentBodyIndex();
    auto typeAsBuilt = [&](ValueID input) {
        Reference built = walker.referenceOf(input);
        return built ? walker.typeOf(built) : TypeID();
    };
    bool someValueIsTaken = false;
    for (const BodyMentionOfIteration& mention : index.mentionsOfIteration(open.id())) {
        if (index.opcodeOf(mention.user) != Opcode::IteratorResume)
            continue;
        for (BodyValue operation : index.block(mention.block).values()) {
            if (operation.opcode() != Opcode::ResultAt || operation.as<Opcode::ResultAt>().resultsOperand() != mention.user || operation.as<Opcode::ResultAt>().index() != static_cast<uint32_t>(IteratorResumeResult::Value) || index.isUnused(operation.id()))
                continue;
            if (!valueIsOnlyReckonedWithAsNumber(walker, operation.id(), TypesOfInputs(typeAsBuilt)))
                return ArrayStorage::Contiguous;
            someValueIsTaken = true;
        }
    }
    return someValueIsTaken ? ArrayStorage::Double : ArrayStorage::Other;
}

static ArrayStorage kindOfListTakenApartInListReadsInLoops(Walker& walker, const BodyValue& open, Reference list)
{
    ArrayStorage written = list && list.isValue() ? predictedArrayStorage(walker, walker.ownTypeOf(list)) : ArrayStorage::Other;
    if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, open.as<Opcode::IteratorOpen>().iterableOperand()).type : TypeID())
        written = predictedArrayStorage(walker, walker.types().withoutNullish(expected));
    return written == ArrayStorage::Other ? kindThatTakersOfPatternSpeakForInListReadsInLoops(walker, open) : written;
}

bool isDestructuredArrayLikelyNumeric(Walker& walker, const BodyValue& open, Reference list)
{
    return walker.has(Mechanism::Folding) && isNumericStorage(kindOfListTakenApartInListReadsInLoops(walker, open, list));
}

std::optional<HoistedArrayGuard> tryHoistDestructuredArrayGuard(Walker& walker, const BodyValue& open, Reference list)
{
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    if (!walker.has(Mechanism::Folding) || !list || !list.isValue() || !walker.isInvariantInSpecializedVersion(list, purity) || !walker.iterationsPreserveArrayGuards(list, purity))
        return std::nullopt;
    if (!isProvenArray(walker, list) || isFreshAllocation(walker, list))
        return std::nullopt;
    ArrayStorage written = kindOfListTakenApartInListReadsInLoops(walker, open, list);
    if (written == ArrayStorage::Other)
        return std::nullopt;
    bool isNumeric = isNumericStorage(written);
    return tryHoistArrayGuard(walker, HoistedArrayGuard { list, isNumeric ? ArrayStorage::Double : written, isNumeric }, walker.originOf(open), purity, Walker::GuardedIterations::Some);
}

static Reference holderThatRowIsReadOffInListReadsInLoops(Walker&, Reference row);

static ValueID holderOfRowAccess(const BodyIndex& index, const BodyValue& access)
{
    ValueID row = index.stripCasts(access.objectOperand());
    return row && !index.isParameter(row) && index.opcodeOf(row) == Opcode::ElemGet ? index.stripCasts(index.value(row).objectOperand()) : ValueID();
}

static bool userNeedsNumberBeside(Walker& walker, const BodyValue& read)
{
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID name = read.id();
    ValueID user = index.onlyUsingValue(name);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    if (!user || !arithmetic.contains(index.opcodeOf(user)))
        return true;
    BodyValue operation = index.value(user);
    ValueID other = operation.operand(0) == name ? operation.operand(1) : operation.operand(0);
    Reference built = other.index() < read.id().index() ? walker.referenceOf(other) : Reference();
    return built && !walker.types().at(walker.typeOf(built)).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
}

enum class RowStores : uint8_t { None, IntoOthers, IntoSameHolder };

static RowStores findRowStores(const BodyIndex& index, const BodyValue& read)
{
    LoopID loop = index.holdsAnyOf(OpcodeSet { Opcode::ElemSet }) && isReadOfBodyInListReadsInLoops(index, read) ? index.loopOf(index.blockOf(read.id())) : LoopID();
    if (!loop)
        return RowStores::None;
    RowStores found = RowStores::None;
    ValueID holder = holderOfRowAccess(index, read);
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::ElemSet)
                continue;
            if (holder && holderOfRowAccess(index, operation) == holder)
                return RowStores::IntoSameHolder;
            found = RowStores::IntoOthers;
        }
    }
    return found;
}

std::optional<HoistedArrayGuard> tryHoistArrayGuardForAccess(Walker& walker, const BodyValue& read, Reference list, ArrayStorage kindStoredInto, bool goesByKindOfStorage)
{
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    if (!walker.has(Mechanism::Folding) || !list || !list.isValue() || !walker.isInvariantInSpecializedVersion(list, purity) || !walker.iterationsPreserveArrayGuards(list, purity))
        return std::nullopt;
    if (!isProvenArray(walker, list) || isFreshAllocation(walker, list))
        return std::nullopt;
    ArrayStorage written = predictedArrayStorage(walker, walker.ownTypeOf(list));
    if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, read.objectOperand()).type : TypeID())
        written = predictedArrayStorage(walker, walker.types().withoutNullish(expected));
    TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
    if (written == ArrayStorage::Other && seenByLink && !walker.types().at(seenByLink).isAnything()) {
        ArrayStorage seen = arrayStorageHolding(walker.types(), seenByLink);
        if (isNumericStorage(seen) || !(walker.types().at(seenByLink).primitives & Primitive::Number))
            written = seen;
    }
    if (written == ArrayStorage::Other && kindStoredInto == ArrayStorage::Other && elementIsOnlyReadByKeyInListReadsInLoops(walker.currentBodyIndex(), read))
        written = ArrayStorage::Contiguous;
    auto typeAsBuilt = [&](ValueID input) {
        Reference built = walker.referenceOf(input);
        return built ? walker.typeOf(built) : TypeID();
    };
    if (written == ArrayStorage::Other && kindStoredInto == ArrayStorage::Other && read.opcode() == Opcode::ElemGet && isFoundByReadAtSiteInListReadsInLoops(walker, list) && valueIsOnlyReckonedWithAsNumber(walker, read.id(), TypesOfInputs(typeAsBuilt)))
        written = ArrayStorage::Double;
    bool isNumeric = isNumericStorage(written);
    HoistedArrayGuard tested { list, isNumeric ? ArrayStorage::Double : written, isNumeric };
    if (seenByLink && (walker.types().at(seenByLink).isInt32OrNullish() || (isReadOfBodyInListReadsInLoops(walker.currentBodyIndex(), read) && holdsKeysOfLoopInListReadsInLoops(walker, read.id(), seenByLink))))
        tested = HoistedArrayGuard { list, ArrayStorage::Int32, false };
    if (kindStoredInto != ArrayStorage::Other && goesByKindOfStorage && holderThatRowIsReadOffInListReadsInLoops(walker, list))
        tested = HoistedArrayGuard { list, ArrayStorage::Double, kindStoredInto == ArrayStorage::Int32, true, true };
    else if (kindStoredInto != ArrayStorage::Other)
        tested = goesByKindOfStorage && (kindStoredInto == ArrayStorage::Int32 || !isNumeric) ? HoistedArrayGuard { list, ArrayStorage::Other, kindStoredInto == ArrayStorage::Int32, true, true } : HoistedArrayGuard { list, kindStoredInto, false, true, true };
    else if (WrittenListOfVariable written = writtenListReadInListReadsInLoops(walker, walker.currentBodyIndex(), read)) {
        tested = HoistedArrayGuard { list, written.storage, false };
        tested.written = written;
    } else if (isNumericStorage(tested.storage) && holderThatRowIsReadOffInListReadsInLoops(walker, list)) {
        RowStores stores = findRowStores(walker.currentBodyIndex(), read);
        bool storesWholeNumbers = stores == RowStores::IntoSameHolder && seenByLink && !isNeverType(seenByLink) && walker.types().at(seenByLink).hasOnlyPrimitives(Primitive::NumberWithoutFraction | Primitive::Undefined);
        if (storesWholeNumbers)
            tested = HoistedArrayGuard { list, ArrayStorage::Int32, false };
        if (stores == RowStores::IntoSameHolder && seenByLink && !walker.types().at(seenByLink).isAnything() && (walker.types().at(seenByLink).primitives & Primitive::FractionalNumber))
            tested.admitsInt32Storage = false;
        bool leavesAtHole = tested.storage == ArrayStorage::Int32 || !onlyUserTakesUndefinedForNaNInTypedElements(walker, read, WhatStandsBesideRead::NumberOrNothing);
        tested.turnsStoreIntoLists = stores != RowStores::None && leavesAtHole && (!tested.admitsInt32Storage || userNeedsNumberBeside(walker, read));
    }
    return tryHoistArrayGuard(walker, tested, walker.originOf(read), purity, kindStoredInto == ArrayStorage::Other ? Walker::GuardedIterations::Some : Walker::GuardedIterations::All);
}

bool isExpectedToBeListOfNumbers(Walker& walker, TypeID expected)
{
    return isNumericStorage(predictedArrayStorage(walker, expected));
}

bool canGuardArrayInIteration(Walker& walker, Reference list)
{
    return walker.has(Mechanism::Folding) && list && list.isValue() && walker.canBuildIterationGuard() && walker.canRestartIteration() && walker.iterationsPreserveArrayGuards(list) && walker.hasNoElementStores() && !isFreshAllocation(walker, list);
}

static bool buildIterationGuard(Walker& walker, Draft&& draft)
{
    Reference test = walker.build(WTF::move(draft));
    if (!test)
        return false;
    BlockID passed = walker.addBlock();
    BlockID failed = walker.addBlock(BlockFrequency::Rare);
    walker.branch(test, passed, failed);
    walker.continueIn(failed);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(passed);
    return !walker.isUnreachable() && !walker.hasFailed();
}

bool guardPristineArrayInIteration(Walker& walker, Reference list, Origin origin)
{
    return buildIterationGuard(walker, questionWhetherListIsAsMadeInListReadsInLoops(walker, list, origin));
}

Reference askWhetherListIsAsMade(Walker& walker, Reference list, Origin origin)
{
    return walker.build(questionWhetherListIsAsMadeInListReadsInLoops(walker, list, origin));
}

Reference askWhetherStorageOfListIsOfKind(Walker& walker, Reference list, ArrayStorage kind, Origin origin, bool isWritable)
{
    return walker.build(questionForKindOfStorageInListReadsInLoops(walker, HoistedArrayGuard { list, kind, false, isWritable }, origin));
}

static constexpr uint32_t wordOfKindOfRowsInListReadsInLoops = 1;

static Reference holderThatRowIsReadOffInListReadsInLoops(Walker& walker, Reference row)
{
    const Code& code = walker.code();
    ValueID held = row.id();
    while (valuePreservingOpcodes.contains(code.at(held).opcode))
        held = code.at(held).operand(0);
    return code.at(held).opcode == Opcode::GetArrayElement ? Reference::value(code.at(held).operand(0)) : Reference();
}

void noteKindOfStorageThatTurnFound(Walker& walker, Reference list, ArrayStorage kind, bool isWritable)
{
    noteListTestedInListReadsInLoops(walker, HoistedArrayGuard { list, kind, false, isWritable });
    if (Reference holder = holderThatRowIsReadOffInListReadsInLoops(walker, list))
        noteKnown(walker, KnownNote::HoistedArrayGuard, holder, { }, wordOfKindOfRowsInListReadsInLoops, LocationMasks(heapsUnderListsReadInLoops), static_cast<uint64_t>(kind));
}

std::optional<HoistedArrayGuard> tryGuardNumericArrayInIteration(Walker& walker, const BodyValue& read, Reference list)
{
    if (!canGuardArrayInIteration(walker, list) || !isProvenArray(walker, list) || !isExpectedToBeListOfNumbers(walker, walker.ownTypeOf(list)))
        return std::nullopt;
    HoistedArrayGuard tested { list, ArrayStorage::Double, true };
    if (Reference holder = holderThatRowIsReadOffInListReadsInLoops(walker, list)) {
        if (std::optional<uint64_t> kind = findKnown(walker, KnownNote::HoistedArrayGuard, holder, { }, wordOfKindOfRowsInListReadsInLoops); kind && static_cast<ArrayStorage>(*kind) == ArrayStorage::Contiguous) {
            tested = HoistedArrayGuard { list, ArrayStorage::Contiguous, false };
            walker.count(StatisticID::RowsGuardedForSiblingIndexingType);
        }
    }
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    auto passesInTurn = [&](Draft&& draft) {
        return buildIterationGuard(walker, WTF::move(draft));
    };
    if (!askWhatListIsAskedInListReadsInLoops(walker, tested, origin, passesInTurn))
        return std::nullopt;
    walker.recordSpeculation();
    walker.count(StatisticID::ArraysGuardedInIterations);
    noteListTestedInListReadsInLoops(walker, tested);
    return tested;
}

std::optional<HoistedArrayGuard> tryGuardDestructuredArrayInIteration(Walker& walker, const BodyValue& open, Reference list)
{
    if (!canGuardArrayInIteration(walker, list) || !isProvenArray(walker, list))
        return std::nullopt;
    ArrayStorage written = kindOfListTakenApartInListReadsInLoops(walker, open, list);
    if (written == ArrayStorage::Other)
        return std::nullopt;
    bool isNumeric = isNumericStorage(written);
    HoistedArrayGuard tested { list, isNumeric ? ArrayStorage::Double : written, isNumeric };
    Origin origin = walker.originOf(open);
    origin.isTailCall = false;
    auto passesInTurn = [&](Draft&& draft) {
        return buildIterationGuard(walker, WTF::move(draft));
    };
    if (isKnownPristine(walker, list) ? !passesInTurn(questionForKindOfStorageInListReadsInLoops(walker, tested, origin)) : !askWhatListIsAskedInListReadsInLoops(walker, tested, origin, passesInTurn))
        return std::nullopt;
    walker.recordSpeculation();
    walker.count(StatisticID::ArraysGuardedInIterations);
    noteListTestedInListReadsInLoops(walker, tested);
    return tested;
}

std::optional<PassedTest> tryHoistElementPresenceCheck(Walker& walker, const BodyValue& read, const HoistedArrayGuard& tested, Reference list, Reference position)
{
    bool isNumber = isNumericStorage(tested.storage);
    if (!walker.has(Mechanism::Folding) || walker.listsAreStruckInRow() || (tested.storage != ArrayStorage::Contiguous && !isNumber) || holdsBoxedValuesOfEitherKind(tested) || tested.isWritable || (isNumber && tested.turnsStoreIntoLists))
        return std::nullopt;
    Walker::IterationPurity purity = isNumber ? Walker::IterationPurity::EffectFree : walker.arrayGuardPurity();
    if (!walker.isInvariantInSpecializedVersion(list, purity) || !walker.isInvariantInSpecializedVersion(position, purity) || !walker.iterationsPreserveArrayGuards(list, purity) || !(isNumber ? walker.hasNoElementStores() : walker.areElementStoresNumeric(purity)))
        return std::nullopt;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    Reference isThere;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        Draft isPresent(Opcode::IsElementPresent, origin);
        walker.chooseConservativeFields(isPresent);
        isPresent.setArrayOperand(list);
        isPresent.setPositionOperand(position);
        isPresent.setArrayStorage(tested.storage);
        isPresent.setAdmitsInt32Storage(tested.admitsInt32Storage);
        isPresent.setKeyIsIntegral(true);
        isThere = walker.build(WTF::move(isPresent));
    }
    std::optional<PassedTest> present = isThere ? walker.showElementPresentBehindTest(list, position, walker.tryHoistConditionalCheck(isThere, true, purity)) : std::nullopt;
    if (!present)
        return std::nullopt;
    walker.recordSpeculation();
    if (isNumber) {
        walker.recordHoistedNumberRead();
        walker.count(StatisticID::NumericElementReadsHoisted);
    } else
        walker.recordHoistedElementCheck();
    return present;
}

static bool continueIfGuardPasses(Walker& walker, Reference test, BlockID& genericStart, bool shouldRecordEdgeOutcomes = false)
{
    if (!test)
        return false;
    BlockID passed = walker.addBlock();
    if (!genericStart)
        genericStart = walker.addBlock();
    walker.branch(test, passed, genericStart, shouldRecordEdgeOutcomes);
    walker.continueIn(passed);
    return !walker.isUnreachable();
}

static bool isBornToHoldDoublesInListReadsInLoops(Walker& walker, Reference list)
{
    const Operation& born = walker.code().at(underlyingCellOf(walker.code(), list.id()));
    return born.opcode == Opcode::NewArrayLike && !born.sourceOperand() && born.arrayStorage() == ArrayStorage::Double;
}

static void appendViewsReadOutOfVariablesInListReadsInLoops(Walker& walker, BlockID inputHead, ListsReadInLoop& found)
{
    if (walker.isUnreachable() || walker.hasFailed() || walker.effectsOfQuietDraftsOfRow().writes.contains(AbstractHeap::TypedArrayStorage))
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.opcodes().contains(Opcode::CheckInitialized) || !index.opcodes().contains(Opcode::GetModuleVariable))
        return;
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::ElemGet && operation.opcode() != Opcode::ElemSet)
                continue;
            ValueID checked = index.stripCasts(operation.objectOperand());
            if (!checked || index.isParameter(checked) || index.opcodeOf(checked) != Opcode::CheckInitialized)
                continue;
            ValueID read = index.stripCasts(index.value(checked).operand(0));
            if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable || !leavesDeadZoneForGood(walker.program(), index.value(read).variable()))
                continue;
            Reference name = walker.state().variable(index.value(read).variable());
            BuiltinClassID classOfView = classOfProvenViewInListReadsInLoops(walker, name);
            if (!classOfView || !hasInlineElementAccess(walker.program().builtins().typedArrayTypeOf(classOfView)) || !viewMayFollowSharedMemoryThatGrows(walker, name) || findNoteInListReadsInLoops(walker, name))
                continue;
            if (found.lists.containsIf([&](const HoistedArrayGuard& list) { return list.name == name; }))
                continue;
            found.lists.append(HoistedArrayGuard { name, ArrayStorage::Other, false });
            found.lists.last().isAskedForItsStorageAlone = true;
        }
    }
}

void buildTestsOfListsReadInLoop(Walker& walker, BlockID inputHead, std::span<const Reference> entering, ListsReadInLoop& found, std::span<const HoistedSpeculation> guesses, std::span<const Reference> proofs, BlockID& genericStart, Origin origin)
{
    appendViewsReadOutOfVariablesInListReadsInLoops(walker, inputHead, found);
    if (found.lists.isEmpty() || walker.isUnreachable() || walker.hasFailed())
        return;

    SpeculationLookupByName provenSpeculationLookup;
    for (HoistedArrayGuard& list : found.lists) {
        if (size_t place = provenSpeculationLookup.find(guesses.first(std::min(guesses.size(), proofs.size())), list.name); place != notFound)
            list.name = proofs[place];
    }
    found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
        return !isProvenArray(walker, list.name) && !classOfProvenViewInListReadsInLoops(walker, list.name);
    });
    if (found.lists.isEmpty())
        return;
    if (!found.areLeftAsTheyAre && !rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), guesses, proofs))
        return;

    static constexpr size_t arraysPerBlockRelease = ArrayLookupByName::linearSearchLimit;
    auto releaseEndedBlocksPeriodically = [&](const HoistedArrayGuard& list) {
        if (size_t place = &list - found.lists.begin(); place && !(place % arraysPerBlockRelease))
            walker.releaseEndedPendingBlocks();
    };
    for (HoistedArrayGuard& list : found.lists) {
        releaseEndedBlocksPeriodically(list);
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
            walker.count(StatisticID::TypedArrayLengthGuardsAllowingSubclasses);
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
        if (!continueIfGuardPasses(walker, test, genericStart))
            return;
        if (lengthAskedByItsByte) {
            if (!continueIfGuardPasses(walker, buildTestOfMemberAsBuilt(walker, lengthAskedByItsByte, origin), genericStart))
                return;
            noteMemberPassedTestOfBeingAsBuilt(walker, lengthAskedByItsByte);
            for (BuiltinID holder : holdersBelowLength) {
                if (!continueIfGuardPasses(walker, buildTestOfHolderWithoutAddedNames(walker, holder, origin), genericStart))
                    return;
                noteHolderPassedTestOfHavingNoAddedNames(walker, holder);
            }
        }
        if (classOfView && isAskedOfCell && (isToldByItsStructure || aspects.contains(PristineAspects::Storage)))
            noteKnown(walker, KnownNote::ViewOverMemoryThatNoOtherThreadGrows, list.name, { }, 0, LocationMasks(), 1);
    }
    for (const HoistedArrayGuard& list : found.lists) {
        if (!isConvertedInPreheader(list))
            continue;
        Draft convert(Opcode::ConvertToDoubleStorage, origin);
        walker.chooseConservativeFields(convert);
        convert.setArrayOperand(list.name);
        walker.build(WTF::move(convert));
        walker.count(StatisticID::ArraysConvertedToDoubleStorage);
    }
    for (const HoistedArrayGuard& list : found.lists) {
        std::optional<uint64_t> fills = isConvertedInPreheader(list) ? findKnown(walker, KnownNote::LoopAppendsToLiteral, list.name, { }, inputHead.index()) : std::nullopt;
        std::optional<uint64_t> wouldFill = fills && *fills >> 32 == walker.currentFunction().index() ? findKnown(walker, KnownNote::CapacityForRemainingIterations, list.name, { }, inputHead.index()) : std::nullopt;
        if (!wouldFill)
            continue;
        Reference readUnderCounter = walker.referenceOf(ValueID(static_cast<uint32_t>(*wouldFill >> 32)));
        for (size_t place = 0; place < guesses.size() && place < proofs.size(); ++place) {
            if (guesses[place].value != readUnderCounter)
                continue;
            readUnderCounter = proofs[place];
            break;
        }
        if (readUnderCounter == list.name || !isProvenArray(walker, readUnderCounter))
            continue;
        Reference requiredCapacity = Reference::fromBits(static_cast<uint32_t>(*wouldFill));
        Reference length = buildArrayLength(walker, readUnderCounter, origin);
        Reference room = length ? buildIntegerMinimum(walker, requiredCapacity, length, origin) : Reference();
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        if (!room)
            continue;
        Draft reserve(Opcode::ReserveArrayCapacity, origin);
        walker.chooseConservativeFields(reserve);
        reserve.setArrayOperand(list.name);
        reserve.setCapacityOperand(room);
        walker.build(WTF::move(reserve));
        walker.count(StatisticID::CapacitiesReservedFromSourceArrays);
    }
    for (HoistedArrayGuard& list : found.lists) {
        releaseEndedBlocksPeriodically(list);
        if (!hasArrayStorageForm(list.storage) && !list.isWritable)
            continue;
        if (list.written) {
            Draft isStillWritten(Opcode::HasArrayStorage, origin);
            walker.chooseConservativeFields(isStillWritten);
            isStillWritten.setArrayOperand(list.name);
            isStillWritten.setArrayStorage(list.storage);
            isStillWritten.setData(list.written.data);
            if (!continueIfGuardPasses(walker, walker.build(WTF::move(isStillWritten)), genericStart))
                return;
            noteStorageOfWrittenList(walker, list.name, list.written);
            continue;
        }
        if (!list.leavesBirthAsItIs)
            askBirthOfListForStorage(walker, list.name, hasArrayStorageForm(list.storage) ? list.storage : list.admitsInt32Storage ? ArrayStorage::Int32 : ArrayStorage::Double);
        if (list.storage == ArrayStorage::Double && list.admitsInt32Storage && isBornToHoldDoublesInListReadsInLoops(walker, list.name)) {
            list.admitsInt32Storage = false;
            walker.count(StatisticID::ArrayGuardsNarrowedToDouble);
        }
        Draft hasKindWritten(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(hasKindWritten);
        hasKindWritten.setArrayOperand(list.name);
        hasKindWritten.setArrayStorage(list.storage);
        hasKindWritten.setAdmitsInt32Storage(list.admitsInt32Storage);
        hasKindWritten.setIsWritable(list.isWritable);
        Reference storageCheck = walker.build(WTF::move(hasKindWritten));
        if (!walker.shouldSkipStorageChecks() && !continueIfGuardPasses(walker, storageCheck, list.hasAlternateStorage ? found.alternateStorageEntry : genericStart))
            return;
    }
    for (const HoistedArrayGuard& list : found.lists) {
        if (list.isAskedForItsStorageAlone)
            continue;
        noteListTestedInListReadsInLoops(walker, list);
        if (list.takesInstancesBelow)
            noteThatLengthOfViewIsFoundAbove(walker, list.name);
        else if (classOfProvenViewInListReadsInLoops(walker, list.name))
            markObjectPristine(walker, list.name);
    }
    for (const HoistedArrayGuard& list : found.lists) {
        std::optional<uint64_t> noted = list.isWritable ? findKnown(walker, KnownNote::LoopAppendsToLiteral, list.name, { }, inputHead.index()) : std::nullopt;
        if (!noted || *noted >> 32 != walker.currentFunction().index() || static_cast<uint32_t>(*noted) > entering.size())
            continue;
        Reference length = buildArrayLength(walker, list.name, origin);
        if (!length || !continueIfGuardPasses(walker, buildIntegerOperator(walker, Opcode::StrictEq, length, entering[static_cast<uint32_t>(*noted) - 1], origin), genericStart))
            return;
        noteKnown(walker, KnownNote::LengthEqualsInitialCounter, list.name, { }, inputHead.index(), LocationMasks(AbstractHeapSet { AbstractHeap::ArrayStorage }), *noted);
        std::optional<uint64_t> filledTo = findKnown(walker, KnownNote::FinalLiteralLength, list.name, { }, inputHead.index());
        if (!filledTo)
            continue;
        Draft room(Opcode::GetArrayCapacity, origin);
        walker.chooseConservativeFields(room);
        room.setArrayOperand(list.name);
        Reference preheaderCapacity = walker.build(WTF::move(room));
        Reference loopCapacityGuard = preheaderCapacity ? buildIntegerOperator(walker, Opcode::LessEq, Reference::fromBits(static_cast<uint32_t>(*filledTo)), preheaderCapacity, origin) : Reference();
        if (!loopCapacityGuard || !continueIfGuardPasses(walker, loopCapacityGuard, genericStart, true))
            return;
        noteKnown(walker, KnownNote::CapacityCoversLoop, list.name, { }, inputHead.index(), LocationMasks(AbstractHeapSet { AbstractHeap::ArrayStorage }), *noted);
        noteKnown(walker, KnownNote::LoopCapacityGuard, list.name, { }, inputHead.index(), LocationMasks(AbstractHeapSet { AbstractHeap::ArrayStorage }), loopCapacityGuard.bits());
        walker.count(StatisticID::LoopCapacityGuardsEmitted);
    }
    found.areTested = true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
