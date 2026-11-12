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
#include "AOTWalkBirths.h"
#include "AOTWalkBlocksThatEveryTurnPasses.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkKnown.h"
#include "AOTWalkNumberGuesses.h"
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

static constexpr uint64_t subclassesWithLength = 2;

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
static constexpr unsigned shiftOfOnlyHoldsRows = 43;

static void noteListTestedInListReadsInLoops(Walker& walker, const HoistedArrayGuard& tested)
{
    uint64_t noted = tested.name.bits()
        | static_cast<uint64_t>(tested.storage) << shiftOfStorageInListReadsInLoops
        | static_cast<uint64_t>(tested.admitsInt32Storage) << shiftOfSecondKindInListReadsInLoops
        | static_cast<uint64_t>(tested.isWritable) << shiftOfWritableInListReadsInLoops
        | static_cast<uint64_t>(tested.turnsStoreIntoLists) << shiftOfStoresInListReadsInLoops
        | static_cast<uint64_t>(tested.onlyHoldsRows) << shiftOfOnlyHoldsRows;
    noteKnown(walker, KnownNote::HoistedArrayGuard, tested.name, { }, 0, LocationMasks(classOfProvenViewInListReadsInLoops(walker, tested.name) ? heapsUnderViewsReadInLoops : heapsUnderListsReadInLoops), noted);
}

static std::optional<HoistedArrayGuard> findNoteInListReadsInLoops(Walker& walker, Reference list)
{
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::HoistedArrayGuard, list);
    if (!noted)
        return std::nullopt;
    HoistedArrayGuard guard {
        Reference::fromBits(static_cast<uint32_t>(*noted)),
        static_cast<ArrayStorage>(*noted >> shiftOfStorageInListReadsInLoops & 0xff),
        !!(*noted >> shiftOfSecondKindInListReadsInLoops & 1),
        !!(*noted >> shiftOfWritableInListReadsInLoops & 1),
        !!(*noted >> shiftOfStoresInListReadsInLoops & 1)
    };
    guard.onlyHoldsRows = *noted >> shiftOfOnlyHoldsRows & 1;
    return guard;
}

static bool isProvenArray(const Walker&, Reference name);

std::optional<uint64_t> findArrayGuard(Walker& walker, const WalkState& state, Reference array)
{
    while (Reference checked = array ? state.checkedNameOf(array) : Reference())
        array = checked;
    if (!array || !array.isValue() || !isProvenArray(walker, array))
        return std::nullopt;
    std::optional<uint64_t> noted = findKnown(walker, state, KnownNote::HoistedArrayGuard, array);
    return noted ? std::optional(*noted >> shiftOfStorageInListReadsInLoops) : std::nullopt;
}

void noteJoinedArrayGuard(Walker& walker, Reference joined, uint64_t guard)
{
    if (isProvenArray(walker, joined))
        noteKnown(walker, KnownNote::HoistedArrayGuard, joined, { }, 0, LocationMasks(heapsUnderListsReadInLoops), joined.bits() | guard << shiftOfStorageInListReadsInLoops);
}

static bool isAskedForStoresAlone(const HoistedArrayGuard& tested)
{
    return tested.isWritable && !hasArrayStorageForm(tested.storage);
}

static bool isShownToReads(const HoistedArrayGuard& tested)
{
    return !tested.onlyHoldsRows && !isAskedForStoresAlone(tested) && accessToElementsHasCode(Opcode::GetArrayElement, tested.storage, tested.admitsInt32Storage, false);
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

static bool allocationsSpareLists(const Walker& walker, std::span<const HoistedArrayGuard> lists, const Walker::AllocationsUsedInLoop& mentioned)
{
    return std::ranges::all_of(lists, [&](const HoistedArrayGuard& list) {
        std::optional<Walker::IsListedAllocation> other = walker.classifyAgainstAllocations(mentioned, list.name);
        return other && (*other == Walker::IsListedAllocation::No || !list.isWritable);
    });
}

bool preservesHoistedArrayGuards(const Walker& walker, const Effects& effects, std::span<const HoistedArrayGuard> lists, const Walker::AllocationsUsedInLoop& mentioned)
{
    if (effects.mayRunOtherCode())
        return false;
    if (!writesWhatLiesUnderListsReadInLoops(walker, effects))
        return true;
    return effects.writesObjectsMadeHereAlone && allocationsSpareLists(walker, lists, mentioned);
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

static ValueID readOfVariableInLoopInListReadsInLoops(const Program& program, const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    ValueID read = valueThatEntersLoopInListReadsInLoops(index, value, firstValueOfLoop);
    if (read && read.index() >= firstValueOfLoop && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = valueThatEntersLoopInListReadsInLoops(index, index.value(read).operand(0), firstValueOfLoop);
    if (!read || read.index() < firstValueOfLoop || index.isParameter(read))
        return { };
    if (index.opcodeOf(read) == Opcode::GetModuleVariable)
        return read;
    return index.opcodeOf(read) == Opcode::GetEnvironmentField && isLoopInvariantScope(program, index, index.value(read).environmentOperand(), firstValueOfLoop) ? read : ValueID();
}

static ValueID findArrayRead(const BodyIndex& index, ValueID input)
{
    ValueID source = index.stripCasts(input);
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
        source = index.stripCasts(index.value(source).operand(0));
    return source && !index.isParameter(source) ? source : ValueID();
}

static WrittenListOfVariable writtenListReadInListReadsInLoops(Walker& walker, const BodyIndex& index, const BodyValue& read)
{
    ValueID source = read.opcode() == Opcode::ElemGet ? findArrayRead(index, read.objectOperand()) : ValueID();
    if (!source || (index.opcodeOf(source) != Opcode::GetModuleVariable && index.opcodeOf(source) != Opcode::GetEnvironmentField))
        return { };
    if (!walker.facts().linked().nobodyChangesReceiverAt(walker.currentFunction(), read.id()))
        return { };
    BodyValue list = index.value(source);
    return list.opcode() == Opcode::GetModuleVariable ? writtenListThatVariableHolds(walker, list.variable()) : literalArrayInInstance(walker, { list.layout(), list.field() });
}

static bool textOfLoopStoresVariableInListReadsInLoops(const BodyIndex& index, const BodyValue& read)
{
    LoopID loop = index.loopOf(index.blockOf(read.id()));
    while (LoopID around = loop ? index.loop(loop).parent : LoopID())
        loop = around;
    return !loop || outermostLoopStoresWhatIsRead(index, loop, read);
}

static Reference valueThatStateHoldsOfVariableInListReadsInLoops(Walker& walker, const BodyIndex& index, const BodyValue& read, uint32_t firstValueOfLoop)
{
    if (textOfLoopStoresVariableInListReadsInLoops(index, read))
        return { };
    if (read.opcode() == Opcode::GetModuleVariable)
        return walker.state().variable(read.variable());
    Reference environment = loopInvariantScope(walker, index, read.environmentOperand(), firstValueOfLoop);
    return environment ? walker.state().environmentField(environment, FieldReference { read.layout(), read.field() }) : Reference();
}

static Reference nameThatEntersLoopInListReadsInLoops(Walker&, const BodyIndex&, ValueID, uint32_t firstValueOfLoop);

static Reference valueOfFieldInPreheader(Walker& walker, Reference holder, PropertyKeyID key)
{
    TypeID type = holder && holder.isValue() ? walker.types().withoutNullish(walker.typeOf(holder)) : TypeID();
    if (!type || isNeverType(type) || !walker.types().at(type).isObject() || !walker.types().at(type).hasOnlyLayouts())
        return { };
    std::optional<FieldReference> field = findField(walker.program(), membersOf(walker.types(), walker.types().at(type))[0], key);
    if (Reference held = field ? walker.state().slot(holder, *field) : Reference())
        return held;
    std::optional<uint64_t> array = field ? findKnown(walker, KnownNote::FieldArrayInPreheader, holder, { }, key.index()) : std::nullopt;
    return array ? Reference::fromBits(static_cast<uint32_t>(*array)) : Reference();
}

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
    if (Reference field = findHoistedUnknownField(walker, key, holder))
        return field;
    return valueOfFieldInPreheader(walker, holder, key);
}

static ValueID fieldPushedOnto(const Program&, const BodyIndex&, const BodyValue& call);
static ValueID findMethodReceiverField(const Program&, const BodyIndex&, const BodyValue& call, BuiltinName method, size_t numberOfArguments);

static ValueID valuePassedIntoCopy(Walker& walker, const BodyIndex& body, const BodyValue& call, FunctionID callee, ValueID value)
{
    const Function& function = walker.program().at(callee);
    value = body.stripCasts(value);
    if (!value || !body.isParameter(value) || body.blockOf(value) != entryBlock)
        return { };
    unsigned place = value.index() - body.block(entryBlock).parameter(0).index();
    if (place == function.thisParameterIndex())
        return call.as<Opcode::Call>().thisValueOperand();
    unsigned first = function.firstParameterIndex();
    std::span<const ValueID> arguments = call.as<Opcode::Call>().argumentsOperands();
    return place >= first && place - first < function.numberOfPositionalParameters() && place - first < arguments.size() ? arguments[place - first] : ValueID();
}

ValueID Walker::findReturnedElement(const BodyIndex& body)
{
    static constexpr OpcodeSet elements { Opcode::ElemGet, Opcode::Call };
    if (!body.opcodes().contains(Opcode::PropGet) || !body.holdsAnyOf(elements))
        return { };
    ValueID element;
    for (uint32_t block = 0; block < body.numberOfBlocks(); ++block) {
        BodyTerminator end = body.terminator(BlockID(block));
        if (!body.isReachable(BlockID(block)) || (end.kind() != TerminatorKind::Return && end.kind() != TerminatorKind::ReturnCall))
            continue;
        ValueID answer = end.operand(0) ? body.stripCasts(end.operand(0)) : ValueID();
        if (answer && !body.isParameter(answer) && body.opcodeOf(answer) == Opcode::Undefined)
            continue;
        if (!answer || element || body.isParameter(answer))
            return { };
        if (body.opcodeOf(answer) != Opcode::ElemGet && !findMethodReceiverField(program(), body, body.value(answer), BuiltinNames::pop, 0))
            return { };
        element = answer;
    }
    return element;
}

static void forEachFieldAccessInCopy(Walker& walker, const BodyValue& call, FunctionID callee, Opcode opcode, const Invocable<void(ValueID holder, PropertyKeyID field, ValueID key, BodyValue access)> auto& take)
{
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || !body->opcodes().contains(opcode) || (opcode == Opcode::ElemGet && body->opcodes().contains(Opcode::PropSet)))
        return;
    auto handedIn = [&](ValueID value) {
        return valuePassedIntoCopy(walker, *body, call, callee, value);
    };
    for (uint32_t block = 0; block < body->numberOfBlocks(); ++block) {
        for (BodyValue access : body->block(BlockID(block)).values()) {
            if (access.opcode() != opcode || (opcode == Opcode::ElemGet && BlockID(block) == entryBlock))
                continue;
            if (opcode == Opcode::Call) {
                ValueID field = fieldPushedOnto(walker.program(), *body, access);
                if (ValueID holder = field ? handedIn(body->value(field).objectOperand()) : ValueID())
                    take(holder, body->value(field).key(), handedIn(access.as<Opcode::Call>().argumentsOperands()[0]), access);
                continue;
            }
            ValueID list = body->stripCasts(access.objectOperand());
            if (!list || body->isParameter(list) || body->opcodeOf(list) != Opcode::PropGet || body->value(list).isFreeName())
                continue;
            if (ValueID holder = handedIn(body->value(list).objectOperand()))
                take(holder, body->value(list).key(), handedIn(findUpdatedKey(walker, *body, access.propertyOperand())), access);
        }
    }
}

static Reference nameThatEntersLoopInListReadsInLoops(Walker& walker, const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    ValueID enters = valueThatEntersLoopInListReadsInLoops(index, value, firstValueOfLoop);
    if (!enters)
        return { };
    if (enters.index() < firstValueOfLoop)
        return walker.referenceOf(enters);
    if (!index.isParameter(enters) && index.opcodeOf(enters) == Opcode::ElemGet) {
        if (Reference row = walker.findHoistedRow(enters))
            return row;
    }
    if (Reference field = !index.isParameter(enters) && index.opcodeOf(enters) == Opcode::PropGet ? walker.findHoistedField(enters) : Reference())
        return field;
    ValueID read = readOfVariableInLoopInListReadsInLoops(walker.program(), index, enters, firstValueOfLoop);
    if (!read && !index.isParameter(enters) && index.opcodeOf(enters) == Opcode::ElemGet && walker.standsInCopyOfLoopBuiltForEachMember())
        return rowFoundUnderKeyInFrontOfLoop(walker, index.value(enters));
    if (!read)
        return valueThatStateHoldsInSlotReadInTurnInListReadsInLoops(walker, index, enters, firstValueOfLoop);
    if (Reference made = index.opcodeOf(read) == Opcode::GetModuleVariable ? objectOfVariablePutOff(walker, index.value(read).variable()) : Reference())
        return made;
    Reference environment = index.opcodeOf(read) == Opcode::GetEnvironmentField ? loopInvariantScope(walker, index, index.value(read).environmentOperand(), firstValueOfLoop) : Reference();
    if (Reference kept = valueOfStableBinding(walker, index.value(read), environment))
        return kept;
    return valueThatStateHoldsOfVariableInListReadsInLoops(walker, index, index.value(read), firstValueOfLoop);
}

void forgetStorageOfStoredLists(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto listBeforeLoop = [&](ValueID stored) -> Reference {
        ValueID enters = valueThatEntersLoopInListReadsInLoops(index, stored, firstValueOfLoop);
        while (enters && enters.index() >= firstValueOfLoop && index.isParameter(enters) && index.valueNarrowedInto(enters))
            enters = valueThatEntersLoopInListReadsInLoops(index, index.valueNarrowedInto(enters), firstValueOfLoop);
        if (!enters || enters.index() < firstValueOfLoop)
            return enters ? walker.referenceOf(enters) : Reference();
        ValueID read = readOfVariableInLoopInListReadsInLoops(walker.program(), index, enters, firstValueOfLoop);
        return read ? valueThatStateHoldsOfVariableInListReadsInLoops(walker, index, index.value(read), firstValueOfLoop) : Reference();
    };
    Effects effects = declaredBoundOf(Opcode::ConvertToDoubleStorage);
    effects.merge(declaredBoundOf(Opcode::ReserveArrayCapacity));
    uint32_t restartMark = walker.state().restartMark();
    Vector<Reference, 4> lists;
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            bool isCall = operation.opcode() == Opcode::Call;
            ValueID stored = operation.opcode() == Opcode::ElemSet ? operation.objectOperand() : isCall ? operation.thisValueOperand() : ValueID();
            Reference list = stored ? listBeforeLoop(stored) : Reference();
            if (!list || !list.isValue() || lists.contains(list))
                continue;
            lists.append(list);
            Draft convert(Opcode::ConvertToDoubleStorage, walker.originOf(operation));
            walker.chooseConservativeFields(convert);
            convert.setArrayOperand(list);
            endWhatOperationWritesOrRuns(walker, convert, effects);
        }
    }
    if (!lists.isEmpty())
        walker.state().restoreRestartMark(restartMark);
}

Reference arrayNameAtHeader(Walker& walker, BlockID inputHead, ValueID array)
{
    const BodyIndex& index = walker.currentBodyIndex();
    return nameThatEntersLoopInListReadsInLoops(walker, index, array, index.block(inputHead).firstParameter().index());
}

Vector<ChosenArray, 3> findChosenArrays(Walker& walker, const BodyIndex& index, ValueID array, uint32_t firstValueOfLoop)
{
    static constexpr size_t maxChosenArrays = 4;
    Vector<ChosenArray, 3> found;
    Vector<ValueID, 4> joins { index.stripCasts(array) };
    for (size_t place = 0; place < joins.size(); ++place) {
        ValueID parameter = joins[place];
        if (!parameter || parameter.index() < firstValueOfLoop || !index.isParameter(parameter) || index.valueNarrowedInto(parameter))
            return { };
        BlockID join = index.blockOf(parameter);
        if (index.isLoopHead(join) || index.isEnteredByHandlerEdge(join))
            return { };
        uint32_t position = parameter.index() - index.block(join).firstParameter().index();
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, join)) {
            std::span<const ValueID> arguments = edge.target.arguments();
            if (position >= arguments.size())
                return { };
            ValueID written = arguments[position];
            for (ValueID stripped = index.stripCasts(written); stripped && index.isParameter(stripped) && index.valueNarrowedInto(stripped); stripped = index.stripCasts(written))
                written = index.valueNarrowedInto(stripped);
            if (Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, written, firstValueOfLoop)) {
                if (!found.containsIf([&](const ChosenArray& other) { return other.name == name; }))
                    found.append({ name, written });
                continue;
            }
            ValueID brought = index.stripCasts(written);
            if (!joins.contains(brought))
                joins.append(brought);
        }
        if (joins.size() > maxChosenArrays || found.size() > maxChosenArrays)
            return { };
    }
    return found;
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
    if (access.opcode() != Opcode::ElemGet && access.opcode() != Opcode::ElemSet)
        return false;
    ValueID key = index.stripCasts(access.propertyOperand());
    if (!index.isParameter(key) && index.opcodeOf(key) == Opcode::ToNumeric)
        key = index.stripCasts(index.value(key).operand(0));
    return !index.isParameter(key);
}

static bool isRoundingMathCall(const Walker& walker, const BodyIndex& index, ValueID value)
{
    if (!value || index.isParameter(value) || index.opcodeOf(value) != Opcode::Call)
        return false;
    ValueID callee = index.stripCasts(index.value(value).as<Opcode::Call>().calleeOperand());
    if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin)
        return false;
    BuiltinID builtin = walker.program().builtins().canonicalID(index.value(callee).as<Opcode::Builtin>().builtin());
    return builtin == Builtins::MathFloor || builtin == Builtins::MathCeil || builtin == Builtins::MathTrunc || builtin == Builtins::MathRound;
}

static bool usesKeyComputedInIteration(const Walker& walker, const BodyIndex& index, const BodyValue& access)
{
    static constexpr OpcodeSet reckonings = allBitwiseOperators | OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Mod, Opcode::ElemGet };
    ValueID key = index.stripCasts(access.propertyOperand());
    while (key && !index.isParameter(key) && index.opcodeOf(key) == Opcode::ToPropertyKey)
        key = index.stripCasts(index.value(key).operand(0));
    return key && !index.isParameter(key) && (reckonings.contains(index.opcodeOf(key)) || isRoundingMathCall(walker, index, key));
}

static bool accessesArrayByPosition(const BodyIndex& index, const BodyValue& access)
{
    static constexpr OpcodeSet terms = allBitwiseOperators | OpcodeSet { Opcode::Add, Opcode::Mul, Opcode::Mod, Opcode::ElemGet };
    if (!goesUnderKeyMadeInTurnInListReadsInLoops(index, access))
        return false;
    ValueID key = index.stripCasts(access.propertyOperand());
    if (index.opcodeOf(key) == Opcode::Sub)
        key = index.stripCasts(index.value(key).operand(0));
    return key && !index.isParameter(key) && terms.contains(index.opcodeOf(key));
}

static bool readGivesViewInListReadsInLoops(Walker& walker, const BodyValue& read)
{
    TypeID widest = widestTypeOfOperation(walker, read);
    TypeID objects = widest ? walker.types().withoutPrimitives(widest, Primitive::Absent | Primitive::UndefinedOrNull) : TypeID();
    return objects && !isNeverType(objects) && isViewOfNumbersReadInLine(walker, objects);
}

static bool readGivesNoObjectInListReadsInLoops(Walker& walker, const BodyValue& read)
{
    TypeID widest = widestTypeOfOperation(walker, read);
    return widest && !walker.types().at(widest).hasObjects();
}

ValueID readOfVariableThatFindsReceiverInLoop(const Program& program, const BodyIndex& index, const BodyValue& access, uint32_t firstValueOfLoop)
{
    return readOfVariableInLoopInListReadsInLoops(program, index, access.objectOperand(), firstValueOfLoop);
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
    ValueID variable = leadsThroughName ? readOfVariableInLoopInListReadsInLoops(walker.program(), index, holder, firstValueOfLoop) : ValueID();
    TypeID widest = variable ? widestTypeOfOperation(walker, index.value(variable)) : TypeID();
    TypeID whereFilled = widest ? walker.types().withoutPrimitives(widest, Primitive::Absent | Primitive::Undefined) : TypeID();
    return whereFilled && walker.types().at(whereFilled).isObject() && walker.types().at(whereFilled).hasOnlyLayouts() ? holder : ValueID();
}

static ValueID receiverOfMethodCall(const Program&, const BodyIndex&, const BodyValue& call, BuiltinName method, size_t numberOfArguments);

static ValueID receiverOfPushOrPop(const Program& program, const BodyIndex& index, const BodyValue& call)
{
    ValueID pushed = receiverOfMethodCall(program, index, call, BuiltinNames::push, 1);
    return pushed ? pushed : receiverOfMethodCall(program, index, call, BuiltinNames::pop, 0);
}

static bool isLiteralOfBody(const BodyIndex& index, ValueID value)
{
    static constexpr OpcodeSet literals { Opcode::NewArray, Opcode::Data };
    return value && !index.isParameter(value) && literals.contains(index.opcodeOf(value));
}

static bool accessesArrayInFieldOf(Walker& walker, const BodyIndex& index, LoopID loop, VariableID variable)
{
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue access) {
            if (access.opcode() != Opcode::ElemGet && access.opcode() != Opcode::ElemSet)
                return false;
            ValueID field = index.stripCasts(access.objectOperand());
            if (index.isParameter(field) || index.opcodeOf(field) != Opcode::PropGet || index.value(field).isFreeName())
                return false;
            ValueID holder = index.stripCasts(index.value(field).objectOperand());
            if (!index.isParameter(holder) && index.opcodeOf(holder) == Opcode::CheckInitialized)
                holder = index.stripCasts(index.value(holder).operand(0));
            if (index.isParameter(holder) || index.opcodeOf(holder) != Opcode::GetModuleVariable || index.value(holder).variable() != variable)
                return false;
            TypeID object = widestTypeOfOperation(walker, index.value(holder));
            object = object ? walker.types().withoutPrimitives(object, Primitive::Absent | Primitive::UndefinedOrNull) : TypeID();
            if (!object || isNeverType(object) || !walker.types().at(object).isObject() || !walker.types().at(object).hasOnlyLayouts())
                return false;
            std::optional<FieldReference> place = findField(walker.program(), membersOf(walker.types(), walker.types().at(object))[0], index.value(field).key());
            TypeID held = place ? walker.positions().readOfSlot(TypeLevel::Proven, place->layout, place->field) : TypeID();
            return held && !isNeverType(held) && hasKind(walker.types(), walker.types().withoutNullish(held), OperandKind::Array);
        });
    });
}

static constexpr uint32_t valuesOfShortBody = 24;

void appendCalleeDeferredArrays(Walker& walker, const BodyValue& call, Vector<VariableID, 4>& variables)
{
    static constexpr OpcodeSet accesses { Opcode::ElemGet, Opcode::ElemSet, Opcode::PropGet, Opcode::PropSet };
    FunctionID callee = call.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), call.id()) : FunctionID();
    const BodyIndex* body = callee ? walker.facts().bodies().find(callee) : nullptr;
    if (!body || !body->opcodes().contains(Opcode::GetModuleVariable) || !body->holdsAnyOf(accesses))
        return;
    std::optional<bool> isCopied;
    if (body->numberOfValues() > valuesOfShortBody && !*(isCopied = lookReadsCopyOfCalleeInListReadsInLoops(walker, call, callee)))
        return;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        for (BodyValue operation : body->block(BlockID(number)).values()) {
            if (!accesses.contains(operation.opcode()) || (operation.opcode() == Opcode::PropGet && operation.isFreeName()))
                continue;
            ValueID array = body->stripCasts(operation.objectOperand());
            if (array && !body->isParameter(array) && body->opcodeOf(array) == Opcode::CheckInitialized)
                array = body->stripCasts(body->value(array).operand(0));
            if (!array || body->isParameter(array) || body->opcodeOf(array) != Opcode::GetModuleVariable)
                continue;
            VariableID variable = body->value(array).variable();
            if (variables.contains(variable) || !objectOfVariablePutOffStays(walker, variable) || objectOfVariablePutOff(walker, variable) || walker.state().variable(variable))
                continue;
            if (walker.plan().deferredDefinitions().findDeferred(variable)->mark == Primitive::Absent)
                continue;
            if (!isCopied)
                isCopied = lookReadsCopyOfCalleeInListReadsInLoops(walker, call, callee);
            if (!*isCopied)
                return;
            variables.append(variable);
        }
    }
}

static bool storesVariable(const BodyIndex& body, VariableID variable)
{
    if (!body.opcodes().contains(Opcode::PutModuleVariable))
        return false;
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        for (BodyValue operation : body.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::PutModuleVariable && operation.variable() == variable)
                return true;
        }
    }
    return false;
}

void appendCalleeConstantObjects(Walker& walker, const BodyValue& call, Vector<VariableID, 4>& variables)
{
    FunctionID callee = call.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), call.id()) : FunctionID();
    const BodyIndex* body = callee ? walker.facts().bodies().find(callee) : nullptr;
    static constexpr OpcodeSet accessesAndCalls { Opcode::PropSet, Opcode::PropGet, Opcode::Call };
    if (!body || !body->opcodes().contains(Opcode::GetModuleVariable) || !body->holdsAnyOf(accessesAndCalls))
        return;
    auto objectStoredInto = [&](const BodyValue& operation) -> ValueID {
        if (operation.opcode() == Opcode::PropSet || (operation.opcode() == Opcode::PropGet && !operation.isFreeName()))
            return body->stripCasts(operation.objectOperand());
        ValueID receiver = operation.opcode() == Opcode::Call ? operation.as<Opcode::Call>().thisValueOperand() : ValueID();
        FunctionID method = receiver ? walker.facts().linked().calleeAt(callee, operation.id()) : FunctionID();
        const BodyIndex* bodyOfMethod = method ? walker.facts().bodies().find(method) : nullptr;
        return bodyOfMethod && bodyOfMethod->opcodes().contains(Opcode::PropSet) ? body->stripCasts(receiver) : ValueID();
    };
    std::optional<bool> isCopied;
    if (body->numberOfValues() > valuesOfShortBody && !*(isCopied = lookReadsCopyOfCalleeInListReadsInLoops(walker, call, callee)))
        return;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        for (BodyValue operation : body->block(BlockID(number)).values()) {
            ValueID object = objectStoredInto(operation);
            if (object && !body->isParameter(object) && body->opcodeOf(object) == Opcode::CheckInitialized)
                object = body->stripCasts(body->value(object).operand(0));
            if (!object || body->isParameter(object) || body->opcodeOf(object) != Opcode::GetModuleVariable)
                continue;
            VariableID variable = body->value(object).variable();
            if (variables.contains(variable) || !leavesDeadZoneForGood(walker.program(), variable) || storesVariable(*body, variable) || storesVariable(walker.currentBodyIndex(), variable) || walker.plan().deferredDefinitions().findDeferred(variable) || walker.state().variable(variable))
                continue;
            TypeID held = walker.positions().variable(TypeLevel::Proven, variable);
            held = held ? walker.types().withoutPrimitives(held, Primitive::Absent) : TypeID();
            if (!held || isNeverType(held) || !walker.types().at(held).isObject() || !walker.types().at(held).hasOnlyLayouts())
                continue;
            if (!isCopied)
                isCopied = lookReadsCopyOfCalleeInListReadsInLoops(walker, call, callee);
            if (!*isCopied)
                return;
            variables.append(variable);
            walker.count(StatisticID::CalleeConstantsReadBefore);
        }
    }
}

void hoistLeadingInitializationChecks(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    static constexpr OpcodeSet unobservable = primitiveConstants | opcodesThatStateTypes | OpcodeSet { Opcode::LoopHint, Opcode::GetModuleVariable };
    const Program& program = walker.program();
    for (BodyValue operation : index.block(inputHead).values()) {
        if (unobservable.contains(operation.opcode()))
            continue;
        ValueID source = operation.opcode() == Opcode::CheckInitialized ? index.stripCasts(operation.operand(0)) : ValueID();
        if (!source || index.isParameter(source) || index.opcodeOf(source) != Opcode::GetModuleVariable || !index.isInBlock(source, inputHead))
            return;
        BodyValue read = index.value(source);
        VariableID variable = read.variable();
        if (program.at(variable).module != program.at(walker.currentFunction()).module || walker.plan().deferredDefinitions().findDeferred(variable) || textOfLoopStoresVariableInListReadsInLoops(index, read))
            return;
        if (walker.state().isPastDeadZone(variable) || valueOfStableBinding(walker, read))
            continue;
        TypeID widest = widestTypeOfOperation(walker, read);
        if (!widest || !(walker.types().at(widest).primitives & Primitive::Absent))
            continue;
        if (!accessesArrayInFieldOf(walker, index, index.loopOf(inputHead), variable))
            return;
        Draft draft = walker.draftOf(read);
        walker.chooseConservativeFields(draft);
        Reference held = walker.build(WTF::move(draft));
        if (!held || walker.isUnreachable() || walker.hasFailed())
            return;
        Draft check(Opcode::CheckInitialized, walker.originOf(operation));
        walker.chooseConservativeFields(check);
        check.setValueOperand(held);
        check.setName(operation.as<Opcode::CheckInitialized>().name());
        Reference checked = walker.build(WTF::move(check));
        if (!checked || walker.isUnreachable() || walker.hasFailed())
            return;
        walker.state().noteEndOfDeadZone(variable);
        walker.state().noteVariable(variable, checked);
        noteValueOfStableBinding(walker, variable, checked);
        walker.count(StatisticID::ModuleConstantReadsHoisted);
    }
}

std::span<const Walker::ListAccessesInBlock> Walker::listAccessesOfLoop(LoopID loop)
{
    const BodyIndex& index = currentBodyIndex();
    Vector<std::optional<Vector<ListAccessesInBlock>>>& ofLoops = m_listAccessesOfLoops.ensure(&index, [&] {
        Vector<std::optional<Vector<ListAccessesInBlock>>> none;
        none.grow(index.numberOfLoops());
        return none;
    }).iterator->value;
    std::optional<Vector<ListAccessesInBlock>>& ofLoop = ofLoops[loop.index()];
    if (ofLoop)
        return ofLoop->span();
    ofLoop.emplace();
    static constexpr OpcodeSet accesses { Opcode::ElemGet, Opcode::ElemSet, Opcode::Call };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!accesses.contains(operation.opcode()) && (operation.opcode() != Opcode::PropGet || operation.isFreeName() || operation.key() != program().keyOf(BuiltinNames::length)))
                continue;
            if (ofLoop->isEmpty() || ofLoop->last().block != member)
                ofLoop->append({ member, { } });
            ofLoop->last().accesses.append(operation.id());
        }
    }
    return ofLoop->span();
}

void readListsOfStableBindingsInFrontOfLoop(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::TypedOperations) || (!index.opcodes().contains(Opcode::GetModuleVariable) && !index.opcodes().contains(Opcode::GetEnvironmentField)) || walker.isUnreachable())
        return;
    bool isInnerLoop = !!index.loop(loop).parent;
    const Program& program = walker.program();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    WhatReadsFromInFrontOfLoopFind foundFromInFront;
    std::optional<BitVector> holdersComparedWithCharacters;
    auto loopComparesElementWithCharacters = [&](const BodyValue& read) {
        if (!holdersComparedWithCharacters)
            holdersComparedWithCharacters = findHoldersOfElementsThatLoopComparesWithCharactersOrNumbers(walker, index, loop);
        return readsElementUnderIndexOffOneOf(index, read, *holdersComparedWithCharacters);
    };
    for (const Walker::ListAccessesInBlock& listed : walker.listAccessesOfLoop(loop)) {
        BlockID member = listed.block;
        if (isInnerLoop && index.loopOf(member) != loop)
            continue;
        for (ValueID access : listed.accesses) {
            BodyValue operation = index.value(access);
            bool isByPosition = index.loopOf(member) == loop && accessesArrayByPosition(index, operation);
            bool hasUnknownKey = !isByPosition && goesUnderKeyMadeInTurnInListReadsInLoops(index, operation);
            bool accessesElement = operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet;
            if (isInnerLoop && (!accessesElement || index.loopOf(member) != loop || hasUnknownKey))
                continue;
            Vector<ValueID, 4> lists;
            ValueID holderThatNamesLeadFrom;
            if (readsListFoundByReadFromInFront(program, index, operation, firstValueOfLoop, foundFromInFront))
                lists.append(operation.objectOperand());
            if ((holderThatNamesLeadFrom = holderOffVariableThatNamesLeadFromInListReadsInLoops(walker, index, operation, firstValueOfLoop)))
                lists.append(holderThatNamesLeadFrom);
            if (FunctionID callee = operation.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()) : FunctionID()) {
                std::span<const ValueID> arguments = operation.as<Opcode::Call>().argumentsOperands();
                bool handsVariableOn = std::ranges::any_of(arguments, [&](ValueID argument) {
                    return argument && readOfVariableInLoopInListReadsInLoops(program, index, argument, firstValueOfLoop);
                });
                if (handsVariableOn && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, callee)) {
                    forEachReadOfArgumentInCopyOfCalleeInListReadsInLoops(walker, callee, arguments, 2, Opcode::ElemGet, [&](ValueID argument, ValueID, FunctionID, ValueID) {
                        if (readOfVariableInLoopInListReadsInLoops(program, index, argument, firstValueOfLoop) && !lists.contains(argument))
                            lists.append(argument);
                    });
                }
            }
            ValueID receiver = operation.opcode() == Opcode::Call && walker.isOutermostPushPopLoop(index.loopOf(member)) ? receiverOfPushOrPop(program, index, operation) : ValueID();
            if (receiver && !lists.contains(receiver))
                lists.append(receiver);
            for (ValueID list : lists) {
            ValueID source = readOfVariableInLoopInListReadsInLoops(program, index, list, firstValueOfLoop);
            if (!source)
                continue;
            bool isHolder = list == holderThatNamesLeadFrom;
            BodyValue read = index.value(source);
            bool isHolderOfAnotherFile = isHolder && read.opcode() == Opcode::GetModuleVariable && program.at(read.variable()).module != program.at(walker.currentFunction()).module;
            if (read.opcode() == Opcode::GetEnvironmentField) {
                Reference environment = loopInvariantScope(walker, index, read.environmentOperand(), firstValueOfLoop);
                FieldReference field { read.layout(), read.field() };
                if (!environment || valueOfStableBinding(walker, read, environment) || walker.state().environmentField(environment, field) || hasUnknownKey || readGivesNoObjectInListReadsInLoops(walker, read) || textOfLoopStoresVariableInListReadsInLoops(index, read))
                    continue;
                Draft draft = read.environmentOperand().index() < firstValueOfLoop ? walker.draftOf(read) : draftScopeRead(walker, read, environment);
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
            if (isInnerLoop && (isHolder || numberReadOutOfVariableStays(walker, variable) || walker.plan().deferredDefinitions().findDeferred(variable)))
                continue;
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
                if (walker.state().variable(variable) || (!isHolder && program.at(variable).module != program.at(walker.currentFunction()).module) || hasUnknownKey || (isByPosition && readGivesViewInListReadsInLoops(walker, read)) || readGivesNoObjectInListReadsInLoops(walker, read) || textOfLoopStoresVariableInListReadsInLoops(index, read))
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
            bool isCallArgument = operation.opcode() == Opcode::Call && list != receiver;
            if (isCallArgument && widest && (isNeverType(walker.types().withoutNullish(widest)) || !hasKind(walker.types(), walker.types().withoutNullish(widest), OperandKind::Array)))
                continue;
            if (!widest || (walker.types().at(widest).primitives & Primitive::Unspecified))
                continue;
            bool mayBeInDeadZone = isHolderOfAnotherFile || (walker.types().at(widest).primitives & (isHolder ? Primitive::Absent | Primitive::Undefined : Primitive::Absent));
            if (mayBeInDeadZone && isHolder && walker.state().variable(variable))
                continue;
            TypeID behindDeadZone = walker.types().withoutPrimitives(widest, Primitive::Absent);
            bool mayBeStoredInto = !isCallArgument && (list == receiver || index.opcodes().contains(Opcode::ElemSet)) && isProvenToBeList(walker.types(), behindDeadZone) && !walker.plan().deferredDefinitions().findDeferred(variable);
            if (mayBeInDeadZone && !isHolder && (isCallArgument || (!mayBeStoredInto && !isElementUsedInArithmetic(walker, operation) && !loopComparesElementWithCharacters(operation) && !(member == inputHead && operation.opcode() == Opcode::PropGet && readsOnlyVariableLength(program, index, variable))) || walker.state().variable(variable) || (!mayBeStoredInto && !walker.types().isUninformative(behindDeadZone))))
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

static Reference readFieldInPreheader(Walker& walker, const BodyIndex& index, ValueID value, LoopID outermost, uint32_t firstValueOfLoop, bool mustHoldArray, bool mayCheckSlot)
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
    Reference holder = readFieldInPreheader(walker, index, read.objectOperand(), outermost, firstValueOfLoop, false, false);
    return readFilledFieldInPreheader(walker, read, holder, mustHoldArray, mayCheckSlot);
}

static bool isProvenArray(const Walker&, Reference name);

static bool loadsView(Walker& walker, const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    ValueID loaded = valueThatEntersLoopInListReadsInLoops(index, value, firstValueOfLoop);
    if (!loaded || loaded.index() < firstValueOfLoop || index.isParameter(loaded))
        return false;
    TypeID widest = widestTypeOfOperation(walker, index.value(loaded));
    return widest && isViewOfNumbersReadInLine(walker, walker.types().withoutNullish(widest));
}

Reference fieldValueInPreheader(Walker& walker, const BodyIndex& index, ValueID read, uint32_t firstValueOfLoop)
{
    ValueID enters = read ? valueThatEntersLoopInListReadsInLoops(index, read, firstValueOfLoop) : ValueID();
    return enters && enters.index() >= firstValueOfLoop ? valueThatStateHoldsInSlotReadInTurnInListReadsInLoops(walker, index, enters, firstValueOfLoop) : Reference();
}

static ValueID fieldBoundOfLoop(const BodyIndex& index, BlockID inputHead)
{
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq };
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    ValueID asked = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!asked || index.isParameter(asked) || !head.holds(asked) || !relations.contains(index.opcodeOf(asked)))
        return { };
    ValueID bound = index.stripCasts(index.value(asked).operand(1));
    return bound && !index.isParameter(bound) && head.holds(bound) && index.opcodeOf(bound) == Opcode::PropGet && !index.value(bound).isFreeName() ? bound : ValueID();
}

void readFieldArraysInPreheader(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::TypedOperations) || !index.holdsAnyOf(OpcodeSet { Opcode::ElemGet, Opcode::ElemSet, Opcode::Call }))
        return;
    bool storesElements = index.opcodes().contains(Opcode::ElemSet);
    bool readsKeysOutOfLists = !storesElements && std::ranges::any_of(walker.listAccessesOfLoop(index.loopOf(inputHead)), [&](const Walker::ListAccessesInBlock& listed) {
        return std::ranges::any_of(listed.accesses, [&](ValueID access) {
            BodyValue read = index.value(access);
            ValueID key = read.opcode() == Opcode::ElemGet ? index.stripCasts(read.propertyOperand()) : ValueID();
            return key && !index.isParameter(key) && index.opcodeOf(key) == Opcode::ElemGet;
        });
    });
    auto storesUnknownValue = [&](const BodyValue& store) {
        ValueID stored = store.opcode() == Opcode::ElemSet ? index.stripCasts(store.valueOperand()) : ValueID();
        if (!stored || index.isParameter(stored) || index.opcodeOf(stored) != Opcode::ElemGet)
            return false;
        TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), stored);
        return !seenByLink || walker.types().at(seenByLink).isAnything();
    };
    bool isToldApart = walker.standsInCopyOfLoopBuiltForEachMember();
    auto readsUnknownContents = [&](const BodyValue& access) {
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
    for (const Walker::ListAccessesInBlock& listed : walker.listAccessesOfLoop(loop)) {
        BlockID member = listed.block;
        for (ValueID access : listed.accesses) {
            if (walker.isUnreachable())
                return;
            BodyValue operation = index.value(access);
            if (FunctionID callee = operation.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()) : FunctionID()) {
                auto readInFront = [&](ValueID holder, PropertyKeyID field, ValueID, BodyValue) {
                    Reference object = nameThatEntersLoopInListReadsInLoops(walker, index, holder, firstValueOfLoop);
                    if (object && !walker.isUnreachable() && !walker.textOfOutermostLoopStoresUnderNameInCurrentBody(outermost, field) && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, callee))
                        readFieldArrayOfHolder(walker, object, field, walker.originOf(operation));
                };
                forEachFieldAccessInCopy(walker, operation, callee, Opcode::ElemSet, readInFront);
                const BodyIndex* body = walker.facts().bodies().find(callee);
                if (body && walker.pushesOntoField(*body))
                    forEachFieldAccessInCopy(walker, operation, callee, Opcode::Call, readInFront);
                ValueID popped = body && index.loop(loop).numberOfLoopsWithin == 1 ? walker.findReturnedElement(*body) : ValueID();
                if (popped && body->opcodeOf(popped) == Opcode::Call) {
                    ValueID field = findMethodReceiverField(walker.program(), *body, body->value(popped), BuiltinNames::pop, 0);
                    if (ValueID holder = field ? valuePassedIntoCopy(walker, *body, operation, callee, body->value(field).objectOperand()) : ValueID())
                        readInFront(holder, body->value(field).key(), { }, body->value(popped));
                }
                if (index.loop(loop).numberOfLoopsWithin == 1)
                    forEachFieldAccessInCopy(walker, operation, callee, Opcode::ElemGet, readInFront);
            }
            ValueID changed = operation.opcode() == Opcode::Call ? fieldPushedOnto(walker.program(), index, operation) : ValueID();
            if (!changed && operation.opcode() == Opcode::Call)
                changed = findMethodReceiverField(walker.program(), index, operation, BuiltinNames::pop, 0);
            if (changed && changed.index() >= firstValueOfLoop && index.loopOf(member) == loop)
                readFieldInPreheader(walker, index, changed, outermost, firstValueOfLoop, true, true);
            if (operation.opcode() != Opcode::ElemSet && operation.opcode() != Opcode::ElemGet)
                continue;
            ValueID list = valueThatEntersLoopInListReadsInLoops(index, operation.objectOperand(), firstValueOfLoop);
            if (!list || list.index() < firstValueOfLoop || index.isParameter(list) || (index.opcodeOf(list) != Opcode::PropGet && index.opcodeOf(list) != Opcode::PrivateGet))
                continue;
            bool isReckoned = usesKeyComputedInIteration(walker, index, operation);
            if ((goesUnderKeyMadeInTurnInListReadsInLoops(index, operation) && !isReckoned) || (isReckoned && storesUnknownValue(operation)))
                continue;
            bool readsView = loadsView(walker, index, operation.objectOperand(), firstValueOfLoop);
            bool readsAnyField = storesElements || readsKeysOutOfLists || index.loop(loop).numberOfLoopsWithin > 1 || readsUnknownContents(operation) || readsView;
            if (ValueID bound = readsView && index.loopOf(member) == loop ? fieldBoundOfLoop(index, inputHead) : ValueID()) {
                readFieldInPreheader(walker, index, bound, outermost, firstValueOfLoop, false, false);
                if (walker.isUnreachable())
                    return;
            }
            bool mayCheckSlot = storesElements || (outermost == loop && index.loop(loop).numberOfLoopsWithin == 1);
            readFieldInPreheader(walker, index, operation.objectOperand(), outermost, firstValueOfLoop, !readsAnyField, mayCheckSlot);
        }
    }
}

bool isRowKeyInvariant(const BodyIndex& index, ValueID key, uint32_t firstValueOfLoop, unsigned depth)
{
    ValueID enters = key ? valueThatEntersLoopInListReadsInLoops(index, key, firstValueOfLoop) : ValueID();
    if (!enters || enters.index() < firstValueOfLoop)
        return !!enters;
    if (index.isParameter(enters) || !depth)
        return false;
    BodyValue made = index.value(enters);
    if (made.opcode() == Opcode::Integer)
        return true;
    return (made.opcode() == Opcode::Add || made.opcode() == Opcode::Sub) && isRowKeyInvariant(index, made.operand(0), firstValueOfLoop, depth - 1) && isRowKeyInvariant(index, made.operand(1), firstValueOfLoop, depth - 1);
}

Reference buildHoistedRowKey(Walker& walker, const BodyIndex& index, ValueID key, uint32_t firstValueOfLoop, unsigned depth)
{
    ValueID enters = key ? valueThatEntersLoopInListReadsInLoops(index, key, firstValueOfLoop) : ValueID();
    if (!enters)
        return { };
    if (enters.index() < firstValueOfLoop)
        return walker.referenceOf(enters);
    if (index.isParameter(enters) || !depth)
        return { };
    BodyValue made = index.value(enters);
    Origin origin = walker.originOf(made);
    origin.isTailCall = false;
    if (made.opcode() == Opcode::Integer) {
        Draft written(Opcode::Integer, origin);
        written.setInteger(made.integer());
        return walker.constant(WTF::move(written));
    }
    if (made.opcode() != Opcode::Add && made.opcode() != Opcode::Sub)
        return { };
    Reference left = buildHoistedRowKey(walker, index, made.operand(0), firstValueOfLoop, depth - 1);
    Reference right = buildHoistedRowKey(walker, index, made.operand(1), firstValueOfLoop, depth - 1);
    auto isInteger = [&](Reference side) {
        return side && side.isRecord() && walker.typeOf(side) && walker.types().at(walker.typeOf(side)).isInteger();
    };
    return isInteger(left) && isInteger(right) ? buildIntegerOperator(walker, made.opcode(), left, right, origin) : Reference();
}

static Reference buildHoistedRow(Walker& walker, const BodyIndex& index, std::span<const ValueID> fieldReads, Vector<Reference, 1>& outerFields, Reference& holder, Reference key, Origin origin)
{
    BlockID missingRow = walker.addBlock(BlockFrequency::Rare);
    auto continueIf = [&](Reference test) {
        if (!test || walker.hasFailed())
            return false;
        BlockID passed = walker.addBlock();
        walker.branchOnTest(test, passed, missingRow);
        walker.continueIn(passed);
        return !walker.isUnreachable();
    };
    auto guardArray = [&](Reference value) -> Reference {
        if (!value || !value.isValue() || isProvenArray(walker, value))
            return value;
        Draft question(Opcode::TypeTest, origin);
        question.setValueOperand(value);
        question.setTestedType(walker.types().builtinClass(BuiltinClasses::Array));
        Reference isList = walker.build(WTF::move(question));
        if (!continueIf(isList))
            return { };
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(value);
        proof.setTestOperand(isList);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        return walker.build(WTF::move(proof));
    };
    auto guardObject = [&](Reference value) -> Reference {
        if (!value || !value.isValue() || walker.hasFailed() || walker.isUnreachable())
            return { };
        if (walker.types().isSubset(walker.typeOf(value), TypeTable::object))
            return value;
        Draft question(Opcode::TypeTest, origin);
        question.setValueOperand(value);
        question.setTestedType(TypeTable::object);
        Reference isObject = walker.build(WTF::move(question));
        if (!continueIf(isObject))
            return { };
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(value);
        proof.setTestOperand(isObject);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        return walker.build(WTF::move(proof));
    };
    Reference found = holder;
    for (size_t place = 0; place < fieldReads.size() && found && !walker.hasFailed() && !walker.isUnreachable(); ++place) {
        found = buildHoistedFieldRead(walker, index.value(fieldReads[place]), found, missingRow);
        if (place + 1 == fieldReads.size())
            break;
        found = guardObject(found);
        outerFields.append(found);
    }
    if (!found && !walker.hasFailed() && !walker.isUnreachable())
        walker.jumpTo(missingRow, { });
    Reference guarded = guardArray(found);
    Reference row;
    Draft hasKind(Opcode::HasArrayStorage, origin);
    walker.chooseConservativeFields(hasKind);
    hasKind.setArrayOperand(guarded);
    hasKind.setArrayStorage(ArrayStorage::Contiguous);
    if (guarded && guarded.isValue() && !walker.hasFailed() && !walker.isUnreachable() && continueIf(walker.build(WTF::move(hasKind)))) {
        Draft isPresent(Opcode::IsElementPresent, origin);
        walker.chooseConservativeFields(isPresent);
        isPresent.setArrayOperand(guarded);
        isPresent.setPositionOperand(key);
        isPresent.setArrayStorage(ArrayStorage::Contiguous);
        isPresent.setKeyIsIntegral(true);
        if (std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(guarded, key))
            isPresent.setIsInBounds(*inside);
        Reference test = walker.build(WTF::move(isPresent));
        std::optional<PassedTest> present = continueIf(test) ? walker.showElementPresentBehindTest(guarded, key, walker.findPassedTest(test)) : std::nullopt;
        if (present) {
            Draft element(Opcode::GetArrayElement, origin);
            walker.chooseConservativeFields(element);
            element.setArrayOperand(guarded);
            element.setPositionOperand(key);
            element.setArrayStorage(ArrayStorage::Contiguous);
            element.setLevel(KnowledgeLevel::Proven);
            element.setKeyIsIntegral(true);
            element.setIsPresent(*present);
            row = walker.build(WTF::move(element));
        }
    }
    if (!walker.hasFailed() && !walker.isUnreachable())
        row = guardArray(row);
    if (walker.hasFailed())
        return { };
    Vector<Walker::JoinArm, 2> arms;
    if (row && row.isValue() && !walker.isUnreachable()) {
        BlockID foundRow = walker.addBlock();
        walker.jumpTo(foundRow, { });
        arms.append(Walker::JoinArm { foundRow, { row } });
        if (!fieldReads.empty()) {
            arms.last().values.append(guarded);
            arms.last().values.appendVector(outerFields);
        }
    }
    Reference nothing = walker.constant(Draft(Opcode::Undefined));
    size_t numberOfValues = 1 + fieldReads.size();
    arms.append(Walker::JoinArm { missingRow, { } });
    arms.last().values.fill(nothing, numberOfValues);
    Vector<Reference, 4> met;
    met.fill(Reference(), numberOfValues);
    if (!walker.joinArms(arms.span(), walker.addBlock(), met.mutableSpan()))
        return { };
    outerFields.clear();
    if (!fieldReads.empty()) {
        holder = met[1];
        outerFields.append(met.span().subspan(2));
    }
    return met[0];
}

Reference findHoistedFieldInCopy(Walker& walker, ValueID fieldRead)
{
    Reference field = walker.findHoistedField(fieldRead);
    std::optional<uint64_t> held = field ? findKnown(walker, KnownNote::HoistedFieldOfCopy, field) : std::nullopt;
    return held ? Reference::fromBits(static_cast<uint32_t>(*held)) : Reference();
}

static bool constructsArrayOfLength(const Walker& walker, const BodyIndex& index, const BodyValue& operation)
{
    if (operation.opcode() != Opcode::Construct)
        return false;
    auto construction = operation.as<Opcode::Construct>();
    ValueID constructor = construction.calleeOperand() ? index.stripCasts(construction.calleeOperand()) : ValueID();
    if (!constructor || index.isParameter(constructor) || index.opcodeOf(constructor) != Opcode::Builtin)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    if (table.canonicalID(index.value(constructor).as<Opcode::Builtin>().builtin()) != table.canonicalID(Builtins::Array))
        return false;
    return construction.argumentsOperands().size() <= 1 && std::ranges::all_of(construction.argumentsOperands(), [&](ValueID argument) {
        ValueID length = index.stripCasts(argument);
        return length && !index.isParameter(length) && index.opcodeOf(length) == Opcode::Integer;
    });
}

template<typename FindRowRead>
static bool comparesRowsWithStrings(const BodyIndex& index, LoopID loop, const FindRowRead& findRowRead)
{
    return index.opcodes().contains(Opcode::ElemGet) && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            return allEqualities.contains(operation.opcode()) && std::ranges::any_of(std::array { 0, 1 }, [&](uint32_t side) {
                ValueID element = index.stripCasts(operation.operand(side));
                ValueID other = index.stripCasts(operation.operand(1 - side));
                return element && other && !index.isParameter(element) && !index.isParameter(other) && index.opcodeOf(other) == Opcode::String && index.opcodeOf(element) == Opcode::ElemGet && findRowRead(index.value(element));
            });
        });
    });
}

template<typename FindRowRead>
static bool comparesRowElements(const BodyIndex& index, LoopID loop, const FindRowRead& findRowRead)
{
    return index.opcodes().contains(Opcode::ElemGet) && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            return allEqualities.contains(operation.opcode()) && std::ranges::any_of(std::array { operation.operand(0), operation.operand(1) }, [&](ValueID operand) {
                ValueID element = index.stripCasts(operand);
                ValueID other = index.stripCasts(operation.operand(0) == operand ? operation.operand(1) : operation.operand(0));
                bool isString = other && !index.isParameter(other) && index.opcodeOf(other) == Opcode::String;
                return element && !isString && !index.isParameter(element) && index.opcodeOf(element) == Opcode::ElemGet && findRowRead(index.value(element));
            });
        });
    });
}

void readRowArraysInPreheader(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::TypedOperations) || walker.isUnreachable() || walker.hasFailed() || walker.code().at(walker.currentBlock()).frequency == BlockFrequency::Rare || !walker.has(Mechanism::Guesses) || !walker.opensGuesses() || walker.canUseIterationGuards(inputHead) || !walker.hoistedRowsOf(inputHead).empty())
        return;
    if (index.loop(loop).numberOfLoopsWithin > 1 && (!index.holdsAnyOf(OpcodeSet { Opcode::ElemGet }) || !walker.hasUnrolledInnerLoops(inputHead)))
        return;
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto findRowRead = [&](const BodyValue& access) -> ValueID {
        bool readsLength = access.opcode() == Opcode::PropGet && !access.isFreeName() && access.key() == walker.program().keyOf(BuiltinNames::length);
        if (access.opcode() != Opcode::ElemGet && access.opcode() != Opcode::ElemSet && !readsLength)
            return { };
        ValueID row = valueThatEntersLoopInListReadsInLoops(index, access.objectOperand(), firstValueOfLoop);
        return row && row.index() >= firstValueOfLoop && !index.isParameter(row) && index.opcodeOf(row) == Opcode::ElemGet && index.loopOf(index.blockOf(row)) == loop ? row : ValueID();
    };
    bool storesElements = false;
    bool preservesHolders = std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::all_of(index.block(member).values(), [&](BodyValue operation) {
            bool isStore = operation.opcode() == Opcode::ElemSet;
            storesElements |= isStore;
            if (effectRuleOf(operation.opcode()) == EffectRule::Call)
                return isMathCall(walker, index, operation) || constructsArrayOfLength(walker, index, operation);
            Effects declared = declaredEffectsOfOperation(walker.program(), operation.opcode(), operation);
            return isStore || declared.mayRunOtherCode() || operandsMayRunCode(operation.opcode()) || !declared.writes.contains(AbstractHeap::Elements);
        });
    });
    if (!preservesHolders)
        return;
    auto findRowHolder = [&](ValueID row) -> Reference {
        Reference holder = nameThatEntersLoopInListReadsInLoops(walker, index, index.value(row).objectOperand(), firstValueOfLoop);
        bool mayBeArray = holder && holder.isValue() && walker.typeOf(holder) && walker.types().overlap(walker.ownTypeOf(holder), walker.types().builtinClass(BuiltinClasses::Array));
        return mayBeArray ? holder : Reference();
    };
    LoopID outermost = loop;
    while (LoopID around = index.loop(outermost).parent)
        outermost = around;
    struct FieldPath {
        explicit operator bool() const { return !reads.isEmpty(); }
        Vector<ValueID, 2> reads;
        Reference object;
    };
    auto findFieldPath = [&](ValueID row) -> FieldPath {
        FieldPath path;
        ValueID holder = index.value(row).objectOperand();
        while (!path.object) {
            ValueID read = valueThatEntersLoopInListReadsInLoops(index, holder, firstValueOfLoop);
            if (!read || read.index() < firstValueOfLoop || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName() || walker.textOfOutermostLoopStoresUnderNameInCurrentBody(outermost, index.value(read).key()))
                return { };
            if (!path.reads.isEmpty() && !siteMayAnswerReadWithoutOtherCode(walker, index.value(path.reads[0])))
                return { };
            path.reads.insert(0, read);
            holder = index.value(read).objectOperand();
            path.object = nameThatEntersLoopInListReadsInLoops(walker, index, holder, firstValueOfLoop);
        }
        BodyValue first = index.value(path.reads[0]);
        bool canHoist = path.reads.size() > 1 ? isFieldOfUnknownObject(walker, first, path.object) : canHoistFieldRead(walker, first, path.object);
        return canHoist ? path : FieldPath();
    };
    bool readsUnknownFields = false;
    auto isReadOfChangingRow = [&](const BodyValue& access, ValueID row) {
        return access.opcode() == Opcode::ElemGet && !isRowKeyInvariant(index, index.value(row).propertyOperand(), firstValueOfLoop, 2) && findRowHolder(row);
    };
    bool canHoistRows = std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::all_of(index.block(member).values(), [&](BodyValue access) {
            ValueID row = findRowRead(access);
            if (!row)
                return true;
            if (isReadOfChangingRow(access, row))
                return true;
            if (!isRowKeyInvariant(index, index.value(row).propertyOperand(), firstValueOfLoop, 2))
                return false;
            if (findRowHolder(row))
                return true;
            FieldPath path = findFieldPath(row);
            readsUnknownFields |= path && (path.reads.size() > 1 || isFieldOfUnknownObject(walker, index.value(path.reads[0]), path.object));
            return !!path;
        });
    });
    if (!canHoistRows || (!storesElements && !readsUnknownFields && !comparesRowElements(index, loop, findRowRead)))
        return;
    if (readsUnknownFields && !storesElements && comparesRowsWithStrings(index, loop, findRowRead))
        return;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue access : index.block(member).values()) {
            ValueID row = findRowRead(access);
            if (!row || walker.findHoistedRow(row) || isReadOfChangingRow(access, row))
                continue;
            BodyValue read = index.value(row);
            Reference holder = findRowHolder(row);
            FieldPath path;
            if (!holder) {
                path = findFieldPath(row);
                holder = path.object;
            }
            Reference key = buildHoistedRowKey(walker, index, read.propertyOperand(), firstValueOfLoop, 2);
            if (walker.isUnreachable() || walker.hasFailed())
                return;
            if (!key || !key.isRecord() || !walker.typeOf(key) || !walker.types().at(walker.typeOf(key)).isInteger())
                continue;
            size_t known = 0;
            std::span<Walker::HoistedRow> rows = walker.hoistedRowsOf(inputHead);
            auto isSameRow = [&](const Walker::HoistedRow& kept) {
                if (kept.key != key || kept.fieldReads.size() != path.reads.size())
                    return false;
                if (!path)
                    return kept.holder == holder;
                for (size_t place = 0; place < path.reads.size(); ++place) {
                    if (index.value(kept.fieldReads[place]).key() != index.value(path.reads[place]).key())
                        return false;
                }
                return kept.fieldBase == path.object;
            };
            while (known < rows.size() && !isSameRow(rows[known]))
                ++known;
            Origin origin = walker.originOf(read);
            origin.isTailCall = false;
            Vector<Reference, 1> outerFields;
            Reference found = known < rows.size() ? rows[known].row : buildHoistedRow(walker, index, path.reads.span(), outerFields, holder, key, origin);
            if (known < rows.size()) {
                holder = rows[known].holder;
                outerFields = rows[known].outerFields;
            }
            if (walker.isUnreachable() || walker.hasFailed())
                return;
            if (found && found.isValue())
                walker.addHoistedRow({ 0, inputHead, row, holder, key, found, false, WTF::move(path.reads), WTF::move(outerFields), path.object });
        }
    }
}

static ValueID elementTakenByPattern(const BodyIndex& index, const BodyValue& result)
{
    ValueID stepped = result.opcode() == Opcode::ResultAt && result.as<Opcode::ResultAt>().index() == 1 ? result.as<Opcode::ResultAt>().resultsOperand() : ValueID();
    if (!stepped || index.isParameter(stepped) || index.opcodeOf(stepped) != Opcode::IteratorResume)
        return { };
    auto step = index.value(stepped).as<Opcode::IteratorResume>();
    ValueID opened = step.iteratorOperand();
    if (!step.stepIndex() || !opened || index.isParameter(opened) || index.opcodeOf(opened) != Opcode::IteratorOpen || !index.value(opened).as<Opcode::IteratorOpen>().isDestructuring())
        return { };
    ValueID literal = index.value(opened).as<Opcode::IteratorOpen>().iterableOperand();
    if (!literal || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewArray || !index.hasOneUse(literal))
        return { };
    auto made = index.value(literal).as<Opcode::NewArray>();
    return made.holes().empty() && *step.stepIndex() < made.elementsOperands().size() ? made.elementsOperands()[*step.stepIndex()] : ValueID();
}

static bool isInstanceOfArraySubclass(Walker& walker, Reference name)
{
    return name && name.isValue() && isArraySubclassInstance(walker, walker.ownTypeOf(name));
}

static bool isOnlyAccessedByKey(const BodyIndex& index, const BodyValue& row)
{
    ValueID user = index.onlyUsingValue(row.id());
    return user && (index.opcodeOf(user) == Opcode::ElemGet || index.opcodeOf(user) == Opcode::ElemSet) && index.value(user).objectOperand() == row.id();
}

static bool iterationsOnlyTakeRows(Walker& walker, const BodyIndex& index, BlockID inputHead, Reference holder, uint32_t firstValueOfLoop)
{
    TypeID arrays = walker.types().builtinClass(BuiltinClasses::Array);
    auto takesRow = [&](const BodyValue& operation) {
        if (operation.opcode() != Opcode::ElemGet)
            return false;
        bool isHoisted = std::ranges::any_of(walker.hoistedRowsOf(inputHead), [&](const Walker::HoistedRow& row) {
            return !row.isRejected && row.holder == holder && row.row == walker.findHoistedRow(operation.id());
        });
        return isHoisted || (isOnlyAccessedByKey(index, operation) && (walker.canUseIterationGuards(inputHead) || isRowKeyInvariant(index, operation.propertyOperand(), firstValueOfLoop, 0)));
    };
    auto readsOwnName = [&](const BodyValue& operation) {
        return operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.key() != walker.program().keyOf(BuiltinNames::length) && !findMemberOnObjectAsMade(walker, arrays, operation.key());
    };
    auto stepsHolder = [&](const BodyValue& operation) {
        const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, operation, true);
        if (!stepped)
            stepped = findDestructuredEntriesIteration(walker, index, operation);
        return stepped && stepped->list == holder;
    };
    bool takesSomeRow = false;
    bool onlyTakesRows = std::ranges::all_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
        bool passesHolder = false;
        for (BodyTarget target : index.block(member).terminator().targets()) {
            passesHolder |= std::ranges::any_of(target.arguments(), [&](ValueID argument) {
                return argument && nameThatEntersLoopInListReadsInLoops(walker, index, argument, firstValueOfLoop) == holder;
            });
        }
        if (passesHolder)
            return false;
        return std::ranges::all_of(index.block(member).values(), [&](BodyValue operation) {
            bool usesHolder = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                usesHolder |= operand && nameThatEntersLoopInListReadsInLoops(walker, index, operand, firstValueOfLoop) == holder;
            });
            if (!usesHolder)
                return !stepsHolder(operation);
            if (opcodesThatStateTypes.contains(operation.opcode()) || readsOwnName(operation))
                return true;
            bool isRow = takesRow(operation);
            takesSomeRow |= isRow;
            return isRow;
        });
    });
    return takesSomeRow && onlyTakesRows;
}

static bool isExpectedArraySubclass(Walker& walker, ValueID list)
{
    TypeID expected = findExpectation(walker, list).type;
    TypeID present = expected ? walker.types().withoutNullish(expected) : TypeID();
    if (!present || holdsOnlyPrimitives(present))
        return false;
    const Type& record = walker.types().recordOf(present);
    return record.isObject() && record.hasOnlyLayouts() && !record.hasOnlyClosures() && std::ranges::all_of(membersOf(walker.types(), record.layouts), [&](LayoutID layout) {
        return builtinClassOfInstances(walker.program().at(layout)) == BuiltinClasses::Array;
    });
}

static bool iterationsOnlyReadSubclassElements(Walker& walker, const BodyIndex& index, BlockID inputHead, Reference holder, uint32_t firstValueOfLoop)
{
    if (!isProvenArray(walker, holder) || !walker.canUseIterationGuards(inputHead))
        return false;
    bool readsSubclass = false;
    bool onlyReadsElements = std::ranges::all_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
        for (BodyTarget target : index.block(member).terminator().targets()) {
            for (ValueID argument : target.arguments()) {
                if (argument && nameThatEntersLoopInListReadsInLoops(walker, index, argument, firstValueOfLoop) == holder)
                    return false;
            }
        }
        return std::ranges::all_of(index.block(member).values(), [&](BodyValue operation) {
            bool isUsed = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                isUsed |= operand && nameThatEntersLoopInListReadsInLoops(walker, index, operand, firstValueOfLoop) == holder;
            });
            if (!isUsed || opcodesThatStateTypes.contains(operation.opcode()))
                return true;
            if (operation.opcode() == Opcode::PropGet)
                return !operation.isFreeName() && operation.key() == walker.program().keyOf(BuiltinNames::length);
            if (operation.opcode() != Opcode::ElemGet || nameThatEntersLoopInListReadsInLoops(walker, index, operation.propertyOperand(), firstValueOfLoop) == holder)
                return false;
            readsSubclass |= isExpectedArraySubclass(walker, operation.objectOperand());
            return true;
        });
    });
    return readsSubclass && onlyReadsElements;
}

static LayoutID expectedSubclass(Walker& walker, const BodyIndex& index, BlockID inputHead, Reference array, uint32_t firstValueOfLoop, const Invocable<LayoutID(TypeID)> auto& findSubclass)
{
    static constexpr OpcodeSet accesses { Opcode::ElemGet, Opcode::ElemSet, Opcode::PropGet };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (!accesses.contains(operation.opcode()) || (operation.opcode() == Opcode::PropGet && operation.isFreeName()))
                continue;
            if (nameThatEntersLoopInListReadsInLoops(walker, index, operation.objectOperand(), firstValueOfLoop) == array)
                return findSubclass(findExpectation(walker, operation.objectOperand()).type);
        }
    }
    return { };
}

static bool rejectMutableRowHolders(Walker& walker, const BodyIndex& index, BlockID inputHead, ListsReadInLoop& found, const Invocable<TypeID(ValueID)> auto& typeInLook)
{
    std::span<Walker::HoistedRow> rows = walker.hoistedRowsOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto placeOf = [&](Reference name) {
        return found.lists.findIf([&](const HoistedArrayGuard& list) { return list.name == name; });
    };
    auto isAllocatedInLoop = [&](ValueID array) {
        ValueID made = index.stripCasts(array);
        if (!made || made.index() < firstValueOfLoop || index.isParameter(made) || !index.isInLoop(index.blockOf(made), index.loopOf(inputHead)))
            return false;
        return index.opcodeOf(made) == Opcode::NewArray || constructsArrayOfLength(walker, index, index.value(made));
    };
    bool storesOnlyIntoGuardedArrays = std::ranges::all_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
        return std::ranges::all_of(index.block(member).values(), [&](BodyValue store) {
            if (store.opcode() != Opcode::ElemSet || isViewOfNumbersReadInLine(walker, typeInLook(store.objectOperand())) || isAllocatedInLoop(store.objectOperand()))
                return true;
            Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, store.objectOperand(), firstValueOfLoop);
            size_t place = name ? placeOf(name) : notFound;
            return place != notFound && found.lists[place].isWritable && found.lists[place].storage != ArrayStorage::Contiguous;
        });
    });
    bool rejectsGuardedRow = false;
    for (Walker::HoistedRow& row : rows) {
        if (row.isRejected)
            continue;
        size_t place = placeOf(row.row);
        bool isStable = storesOnlyIntoGuardedArrays && place != notFound;
        if (isStable) {
            size_t holder = placeOf(row.holder);
            if (holder == notFound)
                found.lists.append(HoistedArrayGuard { row.holder, ArrayStorage::Contiguous });
            else if (found.lists[holder].isWritable)
                isStable = false;
            else {
                found.lists[holder].storage = ArrayStorage::Contiguous;
                found.lists[holder].admitsInt32Storage = false;
                found.lists[holder].hasAlternateStorage = false;
            }
        }
        if (isStable)
            continue;
        row.isRejected = true;
        rejectsGuardedRow |= place != notFound;
    }
    if (rejectsGuardedRow)
        return true;
    for (HoistedArrayGuard& list : found.lists) {
        bool mayHoldRows = list.storage == ArrayStorage::Contiguous && !list.isWritable && !list.admitsInt32Storage && !list.hasAlternateStorage && !list.written && !list.isAskedForItsStorageAlone;
        list.onlyHoldsRows = mayHoldRows && !classOfProvenViewInListReadsInLoops(walker, list.name) && iterationsOnlyTakeRows(walker, index, inputHead, list.name, firstValueOfLoop);
        if (list.onlyHoldsRows)
            walker.count(StatisticID::RowHoldersCheckedByStorage);
        if (walker.program().someClassExtendsArray && !list.onlyHoldsRows && !list.subclass && hasKind(walker.types(), walker.ownTypeOf(list.name), OperandKind::Array)) [[unlikely]] {
            list.subclass = subclassOfArrayName(walker, list.name);
            auto findSubclass = [&](TypeID expected) { return arraySubclassWithElements(walker, expected); };
            if (!list.subclass)
                list.subclass = isProvenArray(walker, list.name) ? expectedSubclass(walker, index, inputHead, list.name, firstValueOfLoop, findSubclass) : arraySubclassWithElements(walker, walker.ownTypeOf(list.name));
        }
        if (BuiltinClassID view = walker.program().someClassExtendsView && !list.subclass && !list.isAskedForItsStorageAlone ? classOfProvenViewInListReadsInLoops(walker, list.name) : BuiltinClassID()) [[unlikely]] {
            LayoutID subclass = expectedSubclass(walker, index, inputHead, list.name, firstValueOfLoop, [&](TypeID expected) { return viewSubclassWithLength(walker, expected); });
            list.subclass = subclass && walker.program().at(subclass).writtenBuiltinBase == view ? subclass : LayoutID();
        }
        bool mayHoldNumbers = !list.subclass && isNumericStorage(list.storage) && !list.isWritable && !list.turnsStoreIntoLists && !list.hasAlternateStorage && !list.written && !list.isAskedForItsStorageAlone && !list.holdsIndices && !list.isStepped;
        if (mayHoldNumbers && !classOfProvenViewInListReadsInLoops(walker, list.name) && iterationsOnlyReadSubclassElements(walker, index, inputHead, list.name, firstValueOfLoop))
            list.onlyHoldsRows = true;
    }
    found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
        return !list.onlyHoldsRows && !list.subclass && isInstanceOfArraySubclass(walker, list.name);
    });
    return false;
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

static bool isHolderSpeculatedView(Walker& walker, const BodyIndex& index, const BodyValue& store, TypeID holder, uint32_t firstValueOfLoop, std::span<const HoistedSpeculation> guesses)
{
    if (holder && isViewOfNumbersReadInLine(walker, holder))
        return true;
    Reference name = guesses.empty() ? Reference() : nameThatEntersLoopInListReadsInLoops(walker, index, store.objectOperand(), firstValueOfLoop);
    return name && std::ranges::any_of(guesses, [&](const HoistedSpeculation& guess) {
        return guess.value == name && guess.tested && isViewOfNumbersReadInLine(walker, guess.tested);
    });
}

static bool isNumericArrayElementInListReads(Walker& walker, const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses, std::span<const Reference> proofs)
{
    ValueID element = index.stripCasts(value);
    if (!element || element.index() < firstValueOfLoop || index.isParameter(element) || index.opcodeOf(element) != Opcode::ElemGet)
        return false;
    Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, index.value(element).objectOperand(), firstValueOfLoop);
    Reference proof;
    for (size_t place = 0; name && !proof && place < guesses.size() && place < proofs.size(); ++place) {
        if (guesses[place].value == name)
            proof = proofs[place];
    }
    return name && std::ranges::any_of(lists, [&](const HoistedArrayGuard& list) {
        return isNumericStorage(list.storage) && (list.name == name || (proof && list.name == proof));
    });
}

bool storesListedNumberIntoView(Walker& walker, const BodyIndex& index, const BodyValue& store, TypeID holder, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses, std::span<const Reference> proofs)
{
    return store.opcode() == Opcode::ElemSet && isHolderSpeculatedView(walker, index, store, holder, firstValueOfLoop, guesses) && isNumericArrayElementInListReads(walker, index, store.valueOperand(), firstValueOfLoop, lists, guesses, proofs);
}

bool isServedUnderListedElement(Walker& walker, const BodyIndex& index, const BodyValue& operation, const TypesOfInputs& typeInRow, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses, std::span<const Reference> proofs)
{
    if (lists.empty())
        return false;
    auto isListedElement = [&](ValueID value) {
        value = index.stripCasts(findUpdatedKey(walker, index, value));
        while (value && index.isParameter(value) && index.valueNarrowedInto(value))
            value = index.stripCasts(index.valueNarrowedInto(value));
        return value && isNumericArrayElementInListReads(walker, index, value, firstValueOfLoop, lists, guesses, proofs);
    };
    if (operation.opcode() == Opcode::ToPropertyKey)
        return isListedElement(operation.operand(0));
    bool isUnderListedElement = false;
    auto accessesView = [&](const BodyValue& access) {
        if (!isHolderSpeculatedView(walker, index, access, typeInRow(access.objectOperand()), firstValueOfLoop, guesses))
            return false;
        bool takesElement = isListedElement(access.propertyOperand());
        isUnderListedElement |= takesElement;
        return takesElement || walker.types().at(typeInRow(access.propertyOperand())).isInteger();
    };
    static constexpr unsigned mostSteps = 3;
    auto isNumber = [&](auto& self, ValueID value, unsigned stepsLeft) -> bool {
        if (walker.types().at(typeInRow(value)).isNumber() || isListedElement(value))
            return true;
        ValueID made = index.stripCasts(value);
        if (!stepsLeft || !made || made.index() < firstValueOfLoop || index.isParameter(made))
            return false;
        BodyValue maker = index.value(made);
        if (maker.opcode() == Opcode::ElemGet)
            return accessesView(maker);
        if (maker.opcode() != Opcode::Add && !numberProducingOperators.contains(maker.opcode()))
            return false;
        bool takesNumbers = true;
        maker.forEachOperand([&](ValueID operand, uint32_t) { takesNumbers = takesNumbers && self(self, operand, stepsLeft - 1); });
        return takesNumbers;
    };
    if (operation.opcode() == Opcode::ElemGet)
        return accessesView(operation) && isUnderListedElement;
    return operation.opcode() == Opcode::ElemSet && accessesView(operation) && isNumber(isNumber, operation.valueOperand(), mostSteps) && isUnderListedElement;
}

bool storesIntoListTestedInFrontOfLoop(Walker& walker, const BodyIndex& index, const BodyValue& store, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists)
{
    auto isWritable = [&](Reference name) {
        return std::ranges::any_of(lists, [&](const HoistedArrayGuard& list) { return list.isWritable && list.name == name; });
    };
    if (Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, store.objectOperand(), firstValueOfLoop))
        return isWritable(name);
    Vector<ChosenArray, 3> chosen = findChosenArrays(walker, index, store.objectOperand(), firstValueOfLoop);
    return !chosen.isEmpty() && std::ranges::all_of(chosen, [&](const ChosenArray& array) { return isWritable(array.name); });
}

bool isUntouchedLiteral(const BodyIndex& index, ValueID array, LoopID loop)
{
    static constexpr OpcodeSet literals { Opcode::NewArray, Opcode::Data };
    ValueID literal = index.stripCasts(array);
    if (!literal || index.isParameter(literal) || !literals.contains(index.opcodeOf(literal)) || index.isInLoop(index.blockOf(literal), loop))
        return false;
    std::optional<BodyUser> first = index.firstUser(literal);
    while (first && first->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(first->value)))
        first = index.firstUser(first->value);
    if (!first)
        return false;
    BlockID block = first->kind == BodyUseKind::Operand ? index.blockOf(first->value) : first->block;
    if (block.index() >= index.loop(loop).head.index())
        return true;
    if (first->kind != BodyUseKind::Operand || index.opcodeOf(first->value) != Opcode::ElemSet)
        return false;
    ValueID key = index.stripCasts(index.value(first->value).propertyOperand());
    return index.stripCasts(index.value(first->value).objectOperand()) == literal && !index.isParameter(key) && index.opcodeOf(key) == Opcode::Integer;
}

bool storesUnderSteppedCounter(Walker& walker, const BodyIndex& index, const BodyValue& store)
{
    LoopID loop = index.loopOf(index.blockOf(store.id()));
    if (!loop)
        return false;
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::ToNumeric };
    auto withoutSteps = [&](ValueID value) {
        for (value = index.stripCasts(value); value && !index.isParameter(value);) {
            BodyValue step = index.value(value);
            ValueID below = steps.contains(step.opcode()) ? step.operand(0) : ValueID();
            for (uint32_t side = 0; !below && step.opcode() == Opcode::Add && side < 2; ++side) {
                ValueID addend = index.stripCasts(step.operand(side));
                if (!index.isParameter(addend) && index.opcodeOf(addend) == Opcode::Integer && index.value(addend).as<Opcode::Integer>().integer() > 0)
                    below = step.operand(1 - side);
            }
            if (!below)
                return value;
            value = index.stripCasts(below);
        }
        return value;
    };
    BlockID head = index.loop(loop).head;
    ValueID counter = withoutSteps(store.propertyOperand());
    if (!counter || !index.isParameter(counter) || index.blockOf(counter) != head)
        return false;
    auto argumentOf = [&](const BodyIncomingEdge& edge, ValueID parameter) -> ValueID {
        BlockID block = index.blockOf(parameter);
        uint32_t position = parameter.index() - index.block(block).firstParameter().index();
        for (BodyTarget target : index.terminator(edge.from).targets()) {
            if (target.block() == block)
                return position < target.arguments().size() ? target.arguments()[position] : ValueID();
        }
        return { };
    };
    auto isSteppedUp = [&](auto& self, ValueID value, unsigned depth) -> bool {
        ValueID base = value ? withoutSteps(value) : ValueID();
        if (base == counter)
            return index.stripCasts(value) != counter;
        if (!base || !depth || !index.isParameter(base) || !index.isInLoop(index.blockOf(base), loop) || index.blockOf(base) == head)
            return false;
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, index.blockOf(base))) {
            if (self(self, argumentOf(edge, base), depth - 1))
                return true;
        }
        return false;
    };
    return walker.isSteppedUpOnBackEdge(counter, [&] {
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, head)) {
            if (index.isInLoop(edge.from, loop))
                return isSteppedUp(isSteppedUp, argumentOf(edge, counter), 3);
        }
        return false;
    });
}

static std::optional<int64_t> lengthOfLiteral(const Walker& walker, const BodyValue& literal)
{
    if (literal.opcode() == Opcode::NewArray)
        return literal.as<Opcode::NewArray>().elementsOperands().size();
    const auto* written = literal.opcode() == Opcode::Data && literal.data() ? walker.program().at(literal.data()).root.tryAs<DataNode::Array>() : nullptr;
    return written ? std::optional<int64_t>(written->elements.size()) : std::nullopt;
}

bool storesFromInitialLength(Walker& walker, const BodyIndex& index, const BodyValue& store)
{
    LoopID loop = index.loopOf(index.blockOf(store.id()));
    ValueID array = index.stripCasts(store.objectOperand());
    ValueID counter = index.stripCasts(store.propertyOperand());
    if (!loop || !array || index.isParameter(array) || index.isInLoop(index.blockOf(array), loop) || !counter || !index.isParameter(counter) || index.blockOf(counter) != index.loop(loop).head)
        return false;
    BodyValue made = index.value(array);
    std::optional<int64_t> length = lengthOfLiteral(walker, made);
    ValueID copied;
    if (!length && made.opcode() == Opcode::Call && made.as<Opcode::Call>().calleeKey() == walker.program().keyOf(BuiltinNames::slice) && made.as<Opcode::Call>().thisValueOperand()) {
        auto arguments = made.as<Opcode::Call>().argumentsOperands();
        ValueID begin = arguments.empty() ? ValueID() : index.stripCasts(arguments[0]);
        if (arguments.size() > 1 || (begin && (index.isParameter(begin) || index.opcodeOf(begin) != Opcode::Integer || index.value(begin).as<Opcode::Integer>().integer())))
            return false;
        copied = index.stripCasts(made.as<Opcode::Call>().thisValueOperand());
    }
    if (!length && !copied)
        return false;
    uint32_t position = counter.index() - index.block(index.loop(loop).head).firstParameter().index();
    bool enters = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, index.loop(loop).head)) {
        if (index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID entering = position < arguments.size() ? index.stripCasts(arguments[position]) : ValueID();
        if (!entering || index.isParameter(entering))
            return false;
        BodyValue entered = index.value(entering);
        bool isLength = length ? entered.opcode() == Opcode::Integer && entered.as<Opcode::Integer>().integer() == *length
            : entered.opcode() == Opcode::PropGet && entered.key() == walker.program().keyOf(BuiltinNames::length) && index.stripCasts(entered.objectOperand()) == copied;
        if (!isLength)
            return false;
        enters = true;
    }
    return enters && storesUnderSteppedCounter(walker, index, store);
}

static bool holdsLiteralOfLength(Walker& walker, const BodyIndex& index, const BodyValue& literal, int64_t length)
{
    if (literal.opcode() == Opcode::NewArray) {
        return std::ranges::any_of(literal.as<Opcode::NewArray>().elementsOperands(), [&](ValueID element) {
            ValueID written = index.stripCasts(element);
            return written && !index.isParameter(written) && lengthOfLiteral(walker, index.value(written)) == length;
        });
    }
    const auto* rows = literal.opcode() == Opcode::Data && literal.data() ? walker.program().at(literal.data()).root.tryAs<DataNode::Array>() : nullptr;
    return rows && std::ranges::any_of(rows->elements, [&](const DataNode& element) {
        const auto* written = element.tryAs<DataNode::Array>();
        return written && static_cast<int64_t>(written->elements.size()) == length;
    });
}

ValueID findUpdatingStore(const BodyIndex& index, ValueID read)
{
    static constexpr unsigned maxOperators = 6;
    LoopID loop = index.loopOf(index.blockOf(read));
    ValueID key = index.stripCasts(index.value(read).propertyOperand());
    if (!loop || !key || (index.isParameter(key) && index.blockOf(key) == index.loop(loop).head))
        return { };
    ValueID value = read;
    for (unsigned left = maxOperators; left; --left) {
        ValueID user = index.onlyUsingValue(value);
        if (!user)
            return { };
        Opcode opcode = index.opcodeOf(user);
        if (opcode == Opcode::ElemSet) {
            BodyValue store = index.value(user);
            BodyValue updated = index.value(read);
            bool isSameElement = store.valueOperand() == value && index.stripCasts(store.objectOperand()) == index.stripCasts(updated.objectOperand()) && index.stripCasts(store.propertyOperand()) == index.stripCasts(updated.propertyOperand());
            return isSameElement && index.loopOf(index.blockOf(user)) == index.loopOf(index.blockOf(read)) ? user : ValueID();
        }
        if (!opcodesThatStateTypes.contains(opcode) && !allBitwiseOperators.contains(opcode))
            return { };
        value = user;
    }
    return { };
}

ValueID findUpdatedRead(const BodyIndex& index, const BodyValue& store)
{
    static constexpr unsigned maxOperators = 6;
    if (store.opcode() != Opcode::ElemSet)
        return { };
    Vector<ValueID, 4> operands { index.stripCasts(store.valueOperand()) };
    for (size_t place = 0; place < operands.size() && place < maxOperators; ++place) {
        ValueID made = operands[place];
        if (!made || index.isParameter(made))
            continue;
        if (index.opcodeOf(made) == Opcode::ElemGet && findUpdatingStore(index, made) == store.id())
            return made;
        if (allBitwiseOperators.contains(index.opcodeOf(made)))
            index.value(made).forEachOperand([&](ValueID operand, uint32_t) { operands.append(index.stripCasts(operand)); });
    }
    return { };
}

template<typename Functor>
static bool callsFunctionReturningLiteral(Walker& walker, const BodyIndex& index, const BodyValue& call, const Functor& isExpectedLiteral)
{
    if (call.opcode() != Opcode::Call)
        return false;
    for (ValueID operand : call.as<Opcode::Call>().argumentsOperands()) {
        ValueID argument = index.stripCasts(operand);
        if (!argument || index.isParameter(argument) || index.opcodeOf(argument) != Opcode::NewClosure)
            continue;
        const BodyIndex* body = walker.facts().bodies().find(index.value(argument).as<Opcode::NewClosure>().function());
        for (uint32_t block = 0; body && block < body->numberOfBlocks(); ++block) {
            if (!body->isReachable(BlockID(block)) || body->terminator(BlockID(block)).kind() != TerminatorKind::Return)
                continue;
            ValueID returned = body->stripCasts(body->terminator(BlockID(block)).operand(0));
            if (returned && !body->isParameter(returned) && isExpectedLiteral(body->value(returned)))
                return true;
        }
    }
    return false;
}

bool storesFromRowLength(Walker& walker, const BodyIndex& index, const BodyValue& store, bool mayStartInside)
{
    LoopID loop = index.loopOf(index.blockOf(store.id()));
    ValueID row = index.stripCasts(store.objectOperand());
    ValueID counter = index.stripCasts(store.propertyOperand());
    if (!loop || !row || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet || !counter || !index.isParameter(counter) || index.blockOf(counter) != index.loop(loop).head)
        return false;
    uint32_t position = counter.index() - index.block(index.loop(loop).head).firstParameter().index();
    std::optional<int64_t> start;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, index.loop(loop).head)) {
        if (index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID entering = position < arguments.size() ? index.stripCasts(arguments[position]) : ValueID();
        if (!entering || index.isParameter(entering) || index.opcodeOf(entering) != Opcode::Integer)
            return false;
        int64_t integer = index.value(entering).as<Opcode::Integer>().integer();
        if (integer < 0 || (start && *start != integer))
            return false;
        start = integer;
    }
    ValueID holder = index.stripCasts(index.value(row).objectOperand());
    if (!start || !holder || !storesUnderSteppedCounter(walker, index, store))
        return false;
    auto isExpectedLiteral = [&](const BodyValue& literal) {
        std::optional<int64_t> length = lengthOfLiteral(walker, literal);
        return length && (*length == *start || (mayStartInside && *length == 1 && !*start));
    };
    if (!index.isParameter(holder) && (holdsLiteralOfLength(walker, index, index.value(holder), *start) || callsFunctionReturningLiteral(walker, index, index.value(holder), isExpectedLiteral)))
        return true;
    auto isExpectedRow = [&](ValueID value) {
        ValueID stored = index.stripCasts(value);
        return stored && !index.isParameter(stored) && isExpectedLiteral(index.value(stored));
    };
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        for (BodyValue rowStore : index.block(BlockID(block)).values()) {
            if (rowStore.opcode() == Opcode::ElemSet && index.stripCasts(rowStore.objectOperand()) == holder && isExpectedRow(rowStore.valueOperand()))
                return true;
            if (rowStore.opcode() != Opcode::Call || rowStore.as<Opcode::Call>().calleeKey() != walker.program().keyOf(BuiltinNames::push))
                continue;
            auto push = rowStore.as<Opcode::Call>();
            if (push.thisValueOperand() && index.stripCasts(push.thisValueOperand()) == holder && std::ranges::any_of(push.argumentsOperands(), isExpectedRow))
                return true;
        }
    }
    return false;
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

static bool isBornBoxed(Walker& walker, Reference literal)
{
    const Operation& born = walker.code().at(underlyingCellOf(walker.code(), literal.id()));
    if (born.opcode != Opcode::NewArray && born.opcode != Opcode::Data)
        return false;
    if (born.opcode == Opcode::NewArray)
        return !born.elementsOperands().empty() && !isNumericStorage(born.arrayStorage());
    const auto* written = born.opcode == Opcode::Data && born.data() ? walker.program().at(born.data()).root.tryAs<DataNode::Array>() : nullptr;
    return !written || !std::ranges::all_of(written->elements, [](const DataNode& element) { return element.is<DataNode::Number>(); });
}

Reference untouchedLiteralOfPush(Walker& walker, const BodyValue& call, LoopID loop)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.isPushOntoLiteral(call))
        return { };
    ValueID literal = index.stripCasts(call.as<Opcode::Call>().thisValueOperand());
    Reference list = index.isInLoop(index.blockOf(literal), loop) ? Reference() : walker.referenceOf(literal);
    return list && list.isValue() && isProvenArray(walker, list) ? list : Reference();
}

static bool callsBuiltinInListReads(Walker& walker, const BodyValue& call, BuiltinID builtin)
{
    Reference callee = walker.referenceOf(call.as<Opcode::Call>().calleeOperand());
    BuiltinID called = callee && walker.typeOf(callee) ? singleBuiltinOf(walker.types(), walker.types().at(walker.typeOf(callee))) : BuiltinID();
    const BuiltinTable& table = walker.program().builtins();
    return called && table.canonicalID(called) == table.canonicalID(builtin);
}

static ValueID receiverOfMethodCall(const Program& program, const BodyIndex& index, const BodyValue& call, BuiltinName method, size_t numberOfArguments)
{
    if (call.opcode() != Opcode::Call || !call.as<Opcode::Call>().thisValueOperand() || call.as<Opcode::Call>().argumentsOperands().size() != numberOfArguments || call.as<Opcode::Call>().calleeKey() != program.keyOf(method))
        return { };
    return index.stripCasts(call.as<Opcode::Call>().thisValueOperand());
}

static ValueID findMethodReceiverField(const Program& program, const BodyIndex& index, const BodyValue& call, BuiltinName method, size_t numberOfArguments)
{
    ValueID list = receiverOfMethodCall(program, index, call, method, numberOfArguments);
    return list && !index.isParameter(list) && index.opcodeOf(list) == Opcode::PropGet && !index.value(list).isFreeName() ? list : ValueID();
}

static ValueID fieldPushedOnto(const Program& program, const BodyIndex& index, const BodyValue& call)
{
    return findMethodReceiverField(program, index, call, BuiltinNames::push, 1);
}

bool Walker::pushesOntoField(const BodyIndex& body)
{
    if (!body.opcodes().contains(Opcode::Call) || !body.opcodes().contains(Opcode::PropGet))
        return false;
    auto added = m_bodiesPushingOntoFields.add(&body, false);
    for (uint32_t block = 0; added.isNewEntry && block < body.numberOfBlocks() && !added.iterator->value; ++block) {
        added.iterator->value = std::ranges::any_of(body.block(BlockID(block)).values(), [&](BodyValue call) {
            return !!fieldPushedOnto(program(), body, call);
        });
    }
    return added.iterator->value;
}

static Reference findMethodReceiverFieldArray(Walker& walker, const BodyValue& call, BuiltinName name, BuiltinID method, size_t numberOfArguments)
{
    if (call.opcode() != Opcode::Call || call.as<Opcode::Call>().calleeKey() != walker.program().keyOf(name) || !walker.isInLoop())
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    if (call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words())
        return { };
    ValueID receiver = receiverOfMethodCall(walker.program(), index, call, name, numberOfArguments);
    if (receiver && (index.isParameter(receiver) || index.opcodeOf(receiver) != Opcode::PropGet || index.value(receiver).isFreeName())) {
        LoopID loop = index.loopOf(index.blockOf(call.id()));
        static constexpr OpcodeSet readsOfVariables { Opcode::CheckInitialized, Opcode::GetModuleVariable, Opcode::GetEnvironmentField };
        bool isReadInTurn = loop && index.isInLoop(index.blockOf(receiver), loop);
        if (!loop || isLiteralOfBody(index, receiver) || (isReadInTurn && (index.isParameter(receiver) || !readsOfVariables.contains(index.opcodeOf(receiver)))) || !walker.isOutermostPushPopLoop(loop) || walker.isPushOntoLiteral(call) || walker.isPopFromLiteral(call))
            return { };
    }
    Reference list = receiver ? walker.referenceOf(receiver) : Reference();
    if (!list || !list.isValue() || !isProvenArray(walker, list))
        return { };
    return callsArrayMethod(walker, call, list, name, method) || callsBuiltinInListReads(walker, call, method) ? list : Reference();
}

Reference fieldArrayPushedOnto(Walker& walker, const BodyValue& call)
{
    return findMethodReceiverFieldArray(walker, call, BuiltinNames::push, Builtins::ArrayPrototypePush, 1);
}

Reference fieldArrayPoppedFrom(Walker& walker, const BodyValue& call)
{
    return findMethodReceiverFieldArray(walker, call, BuiltinNames::pop, Builtins::ArrayPrototypePop, 0);
}

bool callsArrayMethod(Walker& walker, const BodyValue& call, Reference list, BuiltinName name, BuiltinID method)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID called = findConstantMemberReadOff(walker, list, walker.program().keyOf(name));
    return called && table.canonicalID(called) == table.canonicalID(method) && call.opcode() == Opcode::Call;
}

bool callsArrayPush(Walker& walker, const BodyValue& call, Reference list)
{
    return callsArrayMethod(walker, call, list, BuiltinNames::push, Builtins::ArrayPrototypePush);
}

std::optional<HoistedArrayGuard> findHoistedArrayGuard(Walker& walker, const BodyValue& read)
{
    if (walker.state().answers().isEmpty())
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID outermost = index.loopOf(index.blockOf(read.id()));
    if (!outermost && !walker.isInLoop() && !walker.guardedReadArray() && !walker.isInRecursiveCopy())
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

std::optional<HoistedArrayGuard> findRowHolderGuard(Walker& walker, const BodyValue& read)
{
    Reference list = read.opcode() == Opcode::ElemGet && !walker.state().answers().isEmpty() ? walker.referenceOf(read.objectOperand()) : Reference();
    std::optional<HoistedArrayGuard> guard = list && list.isValue() ? findNoteInListReadsInLoops(walker, list) : std::nullopt;
    return guard && guard->onlyHoldsRows ? guard : std::nullopt;
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
    PristineAspectSet aspects { PristineAspects::InheritedNamesKept };
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
    if (std::optional<uint64_t> noted = findKnown(walker, KnownNote::LengthOfViewIsFoundAbove, view); noted && *noted >= subclassesWithLength) {
        LayoutID subclass(static_cast<uint32_t>(*noted - subclassesWithLength));
        checkStructureOfClassThatIsRemembered(walker, view, subclass, origin);
        MemberConditionRecord condition { MemberConditionKind::MemberUnchanged, subclass, ClassSide::Prototype, walker.program().keyOf(BuiltinNames::length) };
        checkGuardsThatAreRemembered(walker, singleElementSpan(condition), origin);
        return;
    }
    Draft test = draftTestOfViewOrInstanceBelow(walker, view, origin);
    const BuiltinTable& table = walker.program().builtins();
    BuiltinClassID above = test.builtinClass() ? table.at(test.builtinClass()).parent : BuiltinClassID();
    if (above && classBelowWithTestedStructure(table, above) == test.builtinClass() && instancesBelowFindTheSameLength(walker, above))
        test.setBuiltinClass(above);
    walker.verifyRememberedTest(WTF::move(test));
}

void noteThatLengthOfViewIsFoundAbove(Walker& walker, Reference view)
{
    noteKnown(walker, KnownNote::LengthOfViewIsFoundAbove, view, { }, 0, LocationMasks(heapsUnderViewsReadInLoops), 1);
}

void noteInheritedSubclassLength(Walker& walker, Reference view, LayoutID subclass)
{
    static constexpr AbstractHeapSet heaps = heapsUnderViewsReadInLoops | AbstractHeapSet { AbstractHeap::MemberGuards };
    noteKnown(walker, KnownNote::LengthOfViewIsFoundAbove, view, { }, 0, LocationMasks(heaps), subclassesWithLength + subclass.index());
}

void noteThatViewKeepsItsLength(Walker& walker, Reference view, Reference test)
{
    if (!view || !view.isValue() || !test || !test.isValue() || walker.code().isConstant(test.id()) || !someThreadMayGrowSharedMemory(walker))
        return;
    const Operation& asked = walker.code().at(test.id());
    if (asked.opcode != Opcode::IsPristine || !isTypedView(walker.program().builtins().typedArrayTypeOf(asked.builtinClass())))
        return;
    bool isToldByItsStructure = pristineTestOf(walker.program().builtins(), asked.builtinClass()) == PristineTest::InstanceStructure;
    if (isToldByItsStructure || asked.aspects().contains(PristineAspects::Storage))
        noteKnown(walker, KnownNote::ViewOverMemoryThatNoOtherThreadGrows, view, { }, 0, LocationMasks(), 1);
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

void takeHoistedObjectStorage(Walker& walker, Draft& read)
{
    Reference object = read.objectOperand();
    if (!walker.isInLoop() || !walker.types().at(walker.typeOf(object)).isObject())
        return;
    Origin origin = read.origin();
    origin.isTailCall = false;
    Draft storage(Opcode::GetObjectStorage, origin);
    walker.chooseConservativeFields(storage);
    storage.setObjectOperand(object);
    Reference elements = buildInFrontOfTurnsInListReadsInLoops(walker, WTF::move(storage));
    if (!elements)
        return;
    std::array<Reference, 2> lengths;
    for (bool countsDoubles : { false, true }) {
        Draft length(Opcode::GetObjectStorageLength, origin);
        walker.chooseConservativeFields(length);
        length.setObjectOperand(object);
        length.setCountsDoubles(countsDoubles);
        lengths[countsDoubles] = buildInFrontOfTurnsInListReadsInLoops(walker, WTF::move(length));
        if (!lengths[countsDoubles])
            return;
    }
    read.setElementsOperand(elements);
    read.setNumberOfValuesOperand(lengths[false]);
    read.setNumberOfDoublesOperand(lengths[true]);
    walker.count(StatisticID::OrdinaryObjectStorageReadsHoisted);
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

static constexpr OpcodeSet accessesThatTakeKindForGranted {
    Opcode::GetContiguousIndex, Opcode::GetDoubleIndex, Opcode::PutContiguousIndex, Opcode::PutDoubleIndex, Opcode::GetOwnDoubleIndex, Opcode::GetOwnInt32Index,
    Opcode::PutOwnContiguousIndex, Opcode::PutOwnDoubleIndex, Opcode::PutOwnInt32Index, Opcode::ArrayStorageKind
};

static bool bindElementOfFreshArray(Walker& walker, const BodyValue& value)
{
    if ((value.opcode() != Opcode::HasContiguousIndex && value.opcode() != Opcode::GetOwnContiguousIndex) || walker.currentBodyIndex().holdsAnyOf(accessesThatTakeKindForGranted))
        return false;
    Reference array = walker.referenceOf(value.arrayOperand());
    Reference position = walker.referenceOf(value.positionOperand());
    if (!array || !position || !position.isConstant() || walker.code().at(position.id()).opcode != Opcode::Integer)
        return false;
    int64_t index = walker.code().at(position.id()).integer();
    Reference element = index >= 0 && index < maxComputedElementsOfArrayLiteral ? elementOfFreshList(walker, array, static_cast<uint32_t>(index)) : Reference();
    if (!element)
        return false;
    if (value.opcode() == Opcode::GetOwnContiguousIndex) {
        walker.bind(value.id(), element);
        return true;
    }
    Draft isPresent(Opcode::Boolean, walker.originOf(value));
    isPresent.setBoolean(true);
    walker.bind(value.id(), walker.constant(WTF::move(isPresent)));
    return true;
}

static std::optional<GuardedReadInListReadsInLoops> findGuardedReadInListReadsInLoops(const BodyIndex& index)
{
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

static bool isSumOfIntegers(const Code& code, Reference number)
{
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement };
    if (!number.isValue())
        return false;
    const Operation& made = code.at(underlyingCellOf(code, number.id()));
    return sums.contains(made.opcode) && (made.form() == OperatorForm::CheckedIntegers || made.form() == OperatorForm::Integers);
}

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
    bool isNumber = stored && walker.typeOf(stored) && walker.types().at(walker.typeOf(stored)).isNumber();
    bool goesByKind = written.arrayStorage() == ArrayStorage::Double && isNumber;
    bool storesInt32s = written.arrayStorage() == ArrayStorage::Int32 && isNumber;
    if (written.arrayStorage() != ArrayStorage::Contiguous && !goesByKind && !storesInt32s)
        return false;
    auto staysTyped = [&] {
        if (goesByKind && remembered && walker.code().at(birth).remembersStorageAtSite())
            walker.code().letListForgetStorageAtSite(birth);
        return false;
    };
    bool mayBeNaN = goesByKind && !canStoreWithoutConversion(walker.types(), ArrayStorage::Double, walker.typeOf(stored)) && !isSumOfIntegers(walker.code(), stored);
    bool mayBeNoInt32 = storesInt32s && !walker.types().isSubset(walker.typeOf(stored), TypeTable::int32);
    if ((mayBeNaN || mayBeNoInt32) && !walker.canRestartIteration())
        return staysTyped();
    ArrayStorage kindAsked = goesByKind ? ArrayStorage::Other : written.arrayStorage();
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
    if (mayBeNoInt32) {
        Draft isInt32(Opcode::TypeTest, origin);
        walker.chooseConservativeFields(isInt32);
        isInt32.setValueOperand(stored);
        isInt32.setTestedType(TypeTable::int32);
        Reference test = walker.build(WTF::move(isInt32));
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
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(stored);
        narrowed.setTestOperand(test);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        stored = walker.build(WTF::move(narrowed));
        if (!stored)
            return true;
    }
    Draft store(Opcode::PutContiguousIndex, origin);
    walker.chooseConservativeFields(store);
    store.setArrayOperand(list);
    store.setPositionOperand(position);
    store.setValueOperand(stored);
    store.setIsInBounds(*inside);
    if (storesInt32s) {
        store.setHasNumericStorage(true);
        walker.count(StatisticID::HoistedInt32ArrayStores);
    }
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
    Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
    return walker.build(WTF::move(storage));
}

bool askPlainVectorBehindRowOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    if (value.opcode() == Opcode::TryPutByValDirect)
        return tryStoreWithHoistedStorageGuard(walker, value);
    if (bindElementOfFreshArray(walker, value))
        return true;
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
    walker.count(StatisticID::ArrayStorageLoadsHoisted);
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

static bool movesElementWithinArray(const BodyIndex& index, const BodyValue& read)
{
    ValueID moved = read.id();
    ValueID user = index.onlyUsingValue(moved);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        moved = user;
        user = index.onlyUsingValue(user);
    }
    if (!user || index.opcodeOf(user) != Opcode::ElemSet)
        return false;
    BodyValue store = index.value(user);
    return store.valueOperand() == moved && index.stripCasts(store.objectOperand()) == index.stripCasts(read.objectOperand());
}

enum class UseByCallers : uint8_t { Unknown, ComputesNumber, StoresElement };
static UseByCallers findUseByCallers(Walker&, const BodyValue& read);

static bool calleeCalculatesWith(Walker& walker, const BodyValue& call, ValueID argument)
{
    static constexpr unsigned maxOperatorsOfUnrolledLoop = 18;
    FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id());
    const BodyIndex* body = callee ? walker.facts().bodies().find(callee) : nullptr;
    if (!body || body->hasLoops())
        return false;
    const Function& function = walker.program().at(callee);
    std::span<const ValueID> arguments = call.as<Opcode::Call>().argumentsOperands();
    auto place = std::ranges::find(arguments, argument);
    size_t position = place - arguments.begin();
    if (place == arguments.end() || position >= function.numberOfPositionalParameters())
        return false;
    ValueID parameter = body->block(BlockID(0u)).parameter(function.firstParameterIndex() + position);
    uint32_t usesLeft = body->useCount(parameter);
    if (!usesLeft || usesLeft >= BodyIndex::maxCountedUses)
        return false;
    unsigned numberOfOperators = 0;
    for (uint32_t block = 0; block < body->numberOfBlocks(); ++block) {
        for (BodyValue operation : body->block(BlockID(block)).values()) {
            numberOfOperators += allNumericOperators.contains(operation.opcode());
            uint32_t uses = 0;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                uses += operand == parameter;
            });
            if (uses && !allNumericOperators.contains(operation.opcode()))
                return false;
            usesLeft -= uses;
        }
    }
    if (usesLeft)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    for (BlockID member : loop ? index.membersOf(loop) : std::span<const BlockID>()) {
        for (BodyValue operation : index.block(member).values())
            numberOfOperators += allNumericOperators.contains(operation.opcode()) || operation.opcode() == Opcode::ElemGet;
    }
    return loop && numberOfOperators > maxOperatorsOfUnrolledLoop;
}

bool isTermOfRemainder(const BodyIndex& index, ValueID term)
{
    static constexpr unsigned maxTerms = 4;
    for (unsigned place = 0; place < maxTerms; ++place) {
        ValueID user = index.onlyUsingValue(term);
        while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
            term = user;
            user = index.onlyUsingValue(user);
        }
        if (!user || index.isParameter(user))
            return false;
        BodyValue operation = index.value(user);
        if (operation.opcode() == Opcode::Mod) {
            ValueID divisor = index.stripCasts(operation.operand(1));
            return operation.operand(0) == term && divisor && !index.isParameter(divisor) && index.opcodeOf(divisor) == Opcode::Integer && index.value(divisor).as<Opcode::Integer>().integer() > 0;
        }
        if (operation.opcode() != Opcode::Add && operation.opcode() != Opcode::Sub && operation.opcode() != Opcode::Mul)
            return false;
        term = user;
    }
    return false;
}

bool isCounterStartTerm(const BodyIndex& index, ValueID term)
{
    static constexpr unsigned maxTerms = 4;
    for (unsigned place = 0; place < maxTerms; ++place) {
        std::optional<BodyUser> user = index.onlyUser(term);
        while (user && user->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(user->value))) {
            term = user->value;
            user = index.onlyUser(term);
        }
        if (!user)
            return false;
        if (user->kind == BodyUseKind::EdgeArgument) {
            for (BodyTarget target : index.terminator(user->block).targets()) {
                if (!std::ranges::contains(target.arguments(), term))
                    continue;
                return index.isLoopHead(target.block()) && !index.isInLoop(user->block, index.loopOf(target.block()));
            }
            return false;
        }
        if (user->kind != BodyUseKind::Operand || index.isParameter(user->value) || (index.opcodeOf(user->value) != Opcode::Add && index.opcodeOf(user->value) != Opcode::Sub))
            return false;
        term = user->value;
    }
    return false;
}

bool hasRemainderTerms(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    if (!index.holdsAnyOf(OpcodeSet { Opcode::Mod }) || !index.holdsAnyOf(OpcodeSet { Opcode::ElemGet }) || index.loop(loop).numberOfLoopsWithin != 1 || index.loop(loop).parent)
        return false;
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue read) {
            return read.opcode() == Opcode::ElemGet && isTermOfRemainder(index, read.id());
        });
    });
}

static bool isReadOfBodyInListReadsInLoops(const BodyIndex&, const BodyValue& read);

static bool isSteppedInPlace(Walker& walker, const BodyIndex& index, const BodyValue& read)
{
    auto isInt32 = [&](ValueID addend) {
        if (!index.isParameter(addend) && index.opcodeOf(addend) == Opcode::Integer)
            return true;
        Reference built = walker.referenceOf(addend);
        TypeID type = built && built.isRecord() ? walker.typeOf(built) : TypeID();
        return type && !isNeverType(type) && walker.types().isSubset(type, TypeTable::int32);
    };
    static constexpr unsigned maxSteps = 4;
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement };
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    if (read.opcode() != Opcode::ElemGet || !isReadOfBodyInListReadsInLoops(index, read))
        return false;
    ValueID value = read.id();
    bool isStepped = false;
    for (unsigned step = 0; step < maxSteps; ++step) {
        ValueID user = index.onlyUsingValue(value);
        if (!user || index.isParameter(user))
            return false;
        BodyValue operation = index.value(user);
        if (operation.opcode() == Opcode::ElemSet)
            return isStepped && operation.valueOperand() == value && operation.objectOperand() == read.objectOperand() && operation.propertyOperand() == read.propertyOperand();
        if (sums.contains(operation.opcode()) && operation.operand(0) == value) {
            ValueID addend = index.stripCasts(operation.operand(1));
            if (!addend || !isInt32(addend) || std::exchange(isStepped, true))
                return false;
        } else if (steps.contains(operation.opcode())) {
            if (std::exchange(isStepped, true))
                return false;
        } else if (operation.opcode() != Opcode::ToNumeric && !opcodesThatStateTypes.contains(operation.opcode()))
            return false;
        value = user;
    }
    return false;
}

bool isDefaultedElement(const Walker& walker, const BodyIndex& index, ValueID read)
{
    BlockID block = index.blockOf(read);
    BodyTerminator end = index.block(block).terminator();
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!condition || index.isParameter(condition) || !index.hasOneUse(condition))
        return false;
    bool isTestOfNullish = index.opcodeOf(condition) == Opcode::TypeTest && holdsOnlyPrimitives(index.value(condition).testedType()) && walker.types().at(index.value(condition).testedType()).hasOnlyPrimitives(Primitive::UndefinedOrNull);
    if ((!isTestOfNullish && index.opcodeOf(condition) != Opcode::ToBoolean) || index.value(condition).operand(0) != read)
        return false;
    uint32_t targetOfDefault = isTestOfNullish ? 0 : 1;
    uint32_t uses = 1;
    bool isUsedWherePresent = false;
    uint32_t targetIndex = 0;
    for (BodyTarget target : end.targets()) {
        for (uint32_t proofIndex = 0; proofIndex < target.proofs().size(); ++proofIndex) {
            std::optional<BodyEdgeResult> result = index.edgeResult(block, targetIndex, target, proofIndex);
            if (!result || (targetIndex == targetOfDefault && !index.isUnused(result->parameter)))
                return false;
            isUsedWherePresent |= targetIndex != targetOfDefault && !index.isUnused(result->parameter);
            ++uses;
        }
        ++targetIndex;
    }
    return isUsedWherePresent && index.useCount(read) == uses;
}

bool holeLeavesTurnBehindRow(Walker& walker, const BodyValue& read, const HoistedArrayGuard& tested)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (tested.storage == ArrayStorage::Int32 && walker.depthOfCopies() && isSteppedInPlace(walker, index, read))
        return true;
    if (holdsBoxedValuesOfEitherKind(tested) && tested.isWritable)
        return movesElementWithinArray(index, read);
    if (!isNumericStorage(tested.storage))
        return false;
    if (read.id().index() < index.numberOfValues() && index.value(read.id()).words() == read.words() && isDefaultedElement(walker, index, read.id())) {
        walker.count(StatisticID::DefaultedElementsReadInIterations);
        return false;
    }
    if (tested.turnsStoreIntoLists || (tested.storage == ArrayStorage::Int32 && (walker.turnHoldsInnerLoops() || walker.isInUnswitchedVersion())))
        return true;
    if (LoopID loop = tested.storage == ArrayStorage::Int32 && !tested.isWritable ? index.loopOf(index.blockOf(read.id())) : LoopID(); loop && searchesNumbersByHalves(walker, index.loop(loop).head))
        return true;
    if (tested.storage == ArrayStorage::Int32 && isTermOfRemainder(index, read.id()))
        return true;
    std::optional<BodyUser> user = index.onlyUser(read.id());
    ValueID name = read.id();
    while (user && user->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(user->value))) {
        name = user->value;
        user = index.onlyUser(user->value);
    }
    if (user && user->kind == BodyUseKind::Operand && index.opcodeOf(user->value) == Opcode::Call && calleeCalculatesWith(walker, index.value(user->value), name)) {
        walker.count(StatisticID::PassedHolesLeaveIterations);
        return true;
    }
    return (user && user->kind == BodyUseKind::EdgeArgument) || turnTakesElementForKeyInListReadsInLoops(walker, read.id()) || (walker.depthOfCopies() && findUseByCallers(walker, read) == UseByCallers::StoresElement);
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
    return name && name.isValue() && hasKind(walker.types(), walker.ownTypeOf(name), OperandKind::Array) && !isArraySubclassInstance(walker, walker.ownTypeOf(name));
}

bool isFreshAllocation(Walker& walker, Reference name)
{
    if (!name || !name.isValue())
        return false;
    ValueID cell = underlyingCellOf(walker.code(), name.id());
    if (walker.code().isConstant(cell) || !walker.state().isFresh(Reference::value(cell)))
        return false;
    Opcode opcode = walker.code().at(cell).opcode;
    bool isOfCallee = opcode == Opcode::CallDirect || (opcode == Opcode::Parameter && walker.isMergedFreshObject(cell));
    return !isOfCallee || isListOfKnownLength(walker, walker.state(), Reference::value(cell));
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

static size_t writtenLengthOfLiteral(const BodyIndex& index, ValueID list, const Operation& born)
{
    ValueID literal = list ? index.stripCasts(list) : ValueID();
    bool isAppended = literal && !index.isParameter(literal) && index.opcodeOf(literal) == Opcode::NewArray && born.elementsOperands().empty();
    return isAppended ? index.value(literal).as<Opcode::NewArray>().elementsOperands().size() : born.elementsOperands().size();
}

static bool isBornEmptyInListReadsInLoops(Walker& walker, const BodyIndex& index, ValueID list, Reference name)
{
    if (isWrittenEmptyInListReadsInLoops(walker, index, list))
        return true;
    if (!name.isValue())
        return false;
    const auto& born = walker.code().at(underlyingCellOf(walker.code(), name.id()));
    return born.opcode == Opcode::NewArray && born.holes().empty() && !writtenLengthOfLiteral(index, list, born);
}

static bool isAllocatedEmpty(Walker& walker, const BodyIndex& index, ValueID list, Reference name)
{
    if (isBornEmptyInListReadsInLoops(walker, index, list, name))
        return true;
    if (!name.isValue())
        return false;
    const Operation& born = walker.code().at(underlyingCellOf(walker.code(), name.id()));
    if (born.opcode != Opcode::NewArrayLike || born.sourceOperand())
        return false;
    const Type& length = walker.types().at(walker.code().at(born.lengthOperand()).type);
    return length.isInteger() && length.integers == Range::one(0);
}

static ArrayStorage predictedArrayStorage(Walker&, TypeID list);

static constexpr OpcodeSet numberConstantOpcodes { Opcode::Double, Opcode::Integer };
static constexpr OpcodeSet operatorsThatMakeNumbersOfNumbers { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };

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

ArrayStorage predictedStorageOfUnbuiltList(Walker& walker, ValueID unbuiltList)
{
    TypeID expected = expectedOfRowThatIsNotBuiltInListReadsInLoops(walker, unbuiltList);
    return expected ? predictedArrayStorage(walker, walker.types().withoutNullish(expected)) : ArrayStorage::Other;
}

bool isUnbuiltListOfNumbers(Walker& walker, ValueID unbuiltList)
{
    return isNumericStorage(predictedStorageOfUnbuiltList(walker, unbuiltList));
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

static bool startsAsNumber(Walker& walker, const BodyIndex& index, ValueID sum)
{
    LoopID loop = index.isParameter(sum) ? index.loopOf(index.blockOf(sum)) : LoopID();
    if (!loop || index.loop(loop).head != index.blockOf(sum))
        return false;
    uint32_t position = sum.index() - index.block(index.blockOf(sum)).firstParameter().index();
    bool enters = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, index.blockOf(sum))) {
        if (index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID entering = position < arguments.size() ? index.stripCasts(arguments[position]) : ValueID();
        if (!entering || index.isParameter(entering) || !numberConstantOpcodes.contains(index.opcodeOf(entering)))
            return false;
        enters = true;
    }
    return enters;
}

static bool isAddedToSumOfNumbers(Walker& walker, const BodyIndex& index, const BodyValue& read)
{
    ValueID name = read.id();
    ValueID user = index.onlyUsingValue(name);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    if (!user || index.opcodeOf(user) != Opcode::Add)
        return false;
    BodyValue sum = index.value(user);
    return startsAsNumber(walker, index, index.stripCasts(sum.operand(sum.operand(0) == name ? 1 : 0)));
}

static ALWAYS_INLINE bool readsRowOfNumbersThatTurnFindsInListedList(Walker& walker, const BodyIndex& index, const BodyValue& operation, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses = { }, Opcode access = Opcode::ElemGet)
{
    ValueID row = operation.opcode() == access ? index.stripCasts(operation.objectOperand()) : ValueID();
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
    list = list ? types.withoutPrimitives(list, Primitive::Absent) : TypeID();
    const ClassPart* part = list && !holdsOnlyPrimitives(list) ? singleBuiltinClassOf(types, types.at(list)) : nullptr;
    if (!part || part->builtinClass != BuiltinClasses::Array)
        return false;
    TypeID rows = part->expectedContentsAt(walker.program().builtins(), ContentsSlot::Element).predicted();
    if (rows && isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(rows))))
        return true;
    auto isSeenAsNumber = [&](const BodyValue& read) {
        TypeID seen = read.opcode() == Opcode::ElemGet ? seenUnderVariableKey(walker, read) : TypeID();
        return seen && !isNeverType(seen) && isNumericStorage(arrayStorageHolding(types, types.withoutNullish(seen)));
    };
    if (access == Opcode::ElemGet && (isSeenAsNumber(operation) || isAddedToSumOfNumbers(walker, index, operation)))
        return true;
    auto holdsNumbersUnderConstantIndexes = [&] {
        LoopID loop = index.loopOf(index.blockOf(operation.id()));
        if (&index != &walker.currentBodyIndex() || !loop)
            return false;
        std::optional<int64_t> firstIndex;
        bool readsSeveralIndexes = false;
        for (Walker::ElementAccess other : walker.elementAccessesOf(row)) {
            BodyValue read = index.value(ValueID(other.access));
            if (read.opcode() != Opcode::ElemGet || index.loopOf(index.blockOf(read.id())) != loop)
                continue;
            TypeID seen = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
            if (!seen || isNeverType(seen) || !isNumericStorage(arrayStorageHolding(types, types.withoutNullish(seen))))
                return false;
            ValueID key = index.stripCasts(read.propertyOperand());
            if (index.isParameter(key) || index.opcodeOf(key) != Opcode::Integer)
                continue;
            int64_t written = index.value(key).integer();
            readsSeveralIndexes |= firstIndex && *firstIndex != written;
            firstIndex = written;
        }
        return readsSeveralIndexes;
    };
    if (holdsNumbersUnderConstantIndexes())
        return true;
    if (access == Opcode::ElemSet) {
        ValueID key = index.stripCasts(index.value(row).propertyOperand());
        LoopID versioned = index.loopOf(index.blockOf(operation.id()));
        while (versioned && index.loop(versioned).parent && index.block(index.loop(index.loop(versioned).parent).head).firstParameter().index() >= firstValueOfLoop)
            versioned = index.loop(versioned).parent;
        for (BlockID member : versioned ? index.membersOf(versioned) : std::span<const BlockID>()) {
            for (BodyValue read : index.block(member).values()) {
                if (read.opcode() != Opcode::ElemGet)
                    continue;
                ValueID other = index.stripCasts(read.objectOperand());
                if (!other || index.isParameter(other) || index.opcodeOf(other) != Opcode::ElemGet || index.stripCasts(index.value(other).propertyOperand()) != key)
                    continue;
                if (isSeenAsNumber(read))
                    return true;
            }
        }
    }
    TypeID expected = expectedOfRowThatIsNotBuiltInListReadsInLoops(walker, row);
    return expected && isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(expected)));
}

static bool movesElementOfListedNumbers(Walker& walker, const BodyIndex& index, ValueID moved, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists)
{
    Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, index.value(moved).objectOperand(), firstValueOfLoop);
    return name && std::ranges::any_of(lists, [&](const HoistedArrayGuard& list) { return list.name == name && isNumericStorage(list.storage); });
}

bool storesNumberIntoRow(Walker& walker, const BodyIndex& index, const BodyValue& store, TypeID stored, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists)
{
    if (!readsRowOfNumbersThatTurnFindsInListedList(walker, index, store, firstValueOfLoop, lists, { }, Opcode::ElemSet))
        return false;
    if (stored && walker.types().at(stored).isNumber())
        return true;
    ValueID moved = index.stripCasts(store.valueOperand());
    return moved && !index.isParameter(moved) && index.opcodeOf(moved) == Opcode::ElemGet && (readsRowOfNumbersThatTurnFindsInListedList(walker, index, index.value(moved), firstValueOfLoop, lists) || movesElementOfListedNumbers(walker, index, moved, firstValueOfLoop, lists));
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
            static constexpr OpcodeSet cutsToNumber { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift, Opcode::Mul, Opcode::Sub, Opcode::Div, Opcode::Mod };
            ValueID user = index.onlyUsingValue(operation.id());
            if (user && !index.isParameter(user) && cutsToNumber.contains(index.opcodeOf(user)))
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

static ArrayStorage predictedArrayStorage(Walker&, TypeID list);
static ArrayStorage kindWrittenOrSeenByLinkInListReadsInLoops(const TypeArena&, ArrayStorage written, TypeID seenByLink);

static ValueID findStepValue(const BodyIndex&, const BodyValue& step);

static bool isElementComputedOrPushed(Walker& walker, const BodyIndex& index, LoopID loop, ValueID element)
{
    static constexpr OpcodeSet computes = numberProducingOperators | allBitwiseOperators | allRelations | allEqualities | OpcodeSet { Opcode::Add, Opcode::Increment, Opcode::Decrement, Opcode::NewArray, Opcode::NewObject };
    bool isUsed = false;
    bool isOnlyComputed = std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::all_of(index.block(member).values(), [&](BodyValue operation) {
            if (opcodesThatStateTypes.contains(operation.opcode()))
                return true;
            bool takesElement = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                takesElement |= operand && index.stripCasts(operand) == element;
            });
            isUsed |= takesElement;
            return !takesElement || computes.contains(operation.opcode()) || (operation.opcode() == Opcode::Call && operation.as<Opcode::Call>().calleeKey() == walker.program().keyOf(BuiltinNames::push));
        });
    });
    return isUsed && isOnlyComputed;
}

static bool isElementPushedAlone(Walker& walker, const BodyIndex& index, LoopID loop, ValueID element)
{
    bool isPushed = false;
    bool hasOtherUse = std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            if (opcodesThatStateTypes.contains(operation.opcode()))
                return false;
            bool takesElement = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                takesElement |= operand && index.stripCasts(operand) == element;
            });
            bool isPush = operation.opcode() == Opcode::Call && operation.as<Opcode::Call>().calleeKey() == walker.program().keyOf(BuiltinNames::push);
            isPushed |= takesElement && isPush;
            return takesElement && !isPush;
        });
    });
    return isPushed && !hasOtherUse;
}

static void forEachSteppedArray(Walker& walker, BlockID inputHead, const Invocable<void(const Walker::IndexedIteration&, bool isNumeric)> auto& take)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.opcodes().contains(Opcode::IteratorResume))
        return;
    for (BodyValue operation : index.block(inputHead).values()) {
        ValueID open = operation.opcode() == Opcode::IteratorResume ? operation.operand(0) : ValueID();
        if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen || !walker.has(Mechanism::TypedOperations))
            continue;
        Reference state = walker.referenceBoundTo(open);
        const Walker::IndexedIteration* stepped = state && state.isVirtual() ? &walker.indexedIteration(state) : nullptr;
        if (!stepped || stepped->hasPrecomputedSteps || stepped->isOverFlatEntries || stepped->literal || stepped->table || stepped->contents != Walker::IndexedIteration::Contents::List || stepped->otherForm != Walker::IndexedIteration::OtherForm::None
            || !stepped->list || !stepped->list.isValue() || !stepped->position)
            continue;
        ArrayStorage predicted = predictedArrayStorage(walker, walker.types().withoutNullish(walker.ownTypeOf(stepped->list)));
        TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id());
        ArrayStorage storage = kindWrittenOrSeenByLinkInListReadsInLoops(walker.types(), predicted, seenByLink);
        LoopID loop = index.loopOf(inputHead);
        ValueID element = storage == ArrayStorage::Other || stepped->hasProtocolFallback ? findStepValue(index, operation) : ValueID();
        if (stepped->hasProtocolFallback && element && (index.loop(loop).parent || index.loop(loop).numberOfLoopsWithin > 1) && isElementPushedAlone(walker, index, loop, element))
            continue;
        take(*stepped, isNumericStorage(storage) || (storage == ArrayStorage::Other && element && isElementComputedOrPushed(walker, index, loop, element)));
    }
}

void appendCarriedNumbers(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, std::span<const HoistedArrayGuard> arrays, std::span<const HoistedSpeculation> guesses, Vector<HoistedCounterCheck, 2>& counters)
{
    if (!walker.has(Mechanism::UnboxedNumbers) || std::ranges::none_of(arrays, [](const HoistedArrayGuard& array) { return isNumericStorage(array.storage); }))
        return;
    static constexpr OpcodeSet elementReads { Opcode::ElemGet, Opcode::GetArrayElement, Opcode::GetTypedArrayElement };
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    uint32_t firstValueOfLoop = head.firstParameter().index();
    auto isGuarded = [&](Reference name) {
        return name && std::ranges::any_of(arrays, [&](const HoistedArrayGuard& array) { return array.name == name && isNumericStorage(array.storage); });
    };
    auto isNumber = [&](auto& self, ValueID carried, ValueID value, unsigned depth) -> bool {
        value = value ? index.stripCasts(value) : ValueID();
        if (!value || value == carried)
            return !!value;
        if (value.index() < firstValueOfLoop) {
            Reference built = walker.referenceOf(value);
            return built && types.at(walker.typeOf(built)).isNumber();
        }
        size_t place = value.index() - preview.firstValue;
        if (TypeID known = value.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID(); known && types.at(known).isNumber())
            return true;
        if (index.isParameter(value)) {
            BlockID block = index.blockOf(value);
            if (!depth || index.isLoopHead(block) || !index.isInLoop(block, loop))
                return false;
            uint32_t position = value.index() - index.block(block).firstParameter().index();
            return std::ranges::all_of(walker.incomingEdgesOf(index, block), [&](const BodyIncomingEdge& edge) {
                return position < edge.target.arguments().size() && self(self, carried, edge.target.arguments()[position], depth - 1);
            });
        }
        if (index.opcodeOf(value) == Opcode::ElemGet)
            return isGuarded(nameThatEntersLoopInListReadsInLoops(walker, index, index.value(value).objectOperand(), firstValueOfLoop));
        ValueID step = index.opcodeOf(value) == Opcode::ResultAt ? index.value(value).operand(0) : ValueID();
        const Walker::IndexedIteration* stepped = step && !index.isParameter(step) ? findIndexedArrayIteration(walker, index, index.value(step)) : nullptr;
        return stepped && isGuarded(stepped->list);
    };
    for (uint32_t position = 0; position < head.numberOfParameters() && position < entering.size(); ++position) {
        Reference enters = entering[position];
        if (!enters || !enters.isValue())
            continue;
        const Type& type = types.at(walker.typeOf(enters));
        bool isChecked = std::ranges::find(entering.first(position), enters) != entering.first(position).end()
            || counters.containsIf([&](const HoistedCounterCheck& counter) { return counter.position == position; })
            || std::ranges::any_of(guesses, [&](const HoistedSpeculation& guess) { return guess.value == enters; });
        ValueID origin = underlyingCellOf(walker.code(), enters.id());
        bool isElement = !walker.code().isConstant(origin) && elementReads.contains(walker.code().at(origin).opcode);
        if (isChecked || type.isNumber() || !(type.primitives & Primitive::Number) || !(isElement || type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull)))
            continue;
        ValueID carried = head.parameter(position);
        auto isSumOfCarried = [&](ValueID value) {
            value = value ? index.stripCasts(value) : ValueID();
            bool isSum = value && value.index() >= firstValueOfLoop && !index.isParameter(value) && sums.contains(index.opcodeOf(value));
            return value && (value == carried || (isSum && (index.stripCasts(index.value(value).operand(0)) == carried || index.stripCasts(index.value(value).operand(1)) == carried)));
        };
        bool isUsedAsInteger = std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::any_of(index.block(member).values(), [&](BodyValue user) {
                if (user.opcode() == Opcode::ElemGet || user.opcode() == Opcode::ElemSet)
                    return isSumOfCarried(user.propertyOperand());
                bool takesBits = false;
                if (allBitwiseOperators.contains(user.opcode()))
                    user.forEachOperand([&](ValueID operand, uint32_t) { takesBits |= isSumOfCarried(operand); });
                return takesBits;
            });
        });
        if (isUsedAsInteger)
            continue;
        bool someNumberIsLoopCarried = false;
        bool onlyNumbersComeRound = std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::all_of(index.terminator(member).targets(), [&](BodyTarget target) {
                if (target.block() != inputHead)
                    return true;
                ValueID next = position < target.arguments().size() ? index.stripCasts(target.arguments()[position]) : ValueID();
                someNumberIsLoopCarried |= next && next != carried;
                return next && isNumber(isNumber, carried, next, 3);
            });
        });
        if (!someNumberIsLoopCarried || !onlyNumbersComeRound)
            continue;
        counters.append({ position, TypeTable::number });
        walker.count(StatisticID::InvariantNumberGuardsHoisted);
    }
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
    bool stepsNumericArray = false;
    bool stepsArrayMade = false;
    forEachSteppedArray(walker, inputHead, [&](const Walker::IndexedIteration& stepped, bool isNumeric) {
        stepsArrayMade |= underlyingCellOf(code, stepped.list.id()) == made;
        stepsNumericArray |= isNumeric;
    });
    return (readsAnother || stepsNumericArray) && !stepsArrayMade;
}

bool readsNumericArray(Walker& walker, BlockID inputHead)
{
    for (uint64_t pair : walker.holdersFromInFrontThatLoopReadsUnderIndexes(inputHead)) {
        Reference read = walker.referenceOf(ValueID(static_cast<uint32_t>(pair)));
        if (read && read.isValue() && isNumericStorage(predictedArrayStorage(walker, walker.types().withoutNullish(walker.ownTypeOf(read)))))
            return true;
    }
    bool stepsNumericArray = false;
    forEachSteppedArray(walker, inputHead, [&](const Walker::IndexedIteration&, bool isNumeric) {
        stepsNumericArray |= isNumeric;
    });
    return stepsNumericArray;
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
static const LinkedEffectsGivenArguments* findCallEffectsForArguments(Walker& walker, const BodyValue& operation, FunctionID callee, const TypeInLook& typeInLook)
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
static bool callPreservesArrays(Walker& walker, const BodyValue& operation, const TypeInLook& typeInLook)
{
    if (operation.opcode() != Opcode::Call)
        return false;
    FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), operation.id());
    const LinkedEffectsGivenArguments* given = callee ? findCallEffectsForArguments(walker, operation, callee, typeInLook) : nullptr;
    return given && preservesHoistedArrayGuards(walker, given->onReturn) && preservesHoistedArrayGuards(walker, given->onThrow);
}

bool convertsCheckedCounter(Walker& walker, const BodyValue& operation, BlockID inputHead, std::span<const Reference> entering)
{
    if (operation.opcode() != Opcode::ToPropertyKey)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID key = index.stripCasts(operation.operand(0));
    while (key && index.isParameter(key) && index.valueNarrowedInto(key))
        key = index.stripCasts(index.valueNarrowedInto(key));
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    return key && key.index() >= firstValueOfLoop && namesOfCountersAskedForInt32InFrontOfLoop(walker, inputHead, entering).get(key.index() - firstValueOfLoop);
}

static bool isGuardedInState(Walker& walker, Reference array, bool isStore)
{
    std::optional<HoistedArrayGuard> noted = findNoteInListReadsInLoops(walker, array);
    return noted && (isStore ? noted->isWritable : isShownToReads(*noted));
}

static bool fillsAllocation(Walker& walker, BlockID inputHead, const Walker::AllocationsUsedInLoop& mentioned)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Program& program = walker.program();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    return !mentioned.isEmpty() && std::ranges::any_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            bool isPush = operation.opcode() == Opcode::Call && operation.as<Opcode::Call>().thisValueOperand() && operation.as<Opcode::Call>().calleeKey() == program.keyOf(BuiltinNames::push);
            if (operation.opcode() != Opcode::ElemSet && !isPush)
                return false;
            ValueID filled = valueThatEntersLoopInListReadsInLoops(index, isPush ? operation.as<Opcode::Call>().thisValueOperand() : operation.objectOperand(), firstValueOfLoop);
            Reference name = !filled ? Reference() : filled.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, filled, firstValueOfLoop) : walker.referenceOf(filled);
            return name && name.isValue() && std::ranges::binary_search(mentioned, underlyingCellOf(walker.code(), name.id()).index());
        });
    });
}

static bool rowFindsListsLeftAsTheyAreInListReadsInLoops(Walker& walker, BlockID inputHead, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses, std::span<const Reference> proofs, std::span<const Reference> entering)
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
    auto storesNumberIntoRow = [&](const BodyValue& operation) {
        if (operation.opcode() != Opcode::ElemSet || !walker.canUseIterationGuards(inputHead) || !readsRowOfNumbersThatTurnFindsInListedList(walker, index, operation, firstValueOfLoop, lists, { }, Opcode::ElemSet))
            return false;
        if (walker.types().at(typeInRow(operation.valueOperand())).isNumber())
            return true;
        ValueID stored = index.stripCasts(operation.valueOperand());
        return stored && !index.isParameter(stored) && index.opcodeOf(stored) == Opcode::ElemGet
            && (readsListFoundByReadFromInFront(walker.program(), index, index.value(stored), firstValueOfLoop, foundFromInFront) || readsRowOfNumbersThatTurnFindsInListedList(walker, index, index.value(stored), firstValueOfLoop, lists) || movesElementOfListedNumbers(walker, index, stored, firstValueOfLoop, lists));
    };
    auto accessesFieldArray = [&](const BodyValue& operation) {
        if (!walker.canUseIterationGuards(inputHead))
            return false;
        if (operation.opcode() == Opcode::Call) {
            FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), operation.id());
            return callee && storesCalculatedElements(walker, callee);
        }
        if (operation.opcode() != Opcode::ElemSet || !hasKind(walker.types(), typeInRow(operation.objectOperand()), OperandKind::Array) || !walker.types().at(typeInRow(operation.valueOperand())).isNumber())
            return false;
        Type key = walker.types().at(typeInRow(operation.propertyOperand()));
        return key.isInteger() && !key.integers.isEmpty() && key.integers.isSubsetOf(Range::length());
    };
    bool sitesMayAnswer = !walker.summaryHasUninlinedCall() && someReadThatAsksItsSiteIsReckonedWith(walker, walker.operationsThatRanOtherCodeOfRow(), TypesOfInputs(typeInRow));
    for (ValueID ran : walker.operationsThatRanOtherCodeOfRow()) {
        if (!ran)
            return false;
        BodyValue operation = index.value(ran);
        if (accessesFieldArray(operation)) {
            walker.count(StatisticID::FieldArraysCheckedByIterations);
            continue;
        }
        if (stepsViewByIndexAlone(walker, index, operation) || stepsTextByIndexAlone(walker, index, operation) || callPreservesArrays(walker, operation, typeInRow) || convertsCheckedCounter(walker, operation, inputHead, entering))
            continue;
        if (operation.opcode() == Opcode::ElemGet && walker.standsInCopyOfLoopBuiltForEachMember() && rowFoundUnderKeyInFrontOfLoop(walker, operation))
            continue;
        if (operation.opcode() == Opcode::ElemSet && storesListedNumberIntoView(walker, index, operation, typeInRow(operation.objectOperand()), firstValueOfLoop, lists, guesses, proofs))
            continue;
        if (isServedUnderListedElement(walker, index, operation, TypesOfInputs(typeInRow), firstValueOfLoop, lists, guesses, proofs))
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
            if (!readsList && entersLoop && entersLoop.index() >= firstValueOfLoop) {
                Vector<ChosenArray, 3> chosen = findChosenArrays(walker, index, operation.objectOperand(), firstValueOfLoop);
                readsList = !chosen.isEmpty() && std::ranges::all_of(chosen, [&](const ChosenArray& each) {
                    Reference array = each.name;
                    Reference proofOfArray;
                    provenSpeculations.anyOf(array, [&](size_t place) {
                        proofOfArray = proofs[place];
                        return false;
                    });
                    return listsByName.anyOf(array, serves) || (proofOfArray && listsByName.anyOf(proofOfArray, serves)) || isGuardedInState(walker, array, isStore);
                });
            }
        } else if (const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, operation, true))
            readsList = listsByName.anyOf(stepped->list, isThere);
        else if (const Walker::IndexedIteration* stepped = findDestructuredEntriesIteration(walker, index, operation))
            readsList = listsByName.anyOf(stepped->list, isThere);
        else if (ValueID step = operation.opcode() == Opcode::ResultAt ? operation.as<Opcode::ResultAt>().resultsOperand() : ValueID(); step && !index.isParameter(step)) {
            if (const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, index.value(step), true))
                readsList = listsByName.anyOf(stepped->list, isThere);
        }
        if (readsList || effectRuleOf(operation.opcode()) == EffectRule::Operator || readsListFoundByReadFromInFront(walker.program(), index, operation, firstValueOfLoop, foundFromInFront) || readsRowOfNumbersThatTurnFindsInListedList(walker, index, operation, firstValueOfLoop, lists) || storesNumberIntoRow(operation))
            continue;
        if (!walker.looksAskOnlyWhetherTurnsRunOtherCode(inputHead) || !guessesMayTypeOperationInCleanTurn(walker, operation, TypesOfInputs(typeInRow), sitesMayAnswer))
            return false;
    }
    if (walker.previewOutOfRow() && turnMayAppendToTextInListReadsInLoops(walker, inputHead, *walker.previewOutOfRow(), lists, guesses, proofs))
        return false;
    bool pushStaysWritten = std::ranges::any_of(walker.pushesOntoLiteralsOf(index.loopOf(inputHead)), [&](uint32_t call) {
        Reference literal = untouchedLiteralOfPush(walker, index.value(ValueID(call)), index.loopOf(index.blockOf(ValueID(call))));
        return literal && !listsByName.anyOf(literal, [&](size_t place) { return lists[place].isWritable; });
    });
    if ((pushStaysWritten || !preservesHoistedArrayGuards(walker, walker.effectsOfQuietDraftsOfRow())) && someListedListIsReceiverOfMethodInListReadsInLoops(walker, inputHead, lists))
        return false;
    if (pushStaysWritten && !allocationsSpareLists(walker, lists, walker.allocationsUsedInLoop(inputHead)))
        return false;
    const Effects& effects = walker.effectsOfQuietDraftsOfRow();
    if (preservesHoistedArrayGuards(walker, effects, lists, inputHead))
        return true;
    if (effects.mayRunOtherCode() || !effects.writesObjectsMadeHereAlone)
        return false;
    const Walker::AllocationsUsedInLoop mentioned = walker.allocationsUsedInLoop(inputHead);
    return fillsAllocation(walker, inputHead, mentioned) && std::ranges::all_of(lists, [&](const HoistedArrayGuard& list) {
        std::optional<Walker::IsListedAllocation> other = walker.classifyAgainstAllocations(mentioned, list.name);
        return !list.isWritable || (other && *other == Walker::IsListedAllocation::No);
    });
}

bool rowLeavesLists(Walker& walker, BlockID inputHead, std::span<const HoistedArrayGuard> lists, std::span<const HoistedSpeculation> guesses, std::span<const Reference> entering)
{
    return rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, lists, guesses, { }, entering);
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

static bool isElementReadByName(Walker&, const BodyValue& read);

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

static bool mayParameterBeMissing(const Walker& walker, Reference name)
{
    const Function& function = walker.program().at(walker.currentFunction());
    BodyBlock entry = walker.currentBodyIndex().block(entryBlock);
    for (uint32_t place = 0; place < entry.numberOfParameters(); ++place) {
        Reference bound = walker.referenceBoundTo(entry.parameter(place));
        if (!bound || !bound.isValue() || underlyingCellOf(walker.code(), bound.id()) != underlyingCellOf(walker.code(), name.id()))
            continue;
        const DeclaredType* declared = function.declaredEntryParameter(place);
        TypeID written = declared ? declared->written() : TypeID();
        return written && !isNeverType(written) && !walker.types().at(written).isAnything() && !(declared->has(PositionFlags::OmitsUndefined) && declared->has(PositionFlags::OmitsNull));
    }
    return false;
}

static TypeID writtenTypeOfParameter(const Walker& walker, FunctionID callee, ValueID parameter)
{
    const BodyIndex& body = walker.facts().bodies().at(callee);
    if (!parameter || !body.isParameter(parameter) || body.blockOf(parameter) != entryBlock)
        return { };
    const Function& function = walker.program().at(callee);
    unsigned entryParameter = parameter.index() - body.block(entryBlock).parameter(0).index();
    unsigned first = function.firstParameterIndex();
    if (entryParameter < first || entryParameter - first >= function.numberOfPositionalParameters())
        return { };
    return function.parameters[entryParameter - first].written();
}

static TypeID writtenTypeOfListReadByCallee(Walker& walker, FunctionID reader, ValueID read)
{
    const BodyIndex& body = walker.facts().bodies().at(reader);
    return body.opcodeOf(read) == Opcode::ElemGet ? writtenTypeOfParameter(walker, reader, body.stripCasts(body.value(read).objectOperand())) : TypeID();
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
    if (born.opcode != Opcode::NewArray || born.storage() != AllocationStorage::Heap || born.literalStorage() == ArrayLiteralStorage::Profiled || !born.holes().empty() || born.elementsOperands().empty())
        return { };
    if (walker.isLoopBeingVersioned(index.loopOf(inputHead)))
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

static bool fillsWithNumbersOfCounter(const BodyIndex& index, LoopID loop, ValueID literal)
{
    BlockID head = index.loop(loop).head;
    auto isNumberOfCounter = [&](auto& self, ValueID value, unsigned stepsLeft) -> bool {
        value = index.stripCasts(value);
        if (index.isParameter(value))
            return index.blockOf(value) == head;
        Opcode opcode = index.opcodeOf(value);
        if (numberConstantOpcodes.contains(opcode))
            return true;
        if (!stepsLeft || (!allBitwiseOperators.contains(opcode) && !numberProducingOperators.contains(opcode) && opcode != Opcode::Add))
            return false;
        bool isNumber = true;
        index.value(value).forEachOperand([&](ValueID operand, uint32_t) { isNumber = isNumber && self(self, operand, stepsLeft - 1); });
        return isNumber;
    };
    static constexpr unsigned mostSteps = 3;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::ElemSet) {
                if (index.stripCasts(operation.objectOperand()) != index.stripCasts(literal) || !isNumberOfCounter(isNumberOfCounter, operation.valueOperand(), mostSteps))
                    return false;
                continue;
            }
            bool usesLiteral = false;
            if (!opcodesThatStateTypes.contains(operation.opcode()))
                operation.forEachOperand([&](ValueID operand, uint32_t) { usesLiteral |= operand && index.stripCasts(operand) == index.stripCasts(literal); });
            if (usesLiteral)
                return false;
        }
    }
    return true;
}

static constexpr uint32_t hasNoFilledArraySource = UINT32_MAX;

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
    auto isInvariantFieldRead = [&](ValueID value) {
        while (value && value.index() >= firstValueOfLoop) {
            if (index.isParameter(value) || index.opcodeOf(value) != Opcode::PropGet || index.value(value).isFreeName())
                return false;
            value = index.stripCasts(index.value(value).objectOperand());
        }
        return !!value;
    };
    auto isSameInvariantField = [&](auto& self, ValueID first, ValueID second) -> bool {
        if (first == second || first.index() < firstValueOfLoop || second.index() < firstValueOfLoop)
            return first == second;
        return index.value(first).key() == index.value(second).key() && self(self, index.stripCasts(index.value(first).objectOperand()), index.stripCasts(index.value(second).objectOperand()));
    };
    Reference textWhoseLengthIsBound;
    ValueID lengthBoundHolder;
    if (bound.index() >= firstValueOfLoop && !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::PropGet && !index.value(bound).isFreeName() && index.value(bound).key() == walker.program().keyOf(BuiltinNames::length)) {
        ValueID text = index.stripCasts(index.value(bound).objectOperand());
        Reference built = text.index() < firstValueOfLoop ? walker.referenceOf(text) : Reference();
        if (built && types.at(walker.typeOf(built)).hasOnlyPrimitives(Primitive::AnyString))
            textWhoseLengthIsBound = built;
        else if (built || isInvariantFieldRead(text))
            lengthBoundHolder = text;
    }
    if (!index.isParameter(counter) || index.blockOf(counter) != inputHead || (bound.index() >= firstValueOfLoop && !boundIsWritten && !textWhoseLengthIsBound && !lengthBoundHolder))
        return;
    size_t placeOfCounter = counter.index() - firstValueOfLoop;
    if (placeOfCounter >= entering.size() || !entering[placeOfCounter])
        return;
    const Type& first = types.at(walker.typeOf(entering[placeOfCounter]));
    if (!first.isInteger() || first.integers.minimum < 0)
        return;
    hasExactTripCount &= first.integers == Range::one(0) && !lengthBoundHolder;
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
        if (walker.has(Mechanism::Folding) && walker.state().isPastDeadZone(index.value(read).variable()))
            name.setIsKnownInitialized(true);
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

    auto readsCodeUnitUnderCounter = [&](const auto& call) {
        ValueID callee = index.stripCasts(call.calleeOperand());
        if (!callee || !call.thisValueOperand() || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::PropGet || call.argumentsOperands().size() != 1 || index.stripCasts(call.argumentsOperands()[0]) != counter)
            return false;
        BodyValue read = index.value(callee);
        if (read.isFreeName() || read.key() != walker.program().keyOf(BuiltinNames::charCodeAt))
            return false;
        ValueID string = call.thisValueOperand() ? index.stripCasts(call.thisValueOperand()) : ValueID();
        return string && string == index.stripCasts(read.objectOperand()) && string.index() < firstValueOfLoop;
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
                if (dominatesBackEdges && !listReadUnderCounter && readsCodeUnitUnderCounter(call)) {
                    listReadUnderCounter = index.stripCasts(call.thisValueOperand());
                    hasExactTripCount = false;
                    continue;
                }
                ValueID input = listWhosePushIsRead(index.stripCasts(call.calleeOperand()));
                if (!dominatesBackEdges || !input || !call.thisValueOperand() || index.stripCasts(call.thisValueOperand()) != input || call.argumentsOperands().size() != 1) {
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
            if (operation.opcode() == Opcode::ElemGet && dominatesBackEdges && !listReadUnderCounter && index.stripCasts(operation.propertyOperand()) == counter && isInvariantFieldRead(index.stripCasts(operation.objectOperand())))
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
    Vector<uint32_t, 2> changesOfLists;
    changesOfLists.fill(0, lists.size());
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            ValueID changed;
            if (operation.opcode() == Opcode::Call && operation.as<Opcode::Call>().thisValueOperand() && (operation.as<Opcode::Call>().calleeKey() == walker.program().keyOf(BuiltinNames::push) || operation.as<Opcode::Call>().calleeKey() == walker.program().keyOf(BuiltinNames::pop)))
                changed = index.stripCasts(operation.as<Opcode::Call>().thisValueOperand());
            else if (operation.opcode() == Opcode::ElemSet || (operation.opcode() == Opcode::PropSet && operation.key() == walker.program().keyOf(BuiltinNames::length)))
                changed = index.stripCasts(operation.objectOperand());
            Reference list = changed && changed.index() < firstValueOfLoop ? walker.referenceOf(changed) : Reference();
            for (size_t place = 0; list && place < lists.size(); ++place) {
                if (lists[place] == list && ++changesOfLists[place] == 2 && !listsFilledTwiceInATurn.contains(list))
                    listsFilledTwiceInATurn.append(list);
            }
        }
    }
    if (lengthJoinedOfTheRest > JSString::MaxLength || timesTextsAreJoined > JSString::MaxLength)
        hasExactTripCount = false;

    Origin origin = walker.originOf(index.value(condition));
    Reference room = textWhoseLengthIsBound ? buildLengthOfString(walker, textWhoseLengthIsBound, origin) : walker.referenceOf(bound);
    if (bound.index() >= firstValueOfLoop && !textWhoseLengthIsBound) {
        Draft written(Opcode::Integer, origin);
        if (!lengthBoundHolder)
            written.setInteger(index.value(bound).integer());
        room = lengthBoundHolder ? Reference() : walker.constant(WTF::move(written));
    }
    if (!lengthBoundHolder && !room)
        return;
    if (room && !types.at(walker.typeOf(room)).isNumber()) {
        room = { };
        hasExactTripCount = false;
    }
    bool countsUpToBoundItself = index.opcodeOf(condition) == Opcode::LessEq;
    if (countsUpToBoundItself && !room)
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
        if (room && !types.at(walker.typeOf(room)).isInteger())
            room = { };
        namesAskedInFront.shrink(0);
        textsJoined.shrink(0);
    }
    Reference roomByCount = room;
    auto roomOfLiteralFilledFromItsEnd = [&](Reference list, ValueID literal) -> Reference {
        const Operation& born = walker.code().at(underlyingCellOf(walker.code(), list.id()));
        if (!roomByCount || born.opcode != Opcode::NewArray || born.storage() != AllocationStorage::Heap || !born.holes().empty() || !writtenLengthOfLiteral(index, literal, born))
            return { };
        int64_t written = writtenLengthOfLiteral(index, literal, born);
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
    auto isBornForThisEntry = [&](ValueID literal) {
        return !index.isParameter(literal) && (index.opcodeOf(literal) == Opcode::NewArray || index.opcodeOf(literal) == Opcode::Data) && index.loopOf(index.blockOf(literal)) == index.loop(loop).parent;
    };
    auto noteCopyMakesRoom = [&](size_t place) {
        Reference list = lists[place];
        const Operation& born = walker.code().at(underlyingCellOf(walker.code(), list.id()));
        bool isShort = roomByCount && types.at(walker.typeOf(roomByCount)).integers.maximum <= BASE_CONTIGUOUS_VECTOR_LEN_MAX;
        if (!isShort && (!listReadUnderCounter || listReadUnderCounter == namesOfLists[place] || (lengthBoundHolder && !isSameInvariantField(isSameInvariantField, lengthBoundHolder, listReadUnderCounter)) || countsUpToBoundItself))
            return;
        if (initialCounterRange != Range::one(0))
            return;
        if (born.opcode != Opcode::NewArray || born.storage() != AllocationStorage::Heap || !born.holes().empty() || !born.elementsOperands().empty() || !isBornForThisEntry(namesOfLists[place]) || listsFilledTwiceInATurn.contains(list))
            return;
        size_t placeOfPush = pushesWithUnusedResult.findIf([&](const auto& push) { return push.first == list; });
        if (listsPushedOnto.contains(list) && placeOfPush == notFound)
            return;
        uint64_t current = static_cast<uint64_t>(walker.currentFunction().index()) << 32;
        if (placeOfPush != notFound) {
            noteKnown(walker, KnownNote::PushAppendsToLiteral, list, { }, inputHead.index(), LocationMasks(), pushesWithUnusedResult[placeOfPush].second.index() | current);
            walker.noteLoopWhosePushFillsLiteral(inputHead);
        }
        noteKnown(walker, KnownNote::LoopAppendsToLiteral, list, { }, inputHead.index(), LocationMasks(), (placeOfCounter + 1) | current);
        noteKnown(walker, KnownNote::CapacityForRemainingIterations, list, { }, inputHead.index(), LocationMasks(), (roomByCount ? roomByCount.bits() : 0) | static_cast<uint64_t>(isShort ? hasNoFilledArraySource : listReadUnderCounter.index()) << 32);
    };
    for (size_t place = 0; place < lists.size(); ++place) {
        Reference list = lists[place];
        if (!hasExactTripCount) {
            room = roomOfLiteralFilledFromItsEnd(list, namesOfLists[place]);
            if (walker.isUnreachable())
                return;
            if (!room) {
                noteCopyMakesRoom(place);
                continue;
            }
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
        const Operation& filled = walker.code().at(underlyingCellOf(walker.code(), list.id()));
        bool startsAtLiteralLength = filled.opcode == Opcode::NewArray && filled.holes().empty() && initialCounterRange == Range::one(static_cast<int64_t>(writtenLengthOfLiteral(index, literal, filled)));
        size_t placeOfPush = pushesWithUnusedResult.findIf([&](const auto& push) { return push.first == lists[place] && (hasCapacityForSpecializedVersion || index.isUnused(push.second)); });
        bool appendsToLiteral = !walker.isUnreachable() && isBornForThisEntry(literal) && startsAtLiteralLength && (!listsPushedOnto.contains(lists[place]) || placeOfPush != notFound) && !listsFilledTwiceInATurn.contains(lists[place]);
        if (appendsToLiteral && placeOfPush != notFound) {
            noteKnown(walker, KnownNote::PushAppendsToLiteral, list, { }, inputHead.index(), LocationMasks(), pushesWithUnusedResult[placeOfPush].second.index() | static_cast<uint64_t>(walker.currentFunction().index()) << 32);
            walker.noteLoopWhosePushFillsLiteral(inputHead);
        }
        bool takesCapacityForRemainingIterations = ((hasMergedNumberStore && !listsPushedOnto.contains(lists[place])) || hasPushesAndArithmeticUsers) && !hasExactTripCount && listReadUnderCounter && listReadUnderCounter != namesOfLists[place] && types.at(walker.typeOf(roomByCount)).isInteger() && types.at(walker.typeOf(roomByCount)).integers.maximum > BASE_CONTIGUOUS_VECTOR_LEN_MAX;
        bool growsInIterations = !hasExactTripCount && placeOfPush == notFound && !takesCapacityForRemainingIterations && types.at(walker.typeOf(roomByCount)).integers.maximum > BASE_CONTIGUOUS_VECTOR_LEN_MAX && !fillsWithNumbersOfCounter(index, loop, literal);
        if (appendsToLiteral && !growsInIterations)
            noteKnown(walker, KnownNote::LoopAppendsToLiteral, list, { }, inputHead.index(), LocationMasks(), (placeOfCounter + 1) | static_cast<uint64_t>(walker.currentFunction().index()) << 32);
        if (appendsToLiteral && hasExactTripCount && everyStoreStandsBehindTestOfHead && types.at(walker.typeOf(roomByCount)).isInteger())
            noteKnown(walker, KnownNote::FinalLiteralLength, list, { }, inputHead.index(), LocationMasks(), roomByCount.bits());
        if (appendsToLiteral && takesCapacityForRemainingIterations)
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
    bool readsInNest = access == Opcode::ElemGet && body.hasLoops();
    OpcodeSet keyOperators = allBitwiseOperators | OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul };
    if (readsInNest)
        keyOperators.add(Opcode::Mod);
    auto isComputedKey = [&](auto& self, ValueID key, unsigned operatorsLeft) -> bool {
        key = body.stripCasts(key);
        if (!key || body.isParameter(key))
            return key && (argumentBehind(key) || (readsInNest && body.isLoopHead(body.blockOf(key))));
        if (body.opcodeOf(key) == Opcode::Integer)
            return true;
        bool isComputed = operatorsLeft && keyOperators.contains(body.opcodeOf(key));
        if (isComputed) {
            body.value(key).forEachOperand([&](ValueID operand, uint32_t) {
                isComputed &= operand && self(self, operand, operatorsLeft - 1);
            });
        }
        return isComputed;
    };
    for (uint32_t block = 0; block < body.numberOfBlocks(); ++block) {
        for (BodyValue operation : body.block(BlockID(block)).values()) {
            if (operation.opcode() == access) {
                ValueID key = body.stripCasts(findUpdatedKey(walker, body, operation.propertyOperand()));
                bool keyIsCounter = key && body.isParameter(key) && body.isLoopHead(body.blockOf(key));
                bool keyIsWritten = keyIsCounter || (key && !body.isParameter(key) && (access == Opcode::ElemSet || readsInNest ? isComputedKey(isComputedKey, key, readsInNest ? 6 : 3) : body.opcodeOf(key) == Opcode::Integer));
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
        ValueID variable = argument.index() < firstValueOfLoop ? ValueID() : readOfVariableInLoopInListReadsInLoops(walker.program(), index, argument, firstValueOfLoop);
        Reference inFront = argument.index() < firstValueOfLoop ? walker.referenceOf(argument) : Reference();
        TypeID known = inFront ? walker.typeOf(inFront) : variable ? widestTypeOfOperation(walker, index.value(variable)) : TypeID();
        readsIt |= known && walker.types().isUninformative(known);
    });
    return readsIt;
}

static bool callReturnsNumber(Walker& walker, FunctionID caller, const BodyValue& call, TypeID number)
{
    const LinkTables& linked = walker.facts().linked();
    FunctionID callee = linked.calleeAt(caller, call.id());
    TypeID result = callee ? linked.function(callee).result : TypeID();
    return result && !isNeverType(result) && walker.types().isSubset(result, number);
}

static bool storesInt32InCopyOfCallee(Walker& walker, FunctionID writer, ValueID value)
{
    static constexpr OpcodeSet int32Operators { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift };
    const BodyIndex& body = walker.facts().bodies().at(writer);
    value = body.stripCasts(value);
    if (!value || body.isParameter(value))
        return false;
    Opcode opcode = body.opcodeOf(value);
    if (opcode == Opcode::ElemGet) {
        TypeID seen = walker.facts().linked().resultOfReadByKey(writer, value);
        return seen && walker.types().isSubset(seen, TypeTable::int32);
    }
    return int32Operators.contains(opcode) || (opcode == Opcode::Call && callReturnsNumber(walker, writer, body.value(value), TypeTable::int32));
}

static bool storesNumberInCopyOfCalleeInListReadsInLoops(Walker& walker, FunctionID writer, ValueID value, unsigned depth, PropertyKeyID fieldOfNumbers = { })
{
    const BodyIndex& body = walker.facts().bodies().at(writer);
    value = body.stripCasts(value);
    if (!value || body.isParameter(value)) {
        TypeID written = writtenTypeOfParameter(walker, writer, value);
        return written && walker.types().at(written).isNumber();
    }
    Opcode opcode = body.opcodeOf(value);
    if (numberConstantOpcodes.contains(opcode) || numberProducingOperators.contains(opcode))
        return true;
    if (opcode == Opcode::ElemGet) {
        ValueID list = fieldOfNumbers ? body.stripCasts(body.value(value).objectOperand()) : ValueID();
        if (list && !body.isParameter(list) && body.opcodeOf(list) == Opcode::PropGet && body.value(list).key() == fieldOfNumbers)
            return true;
        TypeID seenByLink = walker.facts().linked().resultOfReadByKey(writer, value);
        if (TypeID written = !seenByLink || walker.types().at(seenByLink).isAnything() ? writtenTypeOfListReadByCallee(walker, writer, value) : TypeID())
            return isNumericStorage(predictedArrayStorage(walker, walker.types().withoutNullish(written)));
        return seenByLink && (walker.types().at(seenByLink).primitives & Primitive::Number) && walker.types().at(seenByLink).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    }
    BodyValue operation = body.value(value);
    if (ValueID taken = elementTakenByPattern(body, operation))
        return storesNumberInCopyOfCalleeInListReadsInLoops(walker, writer, taken, depth, fieldOfNumbers);
    if (opcode == Opcode::Call)
        return callReturnsNumber(walker, writer, operation, TypeTable::number);
    return opcode == Opcode::Add && depth && storesNumberInCopyOfCalleeInListReadsInLoops(walker, writer, operation.operand(0), depth - 1, fieldOfNumbers) && storesNumberInCopyOfCalleeInListReadsInLoops(walker, writer, operation.operand(1), depth - 1, fieldOfNumbers);
}

static bool countsToConstant(const BodyIndex& body, BlockID head)
{
    BodyTerminator end = body.terminator(head);
    ValueID condition = end.isConditionalBranch() ? body.stripCasts(end.operand(0)) : ValueID();
    if (!condition || body.isParameter(condition) || (body.opcodeOf(condition) != Opcode::Less && body.opcodeOf(condition) != Opcode::LessEq))
        return false;
    ValueID bound = body.stripCasts(body.value(condition).operand(1));
    return bound && !body.isParameter(bound) && body.opcodeOf(bound) == Opcode::Integer;
}

static bool readsCarriedKey(Walker& walker, const BodyIndex& body, ValueID read)
{
    ValueID key = body.stripCasts(body.value(read).propertyOperand());
    if (!key || !body.isParameter(key))
        return key && body.opcodeOf(key) == Opcode::ElemGet;
    BlockID head = body.blockOf(key);
    if (!body.isLoopHead(head) || !countsToConstant(body, head))
        return false;
    uint32_t position = key.index() - body.block(head).firstParameter().index();
    return std::ranges::any_of(walker.incomingEdgesOf(body, head), [&](const BodyIncomingEdge& edge) {
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID carried = position < arguments.size() ? body.stripCasts(arguments[position]) : ValueID();
        return carried && !body.isParameter(carried) && body.opcodeOf(carried) == Opcode::ElemGet && body.stripCasts(body.value(carried).objectOperand()) == body.stripCasts(body.value(read).objectOperand());
    });
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
        if (Reference entering = known || !usesIterationGuards ? Reference() : nameThatEntersLoopInListReadsInLoops(walker, walker.currentBodyIndex(), operation.as<Opcode::Call>().calleeOperand(), firstValueOfLoop); entering && entering.isValue())
            known = singleFunctionOf(types, types.at(walker.ownTypeOf(entering)));
        if (known && usesIterationGuards && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, known))
            return false;
        if (FunctionID expected = known || !usesIterationGuards ? FunctionID() : findExpectedMethod(walker, operation); expected && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, expected)) {
            walker.count(StatisticID::ExpectedMethodsInLoops);
            return false;
        }
        if (FunctionID callee = known) {
            if (const LinkedEffectsGivenArguments* given = findCallEffectsForArguments(walker, operation, callee, typeInLook))
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

TypeID provenElementPassedToBuiltin(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.callThatFrameAtWorkCopies().second || !isLibraryCode(walker.program(), walker.program().at(walker.currentFunction())))
        return { };
    BodyBlock entry = index.block(entryBlock);
    for (uint32_t parameter = 0; parameter < entry.numberOfParameters(); ++parameter) {
        TypeID handed = walker.typeOf(walker.referenceOf(entry.parameter(parameter)));
        if (handed && walker.types().at(handed).isObject() && singleBuiltinOf(walker.types(), walker.types().at(handed)))
            return elementsThatLinkProvesOfReceiverInListReadsInLoops(walker, index, walker.referenceOf(read.arrayOperand()));
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

static ValueID findStepValue(const BodyIndex& index, const BodyValue& step)
{
    for (BodyValue operation : index.block(index.blockOf(step.id())).values()) {
        if (operation.opcode() == Opcode::ResultAt && operation.as<Opcode::ResultAt>().resultsOperand() == step.id() && operation.as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Value))
            return operation.id();
    }
    return { };
}

bool isMathCall(const Walker& walker, const BodyIndex& index, const BodyValue& operation)
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

static bool isTakenByMathOrAddedToElement(Walker&, const BodyIndex&, const BodyValue& read);
static bool callsElementReader(Walker&, const BodyValue& call);

static bool isElementCalled(const BodyIndex& index, const BodyValue& read)
{
    ValueID name = read.id();
    ValueID user = read.opcode() == Opcode::ElemGet ? index.onlyUsingValue(name) : ValueID();
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    return user && index.opcodeOf(user) == Opcode::Call && index.value(user).as<Opcode::Call>().calleeOperand() == name;
}

static bool isFilledWithDoubles(const Walker& walker, const BodyIndex& index, ValueID array)
{
    ValueID made = index.stripCasts(array);
    if (!made || index.isParameter(made) || index.opcodeOf(made) != Opcode::Call)
        return false;
    auto call = index.value(made).as<Opcode::Call>();
    ValueID callee = call.calleeOperand();
    if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin || call.argumentsOperands().size() != 2)
        return false;
    ValueID element = index.stripCasts(call.argumentsOperands()[1]);
    if (!element || index.isParameter(element) || index.opcodeOf(element) != Opcode::Double)
        return false;
    double number = index.value(element).as<Opcode::Double>().number();
    if (!canStoreWithoutConversion(ArrayStorage::Double, number) || canStoreWithoutConversion(ArrayStorage::Int32, number))
        return false;
    BuiltinID filledArray = walker.program().builtins().makerOfFilledArrays();
    return filledArray && index.value(callee).as<Opcode::Builtin>().builtin() == filledArray;
}

static Reference steppedArrayBeforeLoop(Walker& walker, const BodyIndex& index, const BodyValue& step, Reference stepped, uint32_t firstValueOfLoop)
{
    ValueID open = step.operand(0);
    if (open.index() < firstValueOfLoop)
        return stepped;
    ValueID iterable = index.stripCasts(index.value(open).as<Opcode::IteratorOpen>().iterableOperand());
    if (iterable && iterable.index() >= firstValueOfLoop && !index.isParameter(iterable) && index.opcodeOf(iterable) == Opcode::Call) {
        ValueID receiver = index.value(iterable).thisValueOperand();
        iterable = receiver ? index.stripCasts(receiver) : ValueID();
    }
    Reference array = iterable ? nameThatEntersLoopInListReadsInLoops(walker, index, iterable, firstValueOfLoop) : Reference();
    bool isStepped = array && array.isValue() && underlyingCellOf(walker.code(), array.id()) == underlyingCellOf(walker.code(), stepped.id());
    if (isStepped)
        return array;
    return walker.dominatesLoopVersions(stepped) ? stepped : Reference();
}

static bool isReadByKeyInLoop(const BodyIndex& index, LoopID loop, const BodyValue& store)
{
    auto isSame = [&](auto& isSame, ValueID first, ValueID second) -> bool {
        first = index.stripCasts(first);
        second = index.stripCasts(second);
        if (first == second)
            return true;
        if (index.isParameter(first) || index.isParameter(second) || index.opcodeOf(first) != index.opcodeOf(second))
            return false;
        BodyValue one = index.value(first);
        BodyValue other = index.value(second);
        switch (one.opcode()) {
        case Opcode::ElemGet:
            return isSame(isSame, one.objectOperand(), other.objectOperand());
        case Opcode::PropGet:
            return one.key() == other.key();
        case Opcode::GetModuleVariable:
            return one.variable() == other.variable();
        case Opcode::GetEnvironmentField:
            return one.layout() == other.layout() && one.field() == other.field();
        default:
            return false;
        }
    };
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue read) {
            return read.opcode() == Opcode::ElemGet && isSame(isSame, read.objectOperand(), store.objectOperand());
        });
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

    std::optional<BitVector> namesOfInnerCounters;
    auto isInnerLoopCounter = [&](ValueID key) {
        if (key.index() < firstValueOfLoop || index.loop(loop).numberOfLoopsWithin < 2 || !walker.canUseIterationGuards(inputHead))
            return false;
        if (!namesOfInnerCounters)
            namesOfInnerCounters = namesOfInnerLoopCounters(walker, inputHead, preview);
        return namesOfInnerCounters->get(key.index() - firstValueOfLoop);
    };
    auto hasGuessedAddend = [&](ValueID key) {
        static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
        ValueID made = index.stripCasts(key);
        if (!made || made.index() < firstValueOfLoop || index.isParameter(made) || !sums.contains(index.opcodeOf(made)))
            return false;
        BodyValue sum = index.value(made);
        for (uint32_t place : { 0u, 1u }) {
            ValueID counter = index.stripCasts(sum.operand(place));
            ValueID addend = index.stripCasts(sum.operand(1 - place));
            TypeID typeOfCounter = counter && counter.index() >= firstValueOfLoop ? typeInLook(counter) : TypeID();
            bool isCheckedCounter = typeOfCounter && (types.at(typeOfCounter).isInteger() || (namesOfCountersAskedInFront && namesOfCountersAskedInFront->get(counter.index() - firstValueOfLoop)));
            Reference added = addend && addend.index() < firstValueOfLoop ? walker.referenceOf(addend) : Reference();
            if (!added && addend && addend.index() < firstValueOfLoop)
                added = walker.referenceOf(sum.operand(1 - place));
            if (isCheckedCounter && addendMayBeGuessedInt32(walker, added))
                return true;
        }
        return false;
    };
    auto isRoundedKey = [&](ValueID key) {
        TypeID seen = typeInLook(key);
        return seen && types.at(seen).isNumberWithoutFraction() && !types.at(seen).isInteger() && walker.canUseIterationGuards(inputHead) && isRoundingMathCall(walker, index, index.stripCasts(key));
    };
    bool stepsIterators = index.opcodes().contains(Opcode::IteratorResume);
    auto isSteppedPosition = [&](ValueID key) {
        return stepsIterators && key && key.index() >= firstValueOfLoop && findIterableOfPosition(walker, index, key);
    };
    auto typeOfKeyInLook = [&](ValueID key) -> TypeID {
        TypeID seen = typeInLook(key);
        if (isRoundedKey(key))
            return TypeTable::int32;
        if (!(seen && types.at(seen).isInteger()) && isSteppedPosition(key))
            return types.integer(steppedPositions);
        if (!seen || types.at(seen).isInteger() || !(types.at(seen).primitives & Primitive::Integer) || key.index() < firstValueOfLoop || walker.keysAreReadOffLookInRow())
            return seen;
        if (!namesOfCountersAskedInFront)
            namesOfCountersAskedInFront = namesOfCountersAskedForInt32InFrontOfLoop(walker, inputHead, entering);
        if (!namesOfCountersAskedInFront->get(key.index() - firstValueOfLoop) && !isInnerLoopCounter(key) && !hasGuessedAddend(key))
            return seen;
        walker.noteKeyTakenForCounterAskedInFront();
        return TypeTable::int32;
    };
    auto checksCounterBeforeLoop = [&](ValueID key) { return typeOfKeyInLook(key) != typeInLook(key) && !isRoundedKey(key) && !isSteppedPosition(key); };
    std::optional<Vector<ValueID, 4>> elementCarriedIndexes;
    auto isCarriedIndex = [&](ValueID read) {
        if (!elementCarriedIndexes)
            elementCarriedIndexes = findElementCarriedIndexes(walker, inputHead);
        return std::ranges::any_of(*elementCarriedIndexes, [&](ValueID carried) {
            return carried == read || (index.opcodeOf(carried) == Opcode::ElemGet && index.opcodeOf(read) == Opcode::ElemGet && index.stripCasts(index.value(carried).objectOperand()) == index.stripCasts(index.value(read).objectOperand()));
        });
    };
    BitVector innerLoopEntryNumbers;
    std::optional<BitVector> parametersCheckedForBits;
    auto entersLoopAsNumber = [&](ValueID value) {
        ValueID made = index.stripCasts(value);
        if (made && made.index() >= firstValueOfLoop && innerLoopEntryNumbers.get(made.index() - firstValueOfLoop))
            return true;
        std::optional<uint32_t> place = made ? index.block(inputHead).indexOfParameter(made) : std::nullopt;
        TypeID entered = place && *place < entering.size() && entering[*place] ? walker.ownTypeOf(entering[*place]) : TypeID();
        if (!entered || types.at(entered).isNumber())
            return !!entered;
        if (!parametersCheckedForBits)
            parametersCheckedForBits = findParametersCheckedForBits(walker, inputHead, preview, entering);
        if (parametersCheckedForBits->get(*place))
            return true;
        return !walker.sumsAreNotAskedInRow() && *place < preview.typesOfParameters.size() && (isNumberCarriedBesideTextOrBigInt(walker, entering[*place], preview.typesOfParameters[*place]) || findCarriedNumberType(walker, inputHead, preview, entering[*place], *place));
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
                innerLoopEntryNumbers.set(place);
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
    auto isFieldAddedTo = [&](const BodyValue& read) {
        if ((read.opcode() != Opcode::PrivateGet && (read.opcode() != Opcode::PropGet || read.isFreeName())) || !walker.canUseIterationGuards(inputHead))
            return false;
        ValueID sum = index.onlyUsingValue(read.id());
        if (!sum || index.isParameter(sum) || index.opcodeOf(sum) != Opcode::Add)
            return false;
        PropertyKeyID key = keyOfAccessToSlot(walker.program(), read);
        ValueID stored = index.onlyUsingValue(sum);
        bool isStored = stored && !index.isParameter(stored) && (index.opcodeOf(stored) == Opcode::PrivateSet || index.opcodeOf(stored) == Opcode::PropSet) && keyOfAccessToSlot(walker.program(), index.value(stored)) == key;
        TypeID holder = key && isStored ? typeInLook(read.objectOperand()) : TypeID();
        bool isPrivate = read.opcode() == Opcode::PrivateGet && read.layout();
        if (isPrivate && key && isStored && (!holder || types.isUninformative(holder)))
            holder = types.forLayout(read.layout());
        if (!holder || types.isUninformative(holder))
            return false;
        std::optional<FilledSlot> filled = findPermanentlyFilledSlot(walker, types.withoutNullish(holder), key, true);
        TypeID held = filled ? walker.plan().slot(filled->place.layout, filled->place.field).type : TypeID();
        if (filled && isPrivate && !held)
            held = typeInLook(read.id());
        return held && types.at(held).primitives & Primitive::Number && types.at(held).hasOnlyPrimitives(Primitive::Number | Primitive::AnyString | Primitive::UndefinedOrNull);
    };
    auto isKeyFoundByName = [&](const BodyValue& access) {
        ValueID key = index.stripCasts(access.propertyOperand());
        if (!key || key.index() < firstValueOfLoop || index.isParameter(key) || index.opcodeOf(key) != Opcode::PropGet || !walker.canUseIterationGuards(inputHead))
            return false;
        if (index.value(key).isFreeName() && walker.readsNameSteppedInLoop(key))
            return true;
        TypeID holder = typeInLook(index.value(key).objectOperand());
        return (!holder || types.isUninformative(holder)) && siteMayAnswerReadWithoutOtherCode(walker, index.value(key)) && isUsedForKey(walker, key, KeyHolders::AnyValues);
    };
    auto isElementOfViewOfIntegers = [&](ValueID key) {
        if (!key || key.index() < firstValueOfLoop || index.isParameter(key) || index.opcodeOf(key) != Opcode::ElemGet || !walker.canUseIterationGuards(inputHead))
            return false;
        TypeID position = typeInLook(index.value(key).propertyOperand());
        Reference name = position && types.at(position).isInteger() ? nameThatEntersLoopInListReadsInLoops(walker, index, index.value(key).objectOperand(), firstValueOfLoop) : Reference();
        TypeID view = name && name.isValue() ? walker.ownTypeOf(name) : TypeID();
        for (const HoistedSpeculation& guess : guesses) {
            if (view && guess.value == name)
                view = types.intersectionOf(view, guess.tested);
        }
        return view && isViewOfNumbersReadInLine(walker, types.withoutNullish(view)) && !isFloat(typedArrayTypeOf({ walker.program(), types }, types.withoutNullish(view)));
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
    Vector<Reference, 2> arraysOfEitherBoxedStorage;
    auto tryReuseForElementAccess = [&](Reference name) {
        size_t place = foundArrayLookup.find(found.lists.span(), name);
        if (place == notFound)
            return false;
        if (!holdsBoxedValuesOfEitherKind(found.lists[place]) || arraysOfEitherBoxedStorage.contains(name))
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
    Vector<Reference, 2> arraysReadByCopies;
    Vector<Reference, 2> listsComparedByCallees;
    Vector<Reference, 2> arraysNeedingExactStorage;
    Vector<Reference, 2> listsThatTakeMoreThanInt32s;
    bool loopStoresIntoLists = false;
    bool laterStoreMayExit = false;
    Vector<Reference, 2> untouchedLiteralsPushedOnto;
    bool someFieldMayHoldLiteral = false;
    Vector<Reference, 2> listsThatCopiesOfCalleesStoreIntoUncounted;
    Vector<Reference, 2> listsTakenUnderCountersAskedInFront;
    Vector<Reference, 2> listsReachedUnderCountersAskedInFront;
    Vector<Reference, 2> objectArraysUnderCheckedCounters;
    Vector<Reference, 4> chosenArrays;
    static constexpr OpcodeSet stepsOfNumbers { Opcode::ToNumeric, Opcode::Increment, Opcode::Decrement };
    auto holdsOnlyIndices = [&](const BodyValue& read, bool isNumeric) {
        if (index.loop(loop).numberOfLoopsWithin != 1 || !walker.canUseIterationGuards(inputHead) || !isUsedForKey(walker, read.id(), KeyHolders::AnyValues))
            return false;
        auto indexesArray = [&](const BodyValue& access) {
            ValueID row = index.stripCasts(access.objectOperand());
            if (row && !index.isParameter(row) && index.opcodeOf(row) == Opcode::ElemGet)
                return true;
            ValueID entersLoop = valueThatEntersLoopInListReadsInLoops(index, access.objectOperand(), firstValueOfLoop);
            Reference holder = !entersLoop ? Reference() : entersLoop.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, entersLoop, firstValueOfLoop) : walker.referenceOf(entersLoop);
            TypeID type = holder && holder.isValue() ? walker.ownTypeOf(holder) : TypeID();
            for (const HoistedSpeculation& guess : guesses) {
                if (type && guess.value == holder)
                    type = types.intersectionOf(type, guess.tested);
            }
            return type && (hasKind(types, types.withoutNullish(type), OperandKind::Array) || isViewOfNumbersReadInLine(walker, types.withoutNullish(type)));
        };
        return isNumeric || std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::all_of(index.block(member).values(), [&](BodyValue access) {
                bool takesElement = (access.opcode() == Opcode::ElemGet || access.opcode() == Opcode::ElemSet) && index.stripCasts(findUpdatedKey(walker, index, access.propertyOperand())) == read.id();
                return !takesElement || indexesArray(access);
            });
        });
    };
    auto computesWithOtherElements = [&](ValueID moved, Reference list) {
        return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::any_of(index.block(member).values(), [&](BodyValue other) {
                if (other.opcode() != Opcode::ElemGet || other.id() == moved)
                    return false;
                ValueID input = valueThatEntersLoopInListReadsInLoops(index, other.objectOperand(), firstValueOfLoop);
                Reference name = !input ? Reference() : input.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop) : walker.referenceOf(input);
                return name == list && (valueIsOnlyReckonedWithAsNumber(walker, other.id(), TypesOfInputs(typeInLook)) || isTakenByMathOrAddedToElement(walker, index, other));
            });
        });
    };
    auto holdsArrayOrNothing = [&](Reference name) {
        TypeID held = types.withoutNullish(types.withoutPrimitives(walker.ownTypeOf(name), Primitive::Absent));
        return !isNeverType(held) && hasKind(types, held, OperandKind::Array) && !isArraySubclassInstance(walker, held);
    };
    auto findArrayReadByCopy = [&](const BodyValue& call, FunctionID callee) -> Reference {
        const BodyIndex* body = walker.facts().bodies().find(callee);
        ValueID element = body ? walker.findReturnedElement(*body) : ValueID();
        if (!element)
            return { };
        ValueID field = body->stripCasts(body->opcodeOf(element) == Opcode::ElemGet ? body->value(element).objectOperand() : body->value(element).as<Opcode::Call>().thisValueOperand());
        if (!field || body->isParameter(field) || body->opcodeOf(field) != Opcode::PropGet || body->value(field).isFreeName())
            return { };
        ValueID holder = valuePassedIntoCopy(walker, *body, call, callee, body->value(field).objectOperand());
        Reference name = holder ? valueOfFieldInPreheader(walker, nameThatEntersLoopInListReadsInLoops(walker, index, holder, firstValueOfLoop), body->value(field).key()) : Reference();
        return name && name.isValue() && holdsArrayOrNothing(name) ? name : Reference();
    };
    auto computesWithRemovedElements = [&](Reference list) {
        return list && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::any_of(index.block(member).values(), [&](BodyValue call) {
                FunctionID callee = call.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), call.id()) : FunctionID();
                ValueID user = callee ? index.onlyUsingValue(call.id()) : ValueID();
                if (!user || !allNumericOperators.contains(index.opcodeOf(user)) || findArrayReadByCopy(call, callee) != list)
                    return false;
                if (index.opcodeOf(user) != Opcode::Add)
                    return true;
                ValueID other = index.value(user).operand(index.stripCasts(index.value(user).operand(0)) == call.id() ? 1 : 0);
                return entersLoopAsNumber(other) || isNumberBehindListsFound(other);
            });
        });
    };
    bool movedElementIsNumber = false;
    bool versionTakesInt32s = !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected();
    auto setMovedElementStorage = [&](HoistedArrayGuard& list) {
        list.leavesBirthAsItIs = true;
        list.hasAlternateStorage = !movedElementIsNumber || versionTakesInt32s;
        list.alternateStorage = movedElementIsNumber ? ArrayStorage::Double : ArrayStorage::Contiguous;
        list.keepsDoublesWithoutAlternate = movedElementIsNumber;
    };
    auto mayStayInt32Storage = [](const HoistedArrayGuard& list) {
        if (list.isWritable)
            return list.storage == ArrayStorage::Int32 && list.hasAlternateStorage && list.alternateStorage == ArrayStorage::Double;
        return list.storage == ArrayStorage::Double && list.admitsInt32Storage;
    };
    auto isReadForBitsAlone = [&](Reference list) {
        bool isRead = false;
        bool takesBitsAlone = std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::all_of(index.block(member).values(), [&](BodyValue read) {
                if (read.opcode() != Opcode::ElemGet)
                    return true;
                ValueID input = valueThatEntersLoopInListReadsInLoops(index, read.objectOperand(), firstValueOfLoop);
                Reference name = !input ? Reference() : input.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop) : walker.referenceOf(input);
                if (name != list)
                    return true;
                isRead = true;
                return readsTruncatedElement(walker, read);
            });
        });
        return isRead && takesBitsAlone;
    };
    auto storesSumOfMixedBits = [&](const BodyValue& store) {
        static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
        static constexpr OpcodeSet mixers { Opcode::BitXor, Opcode::BitOr, Opcode::BitNot, Opcode::LShift };
        static constexpr size_t mostTerms = 8;
        Vector<ValueID, mostTerms> terms { store.valueOperand() };
        unsigned numberOfMixedWords = 0;
        for (size_t place = 0; place < terms.size(); ++place) {
            ValueID term = index.stripCasts(terms[place]);
            if (!term || term.index() < firstValueOfLoop || index.isParameter(term))
                continue;
            if (sums.contains(index.opcodeOf(term)) && terms.size() + 2 <= mostTerms) {
                index.value(term).forEachOperand([&](ValueID operand, uint32_t) { terms.append(operand); });
                continue;
            }
            TypeID type = place && mixers.contains(index.opcodeOf(term)) ? typeInLook(term) : TypeID();
            numberOfMixedWords += type && !isNeverType(type) && types.isSubset(TypeTable::int32, type);
        }
        return numberOfMixedWords >= 2;
    };
    auto storesStepOfElement = [&](const BodyValue& store, Reference list) {
        ValueID made = index.stripCasts(store.valueOperand());
        static constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
        if (!made || made.index() < firstValueOfLoop || index.isParameter(made) || !steps.contains(index.opcodeOf(made)))
            return false;
        bool takesElement = false;
        bool takesIntegersOnly = true;
        index.value(made).forEachOperand([&](ValueID operand, uint32_t) {
            ValueID term = index.stripCasts(operand);
            if (term && term.index() >= firstValueOfLoop && !index.isParameter(term) && index.opcodeOf(term) == Opcode::ToNumeric)
                term = index.stripCasts(index.value(term).operand(0));
            bool isElement = term && term.index() >= firstValueOfLoop && !index.isParameter(term) && index.opcodeOf(term) == Opcode::ElemGet;
            ValueID input = isElement ? valueThatEntersLoopInListReadsInLoops(index, index.value(term).objectOperand(), firstValueOfLoop) : ValueID();
            Reference holder = !input ? Reference() : input.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop) : walker.referenceOf(input);
            if (holder == list && isElement) {
                takesElement = true;
                return;
            }
            TypeID added = typeInLook(operand);
            added = added ? types.withoutNullish(added) : TypeID();
            takesIntegersOnly &= added && !isNeverType(added) && types.at(added).isNumberWithoutFraction();
        });
        return takesElement && takesIntegersOnly;
    };
    auto nameOfListOf = [&](const BodyValue& access) -> Reference {
        ValueID input = valueThatEntersLoopInListReadsInLoops(index, access.objectOperand(), firstValueOfLoop);
        return !input ? Reference() : input.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop) : walker.referenceOf(input);
    };
    auto keepInt32sForMovedNumber = [&](const BodyValue& store) {
        ValueID made = store.opcode() == Opcode::ElemSet && index.loop(loop).numberOfLoopsWithin == 1 && walker.canUseIterationGuards(inputHead) ? index.stripCasts(store.valueOperand()) : ValueID();
        if (!versionTakesInt32s || !made || made.index() < firstValueOfLoop || index.isParameter(made) || index.opcodeOf(made) != Opcode::ElemGet)
            return false;
        TypeID moved = typeInLook(store.valueOperand());
        moved = moved ? types.withoutPrimitives(moved, Primitive::Undefined) : TypeID();
        Reference taken = nameOfListOf(index.value(made));
        Reference target = nameOfListOf(store);
        if (!moved || isNeverType(moved) || !types.at(moved).isNumber() || !(types.at(moved).primitives & Primitive::FractionalNumber) || !taken || !target || !target.isValue() || isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(target)))))
            return false;
        size_t place = found.lists.findIf([&](const HoistedArrayGuard& list) { return list.name == taken; });
        bool isTargetListed = found.lists.containsIf([&](const HoistedArrayGuard& list) { return list.name == target; });
        if (place == notFound || isTargetListed)
            return false;
        HoistedArrayGuard& list = found.lists[place];
        if (list.isWritable || list.storage != ArrayStorage::Double || !list.admitsInt32Storage || list.hasAlternateStorage)
            return false;
        list.storage = ArrayStorage::Int32;
        list.admitsInt32Storage = false;
        movedElementIsNumber = true;
        setMovedElementStorage(list);
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue read : index.block(member).values()) {
                if (read.opcode() != Opcode::ElemGet || nameOfListOf(read) != taken)
                    continue;
                numbersBehindListsFound.set(read.id().index() - firstValueOfLoop);
                wholeNumbersBehindListsFound.set(read.id().index() - firstValueOfLoop);
            }
        }
        walker.noteInt32Version();
        walker.count(StatisticID::MovedNumbersKeepInt32s);
        return true;
    };
    auto speculateDoublesForMovedElement = [&](const BodyValue& operation, ValueID pushedOntoField, Reference listPushedOnto = { }) {
        bool isStore = operation.opcode() == Opcode::ElemSet;
        if ((!isStore && operation.opcode() != Opcode::Call) || readsOffListsOfUnknownContents.isEmpty() || index.loop(loop).numberOfLoopsWithin != 1 || !walker.canUseIterationGuards(inputHead) || (!isStore && !pushedOntoField && !untouchedLiteralOfPush(walker, operation, loop)))
            return false;
        ValueID made = index.stripCasts(pushedOntoField ? pushedOntoField : isStore ? operation.valueOperand() : operation.as<Opcode::Call>().argumentsOperands()[0]);
        if (!isStore && (!made || index.isParameter(made) || (index.opcodeOf(made) != Opcode::ElemGet && index.opcodeOf(made) != Opcode::ResultAt)))
            return false;
        size_t place = readsOffListsOfUnknownContents.findIf([&](const ReadOffListOfUnknownContents& kept) {
            return kept.read == made;
        });
        if (place == notFound)
            return false;
        Reference taken = readsOffListsOfUnknownContents[place].name;
        static constexpr OpcodeSet visibleAllocators { Opcode::NewArray, Opcode::Data };
        if (visibleAllocators.contains(walker.code().at(underlyingCellOf(walker.code(), taken.id())).opcode))
            return false;
        bool isComparedByCallee = listsComparedByCallees.contains(taken);
        if (isComparedByCallee)
            walker.count(StatisticID::ElementsComparedByCallees);
        movedElementIsNumber = isComparedByCallee || computesWithOtherElements(made, taken) || computesWithRemovedElements(listPushedOnto);
        if (!movedElementIsNumber && walker.isInt32VersionRejected())
            return false;
        for (HoistedArrayGuard& list : found.lists) {
            if (list.name != taken || list.storage != ArrayStorage::Other)
                continue;
            list.storage = movedElementIsNumber && versionTakesInt32s ? ArrayStorage::Int32 : ArrayStorage::Double;
            list.admitsInt32Storage = movedElementIsNumber && !versionTakesInt32s;
            list.isComparedByCallee = isComparedByCallee;
            setMovedElementStorage(list);
        }
        listsOfUnknownContents.removeAll(taken);
        readsOffListsOfUnknownContents.removeAllMatching([&](const ReadOffListOfUnknownContents& kept) {
            if (kept.name == taken) {
                numbersBehindListsFound.set(kept.read.index() - firstValueOfLoop);
                if (movedElementIsNumber && versionTakesInt32s)
                    wholeNumbersBehindListsFound.set(kept.read.index() - firstValueOfLoop);
            }
            return kept.name == taken;
        });
        if (!movedElementIsNumber || versionTakesInt32s)
            walker.noteInt32Version();
        return true;
    };
    bool checksElementsAlongsideElements = false;
    auto expectNumbersWhereElementIsTakenForNumber = [&](const BodyValue& operation) {
        if (readsOffListsOfUnknownContents.isEmpty())
            return false;
        static constexpr OpcodeSet operatorsThatTakeNumbers = numberProducingOperators | allBitwiseOperators | stepsOfNumbers;
        auto isRowElement = [&](ValueID operand) {
            ValueID made = index.stripCasts(operand);
            return readsOffListsOfUnknownContents.containsIf([&](const ReadOffListOfUnknownContents& kept) {
                return kept.read == made && std::ranges::any_of(walker.hoistedRowsOf(inputHead), [&](const Walker::HoistedRow& row) { return !row.isRejected && row.row == kept.name; });
            });
        };
        bool comparesRowElement = allEqualities.contains(operation.opcode()) && index.loop(loop).numberOfLoopsWithin == 1 && (isRowElement(operation.operand(0)) || isRowElement(operation.operand(1)));
        bool isEqualityInLoopThatStandsInNoOther = comparesRowElement || (allEqualities.contains(operation.opcode()) && (!index.loop(loop).parent || (checksElementsAlongsideElements && index.loop(loop).numberOfLoopsWithin == 1)));
        bool takesTextsToo = operationsThatJoinTexts.contains(operation.opcode()) || allRelations.contains(operation.opcode()) || isEqualityInLoopThatStandsInNoOther;
        bool storesIntoView = operation.opcode() == Opcode::ElemSet && isHolderSpeculatedView(walker, index, operation, typeInLook(operation.objectOperand()), firstValueOfLoop, guesses.span());
        if (readsOffListsOfUnknownContents.isEmpty() || !(takesTextsToo || storesIntoView || operatorsThatTakeNumbers.contains(operation.opcode()) || isMathCall(walker, index, operation)))
            return false;
        Reference taken;
        bool meetsNumericElement = false;
        bool meetsUnknownValue = false;
        operation.forEachOperand([&](ValueID operand, uint32_t position) {
            if (storesIntoView && operand != operation.valueOperand())
                return;
            ValueID made = index.stripCasts(operand);
            size_t place = readsOffListsOfUnknownContents.findIf([&](const ReadOffListOfUnknownContents& kept) {
                return kept.read == made;
            });
            if (taken || place == notFound)
                return;
            ValueID other = takesTextsToo && position < 2 ? operation.operand(1 - position) : ValueID();
            bool isHeldAgainstElementOfSuchList = other && (allRelations.contains(operation.opcode()) || operation.opcode() == Opcode::Add || (checksElementsAlongsideElements && isEqualityInLoopThatStandsInNoOther)) && readsOffListsOfUnknownContents.containsIf([&](const ReadOffListOfUnknownContents& kept) {
                return kept.read == index.stripCasts(other);
            });
            ValueID addend = other && operation.opcode() == Opcode::Add ? index.stripCasts(other) : ValueID();
            isHeldAgainstElementOfSuchList |= addend && addend.index() >= firstValueOfLoop && !index.isParameter(addend) && index.opcodeOf(addend) == Opcode::Call && callsElementReader(walker, index.value(addend));
            bool isComparedRowElement = comparesRowElement && isRowElement(operand);
            bool isComparison = allRelations.contains(operation.opcode()) || isEqualityInLoopThatStandsInNoOther;
            ValueID compared = other && isComparison ? index.stripCasts(other) : ValueID();
            ValueID holder = compared;
            while (allRelations.contains(operation.opcode()) && holder && holder.index() >= firstValueOfLoop && !index.isParameter(holder) && index.opcodeOf(holder) == Opcode::PropGet && !index.value(holder).isFreeName())
                holder = index.stripCasts(index.value(holder).objectOperand());
            TypeID typeOfCompared = holder && holder.index() < firstValueOfLoop ? typeInLook(compared) : TypeID();
            if (!typeOfCompared && holder && holder.index() < firstValueOfLoop)
                typeOfCompared = TypeTable::anything;
            std::optional<uint32_t> carried = compared && allRelations.contains(operation.opcode()) ? index.block(inputHead).indexOfParameter(compared) : std::nullopt;
            if (carried && *carried < entering.size() && entering[*carried])
                typeOfCompared = walker.ownTypeOf(entering[*carried]);
            bool isComparedWithUnknown = typeOfCompared && types.isUninformative(typeOfCompared);
            if (takesTextsToo && !isHeldAgainstElementOfSuchList && !isComparedRowElement && !isComparedWithUnknown && (!other || !isNumberBehindListsFound(other)))
                return;
            taken = readsOffListsOfUnknownContents[place].name;
            meetsUnknownValue = isComparedWithUnknown;
            ValueID otherElement = other ? index.stripCasts(other) : ValueID();
            Reference otherArray = otherElement && otherElement.index() >= firstValueOfLoop && !index.isParameter(otherElement) && index.opcodeOf(otherElement) == Opcode::ElemGet ? nameThatEntersLoopInListReadsInLoops(walker, index, index.value(otherElement).objectOperand(), firstValueOfLoop) : Reference();
            meetsNumericElement = otherArray && found.lists.containsIf([&](const HoistedArrayGuard& list) {
                return list.name == otherArray && isNumericStorage(list.storage) && !list.hasAlternateStorage;
            });
        });
        if (!taken)
            return false;
        for (HoistedArrayGuard& list : found.lists) {
            if (list.name != taken || list.storage != ArrayStorage::Other)
                continue;
            list.storage = ArrayStorage::Double;
            list.admitsInt32Storage = true;
            list.hasAlternateStorage = isEqualityInLoopThatStandsInNoOther || meetsUnknownValue;
            if (list.hasAlternateStorage)
                list.alternateStorage = ArrayStorage::Other;
            list.keepsStorageWithoutAlternate |= walker.walksNumericListCopy() || meetsNumericElement;
            if (meetsNumericElement)
                walker.count(StatisticID::ElementsHeldAgainstNumbers);
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

    Vector<Reference, 2> listsPoppedFrom;
    auto listListPoppedFrom = [&](const BodyValue& call, Reference name) {
        if (foundArrayLookup.find(found.lists.span(), name) != notFound || isAlreadyGuardedForReads(name) || isBornBoxed(walker, name))
            return;
        ArrayStorage written = predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(name)));
        if (written == ArrayStorage::Contiguous)
            return;
        found.lists.append(HoistedArrayGuard { name, isNumericStorage(written) ? ArrayStorage::Double : ArrayStorage::Other, isNumericStorage(written) });
        if (isNumericStorage(written))
            return;
        arraysReadByCopies.append(name);
        listsOfUnknownContents.append(name);
        readsOffListsOfUnknownContents.append({ call.id(), name });
        listsPoppedFrom.append(name);
    };
    auto notePopInLoop = [&](const BodyValue& call, Reference name) {
        size_t place = foundArrayLookup.find(found.lists.span(), name);
        if (place == notFound || !isNumericStorage(found.lists[place].storage))
            return false;
        listsPoppedFrom.append(name);
        noteNumberBehindListsFound(call, found.lists[place].storage == ArrayStorage::Int32);
        return true;
    };
    auto noteElementReturnedByCopy = [&](const BodyValue& call, FunctionID callee) {
        Reference name = findArrayReadByCopy(call, callee);
        if (!name)
            return;
        ValueID element = walker.findReturnedElement(walker.facts().bodies().at(callee));
        bool isRead = walker.facts().bodies().at(callee).opcodeOf(element) == Opcode::ElemGet;
        if (!isRead) {
            listListPoppedFrom(call, name);
            if (notePopInLoop(call, name))
                return;
        }
        size_t place = foundArrayLookup.find(found.lists.span(), name);
        if (place != notFound && isNumericStorage(found.lists[place].storage))
            noteNumberBehindListsFound(call, found.lists[place].storage == ArrayStorage::Int32);
        else if (listsOfUnknownContents.contains(name))
            readsOffListsOfUnknownContents.append({ call.id(), name });
        if (place != notFound || !isRead || isFreshAllocation(walker, name) || isAlreadyGuardedForReads(name) || listsThatCopiesOfCalleesStoreIntoUncounted.contains(name) || tryReuseForElementAccess(name))
            return;
        TypeID seenByLink = walker.facts().linked().resultOfReadByKey(callee, element);
        if (kindWrittenOrSeenByLinkInListReadsInLoops(types, predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(name))), seenByLink) != ArrayStorage::Other || (seenByLink && !types.at(seenByLink).isAnything()))
            return;
        found.lists.append(HoistedArrayGuard { name, ArrayStorage::Other, false });
        arraysReadByCopies.append(name);
        listsOfUnknownContents.append(name);
        readsOffListsOfUnknownContents.append({ call.id(), name });
    };

    std::optional<Vector<ValueID, 4>> pushedValues;
    Vector<ValueID, 4> elementsPushed;
    auto isPushed = [&](ValueID element) {
        if (!pushedValues) {
            pushedValues.emplace();
            static constexpr OpcodeSet numbersMadeOfElements { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement };
            for (BlockID member : index.opcodes().contains(Opcode::Call) ? index.membersOf(loop) : std::span<const BlockID>()) {
                for (BodyValue call : index.block(member).values()) {
                    if (call.opcode() != Opcode::Call || call.as<Opcode::Call>().calleeKey() != walker.program().keyOf(BuiltinNames::push))
                        continue;
                    for (ValueID argument : call.as<Opcode::Call>().argumentsOperands()) {
                        ValueID pushed = index.stripCasts(argument);
                        pushedValues->append(pushed);
                        elementsPushed.append(pushed);
                        if (pushed && !index.isParameter(pushed) && numbersMadeOfElements.contains(index.opcodeOf(pushed)))
                            index.value(pushed).forEachOperand([&](ValueID operand, uint32_t) { pushedValues->append(index.stripCasts(operand)); });
                    }
                }
            }
        }
        return pushedValues->contains(element);
    };
    std::optional<Vector<ValueID, 2>> switchedValues;
    auto isSwitchedOn = [&](ValueID element) {
        if (!switchedValues) {
            switchedValues.emplace();
            for (BlockID member : index.membersOf(loop)) {
                BodyTerminator end = index.block(member).terminator();
                if (end.kind() == TerminatorKind::Switch)
                    switchedValues->append(index.stripCasts(end.operand(0)));
            }
        }
        return switchedValues->contains(element);
    };
    auto prefersInt32Storage = [&](ValueID element) {
        return element && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected() && index.loop(loop).numberOfLoopsWithin == 1 && !index.loop(loop).parent && (isPushed(element) || isSwitchedOn(element) || searchesNumbersByHalves(walker, inputHead));
    };
    auto isTableOfBits = [&](const BodyValue& read) {
        if (walker.walksCopyForAnotherKindOfStorage() || walker.isInt32VersionRejected() || index.loop(loop).numberOfLoopsWithin != 1 || index.loop(loop).parent)
            return false;
        constexpr int64_t maxTableIndex = std::numeric_limits<uint16_t>::max();
        ValueID key = index.stripCasts(read.propertyOperand());
        if (!key || key.index() < firstValueOfLoop || index.isParameter(key))
            return false;
        if (index.opcodeOf(key) == Opcode::ElemGet)
            key = index.stripCasts(index.value(key).propertyOperand());
        if (!key || key.index() < firstValueOfLoop || index.isParameter(key))
            return false;
        BodyValue made = index.value(key);
        ValueID right = made.opcode() == Opcode::BitAnd || made.opcode() == Opcode::URShift ? index.stripCasts(made.operand(1)) : ValueID();
        if (!right || index.isParameter(right) || index.opcodeOf(right) != Opcode::Integer)
            return false;
        int64_t written = index.value(right).as<Opcode::Integer>().integer();
        bool isSmall = made.opcode() == Opcode::BitAnd ? written >= 0 && written <= maxTableIndex : written >= 16 && written < 32;
        return isSmall && readsTruncatedElement(walker, read);
    };
    auto isSummedModulo = [&](const BodyValue& read) {
        return !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected() && index.loop(loop).numberOfLoopsWithin == 1 && !index.loop(loop).parent && isTermOfRemainder(index, read.id());
    };
    auto mayNarrowGuard = [&](ValueID element, Reference name) {
        bool takesInt32s = element && !walker.walksCopyForAnotherKindOfStorage() && ((prefersInt32Storage(element) && (isSwitchedOn(element) || elementsPushed.contains(element) || searchesNumbersByHalves(walker, inputHead))) || isCarriedIndex(element) || isSummedModulo(index.value(element)));
        std::optional<HoistedArrayGuard> noted = takesInt32s ? findNoteInListReadsInLoops(walker, name) : std::nullopt;
        return noted && noted->storage == ArrayStorage::Double && noted->admitsInt32Storage && !noted->isWritable && !noted->turnsStoreIntoLists;
    };
    std::optional<Vector<Reference, 4>> listsThatTextOfLoopStoresInto;
    struct ListsReadByText {
        HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> all;
        HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> ofIntegersByLink;
    };
    std::optional<ListsReadByText> listsReadByTextOfLoop;
    auto findListsReadByText = [&]() -> const ListsReadByText& {
        if (listsReadByTextOfLoop)
            return *listsReadByTextOfLoop;
        listsReadByTextOfLoop.emplace();
        for (BlockID member : index.opcodes().contains(Opcode::ElemGet) ? index.membersOf(loop) : std::span<const BlockID>()) {
            for (BodyValue read : index.block(member).values()) {
                if (read.opcode() != Opcode::ElemGet)
                    continue;
                ValueID list = valueThatEntersLoopInListReadsInLoops(index, read.objectOperand(), firstValueOfLoop);
                Reference name = list.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, list, firstValueOfLoop) : walker.referenceOf(list);
                if (!name)
                    continue;
                listsReadByTextOfLoop->all.add(name.bits());
                TypeID seen = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
                if (seen && !isNeverType(seen) && types.at(types.withoutNullish(seen)).isNumberWithoutFraction())
                    listsReadByTextOfLoop->ofIntegersByLink.add(name.bits());
            }
        }
        return *listsReadByTextOfLoop;
    };
    auto textOfLoopReadsList = [&](Reference name) {
        return findListsReadByText().all.contains(name.bits());
    };
    auto holdsIntegersByLink = [&](Reference name) {
        return findListsReadByText().ofIntegersByLink.contains(name.bits());
    };
    auto findListsStoredInto = [&]() -> const Vector<Reference, 4>& {
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
        return *listsThatTextOfLoopStoresInto;
    };
    auto textOfLoopStoresIntoList = [&](Reference name) {
        return findListsStoredInto().contains(name);
    };
    auto storesOnlyIntoUnreadLists = [&] {
        const Vector<Reference, 4>& stored = findListsStoredInto();
        return !stored.isEmpty() && std::ranges::none_of(stored, textOfLoopReadsList);
    };

    auto findInlinedCallees = [&](const BodyValue& call) {
        static constexpr size_t mostCallees = 4;
        Vector<FunctionID, mostCallees> callees;
        TypeID called = typeInLook(call.as<Opcode::Call>().calleeOperand());
        if (Reference entering = called && !types.isUninformative(called) ? Reference() : nameThatEntersLoopInListReadsInLoops(walker, index, call.as<Opcode::Call>().calleeOperand(), firstValueOfLoop); entering && entering.isValue())
            called = walker.ownTypeOf(entering);
        ValueID name = index.stripCasts(call.as<Opcode::Call>().calleeOperand());
        const LinkTables& held = walker.facts().held();
        std::optional<uint32_t> parameter = name && index.isParameter(name) && index.blockOf(name) == entryBlock ? index.block(entryBlock).indexOfParameter(name) : std::nullopt;
        if (parameter && (!called || types.at(called).hasUnknown()) && walker.currentFunction().index() < held.functions.size() && *parameter < held.parametersOf(walker.currentFunction()).size())
            called = held.parametersOf(walker.currentFunction())[*parameter].proven;
        if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
            callees.append(callee);
        else if (called && !holdsOnlyPrimitives(called)) {
            Type closures = types.at(types.withOnlyObjects(called));
            if (closures.hasObjects() && closures.isJavaScriptValue() && closures.hasOnlyClosures() && !closures.hasUnknown() && !closures.namesMergedObjects()) {
                forEachFunctionOf(types, closures, scopedLambda<void(FunctionID)>([&](FunctionID callee) {
                    callees.append(callee);
                }));
            }
        }
        if (callees.size() > mostCallees || !std::ranges::all_of(callees, [&](FunctionID callee) { return lookReadsCopyOfCalleeInListReadsInLoops(walker, call, callee); }))
            callees.shrink(0);
        return callees;
    };

    auto expectNumbersWhereCalleeTakesElement = [&](const BodyValue& call) {
        if (call.opcode() != Opcode::Call || readsOffListsOfUnknownContents.isEmpty() || !walker.canUseIterationGuards(inputHead))
            return;
        auto placeOfRead = [&](ValueID argument) {
            ValueID made = argument ? index.stripCasts(argument) : ValueID();
            return made ? readsOffListsOfUnknownContents.findIf([&](const ReadOffListOfUnknownContents& kept) { return kept.read == made; }) : notFound;
        };
        if (!std::ranges::any_of(call.as<Opcode::Call>().argumentsOperands(), [&](ValueID argument) { return placeOfRead(argument) != notFound; }))
            return;
        auto callees = findInlinedCallees(call);
        if (callees.isEmpty())
            return;
        auto movesElementsWithin = [&](Reference list) {
            return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
                return std::ranges::any_of(index.block(member).values(), [&](BodyValue store) {
                    ValueID moved = store.opcode() == Opcode::ElemSet ? index.stripCasts(store.valueOperand()) : ValueID();
                    return moved && moved.index() >= firstValueOfLoop && !index.isParameter(moved) && index.opcodeOf(moved) == Opcode::ElemGet && nameOfListOf(store) == list && nameOfListOf(index.value(moved)) == list;
                });
            });
        };
        auto isUnknownArgument = [&](ValueID argument) {
            ValueID made = argument ? index.stripCasts(argument) : ValueID();
            if (!made || placeOfRead(made) != notFound)
                return !!made;
            std::optional<uint32_t> carried = index.block(inputHead).indexOfParameter(made);
            TypeID entered = made.index() < firstValueOfLoop ? typeInLook(made) : carried && *carried < entering.size() && entering[*carried] ? walker.ownTypeOf(entering[*carried]) : TypeID();
            return (entered || made.index() < firstValueOfLoop) && (!entered || types.isUninformative(entered));
        };
        bool isOrdered = false;
        auto takesNumber = [&](FunctionID callee, ValueID argument) {
            static constexpr OpcodeSet operatorsThatTakeNumbers = numberProducingOperators | allBitwiseOperators | stepsOfNumbers;
            static constexpr OpcodeSet operatorsThatTakeStringsToo = operationsThatJoinTexts | allRelations | allEqualities;
            const BodyIndex& body = walker.facts().bodies().at(callee);
            bool takes = false;
            for (uint32_t block = 0; block < body.numberOfBlocks(); ++block) {
                for (BodyValue operation : body.block(BlockID(block)).values()) {
                    bool takesStringsToo = operatorsThatTakeStringsToo.contains(operation.opcode());
                    if (!takesStringsToo && !operatorsThatTakeNumbers.contains(operation.opcode()))
                        continue;
                    operation.forEachOperand([&](ValueID operand, uint32_t position) {
                        if (position > 1 || valuePassedIntoCopy(walker, body, call, callee, operand) != argument)
                            return;
                        ValueID other = takesStringsToo ? body.stripCasts(operation.operand(1 - position)) : ValueID();
                        ValueID otherArgument = other ? valuePassedIntoCopy(walker, body, call, callee, other) : ValueID();
                        bool isMadeNumber = other && !body.isParameter(other) && (numberConstantOpcodes.contains(body.opcodeOf(other)) || operatorsThatTakeNumbers.contains(body.opcodeOf(other)));
                        bool takesHere = !takesStringsToo || isMadeNumber || (otherArgument && (isNumberBehindListsFound(otherArgument) || isUnknownArgument(otherArgument)));
                        takes |= takesHere;
                        isOrdered |= takesHere && !allEqualities.contains(operation.opcode());
                    });
                }
            }
            return takes;
        };
        auto readsByName = [&](FunctionID callee, ValueID argument) {
            const BodyIndex& body = walker.facts().bodies().at(callee);
            for (uint32_t block = 0; block < body.numberOfBlocks(); ++block) {
                for (BodyValue read : body.block(BlockID(block)).values()) {
                    if (read.opcode() == Opcode::PropGet && !read.isFreeName() && valuePassedIntoCopy(walker, body, call, callee, read.objectOperand()) == argument)
                        return true;
                }
            }
            return false;
        };
        for (ValueID argument : call.as<Opcode::Call>().argumentsOperands()) {
            size_t place = placeOfRead(argument);
            isOrdered = false;
            if (place == notFound)
                continue;
            Reference taken = readsOffListsOfUnknownContents[place].name;
            if (std::ranges::all_of(callees, [&](FunctionID callee) { return readsByName(callee, argument); })) {
                for (HoistedArrayGuard& list : found.lists) {
                    if (list.name != taken || list.storage != ArrayStorage::Other)
                        continue;
                    list.storage = ArrayStorage::Contiguous;
                    walker.count(StatisticID::ElementsReadByCallees);
                }
                listsOfUnknownContents.removeAll(taken);
                readsOffListsOfUnknownContents.removeAllMatching([&](const ReadOffListOfUnknownContents& kept) { return kept.name == taken; });
                continue;
            }
            if (!std::ranges::all_of(callees, [&](FunctionID callee) { return takesNumber(callee, argument); }))
                continue;
            if (listsComparedByCallees.contains(taken))
                continue;
            if (index.loop(loop).numberOfLoopsWithin == 1 && movesElementsWithin(taken)) {
                listsComparedByCallees.append(taken);
                continue;
            }
            for (HoistedArrayGuard& list : found.lists) {
                if (list.name != taken || list.storage != ArrayStorage::Other)
                    continue;
                list.storage = ArrayStorage::Double;
                list.admitsInt32Storage = true;
                list.hasAlternateStorage = !textOfLoopStoresIntoList(taken);
                list.alternateStorage = ArrayStorage::Other;
                list.keepsStorageWithoutAlternate = isOrdered || walker.walksNumericListCopy();
                list.isComparedByCallee = isOrdered;
                walker.count(StatisticID::ElementsComparedByCallees);
            }
            listsOfUnknownContents.removeAll(taken);
            readsOffListsOfUnknownContents.removeAllMatching([&](const ReadOffListOfUnknownContents& kept) {
                if (kept.name == taken)
                    numbersBehindListsFound.set(kept.read.index() - firstValueOfLoop);
                return kept.name == taken;
            });
        }
    };

    auto storesInvariantValue = [&](const BodyValue& store) {
        ValueID stored = index.stripCasts(store.valueOperand());
        while (stored && stored.index() >= firstValueOfLoop) {
            if (index.isParameter(stored) || index.opcodeOf(stored) != Opcode::PropGet || index.value(stored).isFreeName())
                return false;
            stored = index.stripCasts(index.value(stored).objectOperand());
        }
        return !!stored;
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

    auto guardChosenArrays = [&](const BodyValue& access, bool isStore, bool readsLength) {
        Vector<ChosenArray, 3> chosen = findChosenArrays(walker, index, access.objectOperand(), firstValueOfLoop);
        if (chosen.isEmpty() || !walker.canUseIterationGuards(inputHead))
            return false;
        TypeID stored = isStore ? typeInLook(access.valueOperand()) : TypeID();
        if (isStore && (!stored || isNeverType(stored) || !types.at(stored).isNumber()) && isNumberBehindListsFound(access.valueOperand()))
            stored = isWholeNumberBehindListsFound(access.valueOperand()) ? TypeTable::int32 : TypeTable::number;
        if (isStore && (!stored || isNeverType(stored) || !types.at(stored).isNumber()))
            return false;
        bool everyArrayIsNumeric = true;
        bool canGuardArrays = std::ranges::all_of(chosen, [&](const ChosenArray& array) {
            Reference name = array.name;
            TypeID proven = name.isValue() ? walker.ownTypeOf(name) : TypeID();
            for (const HoistedSpeculation& guess : guesses) {
                if (guess.value == name)
                    proven = types.intersectionOf(proven, guess.tested);
            }
            TypeID list = proven ? types.withoutNullish(proven) : TypeID();
            if (list && !hasKind(types, list, OperandKind::Array)) {
                TypeID expected = findExpectation(walker, array.written).type;
                list = expected && types.overlap(proven, expected) ? types.withoutNullish(expected) : TypeID();
            }
            if (!list || !hasKind(types, list, OperandKind::Array) || isFreshAllocation(walker, name) || mayParameterBeMissing(walker, name))
                return false;
            everyArrayIsNumeric &= isNumericStorage(predictedArrayStorage(walker, list));
            return readsLength || readTakesIndexOfProvenList(types, list, typeOfKeyInLook(access.propertyOperand()));
        });
        if (!canGuardArrays || (!readsLength && !everyArrayIsNumeric))
            return false;
        bool asksForInt32s = !isStore && !readsLength && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected() && index.loop(loop).numberOfLoopsWithin == 1 && !index.loop(loop).parent;
        for (const ChosenArray& array : chosen) {
            Reference name = array.name;
            if (!isStore && !readsLength)
                arraysNeedingExactStorage.append(name);
            if (size_t place = foundArrayLookup.find(found.lists.span(), name); place != notFound) {
                HoistedArrayGuard& listed = found.lists[place];
                if (isStore && isNumericStorage(listed.storage)) {
                    bool keepsInt32s = listed.storage == ArrayStorage::Int32 && types.isSubset(stored, TypeTable::int32);
                    listed.storage = keepsInt32s ? ArrayStorage::Int32 : ArrayStorage::Double;
                    listed.hasAlternateStorage &= keepsInt32s;
                    listed.admitsInt32Storage = false;
                    listed.isWritable = true;
                    arraysNeedingExactStorage.append(name);
                }
                continue;
            }
            if (isAlreadyGuardedForReads(name))
                continue;
            if (readsLength)
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Other, false });
            else if (isStore)
                found.lists.append(HoistedArrayGuard { name, types.isSubset(stored, TypeTable::int32) ? ArrayStorage::Int32 : ArrayStorage::Double, false, true });
            else if (asksForInt32s) {
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                found.lists.last().hasAlternateStorage = true;
                found.lists.last().alternateStorage = ArrayStorage::Double;
                walker.noteInt32Version();
            } else
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Double, true });
            chosenArrays.append(name);
        }
        if (!isStore)
            noteNumberBehindListsFound(access, readsLength || asksForInt32s);
        return true;
    };
    Vector<Reference, 4> preheaderKeys;
    BackEdgeDominators backEdgeDominators(index, loop);
    Vector<uint32_t, 32> namesUsedEveryIteration;
    auto markUsedEveryIteration = [&](Reference name) {
        if (name && name.isValue() && (namesUsedEveryIteration.isEmpty() || namesUsedEveryIteration.last() != name.id().index()))
            namesUsedEveryIteration.append(name.id().index());
    };
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        if (block.isHandlerBlock()) {
            if (walker.handlersRunOtherCode() || storesOnlyIntoUnreadLists())
                continue;
            return { };
        }
        noteNumbersThatMeetInParametersOf(member);
        bool dominatesBackEdges = backEdgeDominators.contains(member);
        for (BodyValue operation : block.values()) {
            while (expectNumbersWhereElementIsTakenForNumber(operation)) { }
            expectNumbersWhereCalleeTakesElement(operation);
            bool movesElement = speculateDoublesForMovedElement(operation, ValueID()) || keepInt32sForMovedNumber(operation);
            expectBoxedValuesWhereElementIsReadByName(operation);
            if (numberConstantOpcodes.contains(operation.opcode()))
                noteNumberBehindListsFound(operation, operation.opcode() == Opcode::Integer && operation.as<Opcode::Integer>().integer() == static_cast<int32_t>(operation.as<Opcode::Integer>().integer()));
            else if (operatorsThatMakeNumbersOfNumbers.contains(operation.opcode()) && isNumberBehindListsFound(operation.operand(0)) && isNumberBehindListsFound(operation.operand(1)))
                noteNumberBehindListsFound(operation, operation.opcode() != Opcode::Div && isWholeNumberBehindListsFound(operation.operand(0)) && isWholeNumberBehindListsFound(operation.operand(1)));
            else if (allBitwiseOperators.contains(operation.opcode()) && (isNumberBehindListsFound(operation.operand(0)) || (operation.opcode() != Opcode::BitNot && isNumberBehindListsFound(operation.operand(1)))))
                noteNumberBehindListsFound(operation, operation.opcode() != Opcode::URShift);
            else if (stepsOfNumbers.contains(operation.opcode()) && isNumberBehindListsFound(operation.operand(0)))
                noteNumberBehindListsFound(operation, isWholeNumberBehindListsFound(operation.operand(0)));
            else if (operation.opcode() == Opcode::Negate && isNumberBehindListsFound(operation.operand(0)))
                noteNumberBehindListsFound(operation, false);
            else if (ValueID taken = elementTakenByPattern(index, operation); taken && isNumberBehindListsFound(taken))
                noteNumberBehindListsFound(operation, isWholeNumberBehindListsFound(taken));
            else if (asksItsSiteAndIsReckonedWith(operation))
                noteNumberBehindListsFound(operation, false);
            else if (isFieldAddedTo(operation)) {
                noteNumberBehindListsFound(operation, false);
                walker.count(StatisticID::FieldSumsTakenForNumbers);
            }
            if ((operatorsThatMakeNumbersOfNumbers.contains(operation.opcode()) || operation.opcode() == Opcode::Mod) && operation.opcode() != Opcode::Div && isWholeNumberForKeyOfRead(operation.operand(0)) && isWholeNumberForKeyOfRead(operation.operand(1))) {
                if (operation.opcode() == Opcode::Mod || comesOfLengthOrRemainder(operation.operand(0)) || comesOfLengthOrRemainder(operation.operand(1)))
                    noteThatComesOfLengthOrRemainder(operation);
                else
                    noteWholeNumberThatKeyOfReadTakes(operation);
            }
            Reference untouchedLiteral = operation.opcode() == Opcode::Call ? untouchedLiteralOfPush(walker, operation, loop) : Reference();
            ValueID pushedByCall;
            if (operation.opcode() == Opcode::Call && !untouchedLiteral && walker.canUseIterationGuards(inputHead)) {
                auto namePushedOnto = [&](Reference name, ValueID value) {
                    if (!name || !name.isValue() || !value || !holdsArrayOrNothing(name))
                        return;
                    untouchedLiteral = name;
                    pushedByCall = value;
                    someFieldMayHoldLiteral |= !allocatesNewCell(walker.code().at(underlyingCellOf(walker.code(), name.id())));
                };
                auto calleePushesOntoField = [&](FunctionID callee) {
                    const BodyIndex* body = walker.facts().bodies().find(callee);
                    return body && walker.pushesOntoField(*body);
                };
                ValueID receiver = walker.isOutermostPushPopLoop(index.loopOf(member)) ? receiverOfMethodCall(walker.program(), index, operation, BuiltinNames::push, 1) : ValueID();
                Reference changed = receiver && !isLiteralOfBody(index, receiver) && (receiver.index() < firstValueOfLoop || !fieldPushedOnto(walker.program(), index, operation)) ? nameThatEntersLoopInListReadsInLoops(walker, index, receiver, firstValueOfLoop) : Reference();
                if (changed && changed.isValue() && holdsArrayOrNothing(changed))
                    namePushedOnto(changed, operation.as<Opcode::Call>().argumentsOperands()[0]);
                else if (ValueID field = fieldPushedOnto(walker.program(), index, operation); field && field.index() >= firstValueOfLoop)
                    namePushedOnto(nameThatEntersLoopInListReadsInLoops(walker, index, field, firstValueOfLoop), operation.as<Opcode::Call>().argumentsOperands()[0]);
                else if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()); callee && calleePushesOntoField(callee) && lookReadsCopyOfCalleeInListReadsInLoops(walker, operation, callee)) {
                    forEachFieldAccessInCopy(walker, operation, callee, Opcode::Call, [&](ValueID holder, PropertyKeyID key, ValueID value, BodyValue) {
                        namePushedOnto(valueOfFieldInPreheader(walker, nameThatEntersLoopInListReadsInLoops(walker, index, holder, firstValueOfLoop), key), value);
                    });
                }
                if (pushedByCall && !movesElement)
                    movesElement = speculateDoublesForMovedElement(operation, pushedByCall, untouchedLiteral);
            }
            if (operation.opcode() == Opcode::Call && !untouchedLiteral && walker.canUseIterationGuards(inputHead) && walker.isPopFromLiteral(operation)) {
                Reference literal = walker.referenceOf(index.stripCasts(operation.as<Opcode::Call>().thisValueOperand()));
                if (literal && literal.isValue() && isProvenArray(walker, literal))
                    listListPoppedFrom(operation, literal);
                if (notePopInLoop(operation, literal))
                    continue;
            }
            ValueID poppedFrom = operation.opcode() == Opcode::Call && !untouchedLiteral && walker.canUseIterationGuards(inputHead) ? receiverOfMethodCall(walker.program(), index, operation, BuiltinNames::pop, 0) : ValueID();
            if (poppedFrom && ((poppedFrom.index() >= firstValueOfLoop && findMethodReceiverField(walker.program(), index, operation, BuiltinNames::pop, 0)) || (walker.isOutermostPushPopLoop(index.loopOf(member)) && !isLiteralOfBody(index, poppedFrom)))) {
                Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, poppedFrom, firstValueOfLoop);
                if (name && name.isValue() && holdsArrayOrNothing(name)) {
                    listListPoppedFrom(operation, name);
                    if (notePopInLoop(operation, name))
                        continue;
                }
            }
            Reference pushedOnto = operation.opcode() == Opcode::Call ? literalThatCallPushesOntoInEveryTurn(walker, operation, inputHead) : Reference();
            if (movesElement && pushedOnto && pushedOnto == untouchedLiteral) {
                for (KnownNote note : { KnownNote::PushAppendsToLiteral, KnownNote::LoopAppendsToLiteral, KnownNote::FinalLiteralLength, KnownNote::CapacityForRemainingIterations })
                    endKnown(walker, note, pushedOnto, { }, inputHead.index());
                pushedOnto = { };
            }
            TypeID pushed;
            if (pushedOnto || untouchedLiteral) {
                ValueID written = pushedByCall ? pushedByCall : operation.as<Opcode::Call>().argumentsOperands()[0];
                pushed = typeInLook(written);
                pushed = pushed ? types.withoutPrimitives(pushed, Primitive::Undefined) : TypeID();
                if ((!pushed || isNeverType(pushed) || !types.at(pushed).isNumber()) && isNumberBehindListsFound(written))
                    pushed = isWholeNumberBehindListsFound(written) ? TypeTable::int32 : TypeTable::number;
                bool pushesNumber = pushed && !isNeverType(pushed) && types.at(pushed).isNumber();
                if (untouchedLiteral && (movesElement || (pushesNumber && !isBornBoxed(walker, untouchedLiteral)))) {
                    if (!pushedOnto || walker.lastStoreByKeyOfLoopInCurrentBody(index.loopOf(member)) > operation.id().index())
                        arraysNeedingExactStorage.append(untouchedLiteral);
                    pushedOnto = untouchedLiteral;
                    untouchedLiteralsPushedOnto.append(untouchedLiteral);
                }
            }
            if (pushedOnto && walker.canUseIterationGuards(inputHead)) {
                bool isListed = foundArrayLookup.find(found.lists.span(), pushedOnto) != notFound;
                if (pushed && !isNeverType(pushed) && types.at(pushed).isNumber() && !isListed && !isFreshAllocation(walker, pushedOnto)) {
                    ArrayStorage kindPushedOnto = types.at(pushed).primitives & Primitive::FractionalNumber ? ArrayStorage::Double : ArrayStorage::Int32;
                    if (!types.isSubset(pushed, TypeTable::int32)) {
                        bool isWrittenToHoldNumbersAlone = isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(pushedOnto))));
                        (kindPushedOnto == ArrayStorage::Double && !isWrittenToHoldNumbersAlone ? listsThatTakeMoreThanInt32s : arraysNeedingExactStorage).append(pushedOnto);
                    }
                    found.lists.append(HoistedArrayGuard { pushedOnto, kindPushedOnto, false, true });
                    if (movesElement)
                        setMovedElementStorage(found.lists.last());
                } else if (size_t place = foundArrayLookup.find(found.lists.span(), pushedOnto); place != notFound && pushed && !isNeverType(pushed) && types.at(pushed).isNumber() && isNumericStorage(found.lists[place].storage)) {
                    found.lists[place].isWritable = true;
                    const Operation& born = walker.code().at(underlyingCellOf(walker.code(), pushedOnto.id()));
                    bool isAllocatedAsDouble = born.opcode == Opcode::NewArray && born.arrayStorage() == ArrayStorage::Double;
                    found.lists[place].storage = isAllocatedAsDouble || (types.at(pushed).primitives & Primitive::FractionalNumber) ? ArrayStorage::Double : ArrayStorage::Int32;
                    found.lists[place].admitsInt32Storage = false;
                }
                continue;
            }
            if (operation.opcode() == Opcode::Call && operation.as<Opcode::Call>().calleeKey() == walker.program().keyOf(BuiltinNames::at) && operation.as<Opcode::Call>().thisValueOperand() && operation.as<Opcode::Call>().argumentsOperands().size() == 1) {
                ValueID receiver = operation.as<Opcode::Call>().thisValueOperand();
                ValueID input = valueThatEntersLoopInListReadsInLoops(index, receiver, firstValueOfLoop);
                Reference name = input.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop) : walker.referenceOf(input);
                TypeID proven = name && name.isValue() ? walker.ownTypeOf(name) : TypeID();
                for (const HoistedSpeculation& guess : guesses) {
                    if (proven && guess.value == name)
                        proven = types.intersectionOf(proven, guess.tested);
                }
                if (proven && hasKind(types, types.withoutNullish(proven), OperandKind::Array)) {
                    if (!tryReuseForElementAccess(name) && !isFreshAllocation(walker, name) && !isAlreadyGuardedForReads(name)) {
                        ArrayStorage written = predictedArrayStorage(walker, types.withoutNullish(proven));
                        if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, receiver).type : TypeID())
                            written = predictedArrayStorage(walker, types.withoutNullish(expected));
                        if (written != ArrayStorage::Other)
                            found.lists.append(HoistedArrayGuard { name, isNumericStorage(written) ? ArrayStorage::Double : written, isNumericStorage(written) });
                    }
                    if (size_t place = foundArrayLookup.find(found.lists.span(), name); place != notFound && isNumericStorage(found.lists[place].storage))
                        noteNumberBehindListsFound(operation, found.lists[place].storage == ArrayStorage::Int32);
                    continue;
                }
            }
            if (surelyEndsWhatIsShownOfListsInListReadsInLoops(walker, operation, firstValueOfLoop, typeInLook, member != inputHead ? walker.canUseIterationGuards(inputHead) && (index.loopOf(member) == loop || comparesElementsAlone(walker, index.loop(index.loopOf(member)).head) || (index.loop(loop).numberOfLoopsWithin == 2 && callsReaders(walker, index.loop(index.loopOf(member)).head))) : walker.canUseIterationGuards(inputHead)))
                return { };
            if (const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, operation, true); stepped && stepped->kind != JSC::IterationKind::Values && stepped->isDeadAfterLoop && stepped->list.isValue()) {
                Reference name = steppedArrayBeforeLoop(walker, index, operation, stepped->list, firstValueOfLoop);
                if (!name)
                    continue;
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
                found.lists.last().subclass = stepped->subclass;
                continue;
            }
            if (const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, operation); stepped && stepped->isDeadAfterLoop && stepped->list.isValue()) {
                Reference name = steppedArrayBeforeLoop(walker, index, operation, stepped->list, firstValueOfLoop);
                if (!name)
                    continue;
                if (dominatesBackEdges)
                    markUsedEveryIteration(name);
                if (tryReuseForElementAccess(name) || isFreshAllocation(walker, name) || (isAlreadyGuardedForReads(name) && !mayNarrowGuard(findStepValue(index, operation), name)))
                    continue;
                ArrayStorage written = predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(name)));
                if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, index.value(operation.operand(0)).as<Opcode::IteratorOpen>().iterableOperand()).type : TypeID())
                    written = predictedArrayStorage(walker, types.withoutNullish(expected));
                TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id());
                written = kindWrittenOrSeenByLinkInListReadsInLoops(types, written, seenByLink);
                bool isNumeric = isNumericStorage(written);
                if (seenByLink && (types.at(seenByLink).isInt32OrNullish() || holdsKeysOfLoopInListReadsInLoops(walker, findStepValue(index, operation), seenByLink)))
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                else if (isNumeric && prefersInt32Storage(findStepValue(index, operation))) {
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                    found.lists.last().hasAlternateStorage = true;
                    found.lists.last().alternateStorage = ArrayStorage::Double;
                    walker.noteInt32Version();
                } else
                    found.lists.append(HoistedArrayGuard { name, isNumeric ? ArrayStorage::Double : written, isNumeric && !(walker.walksOtherKindOf(inputHead) && isPushed(findStepValue(index, operation))) });
                found.lists.last().subclass = stepped->subclass;
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
                if (walker.canUseIterationGuards(inputHead))
                    noteElementReturnedByCopy(operation, copied);
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
                    if (TypeID declared = written == ArrayStorage::Other ? writtenTypeOfListReadByCallee(walker, reader, read) : TypeID())
                        written = predictedArrayStorage(walker, types.withoutNullish(declared));
                    written = kindWrittenOrSeenByLinkInListReadsInLoops(types, written, seenByLink);
                    if (written == ArrayStorage::Other)
                        return;
                    bool isNumeric = isNumericStorage(written);
                    if (isNumeric && !index.loop(loop).parent && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected() && readsCarriedKey(walker, walker.facts().bodies().at(reader), read)) {
                        found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                        found.lists.last().hasAlternateStorage = true;
                        found.lists.last().alternateStorage = ArrayStorage::Double;
                        walker.noteInt32Version();
                        walker.count(StatisticID::InlineeArrayGuardsHoisted);
                        return;
                    }
                    if (!isNumeric && key && typeOfKeyInLook(key) != typeInLook(key))
                        return;
                    if (key && checksCounterBeforeLoop(key) && !listsReachedUnderCountersAskedInFront.contains(name))
                        listsReachedUnderCountersAskedInFront.append(name);
                    found.lists.append(HoistedArrayGuard { name, isNumeric ? ArrayStorage::Double : written, isNumeric });
                    if (readerHasLoops)
                        walker.count(StatisticID::InlineeArrayGuardsHoisted);
                });
                forEachFieldAccessInCopy(walker, operation, copied, Opcode::ElemGet, [&](ValueID holder, PropertyKeyID field, ValueID, BodyValue read) {
                    Reference name = valueOfFieldInPreheader(walker, nameThatEntersLoopInListReadsInLoops(walker, index, holder, firstValueOfLoop), field);
                    if (!name || !name.isValue() || isFreshAllocation(walker, name) || isAlreadyGuardedForReads(name))
                        return;
                    TypeID list = types.withoutNullish(walker.ownTypeOf(name));
                    if (!hasKind(types, list, OperandKind::Array) || tryReuseForElementAccess(name))
                        return;
                    ArrayStorage written = kindWrittenOrSeenByLinkInListReadsInLoops(types, predictedArrayStorage(walker, list), walker.facts().linked().resultOfReadByKey(copied, read.id()));
                    if (isNumericStorage(written))
                        found.lists.append(HoistedArrayGuard { name, ArrayStorage::Double, true });
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
                    TypeID proven = walker.ownTypeOf(name);
                    for (const HoistedSpeculation& guess : guesses) {
                        if (guess.value == name)
                            proven = types.intersectionOf(proven, guess.tested);
                    }
                    TypeID list = types.withoutNullish(proven);
                    if (!hasKind(types, list, OperandKind::Array) || (key && !readTakesIndexOfProvenList(types, list, typeOfKeyInLook(key))))
                        return;
                    if (key && checksCounterBeforeLoop(key) && isBornEmptyInListReadsInLoops(walker, index, input, name))
                        return;
                    if (key && checksCounterBeforeLoop(key) && !listsReachedUnderCountersAskedInFront.contains(name))
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
                    bool holdsInt32s = listed.storage == ArrayStorage::Int32 && !listed.admitsInt32Storage && (!listed.isWritable || listed.hasAlternateStorage);
                    if (holdsInt32s && index.loop(loop).numberOfLoopsWithin == 1 && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected() && storesInt32InCopyOfCallee(walker, writer, walker.facts().bodies().at(writer).value(store).valueOperand())) {
                        if (listed.isWritable)
                            return;
                        listed.isWritable = true;
                        listed.hasAlternateStorage = true;
                        listed.alternateStorage = ArrayStorage::Double;
                        walker.noteInt32Version();
                        walker.count(StatisticID::InlineeInt32ArraysWritten);
                        return;
                    }
                    if (!isNumericStorage(listed.storage) || (!listed.isWritable && listed.storage != ArrayStorage::Double))
                        return;
                    listed.storage = ArrayStorage::Double;
                    listed.admitsInt32Storage = false;
                    listed.isWritable = true;
                    listed.hasAlternateStorage = false;
                    arraysNeedingExactStorage.append(name);
                });
                forEachFieldAccessInCopy(walker, operation, copied, Opcode::ElemSet, [&](ValueID holder, PropertyKeyID field, ValueID key, BodyValue store) {
                    Reference name = valueOfFieldInPreheader(walker, nameThatEntersLoopInListReadsInLoops(walker, index, holder, firstValueOfLoop), field);
                    if (!name || !name.isValue() || isFreshAllocation(walker, name))
                        return;
                    TypeID list = types.withoutNullish(walker.ownTypeOf(name));
                    if (!hasKind(types, list, OperandKind::Array) || !isNumericStorage(predictedArrayStorage(walker, list)) || !storesNumberInCopyOfCalleeInListReadsInLoops(walker, copied, store.valueOperand(), 3, field))
                        return;
                    if (key && (checksCounterBeforeLoop(key) || !readTakesIndexOfProvenList(types, list, typeOfKeyInLook(key))))
                        return;
                    size_t place = found.lists.findIf([&](const HoistedArrayGuard& listed) { return listed.name == name; });
                    if (place == notFound)
                        found.lists.append(HoistedArrayGuard { name, ArrayStorage::Double, false, true });
                    else if (HoistedArrayGuard& listed = found.lists[place]; isNumericStorage(listed.storage) && (listed.isWritable || listed.storage == ArrayStorage::Double)) {
                        listed.storage = ArrayStorage::Double;
                        listed.admitsInt32Storage = false;
                        listed.isWritable = true;
                    } else
                        return;
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
            if (input.index() >= firstValueOfLoop && !nameReadInTurn && !asksPlainVector && (operation.opcode() == Opcode::ElemGet || isStore || readsLength) && guardChosenArrays(operation, isStore, readsLength))
                continue;
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
            bool storesValueOfAnyKind = false;
            bool expectsInvariantNumber = false;
            bool storesBeyondInt32 = false;
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
                else if (movesElement && isWholeNumberBehindListsFound(operation.valueOperand()))
                    stored = TypeTable::int32;
                bool storesNumber = stored && !isNeverType(stored) && types.at(stored).isNumber();
                if (storesNumber && (types.at(stored).primitives & Primitive::FractionalNumber) && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected() && walker.canUseIterationGuards(inputHead) && holdsIntegersByLink(name)) {
                    stored = TypeTable::int32;
                    walker.count(StatisticID::IntegerTablesStayIntegers);
                }
                bool mayBeNumber = !stored || isNeverType(stored) || types.overlap(stored, TypeTable::number);
                if (!storesNumber && isListed && mayBeNumber && index.loop(loop).numberOfLoopsWithin == 1 && isNumericStorage(found.lists[placeInList].storage) && walker.canUseIterationGuards(inputHead)) {
                    HoistedArrayGuard& list = found.lists[placeInList];
                    if (list.storage == ArrayStorage::Double && list.admitsInt32Storage) {
                        list.isWritable = true;
                        continue;
                    }
                    stored = list.storage == ArrayStorage::Int32 ? TypeTable::int32 : TypeTable::number;
                    storesNumber = true;
                }
                laterStoreMayExit |= storesNumber && loopStoresIntoLists && !canStoreWithoutConversion(types, ArrayStorage::Double, stored);
                loopStoresIntoLists = true;
                bool isReadForBoxedValues = isListed && found.lists[placeInList].storage == ArrayStorage::Contiguous && !found.lists[placeInList].admitsInt32Storage;
                bool storesInvariant = !storesNumber && storesInvariantValue(operation);
                storesValueOfAnyKind = !storesNumber && !isListed && mayBeNumber && index.loop(loop).numberOfLoopsWithin == 1 && walker.canUseIterationGuards(inputHead) && (storesInvariant || !walker.walksCopyForAnotherKindOfStorage()) && !textOfLoopReadsList(name);
                if (isListed && found.lists[placeInList].isWritable && found.lists[placeInList].storage == ArrayStorage::Other)
                    continue;
                if (!storesNumber && !storesValueOfAnyKind && (!stored || isNeverType(stored) || (!isReadForBoxedValues && predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(name))) != ArrayStorage::Contiguous)))
                    continue;
                if (std::optional<uint32_t> madeWith = lengthOfListMadeInSight(walker, name); madeWith && !*madeWith && !findKnown(walker, KnownNote::LoopAppendsToLiteral, name, { }, inputHead.index()) && !(isUntouchedLiteral(index, operation.objectOperand(), index.loopOf(member)) && storesUnderSteppedCounter(walker, index, operation)) && !storesFromInitialLength(walker, index, operation) && !findUpdatedRead(index, operation))
                    continue;
                bool takesDoublesInCopyForThem = storesNumber && walker.walksCopyForAnotherKindOfStorage() && !types.isSubset(stored, TypeTable::int32);
                expectsInvariantNumber = storesValueOfAnyKind && storesInvariant && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected();
                storesValueOfAnyKind &= !expectsInvariantNumber;
                bool storesSum = storesNumber && !types.isSubset(stored, TypeTable::int32) && storesSumOfMixedBits(operation);
                if (storesSum)
                    walker.count(StatisticID::ArraysOfMixedSumsHoldDoubles);
                kindStoredInto = expectsInvariantNumber ? ArrayStorage::Double : storesValueOfAnyKind ? ArrayStorage::Other : !storesNumber ? ArrayStorage::Contiguous : (types.at(stored).primitives & Primitive::FractionalNumber) || takesDoublesInCopyForThem || storesSum ? ArrayStorage::Double : ArrayStorage::Int32;
                if (storesNumber && !types.isSubset(stored, TypeTable::int32)) {
                    storesBeyondInt32 = true;
                    bool isWrittenToHoldNumbersAlone = isNumericStorage(predictedArrayStorage(walker, types.withoutNullish(walker.ownTypeOf(name))));
                    (kindStoredInto == ArrayStorage::Double && !isWrittenToHoldNumbersAlone && !walker.walksCopyForAnotherKindOfStorage() ? listsThatTakeMoreThanInt32s : arraysNeedingExactStorage).append(name);
                }
                if (storesNumber && ((isAllocatedAsNumeric && allocationBeforeLoop->arrayStorage() == ArrayStorage::Double) || isFilledWithDoubles(walker, index, operation.objectOperand()))) {
                    kindStoredInto = ArrayStorage::Double;
                    arraysNeedingExactStorage.append(name);
                }
                if (movesElement || expectsInvariantNumber)
                    arraysNeedingExactStorage.append(name);
                if (storesNumber && walker.lastStoreByKeyOfLoopInCurrentBody(index.loopOf(member)) > operation.id().index() && walker.code().at(underlyingCellOf(walker.code(), name.id())).opcode == Opcode::NewArray && !isBornBoxed(walker, name))
                    arraysNeedingExactStorage.append(name);
            }
            bool isMovedElementList = isStore && isListed && found.lists[placeInList].isWritable && found.lists[placeInList].keepsDoublesWithoutAlternate && kindStoredInto != ArrayStorage::Contiguous;
            bool storesInt32s = kindStoredInto == ArrayStorage::Int32;
            bool isStepped = kindStoredInto == ArrayStorage::Double && isListed && !isMovedElementList && index.loop(loop).numberOfLoopsWithin == 1 && mayStayInt32Storage(found.lists[placeInList]) && storesStepOfElement(operation, name);
            bool keepsWholeNumbers = isStepped && versionTakesInt32s;
            bool isWritableByStorage = isListed && found.lists[placeInList].isWritable && found.lists[placeInList].storage == ArrayStorage::Double && found.lists[placeInList].admitsInt32Storage;
            if (isStore && isListed && !isMovedElementList && kindStoredInto == ArrayStorage::Int32 && (isWritableByStorage || (!found.lists[placeInList].isWritable && mayStayInt32Storage(found.lists[placeInList]))) && index.loop(loop).numberOfLoopsWithin == 1 && isReadForBitsAlone(name)) {
                if (isWritableByStorage)
                    walker.count(StatisticID::BitStoresNearStores);
                if (isWritableByStorage && storesBeyondInt32)
                    found.lists[placeInList].admitsInt32Storage = false;
                found.lists[placeInList].isWritable = true;
                found.lists[placeInList].hasAlternateStorage = false;
                if (!storesBeyondInt32 && findUpdatedRead(index, operation) && isAllocatedEmpty(walker, index, input, name)) {
                    found.lists[placeInList].storage = ArrayStorage::Int32;
                    found.lists[placeInList].admitsInt32Storage = false;
                }
                continue;
            }
            if (isStore && isListed && !isMovedElementList && kindStoredInto == ArrayStorage::Int32 && !found.lists[placeInList].isWritable && mayStayInt32Storage(found.lists[placeInList]) && index.loop(loop).numberOfLoopsWithin == 1) {
                keepsWholeNumbers = versionTakesInt32s;
                if (!versionTakesInt32s)
                    kindStoredInto = ArrayStorage::Double;
            }
            if (keepsWholeNumbers)
                kindStoredInto = ArrayStorage::Int32;
            if (isMovedElementList)
                arraysNeedingExactStorage.append(name);
            if (isStore && isListed && !isMovedElementList) {
                HoistedArrayGuard& list = found.lists[placeInList];
                bool isOfEitherKind = list.storage == ArrayStorage::Double && list.admitsInt32Storage;
                if (list.storage == ArrayStorage::Contiguous && !list.admitsInt32Storage && kindStoredInto == ArrayStorage::Int32 && walker.wasAskedForStorageOfNumbersInVain(name)) {
                    list.isWritable = true;
                    continue;
                }
                if (list.isWritable && list.storage != kindStoredInto && (list.storage == ArrayStorage::Contiguous || kindStoredInto == ArrayStorage::Contiguous))
                    continue;
                if (list.isWritable && list.storage != kindStoredInto)
                    kindStoredInto = ArrayStorage::Double;
                else if (!list.isWritable && !isOfEitherKind && list.storage != kindStoredInto)
                    continue;
                list.storage = kindStoredInto;
                list.admitsInt32Storage = false;
                list.isWritable = true;
                if (kindStoredInto == ArrayStorage::Double && list.alternateStorage == ArrayStorage::Double)
                    list.hasAlternateStorage = false;
                list.isStepped = isStepped;
                if (keepsWholeNumbers) {
                    list.hasAlternateStorage = true;
                    list.alternateStorage = ArrayStorage::Double;
                    list.keepsStorageWithoutAlternate = storesInt32s || list.expectedStorage == ArrayStorage::Int32;
                    walker.noteInt32Version();
                }
                if (movesElement)
                    setMovedElementStorage(list);
            }
            if (isListed && operation.opcode() == Opcode::ElemGet && listsOfUnknownContents.contains(name))
                readsOffListsOfUnknownContents.append({ operation.id(), name });
            if (isListed) {
                if (TypeID ownKey = operation.opcode() == Opcode::ElemGet ? typeInLook(operation.propertyOperand()) : TypeID(); ownKey && types.at(ownKey).isInteger())
                    listsTakenUnderCountersAskedInFront.removeAll(name);
                found.lists[placeInList].isAskedForItsStorageAlone &= !readsLength;
                if (readsLength)
                    noteThatComesOfLengthOrRemainder(operation);
                bool readsNumericList = operation.opcode() == Opcode::ElemGet && isNumericStorage(found.lists[placeInList].storage);
                TypeID key = readsNumericList ? typeOfKeyInLook(operation.propertyOperand()) : TypeID();
                if (operation.opcode() == Opcode::ElemGet && walker.wasAskedForStorageOfNumbersInVain(name) && walker.canUseIterationGuards(inputHead) && findTruncatedUse(walker, operation.id()) != TruncatedUse::None)
                    noteNumberBehindListsFound(operation, false);
                if (readsNumericList && ((key && (types.at(key).isInteger() || (types.at(key).isNumber() && walker.canUseIterationGuards(inputHead)))) || isKeyFoundByName(operation)))
                    noteNumberBehindListsFound(operation, found.lists[placeInList].storage == ArrayStorage::Int32);
                continue;
            }

            TypeID proven = walker.ownTypeOf(name);
            if (TypeID behindDeadZone = types.at(proven).primitives & Primitive::Absent ? types.withoutPrimitives(proven, Primitive::Absent) : TypeID(); behindDeadZone && isProvenToBeList(types, types.withoutNullish(behindDeadZone)))
                proven = behindDeadZone;
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
            if (isArraySubclassInstance(walker, types.withoutNullish(proven)) && !(walker.program().someClassExtendsArray && arraySubclassWithElements(walker, proven) && !isOnlyAccessedByKey(index, operation))) {
                TypeID key = operation.opcode() == Opcode::ElemGet ? typeOfKeyInLook(operation.propertyOperand()) : TypeID();
                bool takesRow = key && types.at(key).isInteger() && isOnlyAccessedByKey(index, operation) && (walker.canUseIterationGuards(inputHead) || isRowKeyInvariant(index, operation.propertyOperand(), firstValueOfLoop, 0));
                if (takesRow && !isFreshAllocation(walker, name) && !findNoteInListReadsInLoops(walker, name))
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Contiguous });
                continue;
            }
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
            if (operation.opcode() == Opcode::ElemGet && !dominatesBackEdges && walker.hoistsConditionalArrays() && !hasKind(types, list, OperandKind::Array)) {
                TypeID expected = findExpectation(walker, input).type;
                expected = expected && types.overlap(proven, expected) ? types.withoutNullish(expected) : TypeID();
                if (expected && !isNeverType(expected) && hasKind(types, expected, OperandKind::Array))
                    list = expected;
            }
            TypeID keyInLook = asksPlainVector ? typeInLook(operation.positionOperand()) : typeOfKeyInLook(operation.propertyOperand());
            bool keyIsCounterAskedInFront = !asksPlainVector && keyInLook != typeInLook(operation.propertyOperand()) && !isRoundedKey(operation.propertyOperand()) && !isSteppedPosition(operation.propertyOperand()) && !isInnerLoopCounter(operation.propertyOperand()) && !hasGuessedAddend(operation.propertyOperand());
            ValueID keyAsWritten = asksPlainVector ? ValueID() : index.stripCasts(findUpdatedKey(walker, index, operation.propertyOperand()));
            if (keyAsWritten && keyInLook && !types.at(keyInLook).isNumber() && (isWholeNumberBehindListsFound(operation.propertyOperand()) || isWholeNumberBehindListsFound(keyAsWritten)))
                keyInLook = TypeTable::int32;
            if (!asksPlainVector && !(keyInLook && types.at(keyInLook).isInteger()) && (isKeyFoundByName(operation) || isElementOfViewOfIntegers(keyAsWritten)))
                keyInLook = TypeTable::int32;
            bool keyIsNoted = operation.opcode() == Opcode::ElemGet && !(keyInLook && types.at(keyInLook).isInteger()) && (nothingElseIsGuessedInFront ? isWholeNumberThatKeyOfReadTakes(operation.propertyOperand()) : comesOfLengthOrRemainder(operation.propertyOperand()));
            someReadHasNotedKey |= keyIsNoted;
            bool keyTakesNote = keysOfReadsTakeNotes && keyIsNoted;
            if (keyTakesNote)
                keyInLook = TypeTable::int32;
            bool isIterationKey = keyAsWritten && keyAsWritten.index() >= firstValueOfLoop && !index.isParameter(keyAsWritten);
            bool keyIsCheckedInIteration = isIterationKey && keyOfProvenListMayBeAskedForInt32(types, list, keyInLook) && (operation.opcode() == Opcode::ElemGet || walker.canUseIterationGuards(inputHead));
            Reference keyBeforeLoop = keyAsWritten && keyAsWritten.index() < firstValueOfLoop && walker.canUseIterationGuards(inputHead) ? walker.referenceOf(keyAsWritten) : Reference();
            bool isKeyGuardedInPreheader = keyBeforeLoop && keyBeforeLoop.isValue() && !keyBeforeLoop.isConstant() && keyInLook && !types.at(keyInLook).isNumber() && !isElementCalled(index, operation) && keyOfProvenListMayBeAskedForInt32(types, list, keyInLook);
            if (isKeyGuardedInPreheader && !preheaderKeys.contains(keyBeforeLoop))
                preheaderKeys.append(keyBeforeLoop);
            if (!keyIsCheckedInIteration && !isKeyGuardedInPreheader && !readTakesIndexOfProvenList(types, list, keyInLook))
                continue;
            if (keyIsCounterAskedInFront && operation.opcode() == Opcode::ElemSet && isBornEmptyInListReadsInLoops(walker, index, input, name))
                continue;
            if (keyIsCounterAskedInFront && !listsReachedUnderCountersAskedInFront.contains(name))
                listsReachedUnderCountersAskedInFront.append(name);
            if (isFreshAllocation(walker, name))
                continue;
            if ((asksPlainVector ? !!findNoteInListReadsInLoops(walker, name) : isAlreadyGuardedForReads(name)) && !walker.isArrayGuardedAtHeader(name) && !(operation.opcode() == Opcode::ElemGet && mayNarrowGuard(operation.id(), name)))
                continue;
            if (listsThatCopiesOfCalleesStoreIntoUncounted.contains(name)) {
                walker.count(StatisticID::InlineeStoredArraysNotGuarded);
                continue;
            }
            someListIsTakenForNotedKey |= keyTakesNote;

            if (isStore) {
                found.lists.append(HoistedArrayGuard { name, kindStoredInto, storesValueOfAnyKind, true });
                found.lists.last().leavesBirthAsItIs = storesValueOfAnyKind;
                static constexpr OpcodeSet visibleAllocators { Opcode::NewArray, Opcode::NewArrayLike, Opcode::Data };
                bool isVisibleAllocation = visibleAllocators.contains(walker.code().at(underlyingCellOf(walker.code(), name.id())).opcode);
                if (kindStoredInto == ArrayStorage::Int32 && !isVisibleAllocation && index.loop(loop).numberOfLoopsWithin == 1 && arraysNeedingExactStorage.contains(name) && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected()) {
                    found.lists.last().hasAlternateStorage = true;
                    found.lists.last().alternateStorage = ArrayStorage::Other;
                    walker.noteInt32Version();
                    walker.count(StatisticID::IntegerArraysRechecked);
                } else if (kindStoredInto == ArrayStorage::Int32 && !isVisibleAllocation && index.loop(loop).numberOfLoopsWithin > 1 && !movesElement && !expectsInvariantNumber && !isReadByKeyInLoop(index, loop, operation)) {
                    found.lists.last() = HoistedArrayGuard { name, ArrayStorage::Double, true, true, true };
                    walker.count(StatisticID::IntegersStoredByKind);
                }
                if (movesElement)
                    setMovedElementStorage(found.lists.last());
                else if (expectsInvariantNumber) {
                    found.lists.last().leavesBirthAsItIs = true;
                    found.lists.last().hasAlternateStorage = true;
                    found.lists.last().alternateStorage = ArrayStorage::Other;
                    found.lists.last().alternateTakesAnyKind = true;
                    walker.noteInt32Version();
                }
                continue;
            }
            ArrayStorage written = predictedArrayStorage(walker, list);
            if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, input).type : TypeID())
                written = predictedArrayStorage(walker, types.withoutNullish(expected));
            if (walker.wasAskedForStorageOfNumbersInVain(name)) {
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Contiguous, false });
                if (walker.canUseIterationGuards(inputHead) && findTruncatedUse(walker, operation.id()) != TruncatedUse::None)
                    noteNumberBehindListsFound(operation, false);
                continue;
            }
            if (asksPlainVector) {
                TypeID taken = written == ArrayStorage::Double || written == ArrayStorage::Other ? typeThatCalleeTakesOfElementInListReadsInLoops(walker, index, loop, typeInLook) : TypeID();
                if (written == ArrayStorage::Double && (!taken || types.isUninformative(taken))) {
                    taken = TypeTable::number;
                    walker.count(StatisticID::NumericCallbackLists);
                }
                bool mayHoldDoubles = written == ArrayStorage::Other && (!taken || types.isUninformative(taken)) && !(walker.isInCopyOfFusedChain() && walker.fusedChainCopiesFields()) && !walker.isInt32VersionRejected() && index.loop(loop).numberOfLoopsWithin == 1;
                if (mayHoldDoubles && walker.walksOtherKindOf(inputHead))
                    taken = TypeTable::number;
                TypeID elements = elementsThatLinkProvesOfReceiverInListReadsInLoops(walker, index, name);
                if (elements && types.at(elements).isNumber() && (!taken || types.isUninformative(taken)))
                    taken = elements;
                bool takesFractions = taken && arrayStorageHolding(types, taken) == ArrayStorage::Double && (types.at(taken).primitives & Primitive::FractionalNumber);
                bool takesWholeNumbers = !takesFractions && elements && !(types.at(elements).primitives & (Primitive::UndefinedOrNull | Primitive::FractionalNumber)) && isNumericStorage(arrayStorageHolding(types, elements));
                TypeID besideNullish = elements ? types.withoutNullish(elements) : taken ? types.withoutNullish(taken) : TypeID();
                bool mayTakeWholeNumbers = !walker.walksCopyForAnotherKindOfStorage() && !takesFractions && !takesWholeNumbers && besideNullish && types.at(besideNullish).isNumberWithoutFraction();
                if (takesWholeNumbers)
                    taken = elements;
                std::optional<GuardedReadInListReadsInLoops> guarded = takesFractions || takesWholeNumbers || mayTakeWholeNumbers ? findGuardedReadInListReadsInLoops(index) : std::nullopt;
                bool goesByRowOfNumbers = guarded && guarded->test == operation.id();
                bool canAcceptAnyNumber = takesFractions && (types.at(taken).primitives & Primitive::NumberWithoutFraction);
                bool acceptsAnyNumber = canAcceptAnyNumber && !walker.isInt32VersionRejected();
                bool asksForInt32sFirst = goesByRowOfNumbers && (mayTakeWholeNumbers || (!walker.walksCopyForAnotherKindOfStorage() && (acceptsAnyNumber || (takesWholeNumbers && arrayStorageHolding(types, taken) != ArrayStorage::Int32))));
                if (goesByRowOfNumbers && (asksForInt32sFirst || (takesWholeNumbers && arrayStorageHolding(types, taken) == ArrayStorage::Int32)))
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false, false, false, true });
                else if (goesByRowOfNumbers)
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Double, !!(types.at(taken).primitives & Primitive::NumberWithoutFraction), false, false, true });
                else
                    found.lists.append(HoistedArrayGuard { name, ArrayStorage::Contiguous, true, false, false, isNumericStorage(written) });
                if (asksForInt32sFirst && acceptsAnyNumber)
                    walker.noteInt32Version();
                else if (goesByRowOfNumbers && canAcceptAnyNumber && !acceptsAnyNumber)
                    walker.count(StatisticID::Int32VersionsRejected);
                found.lists.last().hasAlternateStorage = asksForInt32sFirst;
                found.lists.last().alternateStorage = mayTakeWholeNumbers ? ArrayStorage::Contiguous : ArrayStorage::Double;
                if (mayHoldDoubles && !guarded && !walker.walksCopyForAnotherKindOfStorage()) {
                    std::optional<GuardedReadInListReadsInLoops> plain = findGuardedReadInListReadsInLoops(index);
                    if (plain && plain->test == operation.id()) {
                        found.lists.last().hasAlternateStorage = true;
                        walker.noteInt32Version();
                        walker.count(StatisticID::NumericCallbackLists);
                    }
                }
                continue;
            }
            TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), operation.id());
            written = kindWrittenOrSeenByLinkInListReadsInLoops(types, written, seenByLink);
            if (written == ArrayStorage::Other && (elementIsOnlyReadByKeyInListReadsInLoops(index, operation) || isElementReadByName(walker, operation)))
                written = ArrayStorage::Contiguous;
            bool isNumeric = isNumericStorage(written);
            if (keyIsCounterAskedInFront && written == ArrayStorage::Contiguous && operation.opcode() == Opcode::ElemGet && isElementReadByName(walker, operation))
                objectArraysUnderCheckedCounters.append(name);
            if (keyIsCounterAskedInFront && !isNumeric && written != ArrayStorage::Other && !objectArraysUnderCheckedCounters.contains(name))
                continue;
            if (keyIsCounterAskedInFront && !listsTakenUnderCountersAskedInFront.contains(name))
                listsTakenUnderCountersAskedInFront.append(name);
            bool wholeNumbersAreOnlyStoredOn = seenByLink && types.at(types.withoutNullish(seenByLink)).isNumberWithoutFraction() && onlyUserStoresWhatIsReadIntoListInListReadsInLoops(index, operation);
            bool holdsInt32sByLink = seenByLink && (types.at(seenByLink).isInt32OrNullish() || holdsKeysOfLoopInListReadsInLoops(walker, operation.id(), seenByLink));
            bool mayHoldIndices = !holdsInt32sByLink && (isNumeric || written == ArrayStorage::Other);
            bool hasCarriedIndexes = mayHoldIndices && !walker.walksCopyForAnotherKindOfStorage() && isCarriedIndex(operation.id());
            bool holdsIndices = mayHoldIndices && !hasCarriedIndexes && !textOfLoopStoresIntoList(name) && holdsOnlyIndices(operation, isNumeric);
            if (holdsIndices && (written == ArrayStorage::Double || walker.walksCopyForAnotherKindOfStorage() || index.loop(loop).parent)) {
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Double, true });
                found.lists.last().expectedStorage = written;
                noteNumberBehindListsFound(operation, true);
                continue;
            }
            if (isNumeric && !holdsInt32sByLink && index.loop(loop).numberOfLoopsWithin == 1 && textOfLoopStoresIntoList(name) && isReadForBitsAlone(name)) {
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Double, true });
                found.lists.last().expectedStorage = written;
                noteNumberBehindListsFound(operation, false);
                continue;
            }
            if (holdsInt32sByLink || hasCarriedIndexes || holdsIndices || (wholeNumbersAreOnlyStoredOn && !walker.walksCopyForAnotherKindOfStorage())) {
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                found.lists.last().hasAlternateStorage = !holdsInt32sByLink && !index.loop(index.loopOf(inputHead)).parent;
                found.lists.last().alternateStorage = ArrayStorage::Double;
                found.lists.last().keepsStorageWithoutAlternate = true;
                found.lists.last().holdsIndices = holdsIndices;
                noteNumberBehindListsFound(operation, true);
                continue;
            }
            if (isNumeric && (isTableOfBits(operation) || isSummedModulo(operation)) && !textOfLoopStoresIntoList(name)) {
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                found.lists.last().hasAlternateStorage = true;
                found.lists.last().alternateStorage = ArrayStorage::Double;
                found.lists.last().prefersInt32s = true;
                walker.noteInt32Version();
                noteNumberBehindListsFound(operation, true);
                continue;
            }
            bool holdsNumbersByLink = seenByLink && types.at(seenByLink).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent);
            if (isNumeric && prefersInt32Storage(operation.id()) && (holdsNumbersByLink || elementsPushed.contains(operation.id()) || isSwitchedOn(operation.id()))) {
                found.lists.append(HoistedArrayGuard { name, ArrayStorage::Int32, false });
                found.lists.last().hasAlternateStorage = true;
                found.lists.last().alternateStorage = ArrayStorage::Double;
                walker.noteInt32Version();
                noteNumberBehindListsFound(operation, true);
                continue;
            }
            bool checksDoublesFirst = isNumeric && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected() && index.loop(loop).numberOfLoopsWithin > 1 && walker.hasUnrolledInnerLoops(inputHead);
            bool holdsNumbersAndOthers = written == ArrayStorage::Contiguous && seenByLink && types.overlap(seenByLink, TypeTable::number) && !textOfLoopStoresIntoList(name);
            if (holdsNumbersAndOthers)
                arraysOfEitherBoxedStorage.append(name);
            bool takesBoxedNumbers = isNumeric && !checksDoublesFirst && (walker.walksLoopOverBoxedNumbers(inputHead) || walker.walksCopyOverBoxedList());
            if (takesBoxedNumbers)
                arraysOfEitherBoxedStorage.append(name);
            found.lists.append(HoistedArrayGuard { name, takesBoxedNumbers ? ArrayStorage::Contiguous : isNumeric ? ArrayStorage::Double : written, holdsNumbersAndOthers || (isNumeric && !checksDoublesFirst && !(walker.walksOtherKindOf(inputHead) && isPushed(operation.id()))) });
            found.lists.last().expectedStorage = written;
            found.lists.last().holdsIntegersByLink = isNumeric && !takesBoxedNumbers && !checksDoublesFirst && seenByLink && types.at(types.withoutNullish(seenByLink)).isNumberWithoutFraction();
            if (isNumeric && seenByLink && types.at(seenByLink).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent) && !checksDoublesFirst && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected() && index.loop(loop).numberOfLoopsWithin == 1 && !keyIsCounterAskedInFront) {
                found.lists.last().hasAlternateStorage = true;
                found.lists.last().alternateStorage = ArrayStorage::Contiguous;
                found.lists.last().keepsStorageWithoutAlternate = true;
                found.lists.last().isNextWalkedForBoxedNumbers = true;
                walker.noteInt32Version();
            }
            if (checksDoublesFirst) {
                found.lists.last().hasAlternateStorage = true;
                found.lists.last().alternateStorage = ArrayStorage::Double;
                walker.noteInt32Version();
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
    checksElementsAlongsideElements = true;
    for (BlockID member : readsOffListsOfUnknownContents.isEmpty() ? std::span<const BlockID>() : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!allEqualities.contains(operation.opcode()))
                continue;
            while (expectNumbersWhereElementIsTakenForNumber(operation))
                walker.count(StatisticID::ElementsCheckedAlongsideElements);
        }
    }
    if (someListIsTakenForNotedKey && !rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), guesses.span(), { }, entering))
        return findListsReadInLoopInListReadsInLoops(walker, inputHead, preview, entering, guesses, false, someReadHasNotedKey, namesOfCountersAskedInFront);
    found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
        return !isNumericStorage(list.storage) && listsTakenUnderCountersAskedInFront.contains(list.name) && !objectArraysUnderCheckedCounters.contains(list.name);
    });
    bool someListIsTakenUnderCounter = found.lists.containsIf([&](const HoistedArrayGuard& list) {
        return listsTakenUnderCountersAskedInFront.contains(list.name);
    });
    if (someListIsTakenUnderCounter && !rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), guesses.span(), { }, entering)) {
        found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
            return listsTakenUnderCountersAskedInFront.contains(list.name);
        });
    }
    for (HoistedArrayGuard& list : found.lists) {
        if (list.isWritable || !isNumericStorage(list.storage) || !listsPoppedFrom.contains(list.name))
            continue;
        list.isWritable = true;
        arraysNeedingExactStorage.append(list.name);
        if (std::exchange(list.admitsInt32Storage, false) && !list.hasAlternateStorage && !walker.walksCopyForAnotherKindOfStorage() && !walker.isInt32VersionRejected()) {
            list.storage = ArrayStorage::Int32;
            list.hasAlternateStorage = true;
            list.alternateStorage = ArrayStorage::Double;
            walker.noteInt32Version();
        }
    }
    found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
        return list.storage == ArrayStorage::Other && arraysReadByCopies.contains(list.name);
    });
    for (const HoistedArrayGuard& list : found.lists) {
        if (someFieldMayHoldLiteral)
            endKnown(walker, KnownNote::LoopAppendsToLiteral, list.name, { }, inputHead.index());
    }
    bool hasOtherKind = found.lists.containsIf([&](const HoistedArrayGuard& list) { return list.hasAlternateStorage && !untouchedLiteralsPushedOnto.contains(list.name); });
    for (HoistedArrayGuard& list : found.lists) {
        if (!hasOtherKind || list.hasAlternateStorage || !list.isWritable || isConvertedInPreheader(list) || !untouchedLiteralsPushedOnto.contains(list.name))
            continue;
        list.hasAlternateStorage = true;
        list.alternateStorage = ArrayStorage::Double;
        list.keepsDoublesWithoutAlternate = true;
    }
    if (preview.effects.mayRunOtherCode() && std::ranges::all_of(found.lists, [&](const HoistedArrayGuard& list) { return untouchedLiteralsPushedOnto.contains(list.name); }))
        found.lists.shrink(0);
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
    bool storesInt32sFirst = found.lists.containsIf([](const HoistedArrayGuard& list) {
        return list.isWritable && list.storage == ArrayStorage::Int32 && list.hasAlternateStorage;
    });
    for (HoistedArrayGuard& list : found.lists) {
        if (!storesInt32sFirst || !list.holdsIntegersByLink || list.isWritable || list.storage != ArrayStorage::Double || !list.admitsInt32Storage)
            continue;
        list.storage = ArrayStorage::Int32;
        list.admitsInt32Storage = false;
        list.hasAlternateStorage = true;
        list.alternateStorage = ArrayStorage::Double;
        list.isNextWalkedForBoxedNumbers = false;
        walker.count(StatisticID::IntegerArraysRechecked);
    }
    auto isTermOfTruncatedSum = [&](ValueID read) {
        static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
        ValueID user = index.onlyUsingValue(read);
        for (bool isOfSum : { false, true }) {
            while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
                user = index.onlyUsingValue(user);
            if (user && !isOfSum && sums.contains(index.opcodeOf(user)))
                user = index.onlyUsingValue(user);
        }
        return user && allBitwiseOperators.contains(index.opcodeOf(user));
    };
    auto isUsedOnlyInTruncatedSums = [&](Reference list) {
        return std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::all_of(index.block(member).values(), [&](BodyValue read) {
                if (read.opcode() != Opcode::ElemGet)
                    return true;
                ValueID input = valueThatEntersLoopInListReadsInLoops(index, read.objectOperand(), firstValueOfLoop);
                Reference name = !input ? Reference() : input.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, input, firstValueOfLoop) : walker.referenceOf(input);
                return name != list || isTermOfTruncatedSum(read.id());
            });
        });
    };
    bool readsIndicesFirst = !walker.walksCopyForAnotherKindOfStorage() && found.lists.containsIf([](const HoistedArrayGuard& list) {
        return list.holdsIndices && list.storage == ArrayStorage::Int32 && list.hasAlternateStorage;
    });
    for (HoistedArrayGuard& list : found.lists) {
        if (!readsIndicesFirst || list.isWritable || list.storage != ArrayStorage::Double || !list.admitsInt32Storage || list.hasAlternateStorage || list.written || !isUsedOnlyInTruncatedSums(list.name))
            continue;
        list.storage = ArrayStorage::Int32;
        list.admitsInt32Storage = false;
        list.hasAlternateStorage = true;
        list.alternateStorage = ArrayStorage::Double;
        walker.count(StatisticID::IntegerArraysRechecked);
    }
    auto accessesRowOfGuardedArray = [&](BodyValue access, Opcode opcode) {
        ValueID row = access.opcode() == opcode ? index.stripCasts(access.objectOperand()) : ValueID();
        if (!row || row.index() < firstValueOfLoop || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet || walker.findHoistedRow(row))
            return false;
        Reference holder = nameThatEntersLoopInListReadsInLoops(walker, index, index.value(row).objectOperand(), firstValueOfLoop);
        return holder && foundArrayLookup.find(found.lists.span(), holder) != notFound;
    };
    auto turnsTakeRows = [&](Opcode opcode) {
        return index.opcodes().contains(opcode) && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::any_of(index.block(member).values(), [&](BodyValue access) { return accessesRowOfGuardedArray(access, opcode); });
        });
    };
    bool storesIntoHoistedArrays = found.lists.containsIf([](const HoistedArrayGuard& list) { return list.isWritable; });
    bool turnsStoreIntoLists = storesIntoHoistedArrays || turnsTakeRows(Opcode::ElemSet);
    bool turnsReadRows = storesIntoHoistedArrays && turnsTakeRows(Opcode::ElemGet);
    bool holdsOnlyDoubles = found.lists.containsIf([](const HoistedArrayGuard& list) {
        return !list.isWritable && list.hasAlternateStorage && list.storage == ArrayStorage::Double && list.alternateStorage == ArrayStorage::Double;
    });
    for (HoistedArrayGuard& list : found.lists) {
        list.turnsStoreIntoLists = turnsStoreIntoLists;
        list.leavesBirthAsItIs |= walker.walksCopyForAnotherKindOfStorage() && !list.isStepped;
        if (turnsStoreIntoLists && std::exchange(list.isNextWalkedForBoxedNumbers, false)) {
            list.hasAlternateStorage = list.alternateStorage != ArrayStorage::Contiguous;
            list.keepsStorageWithoutAlternate = false;
        }
        if (list.isWritable && isNumericStorage(list.storage) && !turnsReadRows && !arraysNeedingExactStorage.contains(list.name)) {
            if (holdsOnlyDoubles && !laterStoreMayExit && list.storage == ArrayStorage::Double && listsThatTakeMoreThanInt32s.contains(list.name)) {
                list.hasAlternateStorage = true;
                list.alternateStorage = ArrayStorage::Other;
                continue;
            }
            list.storage = laterStoreMayExit ? ArrayStorage::Double : ArrayStorage::Other;
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
        if (!rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), { }, { }, entering))
            return { };
        found.areLeftAsTheyAre = true;
    }
    for (BlockID member : index.membersOf(loop)) {
        if (!backEdgeDominators.contains(member))
            continue;
        for (BodyValue operation : index.block(member).values()) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                markUsedEveryIteration(nameThatEntersLoopInListReadsInLoops(walker, index, operand, firstValueOfLoop));
            });
        }
    }
    SpeculationLookupByName speculationLookup;
    bool didSortNames = namesUsedEveryIteration.size() > ArrayLookupByName::linearSearchLimit;
    if (didSortNames)
        std::ranges::sort(namesUsedEveryIteration);
    bool turnsCall = index.opcodes().contains(Opcode::Call) && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [](BodyValue operation) { return operation.opcode() == Opcode::Call; });
    });
    auto isUsedInSomeIterationsOnly = [&](const HoistedArrayGuard& list) {
        bool isHoistedRow = std::ranges::any_of(walker.hoistedRowsOf(inputHead), [&](const Walker::HoistedRow& row) { return !row.isRejected && row.row == list.name; });
        bool needsOnlyStorageCheck = isHoistedRow || (!list.isWritable && walker.types().isSubset(walker.typeOf(list.name), walker.types().builtinClass(BuiltinClasses::Array)));
        TypeID ownType = types.withoutPrimitives(types.withoutNullish(walker.ownTypeOf(list.name)), turnsCall ? 0 : Primitive::Absent);
        bool isTypedAsArray = hasKind(types, ownType, OperandKind::Array) && (list.isWritable || !mayParameterBeMissing(walker, list.name));
        bool hasSpeculation = speculationLookup.find(guesses.span(), list.name) != notFound;
        bool isUsedEveryIteration = didSortNames ? std::ranges::binary_search(namesUsedEveryIteration, list.name.id().index()) : namesUsedEveryIteration.contains(list.name.id().index());
        return !isUsedEveryIteration && !needsOnlyStorageCheck && !hasSpeculation && !isTypedAsArray && !chosenArrays.contains(list.name);
    };
    auto isTestedInLoop = [&](Reference name) {
        static constexpr OpcodeSet tests = allEqualities | OpcodeSet { Opcode::ToBoolean, Opcode::TypeTest, Opcode::TypeOf };
        return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
                bool testsName = false;
                if (tests.contains(operation.opcode()))
                    operation.forEachOperand([&](ValueID operand, uint32_t) { testsName |= nameThatEntersLoopInListReadsInLoops(walker, index, operand, firstValueOfLoop) == name; });
                return testsName;
            });
        });
    };
    if (found.lists.containsIf(isUsedInSomeIterationsOnly)) {
        BitVector usedOnEveryPath = findWhatEveryPathUses(index, inputHead, found.lists.size(), [&](BodyBlock block, BitVector& used) {
            for (BodyValue operation : block.values()) {
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    Reference name = nameThatEntersLoopInListReadsInLoops(walker, index, operand, firstValueOfLoop);
                    if (size_t place = name ? foundArrayLookup.find(found.lists.span(), name) : notFound; place != notFound)
                        used.set(place);
                });
            }
        });
        uint32_t place = 0;
        found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
            bool isUsedInEveryArm = usedOnEveryPath.get(place++);
            if (!isUsedInSomeIterationsOnly(list))
                return false;
            if (isUsedInEveryArm)
                walker.count(StatisticID::ListsUsedInEveryArm);
            if (!isUsedInEveryArm && walker.hoistsConditionalArrays() && !list.isWritable && !isTestedInLoop(list.name)) {
                found.hasConditionalArray = true;
                return false;
            }
            return !isUsedInEveryArm;
        });
    }
    if (rejectMutableRowHolders(walker, index, inputHead, found, typeInLook))
        return findListsReadInLoopInListReadsInLoops(walker, inputHead, preview, entering, guesses, keysOfReadsTakeNotes, someReadHasNotedKey, namesOfCountersAskedInFront);
    if (found.lists.isEmpty())
        return { };

    NamesBeforeLoop namesBeforeLoop(walker, inputHead);
    for (const HoistedArrayGuard& list : found.lists) {
        bool hasSpeculation = speculationLookup.find(guesses.span(), list.name) != notFound;
        if (hasSpeculation || isProvenArray(walker, list.name) || isInstanceOfArraySubclass(walker, list.name) || classOfProvenViewInListReadsInLoops(walker, list.name))
            continue;
        TypeID ownType = types.withoutNullish(walker.ownTypeOf(list.name));
        BuiltinClassID classOfView = classOfViewInListReadsInLoops(walker, ownType);
        TypeID asked = kindsOfViewsOfNumbersIn(walker.program(), types, ownType) ? ownType : types.builtinClass(classOfView ? classOfView : BuiltinClassID(BuiltinClasses::Array));
        guesses.append(HoistedSpeculation { list.name, asked, { }, namesBeforeLoop.findNamesOf(list.name) });
    }
    for (Reference key : preheaderKeys) {
        if (speculationLookup.find(guesses.span(), key) == notFound)
            guesses.append(HoistedSpeculation { key, TypeTable::int32, { }, namesBeforeLoop.findNamesOf(key) });
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
    LayoutID subclass = tested.subclass ? tested.subclass : subclassOfArrayName(walker, tested.name);
    if (!tested.onlyHoldsRows && !passes(subclass ? draftSubclassElementsTest(tested.name, subclass, origin) : questionWhetherListIsAsMadeInListReadsInLoops(walker, tested.name, origin)))
        return false;
    return (!hasArrayStorageForm(tested.storage) && !tested.isWritable) || passes(questionForKindOfStorageInListReadsInLoops(walker, tested, origin));
}

static bool isFoundByReadAtSiteInListReadsInLoops(Walker& walker, Reference list)
{
    const Code& code = walker.code();
    ValueID held = list.id();
    while (valuePreservingOpcodes.contains(code.at(held).opcode))
        held = code.at(held).operand(0);
    return code.at(held).opcode == Opcode::GetValueAtSiteOrAbsent || code.at(held).opcode == Opcode::GetGlobalProperty || code.at(held).opcode == Opcode::GetKeyedProperty || code.at(held).opcode == Opcode::GetKeyedValueOrAbsent || code.at(held).opcode == Opcode::GetOwnValueUnderKeyOrAbsent || code.at(held).opcode == Opcode::GetValueUnderKeyOrAbsent || walker.types().isUninformative(code.at(held).type);
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

std::optional<HoistedArrayGuard> tryHoistArrayGuardForCall(Walker& walker, const BodyValue& call, Reference list, TypeID contents)
{
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    if (!walker.has(Mechanism::Folding) || !list || !list.isValue() || !walker.isInvariantInSpecializedVersion(list, purity) || !walker.iterationsPreserveArrayGuards(list, purity))
        return std::nullopt;
    if (!isProvenArray(walker, list) || isFreshAllocation(walker, list))
        return std::nullopt;
    ArrayStorage written = predictedArrayStorage(walker, walker.ownTypeOf(list));
    if (written == ArrayStorage::Other && contents)
        written = arrayStorageHolding(walker.types(), walker.types().withoutNullish(contents));
    if (written == ArrayStorage::Other)
        return std::nullopt;
    bool isNumeric = isNumericStorage(written);
    return tryHoistArrayGuard(walker, HoistedArrayGuard { list, isNumeric ? ArrayStorage::Double : written, isNumeric }, walker.originOf(call), purity, Walker::GuardedIterations::Some);
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

static bool userNeedsNumber(Walker& walker, const BodyValue& read)
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

static bool movesElementIntoRowOfSameHolder(const BodyIndex& index, const BodyValue& read)
{
    ValueID holder = read.opcode() == Opcode::ElemGet && isReadOfBodyInListReadsInLoops(index, read) ? holderOfRowAccess(index, read) : ValueID();
    ValueID moved = read.id();
    ValueID user = holder ? index.onlyUsingValue(moved) : ValueID();
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        moved = user;
        user = index.onlyUsingValue(moved);
    }
    return user && index.opcodeOf(user) == Opcode::ElemSet && index.value(user).valueOperand() == moved && holderOfRowAccess(index, index.value(user)) == holder;
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

static bool callsElementReader(Walker& walker, const BodyValue& call)
{
    FunctionID method = findExpectedMethod(walker, call);
    const BodyIndex* body = method ? walker.facts().bodies().find(method) : nullptr;
    if (!body || body->hasLoops())
        return false;
    bool returnsElement = false;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BodyTerminator end = body->block(BlockID(number)).terminator();
        if (end.kind() != TerminatorKind::Return)
            continue;
        ValueID returned = body->stripCasts(end.operand(0));
        if (!returned || body->isParameter(returned) || body->opcodeOf(returned) != Opcode::ElemGet)
            return false;
        returnsElement = true;
    }
    return returnsElement;
}

static bool isTakenByMathOrAddedToElement(Walker& walker, const BodyIndex& index, const BodyValue& read)
{
    ValueID name = read.id();
    ValueID user = index.onlyUsingValue(name);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        name = user;
        user = index.onlyUsingValue(user);
    }
    if (!user)
        return false;
    BodyValue operation = index.value(user);
    if (isMathCall(walker, index, operation)) {
        walker.count(StatisticID::RowsTakenForNumbers);
        return true;
    }
    if (operation.opcode() != Opcode::Add)
        return false;
    ValueID other = index.stripCasts(operation.operand(operation.operand(0) == name ? 1 : 0));
    if (index.isParameter(other) || (index.opcodeOf(other) != Opcode::ElemGet && !(&index == &walker.currentBodyIndex() && callsElementReader(walker, index.value(other)))))
        return false;
    walker.count(StatisticID::RowsTakenForNumbers);
    return true;
}

static bool computesWithElementsOfList(const BodyIndex& index, const BodyValue& store)
{
    static constexpr OpcodeSet arithmetic { Opcode::Sub, Opcode::Mul, Opcode::Div };
    ValueID list = index.stripCasts(store.objectOperand());
    if (!list || index.isParameter(list))
        return false;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue read : index.block(BlockID(number)).values()) {
            if (read.opcode() != Opcode::ElemGet || index.stripCasts(read.objectOperand()) != list)
                continue;
            ValueID user = index.onlyUsingValue(read.id());
            while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
                user = index.onlyUsingValue(user);
            if (user && arithmetic.contains(index.opcodeOf(user)))
                return true;
        }
    }
    return false;
}

static bool storesArgumentIntoList(Walker& walker, FunctionID method, uint32_t argument)
{
    const BodyIndex* body = method ? walker.facts().bodies().find(method) : nullptr;
    if (!body || body->hasLoops() || argument + 1 >= body->block(entryBlock).numberOfParameters())
        return false;
    ValueID parameter = body->block(entryBlock).parameter(argument + 1);
    ValueID user = body->onlyUsingValue(parameter);
    return user && body->opcodeOf(user) == Opcode::ElemSet && body->value(user).valueOperand() == parameter;
}

static UseByCallers findUseByCallers(Walker& walker, const BodyValue& read)
{
    static constexpr OpcodeSet arithmetic { Opcode::Sub, Opcode::Mul, Opcode::Div };
    const BodyIndex* index = &walker.currentBodyIndex();
    if (!isReadOfBodyInListReadsInLoops(*index, read))
        return UseByCallers::Unknown;
    ValueID value = read.id();
    ValueID call;
    for (size_t outward = 0; index; ++outward) {
        ValueID user = index->onlyUsingValue(value);
        for (; user && opcodesThatStateTypes.contains(index->opcodeOf(user)); user = index->onlyUsingValue(value))
            value = user;
        if (user && (!outward || arithmetic.contains(index->opcodeOf(user))))
            return outward ? UseByCallers::ComputesNumber : UseByCallers::Unknown;
        if (user) {
            BodyValue operation = index->value(user);
            if (operation.opcode() == Opcode::ElemSet && operation.valueOperand() == value)
                return computesWithElementsOfList(*index, operation) ? UseByCallers::StoresElement : UseByCallers::Unknown;
            if (operation.opcode() == Opcode::Call) {
                auto called = operation.as<Opcode::Call>();
                auto arguments = called.argumentsOperands();
                size_t argument = std::ranges::find(arguments, value) - arguments.begin();
                Reference method = argument < arguments.size() ? walker.referenceInCaller(called.calleeOperand(), outward - 1) : Reference();
                return method && storesArgumentIntoList(walker, singleFunctionOf(walker.types(), walker.types().at(walker.typeOf(method))), argument) ? UseByCallers::StoresElement : UseByCallers::Unknown;
            }
            ValueID other = operation.opcode() == Opcode::Add ? operation.operand(operation.operand(0) == value ? 1 : 0) : ValueID();
            Reference built = other && other != value ? walker.referenceInCaller(other, outward - 1) : Reference();
            if (built && walker.types().at(walker.typeOf(built)).isNumber())
                return UseByCallers::ComputesNumber;
            auto methodCalled = [&](ValueID call) {
                ValueID callee = index->stripCasts(index->value(call).as<Opcode::Call>().calleeOperand());
                return !index->isParameter(callee) && index->opcodeOf(callee) == Opcode::PropGet ? index->value(callee).key() : PropertyKeyID();
            };
            ValueID beside = other && other != value ? index->stripCasts(other) : ValueID();
            if (!beside || index->isParameter(beside))
                return UseByCallers::Unknown;
            bool isAddedToElement = index->opcodeOf(beside) == Opcode::ElemGet || (index->opcodeOf(beside) == Opcode::Call && methodCalled(call) && methodCalled(call) == methodCalled(beside));
            return isAddedToElement ? UseByCallers::ComputesNumber : UseByCallers::Unknown;
        }
        BodyTerminator end = index->block(index->blockOf(value)).terminator();
        if (!index->hasOneUse(value) || (end.kind() != TerminatorKind::Return && end.kind() != TerminatorKind::ReturnCall) || end.operand(0) != value)
            return UseByCallers::Unknown;
        std::optional<Walker::CallOfCopy> copied = walker.callOfCopy(outward);
        if (!copied)
            return UseByCallers::Unknown;
        index = copied->index;
        value = call = copied->call;
    }
    return UseByCallers::Unknown;
}

bool callersComputeNumbers(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = isReadOfBodyInListReadsInLoops(index, read) ? index.onlyUsingValue(read.id()) : ValueID();
    if (user && index.opcodeOf(user) == Opcode::Call && callsTableCallees(walker, index.value(user)))
        return true;
    return findUseByCallers(walker, read) != UseByCallers::Unknown;
}

static bool isElementReadByName(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (read.opcode() != Opcode::ElemGet || !isReadOfBodyInListReadsInLoops(index, read))
        return false;
    LoopID loop = index.loopOf(index.blockOf(read.id()));
    if (!loop)
        return false;
    ValueID key = index.stripCasts(read.propertyOperand());
    bool stepsKey = key && index.isInLoop(index.blockOf(key), loop);
    bool isReadByName = false;
    bool isOnlyReadByName = std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::all_of(index.block(member).values(), [&](BodyValue user) {
            if (opcodesThatStateTypes.contains(user.opcode()) || user.opcode() == Opcode::TypeTest)
                return true;
            bool takesElement = false;
            user.forEachOperand([&](ValueID operand, uint32_t) {
                ValueID taken = index.stripCasts(operand);
                ValueID narrowed = taken && index.isParameter(taken) ? index.valueNarrowedInto(taken) : ValueID();
                takesElement |= taken == read.id() || (narrowed && index.stripCasts(narrowed) == read.id());
            });
            if (!takesElement)
                return true;
            isReadByName = true;
            return user.opcode() == Opcode::PropGet && (siteMayAnswerReadWithoutOtherCode(walker, user) || (stepsKey && mayReadLengthInIterations(walker, user)));
        });
    });
    return isReadByName && isOnlyReadByName;
}

static bool mayOnlyTakeRow(Walker& walker, const BodyValue& read)
{
    if (read.opcode() != Opcode::ElemGet || !isReadOfBodyInListReadsInLoops(walker.currentBodyIndex(), read) || !isOnlyAccessedByKey(walker.currentBodyIndex(), read))
        return false;
    Reference key = walker.referenceOf(read.propertyOperand());
    return key && key.isRecord() && walker.types().at(walker.typeOf(key)).isInteger() && (walker.canBuildIterationGuard() || walker.canGuardAfterStores());
}

std::optional<HoistedArrayGuard> tryHoistArrayGuardForAccess(Walker& walker, const BodyValue& read, Reference list, ArrayStorage kindStoredInto, bool goesByKindOfStorage)
{
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    if (!walker.has(Mechanism::Folding) || !list || !list.isValue() || !walker.isInvariantInSpecializedVersion(list, purity) || !walker.iterationsPreserveArrayGuards(list, purity))
        return std::nullopt;
    bool isOfSubclass = isArraySubclassInstance(walker, walker.ownTypeOf(list));
    if ((!isProvenArray(walker, list) && !isOfSubclass) || isFreshAllocation(walker, list))
        return std::nullopt;
    if (isOfSubclass) {
        if (kindStoredInto != ArrayStorage::Other || !mayOnlyTakeRow(walker, read))
            return std::nullopt;
        HoistedArrayGuard holder { list, ArrayStorage::Contiguous };
        holder.onlyHoldsRows = true;
        std::optional<HoistedArrayGuard> hoisted = tryHoistArrayGuard(walker, holder, walker.originOf(read), purity, Walker::GuardedIterations::Some);
        if (hoisted)
            walker.count(StatisticID::RowHoldersOfSubclasses);
        return hoisted;
    }
    ArrayStorage written = predictedArrayStorage(walker, walker.ownTypeOf(list));
    if (TypeID expected = written == ArrayStorage::Other ? findExpectation(walker, read.objectOperand()).type : TypeID())
        written = predictedArrayStorage(walker, walker.types().withoutNullish(expected));
    TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
    if (written == ArrayStorage::Other && seenByLink && !walker.types().at(seenByLink).isAnything()) {
        ArrayStorage seen = arrayStorageHolding(walker.types(), seenByLink);
        if (isNumericStorage(seen) || !(walker.types().at(seenByLink).primitives & Primitive::Number))
            written = seen;
    }
    if (written == ArrayStorage::Other && kindStoredInto == ArrayStorage::Other && (elementIsOnlyReadByKeyInListReadsInLoops(walker.currentBodyIndex(), read) || isElementReadByName(walker, read)))
        written = ArrayStorage::Contiguous;
    auto typeAsBuilt = [&](ValueID input) {
        Reference built = walker.referenceOf(input);
        return built ? walker.typeOf(built) : TypeID();
    };
    if (written == ArrayStorage::Other && kindStoredInto == ArrayStorage::Other && read.opcode() == Opcode::ElemGet && (isFoundByReadAtSiteInListReadsInLoops(walker, list) || walker.depthOfCopies())
        && (valueIsOnlyReckonedWithAsNumber(walker, read.id(), TypesOfInputs(typeAsBuilt)) || callersComputeNumbers(walker, read) || (isReadOfBodyInListReadsInLoops(walker.currentBodyIndex(), read) && isTakenByMathOrAddedToElement(walker, walker.currentBodyIndex(), read))))
        written = ArrayStorage::Double;
    bool isNumeric = isNumericStorage(written);
    bool movesElement = false;
    HoistedArrayGuard tested { list, isNumeric ? ArrayStorage::Double : written, isNumeric };
    tested.onlyHoldsRows = written == ArrayStorage::Contiguous && kindStoredInto == ArrayStorage::Other && mayOnlyTakeRow(walker, read);
    if (seenByLink && (walker.types().at(seenByLink).isInt32OrNullish() || (isReadOfBodyInListReadsInLoops(walker.currentBodyIndex(), read) && holdsKeysOfLoopInListReadsInLoops(walker, read.id(), seenByLink))))
        tested = HoistedArrayGuard { list, ArrayStorage::Int32, false };
    else if (seenByLink && walker.depthOfCopies() && !isNeverType(seenByLink) && walker.types().at(seenByLink).hasOnlyPrimitives(Primitive::NumberWithoutFraction | Primitive::Undefined) && isSteppedInPlace(walker, walker.currentBodyIndex(), read))
        tested = HoistedArrayGuard { list, ArrayStorage::Int32, false };
    if (kindStoredInto != ArrayStorage::Other && goesByKindOfStorage && holderThatRowIsReadOffInListReadsInLoops(walker, list))
        tested = HoistedArrayGuard { list, ArrayStorage::Double, kindStoredInto == ArrayStorage::Int32 || walker.canGuardAfterStores(), true, true };
    else if (kindStoredInto != ArrayStorage::Other && goesByKindOfStorage && isFoundByReadAtSiteInListReadsInLoops(walker, list) && walker.canGuardAfterStores()) {
        std::optional<HoistedArrayGuard> askedForReads = findHoistedArrayGuard(walker, list);
        bool isReadForNumbers = askedForReads && askedForReads->storage == ArrayStorage::Double;
        tested = HoistedArrayGuard { list, isReadForNumbers || isNumeric ? ArrayStorage::Double : ArrayStorage::Other, true, true, true };
    }
    else if (kindStoredInto != ArrayStorage::Other)
        tested = goesByKindOfStorage && (kindStoredInto == ArrayStorage::Int32 || !isNumeric) ? HoistedArrayGuard { list, ArrayStorage::Other, kindStoredInto == ArrayStorage::Int32, true, true } : HoistedArrayGuard { list, kindStoredInto, false, true, true };
    else if (WrittenListOfVariable written = writtenListReadInListReadsInLoops(walker, walker.currentBodyIndex(), read)) {
        tested = HoistedArrayGuard { list, written.storage, false };
        tested.written = written;
    } else if (isNumericStorage(tested.storage) && holderThatRowIsReadOffInListReadsInLoops(walker, list)) {
        RowStores stores = findRowStores(walker.currentBodyIndex(), read);
        bool movesElements = stores == RowStores::IntoSameHolder || (stores == RowStores::IntoOthers && onlyUserStoresWhatIsReadIntoListInListReadsInLoops(walker.currentBodyIndex(), read));
        bool storesWholeNumbers = movesElements && seenByLink && !isNeverType(seenByLink) && walker.types().at(seenByLink).hasOnlyPrimitives(Primitive::NumberWithoutFraction | Primitive::Undefined);
        if (storesWholeNumbers)
            tested = HoistedArrayGuard { list, ArrayStorage::Int32, false };
        bool exitsAtHole = tested.storage == ArrayStorage::Int32 || !onlyUserTakesUndefinedForNaNInTypedElements(walker, read, WhatStandsBesideRead::NumberOrNothing);
        tested.turnsStoreIntoLists = stores != RowStores::None && exitsAtHole && (!tested.admitsInt32Storage || userNeedsNumber(walker, read));
        movesElement = stores == RowStores::IntoSameHolder && !storesWholeNumbers && (walker.mayCheckOutsideCopy() || walker.walksCopyWithoutSomeIterationGuesses()) && movesElementIntoRowOfSameHolder(walker.currentBodyIndex(), read);
        if (movesElement) {
            tested = HoistedArrayGuard { list, walker.walksCopyWithoutSomeIterationGuesses() ? ArrayStorage::Double : ArrayStorage::Int32, false };
            tested.turnsStoreIntoLists = true;
        }
    }
    bool takesInt32s = kindStoredInto == ArrayStorage::Other && tested.storage == ArrayStorage::Double && tested.admitsInt32Storage && !tested.turnsStoreIntoLists && walker.isInInt32Version();
    if (takesInt32s)
        tested = HoistedArrayGuard { list, *walker.storageOfUnswitchedVersion(), false };
    if (kindStoredInto == ArrayStorage::Other && walker.walksCopyWithoutSomeIterationGuesses() && walker.depthOfCopies() && isNumericStorage(tested.storage) && !tested.written && read.opcode() == Opcode::ElemGet && valueIsOnlyReckonedWithAsNumber(walker, read.id(), TypesOfInputs(typeAsBuilt)))
        tested = HoistedArrayGuard { list, ArrayStorage::Other, false };
    std::optional<HoistedArrayGuard> hoisted = tryHoistArrayGuard(walker, tested, walker.originOf(read), purity, movesElement ? Walker::GuardedIterations::AllBesideCopy : kindStoredInto == ArrayStorage::Other ? Walker::GuardedIterations::Some : Walker::GuardedIterations::All);
    if (hoisted && takesInt32s)
        walker.noteInt32Guard();
    if (hoisted && movesElement)
        noteKindOfStorageThatTurnFound(walker, list, tested.storage, false, false, true);
    return hoisted;
}

TypeID seenUnderVariableKey(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID key = index.stripCasts(read.propertyOperand());
    if (!index.isParameter(key) && index.opcodeOf(key) == Opcode::Integer)
        return { };
    return walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
}

bool rowsHoldIntegers(Walker& walker, const BodyValue& access)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!isReadOfBodyInListReadsInLoops(index, access))
        return false;
    auto isSeenAsInteger = [&](const BodyValue& read) {
        TypeID seen = seenUnderVariableKey(walker, read);
        return seen && !isNeverType(seen) && (walker.types().at(seen).primitives & Primitive::NumberWithoutFraction) && walker.types().at(seen).hasOnlyPrimitives(Primitive::NumberWithoutFraction | Primitive::Undefined);
    };
    if (access.opcode() == Opcode::ElemGet)
        return isSeenAsInteger(access);
    ValueID row = index.stripCasts(access.objectOperand());
    if (!row || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet)
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    if (!loop)
        return false;
    ValueID key = index.stripCasts(index.value(row).propertyOperand());
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue read) {
            if (read.opcode() != Opcode::ElemGet)
                return false;
            ValueID other = index.stripCasts(read.objectOperand());
            if (!other || index.isParameter(other) || index.opcodeOf(other) != Opcode::ElemGet)
                return false;
            return (other == row || index.stripCasts(index.value(other).propertyOperand()) == key) && isSeenAsInteger(read);
        });
    });
}

bool tryHoistInt32Guard(Walker& walker, const BodyValue& read, Reference list)
{
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    bool isCandidate = walker.isQuietVersionOfUnswitchLoop();
    if ((!walker.isInUnswitchedVersion() && !isCandidate) || walker.storageOfUnswitchedVersion() == ArrayStorage::Double || !walker.has(Mechanism::Folding) || !list || !list.isValue() || !isProvenArray(walker, list) || isFreshAllocation(walker, list) || !walker.isInvariantInSpecializedVersion(list, purity) || !walker.iterationsPreserveArrayGuards(list, purity))
        return false;
    if (isCandidate) {
        walker.noteInt32Candidate();
        return false;
    }
    if (!tryHoistArrayGuard(walker, HoistedArrayGuard { list, ArrayStorage::Int32, false }, walker.originOf(read), purity, Walker::GuardedIterations::Some))
        return false;
    walker.noteInt32Guard();
    return true;
}

void retestArrayForInt32s(Walker& walker, const BodyValue& read)
{
    bool isCandidate = walker.isQuietIterationOfUnswitchLoop();
    if ((!isCandidate && !walker.isInUnswitchedVersion()) || walker.storageOfUnswitchedVersion() == ArrayStorage::Double || !walker.has(Mechanism::Folding))
        return;
    std::optional<HoistedArrayGuard> tested = findHoistedArrayGuard(walker, read);
    if (!tested || tested->storage != ArrayStorage::Double || !tested->admitsInt32Storage || tested->isWritable || tested->turnsStoreIntoLists || tested->written)
        return;
    if (isCandidate) {
        walker.noteInt32Candidate();
        return;
    }
    Reference list = walker.referenceOf(read.objectOperand());
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    if (!walker.isInInt32Version() || !list || !list.isValue() || !isProvenArray(walker, list) || !walker.isInvariantInSpecializedVersion(list, purity) || !walker.iterationsPreserveArrayGuards(list, purity))
        return;
    if (tryHoistArrayGuard(walker, HoistedArrayGuard { list, ArrayStorage::Int32, false }, walker.originOf(read), purity, Walker::GuardedIterations::Some))
        walker.noteInt32Guard();
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
    LayoutID subclass = subclassOfArrayName(walker, list);
    return buildIterationGuard(walker, subclass ? draftSubclassElementsTest(list, subclass, origin) : questionWhetherListIsAsMadeInListReadsInLoops(walker, list, origin));
}

bool guardRowInIteration(Walker& walker, Reference row, Origin origin)
{
    if (!guardPristineArrayInIteration(walker, row, origin) || !buildIterationGuard(walker, questionForKindOfStorageInListReadsInLoops(walker, HoistedArrayGuard { row, ArrayStorage::Int32, false, false }, origin)))
        return false;
    walker.count(StatisticID::ArraysGuardedInIterations);
    walker.recordSpeculation();
    noteKindOfStorageThatTurnFound(walker, row, ArrayStorage::Int32);
    return true;
}

Reference askWhetherListIsAsMade(Walker& walker, Reference list, Origin origin)
{
    return walker.build(questionWhetherListIsAsMadeInListReadsInLoops(walker, list, origin));
}

Reference askWhetherStorageOfListIsOfKind(Walker& walker, Reference list, ArrayStorage kind, Origin origin, bool isWritable, bool admitsInt32Storage)
{
    return walker.build(questionForKindOfStorageInListReadsInLoops(walker, HoistedArrayGuard { list, kind, admitsInt32Storage, isWritable }, origin));
}

static constexpr uint32_t wordOfKindOfRowsInListReadsInLoops = 1;

std::optional<ArrayStorage> kindOfRowsFoundBeforeLoop(Walker& walker, Reference row)
{
    Reference holder = row && row.isValue() ? holderThatRowIsReadOffInListReadsInLoops(walker, row) : Reference();
    std::optional<uint64_t> kind = holder ? findKnown(walker, KnownNote::HoistedArrayGuard, holder, { }, wordOfKindOfRowsInListReadsInLoops) : std::nullopt;
    return kind ? std::optional(static_cast<ArrayStorage>(*kind)) : std::nullopt;
}

static Reference holderThatRowIsReadOffInListReadsInLoops(Walker& walker, Reference row)
{
    const Code& code = walker.code();
    ValueID held = row.id();
    while (valuePreservingOpcodes.contains(code.at(held).opcode))
        held = code.at(held).operand(0);
    return code.at(held).opcode == Opcode::GetArrayElement ? Reference::value(code.at(held).operand(0)) : Reference();
}

void noteKindOfStorageThatTurnFound(Walker& walker, Reference list, ArrayStorage kind, bool isWritable, bool admitsInt32Storage, bool turnsStoreIntoLists)
{
    noteListTestedInListReadsInLoops(walker, HoistedArrayGuard { list, kind, admitsInt32Storage, isWritable, turnsStoreIntoLists });
    if (Reference holder = admitsInt32Storage ? Reference() : holderThatRowIsReadOffInListReadsInLoops(walker, list))
        noteKnown(walker, KnownNote::HoistedArrayGuard, holder, { }, wordOfKindOfRowsInListReadsInLoops, LocationMasks(heapsUnderListsReadInLoops), static_cast<uint64_t>(kind));
}

std::optional<HoistedArrayGuard> tryGuardNumericArrayInIteration(Walker& walker, const BodyValue& read, Reference list)
{
    bool turnsStore = !walker.hasNoElementStores();
    bool canCheckInIteration = !turnsStore ? canGuardArrayInIteration(walker, list) : walker.has(Mechanism::Folding) && list && list.isValue() && walker.canGuardAfterStores() && walker.iterationsPreserveArrayGuards(list) && !isFreshAllocation(walker, list);
    if (!canCheckInIteration || !isProvenArray(walker, list))
        return std::nullopt;
    if (!isExpectedToBeListOfNumbers(walker, walker.ownTypeOf(list))) {
        if (!holderThatRowIsReadOffInListReadsInLoops(walker, list))
            return std::nullopt;
        TypeID seen = isReadOfBodyInListReadsInLoops(walker.currentBodyIndex(), read) ? seenUnderVariableKey(walker, read) : TypeID();
        if (seen && walker.types().at(seen).isAnything())
            seen = TypeID();
        if (!seen && callersComputeNumbers(walker, read))
            seen = TypeTable::number;
        if (!seen || isNeverType(seen) || !isNumericStorage(arrayStorageHolding(walker.types(), walker.types().withoutNullish(seen))))
            return std::nullopt;
    }
    HoistedArrayGuard tested { list, ArrayStorage::Double, true };
    if (rowsHoldIntegers(walker, read))
        tested = HoistedArrayGuard { list, ArrayStorage::Int32, false };
    else if (findRowStores(walker.currentBodyIndex(), read) != RowStores::None && !readsTruncatedElement(walker, read))
        tested = HoistedArrayGuard { list, ArrayStorage::Double, false };
    tested.turnsStoreIntoLists = turnsStore;
    if (turnsStore)
        walker.count(StatisticID::RowsCheckedInStoringIterations);
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

std::optional<HoistedArrayGuard> tryGuardInlineeArrayInIteration(Walker& walker, const BodyValue& read, Reference list)
{
    if (!walker.has(Mechanism::Folding) || !list || !list.isValue() || !walker.depthOfCopies() || !walker.canBuildIterationGuard() || !walker.canRestartIteration() || walker.readStandsUnderHandler())
        return std::nullopt;
    if (isFreshAllocation(walker, list) || !isProvenArray(walker, list))
        return std::nullopt;
    TypeID seen = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
    if (!seen || walker.types().at(seen).isAnything() || walker.types().at(seen).primitives & Primitive::Number || arrayStorageHolding(walker.types(), seen) != ArrayStorage::Contiguous)
        return std::nullopt;
    HoistedArrayGuard tested { list, ArrayStorage::Contiguous, false };
    tested.turnsStoreIntoLists = !walker.hasNoElementStores();
    tested.isWritable = tested.turnsStoreIntoLists;
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

bool isElementUsedNumerically(Walker& walker, const BodyIndex& index, ValueID read)
{
    static constexpr OpcodeSet calculations = allNumericOperators | allBitwiseOperators | allRelations | OpcodeSet { Opcode::ElemSet };
    if (index.opcodeOf(read) != Opcode::ElemGet)
        return false;
    bool isUsedNumerically = false;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID block(number);
        if (!index.isReachable(block))
            continue;
        bool isUsedOtherwise = false;
        for (BodyValue operation : index.block(block).values()) {
            if (opcodesThatStateTypes.contains(operation.opcode()))
                continue;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                if (!operand || index.stripCasts(operand) != read)
                    return;
                bool isKey = operation.opcode() == Opcode::ElemGet && operand == operation.propertyOperand();
                bool isCalculation = isKey || calculations.contains(operation.opcode());
                if (operation.opcode() == Opcode::StrictEq) {
                    Reference other = walker.referenceBoundTo(index.stripCasts(operation.operand(operation.operand(0) == operand ? 1 : 0)));
                    isCalculation = other && walker.types().at(walker.typeOf(other)).isNumber();
                }
                isUsedNumerically |= isCalculation;
                isUsedOtherwise |= !isCalculation;
            });
        }
        BodyTerminator end = index.terminator(block);
        end.forEachOperand([&](ValueID operand, uint32_t) { isUsedOtherwise |= operand && index.stripCasts(operand) == read; });
        index.block(block).forEachOutgoingEdge([&](BodyTarget target, bool) {
            for (ValueID argument : target.arguments())
                isUsedOtherwise |= argument && index.stripCasts(argument) == read;
        });
        if (isUsedOtherwise)
            return false;
    }
    return isUsedNumerically;
}

static bool storesIntoArray(const BodyIndex& index, ValueID list)
{
    auto holderOf = [&](ValueID object) {
        object = index.stripCasts(object);
        while (!index.isParameter(object) && index.opcodeOf(object) == Opcode::CheckInitialized)
            object = index.stripCasts(index.value(object).operand(0));
        return object;
    };
    ValueID holder = holderOf(list);
    auto areSame = [&](ValueID other) {
        if (other == holder || index.isParameter(other) || index.isParameter(holder))
            return other == holder;
        return index.opcodeOf(other) == Opcode::GetModuleVariable && index.opcodeOf(holder) == Opcode::GetModuleVariable && index.value(other).variable() == index.value(holder).variable();
    };
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::ElemSet && areSame(holderOf(operation.objectOperand())))
                return true;
        }
    }
    return false;
}

std::optional<ArrayStorage> predictStorageInRecursion(Walker& walker, const BodyValue& read, TypeID list)
{
    TypeArena& types = walker.types();
    TypeID seen = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
    TypeID present = seen ? types.withoutPrimitives(seen, Primitive::Undefined | Primitive::Absent) : TypeID();
    if (walker.walksVersionOfArrayKind() && (!present || !types.at(present).isNumber())) {
        seen = walker.facts().held().resultOfReadByKey(walker.currentFunction(), read.id());
        present = seen ? types.withoutPrimitives(seen, Primitive::Undefined | Primitive::Absent) : TypeID();
    }
    bool holdsIntegersAlone = present && !isNeverType(present) && types.at(present).isInteger();
    seen = seen ? types.withoutPrimitives(seen, Primitive::UndefinedOrNull | Primitive::Absent) : TypeID();
    if (!seen || isNeverType(seen) || !types.at(seen).isNumber() || !hasKind(types, list, OperandKind::Array) || !isElementUsedNumerically(walker, walker.currentBodyIndex(), read.id()))
        return std::nullopt;
    bool isWhole = types.at(seen).hasOnlyPrimitives(Primitive::NumberWithoutFraction);
    if (isWhole && !holdsIntegersAlone && !isExpectedToBeListOfNumbers(walker, list))
        return std::nullopt;
    return isWhole ? ArrayStorage::Int32 : ArrayStorage::Double;
}

static bool holdsUnknownNumbers(Walker& walker, const BodyValue& read)
{
    TypeID seen = walker.facts().held().resultOfReadByKey(walker.currentFunction(), read.id());
    return (!seen || walker.types().isUninformative(seen)) && isElementUsedNumerically(walker, walker.currentBodyIndex(), read.id());
}

std::optional<HoistedArrayGuard> tryGuardArrayInRecursion(Walker& walker, const BodyValue& read, Reference list)
{
    if (!walker.canGuardRecursion() || read.id().index() >= walker.currentBodyIndex().numberOfValues() || walker.currentBodyIndex().value(read.id()).words() != read.words() || !list || !list.isValue() || isFreshAllocation(walker, list) || !isProvenArray(walker, list))
        return std::nullopt;
    std::optional<ArrayStorage> storage = predictStorageInRecursion(walker, read, walker.ownTypeOf(list));
    bool isOfEitherNumericStorage = !storage && walker.walksVersionOfArrayKind() && holdsUnknownNumbers(walker, read);
    if (isOfEitherNumericStorage)
        storage = ArrayStorage::Double;
    if (!storage)
        return std::nullopt;
    HoistedArrayGuard tested { list, *storage, isOfEitherNumericStorage };
    tested.isWritable = isNumericStorage(*storage) && storesIntoArray(walker.currentBodyIndex(), read.objectOperand());
    tested.turnsStoreIntoLists = walker.currentBodyIndex().opcodes().contains(Opcode::ElemSet);
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    auto guard = [&](Draft&& draft) {
        Reference test = walker.build(WTF::move(draft));
        if (!test)
            return false;
        BlockID passed = walker.addBlock();
        walker.branch(test, passed, walker.elementGuardFallback());
        walker.continueIn(passed);
        return !walker.isUnreachable() && !walker.hasFailed();
    };
    if (!askWhatListIsAskedInListReadsInLoops(walker, tested, origin, guard))
        return std::nullopt;
    walker.recordSpeculation();
    walker.count(StatisticID::ArraysGuardedInIterations);
    noteListTestedInListReadsInLoops(walker, tested);
    return tested;
}

void guardArraysAtEntry(Walker& walker)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Function& function = walker.program().at(walker.currentFunction());
    BodyBlock entry = index.block(entryBlock);
    for (uint32_t place = 0; place < entry.numberOfParameters() && !walker.isUnreachable() && !walker.hasFailed(); ++place) {
        if (function.entryParameterKind(place) != EntryParameter::Parameter)
            continue;
        ValueID list = entry.parameter(place);
        ValueID firstRead;
        Vector<BlockID, 8> blocks;
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            if (!index.isReachable(BlockID(number)))
                continue;
            for (BodyValue operation : index.block(BlockID(number)).values()) {
                if ((operation.opcode() != Opcode::ElemGet && operation.opcode() != Opcode::ElemSet) || index.stripCasts(operation.objectOperand()) != list)
                    continue;
                if (!firstRead && operation.opcode() == Opcode::ElemGet && isElementUsedNumerically(walker, index, operation.id()))
                    firstRead = operation.id();
                blocks.appendIfNotContains(BlockID(number));
            }
        }
        bool isTestedAtFirstAccess = std::ranges::any_of(blocks, [&](BlockID first) {
            return std::ranges::all_of(blocks, [&](BlockID other) { return index.dominates(first, other); });
        });
        if (!firstRead || isTestedAtFirstAccess)
            continue;
        BodyValue read = index.value(firstRead);
        tryGuardArrayInRecursion(walker, read, walker.referenceOf(list));
    }
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

static Reference nameOfTestedRow(Walker& walker, BlockID inputHead, Reference row)
{
    auto nameBehindChecks = [&](Reference name) {
        while (Reference checked = walker.state().checkedNameOf(name))
            name = checked;
        return name;
    };
    for (const Walker::HoistedRow& hoisted : walker.hoistedRowsOf(inputHead)) {
        if (hoisted.isRejected || hoisted.row != row)
            continue;
        if (std::optional<uint64_t> tested = findKnown(walker, KnownNote::NumberReadOutOfList, nameBehindChecks(hoisted.holder), hoisted.key))
            return Reference::fromBits(static_cast<uint32_t>(*tested));
    }
    return nameBehindChecks(row);
}

static std::optional<Vector<Reference, 2>> findArraysStoredInLoop(Walker& walker, const BodyValue& read)
{
    static constexpr OpcodeSet opcodesWithoutElementStores = primitiveConstants | opcodesThatStateTypes | allNumericOperators | allRelations | allEqualities
        | OpcodeSet { Opcode::LoopHint, Opcode::ElemGet, Opcode::ToBoolean, Opcode::LogicalNot, Opcode::ToNumeric, Opcode::ToNumber, Opcode::ToPropertyKey, Opcode::GetModuleVariable, Opcode::CheckInitialized, Opcode::GetEnvironmentField, Opcode::Builtin, Opcode::RequireObjectCoercible, Opcode::TypeTest };
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = isReadOfBodyInListReadsInLoops(index, read) ? index.loopOf(index.blockOf(read.id())) : LoopID();
    while (loop && !walker.specializesLoopOfCurrentBody(index.loop(loop).head))
        loop = index.loop(loop).parent;
    if (!loop)
        return std::nullopt;
    uint32_t firstValueOfLoop = index.block(index.loop(loop).head).firstParameter().index();
    Vector<Reference, 2> lists;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::ElemSet) {
                if (!opcodesWithoutElementStores.contains(operation.opcode()) && !isMathCall(walker, index, operation))
                    return std::nullopt;
                continue;
            }
            ValueID stored = valueThatEntersLoopInListReadsInLoops(index, operation.objectOperand(), firstValueOfLoop);
            Reference list = stored.index() >= firstValueOfLoop ? nameThatEntersLoopInListReadsInLoops(walker, index, stored, firstValueOfLoop) : walker.referenceOf(stored);
            if (list)
                list = nameOfTestedRow(walker, index.loop(loop).head, list);
            if (!list || !list.isValue())
                return std::nullopt;
            if (!lists.contains(list))
                lists.append(list);
        }
    }
    return lists;
}

static bool tryHoistAliasChecks(Walker& walker, const BodyValue& read, Reference list, Walker::IterationPurity purity)
{
    std::optional<Vector<Reference, 2>> stored = isProvenArray(walker, list) ? findArraysStoredInLoop(walker, read) : std::nullopt;
    if (!stored)
        return false;
    ValueID cell = underlyingCellOf(walker.code(), list.id());
    for (Reference other : *stored) {
        if (underlyingCellOf(walker.code(), other.id()) == cell || !walker.isInvariantInSpecializedVersion(other, purity))
            return false;
    }
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    for (Reference other : *stored) {
        if (isFreshAllocation(walker, other) || isFreshAllocation(walker, list) || !walker.types().overlap(walker.typeOf(other), walker.typeOf(list)))
            continue;
        if (!isProvenArray(walker, other))
            return false;
        Reference isSame;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
            Draft asked(Opcode::StrictEq, origin);
            walker.chooseConservativeFields(asked);
            asked.setLeftOperand(other);
            asked.setRightOperand(list);
            asked.setForm(operatorFormFor(walker, asked));
            isSame = walker.build(WTF::move(asked));
        }
        if (!isSame || !walker.tryHoistConditionalCheck(isSame, false, purity))
            return false;
        walker.recordSpeculation();
    }
    return true;
}

bool compareWithStoredArrays(Walker& walker, const BodyValue& read, Reference list)
{
    if (walker.isUntouchedInIterations(list))
        return true;
    if (!walker.has(Mechanism::Folding) || walker.listsAreStruckInRow() || !list.isValue() || !walker.iterationsRunNoOtherCode() || !walker.isInvariantInSpecializedVersion(list) || !tryHoistAliasChecks(walker, read, list, Walker::IterationPurity::EffectFree))
        return false;
    walker.noteUntouchedArray(list);
    walker.count(StatisticID::ArraysComparedWithStoredArrays);
    return true;
}

std::optional<PassedTest> tryHoistElementPresenceCheck(Walker& walker, const BodyValue& read, const HoistedArrayGuard& tested, Reference list, Reference position)
{
    bool isNumber = isNumericStorage(tested.storage);
    if (!walker.has(Mechanism::Folding) || walker.listsAreStruckInRow() || (tested.storage != ArrayStorage::Contiguous && !isNumber) || holdsBoxedValuesOfEitherKind(tested) || tested.isWritable)
        return std::nullopt;
    Walker::IterationPurity purity = isNumber ? Walker::IterationPurity::EffectFree : walker.arrayGuardPurity();
    if (!walker.isInvariantInSpecializedVersion(list, purity) || !walker.isInvariantInSpecializedVersion(position, purity))
        return std::nullopt;
    bool isBesideStores = isNumber && (tested.turnsStoreIntoLists || !walker.hasNoElementStores());
    if (!isBesideStores && !walker.iterationsPreserveArrayGuards(list, purity))
        return std::nullopt;
    if (isBesideStores ? !compareWithStoredArrays(walker, read, list) : !isNumber && !walker.areElementStoresNumeric(purity))
        return std::nullopt;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    if (isNumber) {
        Reference hasStorage;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
            Draft storage(Opcode::HasArrayStorage, origin);
            walker.chooseConservativeFields(storage);
            storage.setArrayOperand(list);
            storage.setArrayStorage(tested.storage);
            storage.setAdmitsInt32Storage(tested.admitsInt32Storage);
            hasStorage = walker.build(WTF::move(storage));
        }
        if (!hasStorage || (hasStorage.isValue() && !walker.tryHoistConditionalCheck(hasStorage, true, purity)))
            return std::nullopt;
    }
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
        if (tested.name == list && walker.isBuildingInsideLoop())
            takeStorageAndLengthFromInFrontOfLoop(walker, isPresent, tested);
        if (isPresent.storageOperand())
            walker.count(StatisticID::RowStoragesFromOuterPreheaders);
        isThere = walker.build(WTF::move(isPresent));
    }
    std::optional<PassedTest> present = isThere ? walker.showElementPresentBehindTest(list, position, walker.tryHoistConditionalCheck(isThere, true, purity)) : std::nullopt;
    if (!present)
        return std::nullopt;
    walker.recordSpeculation();
    if (isBesideStores) {
        walker.recordNumberReadNearStores();
        walker.count(StatisticID::NumericElementReadsHoisted);
    } else if (isNumber) {
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

static void appendViewsFromVariables(Walker& walker, BlockID inputHead, ListsReadInLoop& found)
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

static Reference guardArrayOfVariable(Walker& walker, Reference read, BlockID& genericStart, Origin origin)
{
    TypeArena& types = walker.types();
    if (!read || !read.isValue() || isProvenArray(walker, read) || (!(walker.types().at(walker.ownTypeOf(read)).primitives & Primitive::Absent) && !walker.isFieldArrayRead(read)))
        return { };
    TypeID filled = types.withoutNullish(types.withoutPrimitives(walker.typeOf(read), Primitive::Absent));
    TypeID arrays = types.builtinClass(BuiltinClasses::Array);
    if (isNeverType(filled) || !types.isSubset(filled, arrays))
        return { };
    Draft question(Opcode::TypeTest, origin);
    question.setValueOperand(read);
    question.setTestedType(arrays);
    Reference test = walker.build(WTF::move(question));
    if (!test || !test.isValue() || !continueIfGuardPasses(walker, test, genericStart))
        return { };
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(read);
    proof.setTestOperand(test);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    Reference array = walker.build(WTF::move(proof));
    if (array && !walker.state().checkedNameOf(read))
        walker.state().noteCheckedName(read, array);
    walker.noteFieldArrayProof(read, array);
    return array;
}

static void compareWithAllocations(Walker& walker, BlockID inputHead, ListsReadInLoop& found, BlockID& genericStart, Origin origin)
{
    const Walker::AllocationsUsedInLoop mentioned = walker.allocationsUsedInLoop(inputHead);
    if (!fillsAllocation(walker, inputHead, mentioned))
        return;
    for (const HoistedArrayGuard& list : found.lists) {
        if (list.isWritable || !list.name.isValue() || walker.classifyAgainstAllocations(mentioned, list.name))
            continue;
        ValueID cell = underlyingCellOf(walker.code(), list.name.id());
        for (uint32_t made : mentioned) {
            if (cell.index() < made)
                continue;
            Reference allocation = Reference::value(ValueID(made));
            if (walker.types().overlap(walker.typeOf(allocation), walker.typeOf(list.name))) {
                Draft isSame(Opcode::StrictEq, origin);
                walker.chooseConservativeFields(isSame);
                isSame.setLeftOperand(list.name);
                isSame.setRightOperand(allocation);
                isSame.setForm(operatorFormFor(walker, isSame));
                Reference test = walker.build(WTF::move(isSame));
                if (!test || !test.isValue())
                    return;
                BlockID differs = walker.addBlock();
                if (!genericStart)
                    genericStart = walker.addBlock(BlockFrequency::Rare);
                walker.branch(test, genericStart, differs);
                walker.continueIn(differs);
                if (walker.isUnreachable())
                    return;
                walker.count(StatisticID::ArraysComparedWithAllocations);
            }
            found.distinctAllocations.append({ cell.index(), made });
        }
    }
}

void buildTestsOfListsReadInLoop(Walker& walker, BlockID inputHead, std::span<const Reference> entering, ListsReadInLoop& found, std::span<const HoistedSpeculation> guesses, std::span<const Reference> proofs, BlockID& genericStart, Origin origin)
{
    appendViewsFromVariables(walker, inputHead, found);
    if (found.lists.isEmpty() || walker.isUnreachable() || walker.hasFailed())
        return;

    SpeculationLookupByName provenSpeculationLookup;
    for (HoistedArrayGuard& list : found.lists) {
        if (size_t place = provenSpeculationLookup.find(guesses.first(std::min(guesses.size(), proofs.size())), list.name); place != notFound)
            list.name = proofs[place];
        if (Reference array = guardArrayOfVariable(walker, list.name, genericStart, origin))
            list.name = array;
        if (walker.isUnreachable() || walker.hasFailed())
            return;
    }
    for (HoistedArrayGuard& list : found.lists) {
        if (!list.subclass || isProvenArray(walker, list.name) || classOfProvenViewInListReadsInLoops(walker, list.name))
            continue;
        if (Reference array = arrayNameOfInstance(walker, list.name, origin))
            list.name = array;
        if (walker.isUnreachable() || walker.hasFailed())
            return;
    }
    found.lists.removeAllMatching([&](const HoistedArrayGuard& list) {
        return !isProvenArray(walker, list.name) && !(list.onlyHoldsRows && isInstanceOfArraySubclass(walker, list.name)) && !classOfProvenViewInListReadsInLoops(walker, list.name);
    });
    if (found.lists.isEmpty())
        return;
    if (!found.areLeftAsTheyAre && !rowFindsListsLeftAsTheyAreInListReadsInLoops(walker, inputHead, found.lists.span(), guesses, proofs, entering))
        return;

    static constexpr size_t arraysPerBlockRelease = ArrayLookupByName::linearSearchLimit;
    auto releaseEndedBlocksPeriodically = [&](const HoistedArrayGuard& list) {
        if (size_t place = &list - found.lists.begin(); place && !(place % arraysPerBlockRelease))
            walker.releaseEndedPendingBlocks();
    };
    auto checkStorage = [&](HoistedArrayGuard& list) {
        releaseEndedBlocksPeriodically(list);
        if (!hasArrayStorageForm(list.storage) && !list.isWritable)
            return true;
        if (list.written) {
            Draft isStillWritten(Opcode::HasArrayStorage, origin);
            walker.chooseConservativeFields(isStillWritten);
            isStillWritten.setArrayOperand(list.name);
            isStillWritten.setArrayStorage(list.storage);
            isStillWritten.setData(list.written.data);
            if (!continueIfGuardPasses(walker, walker.build(WTF::move(isStillWritten)), genericStart))
                return false;
            noteStorageOfWrittenList(walker, list.name, list.written);
            return true;
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
            return false;
        return true;
    };
    bool hasOneFallback = !found.lists.containsIf([](const HoistedArrayGuard& list) { return list.hasAlternateStorage; });
    BitVector listsChecked(found.lists.size());
    std::optional<Draft> skippedGuard;
    for (HoistedArrayGuard& list : found.lists) {
        releaseEndedBlocksPeriodically(list);
        if (list.onlyHoldsRows)
            continue;
        if (list.subclass) {
            bool isView = !!classOfProvenViewInListReadsInLoops(walker, list.name);
            Draft look = draftSubclassElementsTest(list.name, list.subclass, origin);
            look.setAsksElements(!isView);
            if (!continueIfGuardPasses(walker, walker.build(WTF::move(look)), genericStart))
                return;
            walker.count(StatisticID::BuiltinSubclassesCheckedByStructure);
            if (isView) {
                MemberConditionRecord condition { MemberConditionKind::MemberUnchanged, list.subclass, ClassSide::Prototype, walker.program().keyOf(BuiltinNames::length) };
                Draft isFoundAbove(Opcode::AreMembersUnchanged, origin);
                isFoundAbove.setConditions(singleElementSpan(condition));
                if (!continueIfGuardPasses(walker, walker.build(WTF::move(isFoundAbove)), genericStart))
                    return;
            }
            continue;
        }
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
            aspects.add(PristineAspects::InheritedNamesKept);
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
        bool isStorageChecked = (hasArrayStorageForm(list.storage) || list.isWritable) && !isConvertedInPreheader(list);
        if (isStorageChecked && !classOfView && !asksCellAlone && isKnownPristine(walker, list.name)) {
            walker.verifyPristineObject(list.name);
            if (!skippedGuard)
                skippedGuard = WTF::move(isAsMade);
            walker.count(StatisticID::PristineArrayGuardsSkipped);
            continue;
        }
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
        if (hasOneFallback && !classOfView && !isConvertedInPreheader(list)) {
            if (!checkStorage(list))
                return;
            listsChecked.quickSet(&list - found.lists.begin());
        }
    }
    for (HoistedArrayGuard& list : found.lists) {
        if (!isConvertedInPreheader(list) && !listsChecked.quickGet(&list - found.lists.begin()) && !checkStorage(list))
            return;
    }
    if (skippedGuard && !genericStart && !continueIfGuardPasses(walker, walker.build(WTF::move(*skippedGuard)), genericStart))
        return;
    for (const HoistedArrayGuard& list : found.lists) {
        if (!isConvertedInPreheader(list))
            continue;
        Draft convert(Opcode::ConvertToDoubleStorage, origin);
        walker.chooseConservativeFields(convert);
        convert.setArrayOperand(list.name);
        walker.buildConversionBeforeLoop(WTF::move(convert), inputHead);
        walker.count(StatisticID::ArraysConvertedToDoubleStorage);
    }
    for (const HoistedArrayGuard& list : found.lists) {
        std::optional<uint64_t> fills = list.isWritable || isConvertedInPreheader(list) ? findKnown(walker, KnownNote::LoopAppendsToLiteral, list.name, { }, inputHead.index()) : std::nullopt;
        std::optional<uint64_t> wouldFill = fills && *fills >> 32 == walker.currentFunction().index() ? findKnown(walker, KnownNote::CapacityForRemainingIterations, list.name, { }, inputHead.index()) : std::nullopt;
        if (!wouldFill)
            continue;
        auto behindGuess = [&](Reference entered) {
            for (size_t place = 0; place < guesses.size() && place < proofs.size(); ++place) {
                if (guesses[place].value == entered)
                    return proofs[place];
            }
            return entered;
        };
        uint32_t source = static_cast<uint32_t>(*wouldFill >> 32);
        uint32_t firstValueOfLoop = walker.currentBodyIndex().block(inputHead).firstParameter().index();
        Reference readUnderCounter = source == hasNoFilledArraySource ? Reference() : behindGuess(nameThatEntersLoopInListReadsInLoops(walker, walker.currentBodyIndex(), ValueID(source), firstValueOfLoop));
        bool isView = readUnderCounter && readUnderCounter.isValue() && isViewOfNumbersReadInLine(walker, walker.typeOf(readUnderCounter));
        bool isString = readUnderCounter && !isView && walker.types().at(walker.typeOf(readUnderCounter)).isAnyString();
        auto growInIterations = [&] {
            endKnown(walker, KnownNote::LoopAppendsToLiteral, list.name, { }, inputHead.index());
        };
        if (source != hasNoFilledArraySource && (readUnderCounter == list.name || (!isView && !isString && !isProvenArray(walker, readUnderCounter)))) {
            growInIterations();
            continue;
        }
        Reference requiredCapacity = static_cast<uint32_t>(*wouldFill) ? Reference::fromBits(static_cast<uint32_t>(*wouldFill)) : Reference();
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID bound = requiredCapacity ? ValueID() : index.stripCasts(index.value(index.terminator(inputHead).operand(0)).operand(1));
        if (bound && bound.index() < index.block(inputHead).firstParameter().index()) {
            Reference entered = walker.referenceOf(bound);
            requiredCapacity = behindGuess(entered);
            bool isInteger = requiredCapacity && walker.types().at(walker.typeOf(requiredCapacity)).isInteger();
            if (!isInteger && found.counterBound && entered == found.counterBound) {
                requiredCapacity = found.clampedCounterBound;
                isInteger = walker.types().at(walker.typeOf(requiredCapacity)).isInteger();
            }
            if (!isInteger) {
                growInIterations();
                continue;
            }
        }
        Reference length = !readUnderCounter ? requiredCapacity : isString ? buildLengthOfString(walker, readUnderCounter, origin) : isView ? buildRoomOfViewBesideAddend(walker, readUnderCounter, { }, origin) : buildArrayLength(walker, readUnderCounter, origin);
        Reference room = length && readUnderCounter && requiredCapacity ? buildIntegerMinimum(walker, requiredCapacity, length, origin) : length;
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        if (!room) {
            growInIterations();
            continue;
        }
        Draft reserve(Opcode::ReserveArrayCapacity, origin);
        walker.chooseConservativeFields(reserve);
        reserve.setArrayOperand(list.name);
        reserve.setCapacityOperand(room);
        walker.build(WTF::move(reserve));
        walker.count(StatisticID::CapacitiesReservedFromSourceArrays);
    }
    auto mayHaveBeenConverted = [&](const HoistedArrayGuard& list) {
        if (list.storage == ArrayStorage::Double && list.admitsInt32Storage && !list.isWritable)
            return false;
        return (list.storage == ArrayStorage::Int32 || list.admitsInt32Storage) && found.lists.containsIf([&](const HoistedArrayGuard& converted) {
            return isConvertedInPreheader(converted) && mayBeSameObject(walker, list.name, converted.name);
        });
    };
    for (HoistedArrayGuard& list : found.lists) {
        if ((isConvertedInPreheader(list) || mayHaveBeenConverted(list)) && !checkStorage(list))
            return;
    }
    for (const HoistedArrayGuard& list : found.lists) {
        if (list.isAskedForItsStorageAlone)
            continue;
        noteListTestedInListReadsInLoops(walker, list);
        bool isView = !!classOfProvenViewInListReadsInLoops(walker, list.name);
        if (isView && list.subclass)
            noteInheritedSubclassLength(walker, list.name, list.subclass);
        else if (list.takesInstancesBelow)
            noteThatLengthOfViewIsFoundAbove(walker, list.name);
        else if (isView)
            markObjectPristine(walker, list.name);
    }
    for (const HoistedArrayGuard& list : found.lists) {
        std::optional<uint64_t> noted = list.isWritable ? findKnown(walker, KnownNote::LoopAppendsToLiteral, list.name, { }, inputHead.index()) : std::nullopt;
        if (!noted || *noted >> 32 != walker.currentFunction().index() || static_cast<uint32_t>(*noted) > entering.size())
            continue;
        Reference length = buildArrayLength(walker, list.name, origin);
        if (!length || !continueIfGuardPasses(walker, buildIntegerOperator(walker, Opcode::StrictEq, length, entering[static_cast<uint32_t>(*noted) - 1], origin), genericStart))
            return;
        noteKnown(walker, KnownNote::LengthEqualsInitialCounter, list.name, { }, inputHead.index(), slotsUnderInBoundsIndices, *noted);
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
    for (const Walker::HoistedRow& row : walker.hoistedRowsOf(inputHead)) {
        if (row.isRejected)
            continue;
        Reference name = row.row;
        if (size_t place = provenSpeculationLookup.find(guesses.first(std::min(guesses.size(), proofs.size())), name); place != notFound)
            name = proofs[place];
        Reference holder = row.holder;
        if (size_t place = provenSpeculationLookup.find(guesses.first(std::min(guesses.size(), proofs.size())), holder); place != notFound)
            holder = proofs[place];
        auto isTested = [&](Reference list) {
            return found.lists.containsIf([&](const HoistedArrayGuard& tested) { return tested.name == list; });
        };
        if (!isTested(name) || !isTested(holder))
            continue;
        for (const HoistedArrayGuard& list : found.lists) {
            if (!list.isWritable || isNumericStorage(list.storage) || isFreshAllocation(walker, list.name))
                continue;
            Draft isSame(Opcode::StrictEq, origin);
            walker.chooseConservativeFields(isSame);
            isSame.setLeftOperand(list.name);
            isSame.setRightOperand(holder);
            isSame.setForm(operatorFormFor(walker, isSame));
            Reference test = walker.build(WTF::move(isSame));
            if (!test || !test.isValue())
                return;
            BlockID differs = walker.addBlock();
            if (!genericStart)
                genericStart = walker.addBlock();
            walker.branch(test, genericStart, differs);
            walker.continueIn(differs);
            if (walker.isUnreachable())
                return;
        }
        noteKnown(walker, KnownNote::NumberReadOutOfList, holder, row.key, 0, LocationMasks(heapsUnderListsReadInLoops), name.bits());
        walker.count(StatisticID::ArrayRowsHoisted);
        if (!row.fieldReads.isEmpty())
            noteKnown(walker, KnownNote::HoistedFieldOfCopy, row.holder, { }, 0, LocationMasks(heapsUnderListsReadInLoops), holder.bits());
        for (Reference field : row.outerFields)
            noteKnown(walker, KnownNote::HoistedFieldOfCopy, field, { }, 0, LocationMasks(heapsUnderListsReadInLoops), field.bits());
    }
    compareWithAllocations(walker, inputHead, found, genericStart, origin);
    found.areTested = !walker.isUnreachable() && !walker.hasFailed();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
