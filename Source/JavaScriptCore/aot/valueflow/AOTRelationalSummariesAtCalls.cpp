/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelationalSummariesInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinCalls.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTFlowResults.h"
#include "AOTPropertyAccessElements.h"
#include "AOTTypeArena.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

// What becomes of one operand of one operation that enters code: a call, an access that finds an accessor, an operation that runs a protocol.

namespace {

constexpr OptionSet<ValueFate> fatesOfBeingCalled { ValueFate::IsCalled, ValueFate::IsConstructed };

class FatesBehindFlow final : public FatesOfParameters {
public:
    explicit FatesBehindFlow(const FlowResults& flow)
        : m_flow(flow)
    {
    }

    LinkedFate whole(FunctionID function, uint32_t entryParameter) const final { return m_flow.fateOfParameter(function, entryParameter); }
    LinkedFate apartFromCalledParameters(FunctionID function, uint32_t entryParameter) const final { return m_flow.fateApartFromCalledParameters(function, entryParameter); }
    bool listRows(FunctionID function, Vector<HandedToCalledParameter, 4>& rows) const final
    {
        bool isComplete = false;
        rows = m_flow.handedToCalledParameters(function, isComplete);
        return isComplete;
    }

private:
    const FlowResults& m_flow;
};

class FatesInLink final : public FatesOfParameters {
public:
    explicit FatesInLink(const RelationalSummaries& linked)
        : m_linked(linked)
    {
    }

    LinkedFate whole(FunctionID function, uint32_t entryParameter) const final { return m_linked.fateOfParameter(function, entryParameter); }
    LinkedFate apartFromCalledParameters(FunctionID function, uint32_t entryParameter) const final { return m_linked.fateApartFromCalledParameters(function, entryParameter); }
    bool listRows(FunctionID function, Vector<HandedToCalledParameter, 4>& rows) const final
    {
        rows.append(m_linked.handedToCalledParameters(function));
        return true;
    }

private:
    const RelationalSummaries& m_linked;
};

// The operands of one entry into functions by value, by the place where each stands. An accessor and a method that a protocol runs are entered in the same way.
struct OperandsByPlace {
    std::optional<uint32_t> called;
    std::optional<uint32_t> thisValue;
    std::optional<uint32_t> newTarget;
    std::optional<uint32_t> list;
    uint32_t firstArgument { 0 };
    uint32_t numberOfArguments { 0 };
    bool isConstruction { false };
};

struct Site {
    const Function& caller;
    const Value& user;
    OperandsByPlace operands;
    // The variadic operands are the entry parameters of the callee, in their order.
    bool passesEntryParameters { false };
};

struct OperandBehind {
    std::optional<uint32_t> operand;
    // False: the entry parameter is read through the operand. A capture is read out of the closure, an argument out of the list.
    bool isOperandItself { true };
};

// The operands of a call of a built-in, wherever they stand in the operation.
struct OperandsOfBuiltin {
    const Function* caller { nullptr };
    const Value* user { nullptr };
    std::optional<uint32_t> receiver;
    uint32_t firstArgument { 0 };
    uint32_t numberOfArguments { 0 };
    // The arguments are the elements of a list, and nobody knows how many there are.
    bool argumentsAreListed { false };
};

enum class Mention : uint8_t { None, Contents, Itself };

// How a place of a record speaks of an operand: the value that native code takes from the place is the operand, or something that it reads through the operand.
struct MentionInPlace {
    Mention mention { Mention::None };
    // The value is a new object that holds it.
    bool liesInNewObject { false };
    // The value is what a promise or a thenable at the place settles with. A job calls `then` when the call has long returned.
    bool isAwaited { false };
};

} // anonymous namespace

static OptionSet<ValueFate> withoutMasks(const LinkedFate& fate)
{
    OptionSet<ValueFate> fates = fate.itself | fate.contents;
    if (fate.results)
        fates.add(ValueFate::IsReturned);
    if (fate.storedIntoParameters || fate.storedIntoResults)
        fates.add(ValueFate::IsStoredInUnknownObject);
    return fates;
}

// "Returned" and the mask of the results go together, and whoever applies a fate relies on it.
static void normalize(LinkedFate& fate)
{
    if (fate.isAnything()) {
        fate = LinkedFate::anything();
        return;
    }
    if (fate.contents.contains(ValueFate::IsIncomplete))
        fate.contents = everyValueFate;
    if (fate.results)
        fate.itself.add(ValueFate::IsReturned);
    else if (fate.itself.contains(ValueFate::IsReturned))
        fate.results = UINT32_MAX;
}

static void addToContents(LinkedFate& fate, const LinkedFate& ofContents)
{
    fate.contents.add(withoutMasks(ofContents));
}

static void readAnyMember(LinkedFate& fate)
{
    fate.itself.add(ValueFate::HasMembersRead);
    fate.keysRead = KeysOfMembers::any();
}

static void addKey(const Program& program, KeysOfMembers& keys, BuiltinName name)
{
    PropertyKeyID key = name ? program.keyOf(name) : PropertyKeyID();
    if (key)
        keys.add(key);
    else
        keys = KeysOfMembers::any();
}

// Of a value that lies in an object which the entry makes and of which `holder` is said: the array of the rest arguments.
static LinkedFate fateOfWhatIsReadThrough(const LinkedFate& holder)
{
    if (holder.isAnything() || !isKeptNowhere(holder))
        return LinkedFate::anything();
    LinkedFate fate = LinkedFate::nothing();
    fate.itself = holder.contents;
    fate.contents = holder.contents;
    if (holder.contents.contains(ValueFate::HasMembersRead))
        fate.keysRead = KeysOfMembers::any();
    if (holder.contents.containsAny({ ValueFate::HasMembersWritten, ValueFate::HasMembersDeletedOrDefined }))
        fate.keysWritten = KeysOfMembers::any();
    normalize(fate);
    return fate;
}

// Nobody looks at what the callee answers: a setter.
static void dropResults(LinkedFate& fate)
{
    if (fate.isAnything())
        return;
    fate.itself.remove(ValueFate::IsReturned);
    fate.contents.remove(ValueFate::IsReturned);
    fate.results = 0;
    fate.storedIntoResults = 0;
}

static bool refusesEntry(const Function& callee, bool isConstruction)
{
    if (isConstruction)
        return callee.constructorForm() == ConstructorForm::NotConstructor;
    return callee.kind == FunctionKind::BaseConstructor || callee.kind == FunctionKind::DerivedConstructor;
}

// The activation of a coroutine outlives the call, with the closure that it was entered through and all that it was handed.
static bool isCoroutine(const Function& function)
{
    return function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
}

static std::optional<OperandsByPlace> operandsOfCallByValue(const Value& call)
{
    // A call through a table passes entry parameters to whichever entrypoint it finds.
    if (effectRuleOf(call.opcode) != EffectRule::Call || !Value::opcodesWithCallees.contains(call.opcode) || call.opcode == Opcode::CallTable)
        return std::nullopt;

    auto ifPresent = [&](std::optional<unsigned> position) -> std::optional<uint32_t> {
        if (!position || !call.operand(*position))
            return std::nullopt;
        return *position;
    };
    OperandsByPlace operands;
    operands.isConstruction = OperandPosition::newTarget(call.opcode).has_value();
    for (uint32_t index = 0; index < call.numberOfNamedOperands(); ++index) {
        if (operandUsesOf(call.opcode, index).containsAny(callingUses))
            operands.called = index;
    }
    operands.thisValue = ifPresent(OperandPosition::thisValue(call.opcode));
    operands.newTarget = ifPresent(OperandPosition::newTarget(call.opcode));
    operands.list = ifPresent(OperandPosition::list(call.opcode));
    if (VariadicOperandPosition::arguments(call.opcode)) {
        operands.firstArgument = call.numberOfNamedOperands();
        operands.numberOfArguments = call.numberOfVariadicOperands();
    }
    return operands;
}

static std::optional<PlaceAtCall> placeOf(const OperandsByPlace& operands, uint32_t operandIndex)
{
    auto place = [&](PlaceAtCall::Kind kind, uint32_t argument = 0) {
        return PlaceAtCall { kind, operands.isConstruction, argument };
    };
    if (operands.called == operandIndex)
        return place(PlaceAtCall::Kind::Callee);
    if (operands.thisValue == operandIndex)
        return place(PlaceAtCall::Kind::This);
    if (operands.newTarget == operandIndex)
        return place(PlaceAtCall::Kind::NewTarget);
    if (operands.list == operandIndex)
        return place(PlaceAtCall::Kind::ElementOfList);
    if (operandIndex >= operands.firstArgument && operandIndex - operands.firstArgument < operands.numberOfArguments)
        return place(PlaceAtCall::Kind::Argument, operandIndex - operands.firstArgument);
    return std::nullopt;
}

std::optional<PlaceAtCall> placeAtCall(const Value& call, uint32_t operandIndex)
{
    std::optional<OperandsByPlace> operands = operandsOfCallByValue(call);
    if (!operands)
        return std::nullopt;
    return placeOf(*operands, operandIndex);
}

Vector<EntryOfPlace, 4> entriesAt(const Function& callee, PlaceAtCall place)
{
    Vector<EntryOfPlace, 4> entries;
    auto addParameter = [&](std::optional<unsigned> parameter) {
        if (parameter)
            entries.append(EntryOfPlace { EntryOfPlace::Kind::Parameter, *parameter });
    };
    // Such a function finds every argument in `arguments`, whatever it has declared.
    bool findsEveryArgument = callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest });
    unsigned first = callee.firstParameterIndex();
    unsigned numberOfPositionalParameters = callee.numberOfPositionalParameters();
    auto addElementOfRest = [&] {
        if (callee.has(FunctionFlags::HasRestParameter))
            entries.append(EntryOfPlace { EntryOfPlace::Kind::ElementOfParameter, first + numberOfPositionalParameters });
    };

    switch (place.kind) {
    case PlaceAtCall::Kind::Callee:
        addParameter(callee.calleeParameterIndex());
        if (place.isConstruction)
            addParameter(callee.newTargetParameterIndex());
        break;
    case PlaceAtCall::Kind::This:
        addParameter(callee.thisParameterIndex());
        break;
    case PlaceAtCall::Kind::NewTarget:
        addParameter(callee.newTargetParameterIndex());
        break;
    case PlaceAtCall::Kind::Argument:
        if (findsEveryArgument)
            entries.append(EntryOfPlace { });
        else if (place.argument < numberOfPositionalParameters)
            addParameter(first + place.argument);
        else
            addElementOfRest();
        break;
    case PlaceAtCall::Kind::ElementOfList:
        if (findsEveryArgument) {
            entries.append(EntryOfPlace { });
            break;
        }
        for (unsigned index = 0; index < numberOfPositionalParameters; ++index)
            addParameter(first + index);
        addElementOfRest();
        break;
    }
    return entries;
}

std::optional<uint32_t> entryParameterAt(const Function& callee, PlaceAtCall place)
{
    Vector<EntryOfPlace, 4> entries = entriesAt(callee, place);
    if (entries.size() != 1 || entries[0].kind != EntryOfPlace::Kind::Parameter)
        return std::nullopt;
    return entries[0].parameter;
}

static OperandBehind operandBehind(const Site& site, const Function& callee, uint32_t entryParameter)
{
    if (entryParameter >= callee.numberOfEntryParameters())
        return { };
    if (site.passesEntryParameters) {
        uint32_t operand = site.user.numberOfNamedOperands() + entryParameter;
        if (operand >= site.user.numberOfOperands() || !site.user.operand(operand))
            return { };
        return { operand, true };
    }

    const OperandsByPlace& operands = site.operands;
    switch (callee.entryParameterKind(entryParameter)) {
    case EntryParameter::Capture:
        return { operands.called, false };
    case EntryParameter::Callee:
        return { operands.called, true };
    case EntryParameter::This:
        return { operands.thisValue, true };
    case EntryParameter::NewTarget:
        if (operands.newTarget)
            return { operands.newTarget, true };
        return operands.isConstruction ? OperandBehind { operands.called, true } : OperandBehind { };
    case EntryParameter::Parameter: {
        if (operands.list)
            return { operands.list, false };
        uint32_t argument = entryParameter - callee.firstParameterIndex();
        if (argument >= callee.numberOfPositionalParameters() || argument >= operands.numberOfArguments)
            return { };
        return { operands.firstArgument + argument, true };
    }
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayRunUnnamedCode(const BuiltinOverload& overload)
{
    if (overload.isUndeclared())
        return true;
    return overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
}

// Whether a record that names `named` speaks of `asked`. "The arguments from k on" is asked for the elements of a list.
static bool covers(BuiltinOperand named, BuiltinOperand asked)
{
    if (named == asked)
        return true;
    if (named.isReceiver() || asked.isReceiver())
        return false;
    if (asked.kind() == BuiltinOperand::Kind::Argument)
        return named.coversArgument(asked.argumentIndex());
    return named.kind() == BuiltinOperand::Kind::ArgumentsFrom || named.argumentIndex() >= asked.argumentIndex();
}

// A type that lists several things stands for a value that is one of them: an element or undefined, the initial value or what the callback answered last.
static void forEachValueNamedIn(const BuiltinTable& table, BuiltinTypeID id, unsigned depth, NOESCAPE const Invocable<void(const BuiltinTypePart*, bool)> auto& functor)
{
    if (!id || !table.contains(id))
        return;
    if (depth > 4) {
        functor(nullptr, true);
        return;
    }
    for (const BuiltinTypePart& part : table.at(id).parts) {
        switch (part.kind) {
        case BuiltinTypePartKind::Operand:
        case BuiltinTypePartKind::Slot:
        case BuiltinTypePartKind::IteratedValues:
        case BuiltinTypePartKind::Prototype:
        case BuiltinTypePartKind::CallbackResult:
            functor(&part, depth > 0);
            break;
        case BuiltinTypePartKind::Instance:
            for (const BuiltinSlotContents& contents : part.contents)
                forEachValueNamedIn(table, contents.type, depth + 1, functor);
            break;
        case BuiltinTypePartKind::InstanceOfReceiverClass:
        case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
        case BuiltinTypePartKind::Layout:
        case BuiltinTypePartKind::Builtin:
            break;
        }
    }
}

static MentionInPlace mentionIn(const BuiltinTable& table, const BuiltinPlace& place, BuiltinOperand asked)
{
    bool namesOperand = place.operand && covers(*place.operand, asked);
    switch (place.kind) {
    case BuiltinPlaceKind::Operand:
        return { namesOperand ? Mention::Itself : Mention::None, false, place.isSettled };
    case BuiltinPlaceKind::Slot:
    case BuiltinPlaceKind::IteratedValues:
    case BuiltinPlaceKind::OwnValues:
    case BuiltinPlaceKind::Prototype:
    case BuiltinPlaceKind::Member:
        return { namesOperand ? Mention::Contents : Mention::None, false, place.isSettled };
    case BuiltinPlaceKind::NewValue: {
        MentionInPlace found;
        found.isAwaited = place.isSettled;
        forEachValueNamedIn(table, place.newValueType, 0, [&](const BuiltinTypePart* part, bool liesInNewObject) {
            Mention mention = Mention::Itself;
            if (part) {
                if (part->kind == BuiltinTypePartKind::CallbackResult || !part->operand || !covers(*part->operand, asked))
                    return;
                mention = part->kind == BuiltinTypePartKind::Operand ? Mention::Itself : Mention::Contents;
                found.isAwaited |= part->isSettled;
            }
            found.mention = std::max(found.mention, mention);
            found.liesInNewObject |= liesInNewObject;
        });
        return found;
    }
    case BuiltinPlaceKind::None:
    case BuiltinPlaceKind::Result:
    case BuiltinPlaceKind::ResultSlot:
    case BuiltinPlaceKind::ResultField:
    case BuiltinPlaceKind::NativeMemory:
    case BuiltinPlaceKind::NativeCell:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The same for what a callback has answered: a fold hands it to the next call of the callback.
static MentionInPlace mentionOfCallbackResultIn(const BuiltinTable& table, const BuiltinPlace& place)
{
    MentionInPlace found;
    if (place.kind != BuiltinPlaceKind::NewValue)
        return found;
    forEachValueNamedIn(table, place.newValueType, 0, [&](const BuiltinTypePart* part, bool liesInNewObject) {
        if (part && part->kind != BuiltinTypePartKind::CallbackResult)
            return;
        found.mention = Mention::Itself;
        found.liesInNewObject |= liesInNewObject;
        found.isAwaited |= place.isSettled || (part && part->isSettled);
    });
    return found;
}

// What native code itself does with what the callback answers.
static OptionSet<ValueFate> fatesOfResultInNativeCode(const BuiltinCallback& callback)
{
    if (isCalledAfterReturn(callback.times))
        return everyValueFate;
    switch (callback.resultUse) {
    case BuiltinCallbackResultUse::Ignored:
    case BuiltinCallbackResultUse::Tested:
        return { };
    case BuiltinCallbackResultUse::Returned:
        return { ValueFate::IsReturned };
    case BuiltinCallbackResultUse::Stored:
        switch (callback.resultTo.kind) {
        case BuiltinPlaceKind::ResultSlot:
        case BuiltinPlaceKind::Slot:
            // A promise that is resolved with a thenable calls its `then`.
            if (callback.resultTo.slot == BuiltinSlots::PromiseValue)
                return everyValueFate;
            [[fallthrough]];
        case BuiltinPlaceKind::ResultField:
            return { ValueFate::IsStoredInUnknownObject };
        default:
            return everyValueFate;
        }
    case BuiltinCallbackResultUse::Escapes:
        return everyValueFate;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Calls functor(place in the record, where what lies there stands in the call that native code makes), as flow's passArguments() has it.
static void forEachPlacePassedTo(const BuiltinCallback& callback, BuiltinOperand asked, NOESCAPE const Invocable<void(const BuiltinPlace&, PlaceAtCall)> auto& functor)
{
    PlaceAtCall anywhere { PlaceAtCall::Kind::ElementOfList, callback.constructs, 0 };
    functor(callback.newTarget, PlaceAtCall { PlaceAtCall::Kind::NewTarget, true, 0 });
    functor(callback.thisValue, PlaceAtCall { PlaceAtCall::Kind::This, callback.constructs, 0 });
    bool positionIsKnown = true;
    for (uint32_t position = 0; position < callback.arguments.size(); ++position) {
        const BuiltinPlace& passed = callback.arguments[position];
        bool passesSeveral = passed.kind == BuiltinPlaceKind::Operand && !passed.isSettled && passed.operand->kind() == BuiltinOperand::Kind::ArgumentsFrom;
        if (!positionIsKnown)
            functor(passed, anywhere);
        else if (!passesSeveral)
            functor(passed, PlaceAtCall { PlaceAtCall::Kind::Argument, callback.constructs, position });
        else if (asked.kind() == BuiltinOperand::Kind::Argument && asked.argumentIndex() >= passed.operand->argumentIndex())
            functor(passed, PlaceAtCall { PlaceAtCall::Kind::Argument, callback.constructs, position + asked.argumentIndex() - passed.operand->argumentIndex() });
        else
            functor(passed, anywhere);
        positionIsKnown &= !passesSeveral;
    }
    functor(callback.spreadArguments, anywhere);
}

static std::optional<uint32_t> indexOf(const OperandsOfBuiltin& operands, BuiltinOperand operand)
{
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
        return operands.receiver;
    case BuiltinOperand::Kind::Argument:
        if (operand.argumentIndex() < operands.numberOfArguments)
            return operands.firstArgument + operand.argumentIndex();
        return std::nullopt;
    case BuiltinOperand::Kind::ArgumentsFrom:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The overloads that the call may take. What is proven of the operands rules out those that are written for other operands. Where nothing is known of the operands: every overload of the mode.
static void forEachOverloadThatCallMayTake(const Program& program, const OperandsOfBuiltin& operands, BuiltinID builtin, BuiltinCallMode mode, NOESCAPE const Invocable<void(const BuiltinOverload&)> auto& functor)
{
    const BuiltinTable& table = program.builtins();
    builtin = table.canonicalID(builtin);
    bool hasReachableOverload = false;
    if (operands.caller && !operands.argumentsAreListed) {
        Vector<ValueID, 8> arguments(operands.numberOfArguments, [&](size_t index) {
            return operands.user->operand(operands.firstArgument + index);
        });
        ScratchTypeArena scratch(program.types);
        TypingContext context { program, *operands.caller, scratch.arena() };
        BuiltinOperandTypes types = operandTypesOf(context, operands.receiver ? operands.user->operand(*operands.receiver) : ValueID(), arguments.span());
        forEachReachableOverload(OverloadQuery { context, builtin, mode, types, types }, [&](BuiltinOverloadID id) {
            hasReachableOverload = true;
            functor(table.at(id));
        });
    }
    if (hasReachableOverload)
        return;
    for (const BuiltinOverload& overload : table.overloadsOf(builtin)) {
        if (overload.mode == mode)
            functor(overload);
    }
}

namespace {

class FatesAtCalls {
public:
    FatesAtCalls(const Program& program, const FlowResults& flow, const FatesOfParameters& parameters)
        : m_program(program)
        , m_flow(flow)
        , m_parameters(parameters)
    {
    }

    LinkedFate ofOperand(const Function& caller, const Value& user, uint32_t operandIndex) const;
    LinkedFate ofOperandOfBuiltinAt(const Site&, BuiltinID, PlaceAtCall, Callbacks) const;

private:
    const Type& typeOf(const Site& site, uint32_t operandIndex) const { return m_program.typeOf(site.caller, site.user.operand(operandIndex)); }
    const Type& typeAt(const OperandsOfBuiltin&, BuiltinOperand) const;
    CalleeList calleesAt(const Site&) const;

    void putIntoTermsOf(const Site&, FunctionID, const Function& callee, LinkedFate&) const;
    void giveBackToCaller(LinkedFate&, const Function* closure, std::optional<uint32_t> operandOfClosure, OptionSet<ValueFate> resultOfCall) const;
    LinkedFate ofEntryParameter(const Site&, FunctionID callee, uint32_t entryParameter, std::optional<uint32_t> operandItself) const;
    LinkedFate ofEntry(const Site&, FunctionID callee, EntryOfPlace, std::optional<uint32_t> operandItself) const;
    std::optional<LinkedFate> followCalledParameters(const Site&, FunctionID, const Function& callee, uint32_t entryParameter, std::optional<uint32_t> operandItself, std::span<const HandedToCalledParameter>) const;
    std::optional<LinkedFate> ofValueHandedTo(const Type& called, std::optional<uint32_t> operandOfClosure, PlaceAtCall, OptionSet<ValueFate> resultOfCall) const;
    LinkedFate ofCalledValue(const Type& called, std::optional<uint32_t> operandOfClosure, bool isConstruction, OptionSet<ValueFate> resultOfCall) const;

    LinkedFate ofOperandOfCall(const Function& caller, const Value& call, uint32_t operandIndex) const;
    LinkedFate ofOperandOfCallByValue(const Site&, uint32_t operandIndex) const;
    LinkedFate ofOperandOfBuiltinCall(const Function& caller, const Value& call, uint32_t operandIndex) const;
    LinkedFate ofOperandOfAccess(const Function& caller, const Value& access, uint32_t operandIndex) const;
    LinkedFate ofOperandOfOperation(const Function& caller, const Value& user, uint32_t operandIndex) const;
    void enterGetters(const Function& caller, const Value& user, uint32_t operandIndex, LinkedFate&) const;

    LinkedFate ofOperandOfBuiltin(const OperandsOfBuiltin&, BuiltinID, BuiltinCallMode, BuiltinOperand, Callbacks) const;
    LinkedFate ofOperandOfOverload(const OperandsOfBuiltin&, const BuiltinOverload&, BuiltinOperand, Callbacks) const;
    LinkedFate ofListOfArguments(const OperandsOfBuiltin&, std::span<const BuiltinOverload>, BuiltinCallMode, Callbacks) const;
    OptionSet<ValueFate> fatesOfResultOf(const OperandsOfBuiltin&, const BuiltinCallback&, Callbacks) const;
    bool doRecordsSayWhatCallDoes(const OperandsOfBuiltin&, const BuiltinOverload&) const;
    bool storeMayRunCodeOf(const Type& object) const;
    bool mayRunCode(const Type& operand, OperandBehavior, Protocols) const;
    // Answer false where the operand gets into the hands of code that nobody knows.
    bool addProtocols(LinkedFate&, const OperandBehavior&, const Type& operand) const;
    void addUses(LinkedFate&, OperandUses) const;

    const Program& m_program;
    const FlowResults& m_flow;
    const FatesOfParameters& m_parameters;
};

} // anonymous namespace

const Type& FatesAtCalls::typeAt(const OperandsOfBuiltin& operands, BuiltinOperand operand) const
{
    if (!operands.caller)
        return m_program.at(TypeTable::anything);
    if (std::optional<uint32_t> index = indexOf(operands, operand))
        return m_program.typeOf(*operands.caller, operands.user->operand(*index));
    bool isAbsent = !operands.argumentsAreListed && operand.kind() != BuiltinOperand::Kind::ArgumentsFrom;
    return m_program.at(isAbsent ? TypeTable::undefined : TypeTable::anything);
}

// The stamp of flow. A pass that has found out more about the called value has put it into the type of the value.
CalleeList FatesAtCalls::calleesAt(const Site& site) const
{
    CalleeList callees = calleesOfCall(m_program, site.user);
    if (callees.isComplete || !site.operands.called)
        return callees;
    CalleeList calleesInType = calleesOf(m_program, typeOf(site, *site.operands.called));
    return calleesInType.isComplete ? calleesInType : callees;
}

// The masks of the results stay: what the callee answers is what the operation answers.
void FatesAtCalls::putIntoTermsOf(const Site& site, FunctionID calleeID, const Function& callee, LinkedFate& fate) const
{
    if (fate.isAnything())
        return;

    uint32_t operands = 0;
    for (uint32_t rest = fate.storedIntoParameters; rest; rest &= rest - 1) {
        uint32_t parameter = static_cast<uint32_t>(std::countr_zero(rest));
        // Whoever constructs makes the object and answers it. Nobody but the constructor has seen it, so it is the caller's own if the constructor has kept it nowhere.
        if (!site.passesEntryParameters && site.operands.isConstruction && callee.thisParameterIndex() == parameter) {
            LinkedFate ofNewObject = m_parameters.whole(calleeID, parameter);
            ofNewObject.itself.remove(ValueFate::IsReturned);
            ofNewObject.results = 0;
            if (isKeptNowhere(ofNewObject))
                fate.storedIntoResults |= 1;
            else
                fate.itself.add(ValueFate::IsStoredInUnknownObject);
            continue;
        }
        OperandBehind behind = operandBehind(site, callee, parameter);
        if (behind.operand && *behind.operand < 32) {
            operands |= 1u << *behind.operand;
            // A capture is read out of the closure and an argument out of the list, so a member of one lies deeper than the members of the operand.
            fate.mayBeStoredDeeper |= !behind.isOperandItself;
        } else
            fate.itself.add(ValueFate::IsStoredInUnknownObject);
    }
    fate.storedIntoParameters = operands;
    if (!operands)
        fate.mayBeStoredDeeper = false;
}

// Of a value that somebody below the operation hands to a closure which an operand of the operation holds: from the terms of the closure's function into those of the operation.
void FatesAtCalls::giveBackToCaller(LinkedFate& fate, const Function* closure, std::optional<uint32_t> operandOfClosure, OptionSet<ValueFate> resultOfCall) const
{
    if (fate.isAnything())
        return;

    uint32_t operands = 0;
    for (uint32_t rest = fate.storedIntoParameters; rest; rest &= rest - 1) {
        uint32_t parameter = static_cast<uint32_t>(std::countr_zero(rest));
        bool liesBehindClosure = false;
        bool isCapture = false;
        if (closure && parameter < closure->numberOfEntryParameters()) {
            EntryParameter kind = closure->entryParameterKind(parameter);
            isCapture = kind == EntryParameter::Capture;
            liesBehindClosure = isCapture || kind == EntryParameter::Callee;
        }
        if (liesBehindClosure && operandOfClosure && *operandOfClosure < 32) {
            operands |= 1u << *operandOfClosure;
            fate.mayBeStoredDeeper |= isCapture;
        } else
            fate.itself.add(ValueFate::IsStoredInUnknownObject);
    }
    fate.storedIntoParameters = operands;
    if (!operands)
        fate.mayBeStoredDeeper = false;

    bool isGivenBack = fate.results || fate.storedIntoResults || fate.itself.contains(ValueFate::IsReturned);
    bool contentsAreGivenBack = isGivenBack || fate.contents.contains(ValueFate::IsReturned);
    bool liesInWhatIsGivenBack = fate.storedIntoResults;
    fate.itself.remove(ValueFate::IsReturned);
    fate.contents.remove(ValueFate::IsReturned);
    fate.results = 0;
    fate.storedIntoResults = 0;
    if (contentsAreGivenBack)
        fate.contents.add(resultOfCall);
    if (!isGivenBack) {
        normalize(fate);
        return;
    }

    fate.itself.add(resultOfCall);
    if (resultOfCall.contains(ValueFate::HasMembersRead))
        fate.keysRead = KeysOfMembers::any();
    if (resultOfCall.containsAny({ ValueFate::HasMembersWritten, ValueFate::HasMembersDeletedOrDefined }))
        fate.keysWritten = KeysOfMembers::any();
    if (resultOfCall.contains(ValueFate::IsReturned)) {
        fate.results = UINT32_MAX;
        if (liesInWhatIsGivenBack)
            fate.storedIntoResults = UINT32_MAX;
    }
    normalize(fate);
}

// All that the functions do which the type lists, with the value that stands at the place when one of them is called. No answer where the type does not list them all.
std::optional<LinkedFate> FatesAtCalls::ofValueHandedTo(const Type& called, std::optional<uint32_t> operandOfClosure, PlaceAtCall place, OptionSet<ValueFate> resultOfCall) const
{
    CalleeList closures = calleesOf(m_program, called);
    if (!closures.isComplete)
        return std::nullopt;

    LinkedFate fate = LinkedFate::nothing();
    for (FunctionID id : closures.functions) {
        const Function& closure = m_program.at(id);
        if (refusesEntry(closure, place.isConstruction))
            continue;
        for (EntryOfPlace entry : entriesAt(closure, place)) {
            if (isCoroutine(closure))
                return std::nullopt;
            LinkedFate one = LinkedFate::anything();
            switch (entry.kind) {
            case EntryOfPlace::Kind::Unknown:
                break;
            case EntryOfPlace::Kind::Parameter:
                one = m_parameters.whole(id, entry.parameter);
                break;
            case EntryOfPlace::Kind::ElementOfParameter:
                one = fateOfWhatIsReadThrough(m_parameters.whole(id, entry.parameter));
                break;
            }
            giveBackToCaller(one, &closure, operandOfClosure, resultOfCall);
            fate.unite(one);
        }
    }

    OperandsOfBuiltin unknownOperands;
    unknownOperands.argumentsAreListed = true;
    BuiltinCallMode mode = place.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    for (BuiltinID builtin : closures.builtins) {
        LinkedFate one = LinkedFate::nothing();
        switch (place.kind) {
        case PlaceAtCall::Kind::Callee:
            break;
        case PlaceAtCall::Kind::This:
            one = ofOperandOfBuiltin(unknownOperands, builtin, mode, BuiltinOperand::receiver(), Callbacks::AreFollowed);
            break;
        case PlaceAtCall::Kind::Argument:
            if (place.argument <= BuiltinOperand::maxArgumentIndex) {
                one = ofOperandOfBuiltin(unknownOperands, builtin, mode, BuiltinOperand::argument(place.argument), Callbacks::AreFollowed);
                break;
            }
            [[fallthrough]];
        case PlaceAtCall::Kind::NewTarget:
        case PlaceAtCall::Kind::ElementOfList:
            one = LinkedFate::anything();
            break;
        }
        giveBackToCaller(one, nullptr, std::nullopt, resultOfCall);
        fate.unite(one);
    }
    if (fate.isAnything())
        return std::nullopt;
    return fate;
}

// Of a value that is called. A function that takes its callee gets hold of the closure it is entered through, and every function gets hold of what the closure has captured.
LinkedFate FatesAtCalls::ofCalledValue(const Type& called, std::optional<uint32_t> operandOfClosure, bool isConstruction, OptionSet<ValueFate> resultOfCall) const
{
    CalleeList closures = calleesOf(m_program, called);
    if (!closures.isComplete)
        return LinkedFate::anything();

    std::optional<LinkedFate> fate = ofValueHandedTo(called, operandOfClosure, PlaceAtCall { PlaceAtCall::Kind::Callee, isConstruction, 0 }, resultOfCall);
    if (!fate)
        return LinkedFate::anything();
    for (FunctionID id : closures.functions) {
        const Function& closure = m_program.at(id);
        if (refusesEntry(closure, isConstruction))
            continue;
        if (isCoroutine(closure))
            return LinkedFate::anything();
        for (uint32_t capture = 0; capture < closure.captures.size(); ++capture) {
            LinkedFate one = m_parameters.whole(id, capture);
            giveBackToCaller(one, &closure, operandOfClosure, resultOfCall);
            addToContents(*fate, one);
        }
    }
    normalize(*fate);
    return *fate;
}

// The callee, or one below it, calls what it was passed. This caller knows what it has passed.
std::optional<LinkedFate> FatesAtCalls::followCalledParameters(const Site& site, FunctionID calleeID, const Function& callee, uint32_t entryParameter, std::optional<uint32_t> operandItself, std::span<const HandedToCalledParameter> rows) const
{
    LinkedFate fate = m_parameters.apartFromCalledParameters(calleeID, entryParameter);
    if (fate.isAnything())
        return std::nullopt;
    putIntoTermsOf(site, calleeID, callee, fate);

    bool hasRowOfOwnCall = false;
    for (const HandedToCalledParameter& row : rows) {
        if (row.handed != entryParameter)
            continue;
        OperandBehind called = operandBehind(site, callee, row.called);
        if (!called.operand || !called.isOperandItself)
            return std::nullopt;
        const Type& typeOfCalled = typeOf(site, *called.operand);

        if (row.called == entryParameter && row.place.kind == PlaceAtCall::Kind::Callee) {
            if (!operandItself)
                return std::nullopt;
            hasRowOfOwnCall = true;
            fate.unite(ofCalledValue(typeOfCalled, called.operand, row.place.isConstruction, row.resultOfCall));
        } else {
            std::optional<LinkedFate> handed = ofValueHandedTo(typeOfCalled, called.operand, row.place, row.resultOfCall);
            if (!handed)
                return std::nullopt;
            if (row.isReadThroughHanded)
                addToContents(fate, *handed);
            else
                fate.unite(*handed);
        }
        if (fate.isAnything())
            return std::nullopt;
    }
    // Each call of a parameter has its row. Without one nobody has said what the function does with what the closure has captured.
    if (fate.itself.containsAny(fatesOfBeingCalled) && !hasRowOfOwnCall)
        return std::nullopt;
    normalize(fate);
    return fate;
}

// `operandItself`: the operand that is the entry parameter. None where the entry parameter is read through the operand that was asked about.
LinkedFate FatesAtCalls::ofEntryParameter(const Site& site, FunctionID calleeID, uint32_t entryParameter, std::optional<uint32_t> operandItself) const
{
    const Function& callee = m_program.at(calleeID);
    if (isCoroutine(callee))
        return LinkedFate::anything();
    Vector<HandedToCalledParameter, 4> rows;
    bool hasRow = m_parameters.listRows(calleeID, rows) && std::ranges::any_of(rows, [&](const HandedToCalledParameter& row) {
        return row.handed == entryParameter;
    });
    if (hasRow) {
        if (std::optional<LinkedFate> fate = followCalledParameters(site, calleeID, callee, entryParameter, operandItself, rows.span()))
            return *fate;
    }
    LinkedFate fate = m_parameters.whole(calleeID, entryParameter);
    putIntoTermsOf(site, calleeID, callee, fate);
    normalize(fate);
    return fate;
}

LinkedFate FatesAtCalls::ofEntry(const Site& site, FunctionID callee, EntryOfPlace entry, std::optional<uint32_t> operandItself) const
{
    switch (entry.kind) {
    case EntryOfPlace::Kind::Unknown:
        return LinkedFate::anything();
    case EntryOfPlace::Kind::Parameter:
        return ofEntryParameter(site, callee, entry.parameter, operandItself);
    case EntryOfPlace::Kind::ElementOfParameter:
        return fateOfWhatIsReadThrough(ofEntryParameter(site, callee, entry.parameter, std::nullopt));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LinkedFate FatesAtCalls::ofOperandOfCallByValue(const Site& site, uint32_t operandIndex) const
{
    const OperandsByPlace& operands = site.operands;
    std::optional<PlaceAtCall> place = placeOf(operands, operandIndex);
    if (!place)
        return LinkedFate::anything();
    // A bound function puts its own arguments in front of these and its own `this` in the place of this one.
    if (AccessSiteID accessSite = site.user.accessSite(); accessSite && m_flow.mayCallBoundFunction(accessSite))
        return LinkedFate::anything();
    CalleeList callees = calleesAt(site);
    if (!callees.isComplete)
        return LinkedFate::anything();

    bool isCalledValue = place->kind == PlaceAtCall::Kind::Callee;
    bool isList = place->kind == PlaceAtCall::Kind::ElementOfList;
    LinkedFate fate = LinkedFate::nothing();
    if (isCalledValue)
        fate.itself.add(operands.isConstruction ? ValueFate::IsConstructed : ValueFate::IsCalled);
    if (isList)
        readAnyMember(fate);
    // Whoever constructs reads `prototype` out of new.target, and the new object holds what is found there.
    bool isNewTarget = place->kind == PlaceAtCall::Kind::NewTarget || (isCalledValue && operands.isConstruction && !operands.newTarget);
    if (isNewTarget) {
        fate.itself.add(ValueFate::HasMembersRead);
        addKey(m_program, fate.keysRead, BuiltinNames::prototype);
        fate.contents.add(ValueFate::IsStoredInUnknownObject);
    }

    for (FunctionID id : callees.functions) {
        const Function& callee = m_program.at(id);
        if (refusesEntry(callee, operands.isConstruction))
            continue;
        for (EntryOfPlace entry : entriesAt(callee, *place)) {
            // A construction that names its new.target does not pass the called value as that.
            if (isCalledValue && !isNewTarget && callee.newTargetParameterIndex() == entry.parameter)
                continue;
            LinkedFate one = ofEntry(site, id, entry, isList ? std::nullopt : std::optional<uint32_t>(operandIndex));
            if (isList)
                addToContents(fate, one);
            else
                fate.unite(one);
        }
        if (isCalledValue) {
            if (isCoroutine(callee))
                return LinkedFate::anything();
            for (uint32_t capture = 0; capture < callee.captures.size(); ++capture)
                addToContents(fate, ofEntryParameter(site, id, capture, std::nullopt));
        }
        if (fate.isAnything())
            return LinkedFate::anything();
    }
    for (BuiltinID builtin : callees.builtins)
        fate.unite(ofOperandOfBuiltinAt(site, builtin, *place, Callbacks::AreFollowed));
    normalize(fate);
    return fate;
}

// The call of a ReturnCall is a call like any other: what it answers is what the caller answers, which whoever follows the result of the call sees.
LinkedFate FatesAtCalls::ofOperandOfCall(const Function& caller, const Value& call, uint32_t operandIndex) const
{
    // A saved frame holds what is live in it.
    if (maySuspend(call.opcode))
        return LinkedFate::anything();
    if (std::optional<OperandsByPlace> operands = operandsOfCallByValue(call))
        return ofOperandOfCallByValue(Site { caller, call, *operands, false }, operandIndex);

    if (call.opcode != Opcode::CallDirect || !call.function())
        return LinkedFate::anything();
    std::optional<uint32_t> entryParameter = entryParameterOf(m_program.at(call.function()), call, operandIndex);
    if (!entryParameter)
        return LinkedFate::anything();
    return ofEntryParameter(Site { caller, call, { }, true }, call.function(), *entryParameter, operandIndex);
}

bool FatesAtCalls::mayRunCode(const Type& operand, OperandBehavior behavior, Protocols protocols) const
{
    behavior.uses = (behavior.uses & OperandUses { OperandUse::AppliesTransitively, OperandUse::AppliesToContents }) | OperandUse::Inspects;
    behavior.protocols = protocols;
    EffectDescription description;
    describeOperandCode(m_program, operand, behavior, description);
    return description.callsUnknownCode || !description.functions.isEmpty();
}

// A protocol that only reads is said as what it reads: whether an accessor is there depends on the object, which whoever has made the object knows.
bool FatesAtCalls::addProtocols(LinkedFate& fate, const OperandBehavior& behavior, const Type& operand) const
{
    constexpr Protocols readingProtocols { Protocol::OwnKeys, Protocol::OwnValues, Protocol::ToPrimitive, Protocol::Iterate, Protocol::PrototypeChain, Protocol::Named };
    constexpr Protocols callingProtocols { Protocol::Thenable, Protocol::Species, Protocol::ToJSON, Protocol::HasInstance, Protocol::CallsNamed };

    Protocols protocols = behavior.protocols;
    if (protocols.isEmpty())
        return true;
    // "Every protocol" is written with every bit of the word set.
    if (!(readingProtocols | callingProtocols).containsAll(protocols))
        return false;

    OptionSet<ValueFate> fates;
    if (protocols.contains(Protocol::OwnKeys))
        fates.add(ValueFate::HasKeysEnumerated);
    if (protocols.contains(Protocol::ToPrimitive))
        fates.add(ValueFate::IsConvertedToPrimitive);
    if (protocols.contains(Protocol::Iterate))
        fates.add(ValueFate::IsIterated);
    if (protocols.containsAny({ Protocol::OwnValues, Protocol::PrototypeChain, Protocol::Named }))
        fates.add(ValueFate::HasMembersRead);

    bool runsOnOperand = !behavior.uses.contains(OperandUse::AppliesToContents);
    bool runsOnContents = behavior.uses.containsAny({ OperandUse::AppliesToContents, OperandUse::AppliesTransitively });
    if (runsOnContents) {
        readAnyMember(fate);
        fate.contents.add(fates);
    }
    if (runsOnOperand) {
        fate.itself.add(fates);
        // "This field is always filled" is a fact about the reads that flow has seen. Nothing says that flow has seen a read under each name that a record lists, so the names are not passed on.
        if (protocols.containsAny({ Protocol::OwnValues, Protocol::Named }))
            fate.keysRead = KeysOfMembers::any();
    }
    // What is read under a name is converted where the operand is.
    if (protocols.containsAll({ Protocol::Named, Protocol::ToPrimitive }) && behavior.namedMemberUse == NamedMembers::AreOnlyRead)
        fate.contents.add(ValueFate::IsConvertedToPrimitive);

    Protocols calling = protocols & callingProtocols;
    if (calling.isEmpty() || !mayBeReference(operand))
        return true;
    // A job calls `then` when the operation has long returned.
    if (calling.contains(Protocol::Thenable) && (runsOnContents || protocolMayFindMember(m_program, operand, Protocol::Thenable)))
        return false;
    return !mayRunCode(operand, behavior, calling);
}

// The uses that say what is done to the operand. Where it goes is said by whoever knows the places.
void FatesAtCalls::addUses(LinkedFate& fate, OperandUses uses) const
{
    if (uses.contains(OperandUse::ReadsContents))
        readAnyMember(fate);
    if (uses.containsAny({ OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::AssignsFrom })) {
        fate.itself.add(ValueFate::HasMembersWritten);
        fate.keysWritten = KeysOfMembers::any();
    }
    if (uses.containsAny({ OperandUse::DeletesByKey, OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype })) {
        fate.itself.add(ValueFate::HasMembersDeletedOrDefined);
        fate.keysWritten = KeysOfMembers::any();
    }
    // Every object below it finds its members from then on.
    if (uses.contains(OperandUse::BecomesPrototype))
        fate.itself.add(ValueFate::IsStoredInUnknownObject);
}

static bool mayReturnUnknownObject(const BuiltinType& type)
{
    if (type.isAnything() || type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction }))
        return true;
    return type.freshness == BuiltinFreshness::No && std::ranges::contains(type.parts, BuiltinTypePartKind::Instance, &BuiltinTypePart::kind);
}

static bool putsValueInto(const BuiltinTable& table, const BuiltinHostUses& uses, BuiltinID entry)
{
    return std::ranges::any_of(uses.list, [&](const BuiltinHostUse& use) {
        if (use.use == BuiltinHostReadUse::IsValueOf)
            return table.canonicalID(use.builtin) == entry;
        return use.use == BuiltinHostReadUse::Called && use.call && putsValueInto(table, use.call->result, entry);
    });
}

// Flow does not go by the records alone, and what it goes by in their place is written nowhere that could be read here. Wherever it does that, nothing is said.
bool FatesAtCalls::doRecordsSayWhatCallDoes(const OperandsOfBuiltin& operands, const BuiltinOverload& overload) const
{
    const BuiltinTable& table = m_program.builtins();
    BuiltinID builtin = table.canonicalID(overload.builtin);
    const Builtin& entry = table.at(builtin);
    bool isConstruction = overload.mode == BuiltinCallMode::Construct;

    // A model, or the function of the library where the records leave an operand or the result open, is entered with the operands, and the records are not read.
    if ((overload.code.model && !isConstruction) || (!doRecordsDescribe(table, overload) && overload.libraryFunction()))
        return false;
    // The entry holds what code of the library has made, and a call of the entry enters that.
    bool holdsValueFromLibrary = std::ranges::any_of(table.calledFunctionsOf(builtin), [&](const BuiltinCalledFunction& called) {
        return putsValueInto(table, called.uses, builtin);
    });
    if (holdsValueFromLibrary)
        return false;
    // Whoever holds the proxy reaches the target and the handler.
    if (builtin == table.canonicalID(Builtins::Proxy) || builtin == table.canonicalID(Builtins::ProxyRevocable))
        return false;
    // next, return and throw of a built-in iterator or a generator: what is sent goes into the frame of the generator.
    bool isNamedLikeStep = entry.name == BuiltinNames::next || entry.name == BuiltinNames::returnKeyword || entry.name == BuiltinNames::throwKeyword;
    if (isNamedLikeStep && entry.holder && table.at(entry.holder).kind == BuiltinKind::Prototype)
        return false;
    // The overloads of such an entry say what it does with its holder as `this`. Array.from, Array.of and Promise.resolve construct a class of the program that they find there.
    if (entry.flags.contains(BuiltinFlag::RequiresHolderAsThis) && !isConstruction) {
        const Type& receiver = typeAt(operands, BuiltinOperand::receiver());
        bool mayBeOtherObject = receiver.anyObject || receiver.anyFunction || receiver.layouts || receiver.environment || receiver.opaque || !receiver.classes.isEmpty() || !receiver.functions.isEmpty();
        bool mayBeOtherBuiltin = std::ranges::any_of(receiver.builtins, [&](BuiltinID object) {
            return !entry.holder || table.canonicalID(object) != table.canonicalID(entry.holder);
        });
        if (mayBeOtherObject || mayBeOtherBuiltin || (receiver.primitives & ~Primitive::Undefined))
            return false;
    }
    return true;
}

bool FatesAtCalls::storeMayRunCodeOf(const Type& object) const
{
    EffectDescription description;
    describeOperandCode(m_program, object, OperandBehavior { .uses = OperandUse::StoresByKey, .protocols = { }, .declarer = OperandDeclarer::Builtin }, description);
    return description.callsUnknownCode || !description.functions.isEmpty();
}

// All that becomes of what the callback answers. Where native code hands it to the callback again, the function of the closure says what becomes of it then, and may return it once more.
OptionSet<ValueFate> FatesAtCalls::fatesOfResultOf(const OperandsOfBuiltin& operands, const BuiltinCallback& callback, Callbacks callbacks) const
{
    const BuiltinTable& table = m_program.builtins();
    OptionSet<ValueFate> fates = fatesOfResultInNativeCode(callback);
    Vector<PlaceAtCall, 2> placesOfResult;
    bool isLost = false;
    forEachPlacePassedTo(callback, callback.callee, [&](const BuiltinPlace& passed, PlaceAtCall place) {
        auto [mention, liesInNewObject, isAwaited] = mentionOfCallbackResultIn(table, passed);
        if (mention == Mention::None)
            return;
        isLost |= liesInNewObject || isAwaited;
        placesOfResult.append(place);
    });
    if (placesOfResult.isEmpty())
        return fates;
    if (isLost || callbacks == Callbacks::AreLeftOut)
        return everyValueFate;

    while (!fates.contains(ValueFate::IsIncomplete)) {
        OptionSet<ValueFate> grown = fates;
        for (PlaceAtCall place : placesOfResult) {
            std::optional<LinkedFate> handed = ofValueHandedTo(typeAt(operands, callback.callee), indexOf(operands, callback.callee), place, fates);
            if (!handed)
                return everyValueFate;
            grown.add(withoutMasks(*handed));
        }
        if (grown == fates)
            return fates;
        fates = grown;
    }
    return everyValueFate;
}

LinkedFate FatesAtCalls::ofOperandOfOverload(const OperandsOfBuiltin& operands, const BuiltinOverload& overload, BuiltinOperand asked, Callbacks callbacks) const
{
    // An operand of which the table says nothing has every use.
    constexpr OperandUses usesOfUndescribedOperand { OperandUse::Retains, OperandUse::Returns, OperandUse::PassesToUserCode, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype };

    const BuiltinTable& table = m_program.builtins();
    if (mayRunUnnamedCode(overload) || !doRecordsSayWhatCallDoes(operands, overload))
        return LinkedFate::anything();
    const BuiltinParameter* parameter = table.parameterFor(overload, asked.isReceiver() ? asked : BuiltinOperand::argument(asked.argumentIndex()));
    // The receiver of a construction is new.target. A declared overload that says nothing of it does no more with it than every construction does.
    if (!parameter)
        return asked.isReceiver() && overload.mode != BuiltinCallMode::Construct ? LinkedFate::anything() : LinkedFate::nothing();
    OperandUses uses = parameter->uses;
    if (uses.containsAll(usesOfUndescribedOperand))
        return LinkedFate::anything();

    // Each use of the table comes with a record that says where the value goes. The records are followed, and a use that no record accounts for counts as the worst that it can mean.
    LinkedFate fate = LinkedFate::nothing();
    bool isKeptByRecord = false;
    bool isReturnedByRecord = false;
    bool isPassedByRecord = false;
    bool contentsGoSomewhereByRecord = false;

    auto noteMention = [&](Mention mention) {
        if (mention == Mention::Contents) {
            contentsGoSomewhereByRecord = true;
            readAnyMember(fate);
        }
    };
    auto keep = [&](Mention mention) {
        if (mention == Mention::Contents) {
            fate.contents.add(ValueFate::IsStoredInUnknownObject);
            return;
        }
        fate.itself.add(ValueFate::IsStoredInUnknownObject);
        isKeptByRecord = true;
    };
    auto loseToUnknownCode = [&](Mention mention) {
        if (mention == Mention::Contents)
            fate.contents = everyValueFate;
        else
            fate = LinkedFate::anything();
    };
    auto storeInto = [&](Mention mention, BuiltinOperand target) {
        if (mention == Mention::Contents) {
            // What moves inside one object is still reached through that object alone.
            if (!covers(target, asked))
                fate.contents.add(ValueFate::IsStoredInUnknownObject);
            return;
        }
        isKeptByRecord = true;
        std::optional<uint32_t> index = indexOf(operands, target);
        if (index && *index < 32)
            fate.storedIntoParameters |= 1u << *index;
        else
            fate.itself.add(ValueFate::IsStoredInUnknownObject);
    };
    auto storeIntoResult = [&](Mention mention) {
        if (mention == Mention::Contents) {
            fate.contents.add(ValueFate::IsStoredInUnknownObject);
            return;
        }
        isKeptByRecord = true;
        fate.storedIntoResults |= 1;
    };
    auto goTo = [&](Mention mention, const BuiltinPlace& to, bool liesInNewValue) {
        switch (to.kind) {
        case BuiltinPlaceKind::Result:
            if (liesInNewValue)
                storeIntoResult(mention);
            if (mention == Mention::Contents)
                fate.contents.add(ValueFate::IsReturned);
            else {
                fate.results |= 1;
                isReturnedByRecord = true;
            }
            return;
        case BuiltinPlaceKind::ResultSlot:
        case BuiltinPlaceKind::ResultField:
            storeIntoResult(mention);
            return;
        case BuiltinPlaceKind::Slot:
            // A promise that is resolved with a thenable calls its `then`.
            if (to.operand && !liesInNewValue && to.slot != BuiltinSlots::PromiseValue) {
                storeInto(mention, *to.operand);
                return;
            }
            break;
        case BuiltinPlaceKind::None:
            return;
        default:
            break;
        }
        // Memory and cells of native code are unknown hands for good: whatever lies there is handed out again, as `error.cause` is. So flow's forEachTargetAt() has it.
        loseToUnknownCode(mention);
    };

    for (const BuiltinTransfer& transfer : overload.flow.transfers) {
        auto [mention, liesInNewObject, isAwaited] = mentionIn(table, transfer.from, asked);
        if (mention == Mention::None)
            continue;
        noteMention(mention);
        if (isAwaited || (transfer.to.kind == BuiltinPlaceKind::ResultSlot && transfer.to.slot == BuiltinSlots::PromiseValue))
            loseToUnknownCode(mention);
        else
            goTo(mention, transfer.to, liesInNewObject);
        if (fate.isAnything())
            return LinkedFate::anything();
    }

    for (const BuiltinCallback& callback : overload.flow.callbacks) {
        bool runsLater = isCalledAfterReturn(callback.times);
        OptionSet<ValueFate> resultOfCall = fatesOfResultOf(operands, callback, callbacks);
        if (covers(callback.callee, asked)) {
            fate.itself.add(callback.constructs ? ValueFate::IsConstructed : ValueFate::IsCalled);
            if (runsLater)
                keep(Mention::Itself);
            if (callbacks == Callbacks::AreFollowed)
                fate.unite(ofCalledValue(typeAt(operands, asked), indexOf(operands, asked), callback.constructs, resultOfCall));
        }
        forEachPlacePassedTo(callback, asked, [&](const BuiltinPlace& passed, PlaceAtCall place) {
            auto [mention, liesInNewObject, isAwaited] = mentionIn(table, passed, asked);
            if (mention == Mention::None || fate.isAnything())
                return;
            if (mention == Mention::Itself)
                isPassedByRecord = true;
            noteMention(mention);
            if (runsLater)
                keep(mention);
            // The closure finds the value in an object that native code has made for it.
            if (liesInNewObject || isAwaited) {
                loseToUnknownCode(mention);
                return;
            }
            if (callbacks == Callbacks::AreLeftOut)
                return;
            std::optional<LinkedFate> handed = ofValueHandedTo(typeAt(operands, callback.callee), indexOf(operands, callback.callee), place, resultOfCall);
            if (!handed)
                loseToUnknownCode(mention);
            else if (mention == Mention::Contents)
                addToContents(fate, *handed);
            else
                fate.unite(*handed);
        });
        if (fate.isAnything())
            return LinkedFate::anything();
    }

    // The function is a member of an operand or lies in a cell of native code. Nobody here knows it.
    for (const BuiltinHandlerCall& handler : overload.flow.handlerCalls) {
        auto lose = [&](const BuiltinPlace& place) {
            Mention mention = mentionIn(table, place, asked).mention;
            if (mention == Mention::None || fate.isAnything())
                return;
            noteMention(mention);
            loseToUnknownCode(mention);
        };
        lose(handler.callee);
        lose(handler.thisValue);
        for (const BuiltinPlace& argument : handler.arguments)
            lose(argument);
        if (fate.isAnything())
            return LinkedFate::anything();
    }

    for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
        auto [value, valueLiesInNewObject, valueIsAwaited] = mentionIn(table, action.value, asked);
        switch (action.kind) {
        case BuiltinPropertyActionKind::ReadsByKey:
        case BuiltinPropertyActionKind::ReadsGetter:
        case BuiltinPropertyActionKind::ReadsSetter:
            // `value` is where what was read goes.
            if (covers(action.object, asked)) {
                noteMention(Mention::Contents);
                goTo(Mention::Contents, action.value, false);
            }
            break;
        case BuiltinPropertyActionKind::StoresByKey:
        case BuiltinPropertyActionKind::DefinesName:
        case BuiltinPropertyActionKind::DefinesGetter:
        case BuiltinPropertyActionKind::DefinesSetter:
        case BuiltinPropertyActionKind::AssignsFrom:
        case BuiltinPropertyActionKind::DefinesFrom:
            if (value == Mention::None)
                break;
            noteMention(value);
            // A setter or a trap of the object takes what is stored.
            if (valueIsAwaited || storeMayRunCodeOf(typeAt(operands, action.object)))
                loseToUnknownCode(value);
            else if (valueLiesInNewObject)
                keep(value);
            else
                storeInto(value, action.object);
            // A typed array converts what is stored into it.
            (value == Mention::Itself ? fate.itself : fate.contents).add(ValueFate::IsConvertedToPrimitive);
            break;
        case BuiltinPropertyActionKind::TakesDescriptor:
        case BuiltinPropertyActionKind::TakesDescriptorMap:
            // What the members of the descriptor hold goes into the object. The descriptor itself goes nowhere.
            if (value != Mention::None) {
                noteMention(Mention::Contents);
                keep(Mention::Contents);
            }
            break;
        case BuiltinPropertyActionKind::ChangesPrototype:
            if (value != Mention::None) {
                noteMention(value);
                keep(value);
            }
            break;
        case BuiltinPropertyActionKind::BecomesPrototype:
            if (covers(action.object, asked))
                keep(Mention::Itself);
            break;
        case BuiltinPropertyActionKind::DescribesOwnProperties:
        case BuiltinPropertyActionKind::DescribesResult:
            if (covers(action.object, asked)) {
                noteMention(Mention::Contents);
                fate.itself.add(ValueFate::HasKeysEnumerated);
                keep(Mention::Contents);
            }
            break;
        case BuiltinPropertyActionKind::TestsByKey:
            if (covers(action.object, asked))
                readAnyMember(fate);
            break;
        case BuiltinPropertyActionKind::DeletesByKey:
        case BuiltinPropertyActionKind::ChangesIntegrity:
            break;
        }
        if (action.key.kind == BuiltinKeyKind::ElementsOf && action.key.operand && covers(*action.key.operand, asked))
            readAnyMember(fate);
    }

    // An iterator holds what it walks over, and a new object the prototype it was given.
    for (std::optional<BuiltinOperand> held : { overload.result.iteratedOperand, overload.result.prototypeSource, overload.result.typedResultSource }) {
        if (held && covers(*held, asked))
            storeIntoResult(Mention::Itself);
    }

    if (fate.isAnything())
        return LinkedFate::anything();
    // The built-in may return what a member of the operand holds, and a result that nobody knows stands only for what is in unknown hands. So flow's applyParameter() has it.
    if (parameter->protocols.containsAny({ Protocol::Named, Protocol::CallsNamed }) && mayReturnUnknownObject(table.at(overload.result.type))) {
        readAnyMember(fate);
        fate.contents = everyValueFate;
    }

    if (uses.containsAny(callingUses) && !fate.itself.containsAny(fatesOfBeingCalled))
        return LinkedFate::anything();
    if (uses.contains(OperandUse::PassesToUserCode) && !isPassedByRecord)
        return LinkedFate::anything();
    if (uses.contains(OperandUse::Retains) && !isKeptByRecord)
        fate.itself.add(ValueFate::IsStoredInUnknownObject);
    if (uses.contains(OperandUse::Returns) && !isReturnedByRecord)
        fate.results = UINT32_MAX;
    if (uses.contains(OperandUse::RetainsContents) && !contentsGoSomewhereByRecord)
        fate.contents.add(ValueFate::IsStoredInUnknownObject);
    addUses(fate, uses);
    // What is said of what the operand holds is said of all that is reached from there. `contents` means that already.

    if (!addProtocols(fate, declaredBehaviorOf(overload, asked), typeAt(operands, asked)))
        return LinkedFate::anything();
    normalize(fate);
    return fate;
}

LinkedFate FatesAtCalls::ofOperandOfBuiltin(const OperandsOfBuiltin& operands, BuiltinID builtin, BuiltinCallMode mode, BuiltinOperand asked, Callbacks callbacks) const
{
    LinkedFate fate = LinkedFate::nothing();
    forEachOverloadThatCallMayTake(m_program, operands, builtin, mode, [&](const BuiltinOverload& overload) {
        if (!fate.isAnything())
            fate.unite(ofOperandOfOverload(operands, overload, asked, callbacks));
    });
    return fate;
}

// Each element may be any argument.
LinkedFate FatesAtCalls::ofListOfArguments(const OperandsOfBuiltin& operands, std::span<const BuiltinOverload> overloads, BuiltinCallMode mode, Callbacks callbacks) const
{
    LinkedFate fate = LinkedFate::nothing();
    readAnyMember(fate);
    for (const BuiltinOverload& overload : overloads) {
        if (overload.mode != mode)
            continue;
        // Also where the overload declares no parameter: what is sent to a generator goes into its frame.
        if (mayRunUnnamedCode(overload) || !doRecordsSayWhatCallDoes(operands, overload)) {
            fate.contents = everyValueFate;
            break;
        }
        for (const BuiltinParameter& parameter : overload.parameters)
            addToContents(fate, ofOperandOfOverload(operands, overload, parameter.operand, callbacks));
    }
    normalize(fate);
    return fate;
}

LinkedFate FatesAtCalls::ofOperandOfBuiltinAt(const Site& site, BuiltinID builtin, PlaceAtCall place, Callbacks callbacks) const
{
    const BuiltinTable& table = m_program.builtins();
    OperandsOfBuiltin operands { &site.caller, &site.user, site.operands.thisValue, site.operands.firstArgument, site.operands.numberOfArguments, site.operands.list.has_value() };
    BuiltinCallMode mode = place.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    // forEachCallbackOfBuiltin() lists nothing for a call with a list: the callback is an element that has no operand of its own. So nothing may be left out here, and nobody knows the callback.
    if (operands.argumentsAreListed)
        callbacks = Callbacks::AreFollowed;
    switch (place.kind) {
    case PlaceAtCall::Kind::Callee:
        // Native code never takes its callee. Without the operand the callee is new.target.
        if (!place.isConstruction || site.operands.newTarget)
            return LinkedFate::nothing();
        [[fallthrough]];
    case PlaceAtCall::Kind::NewTarget:
        // The table has new.target as the receiver of a construction.
        operands.receiver = site.operands.newTarget ? site.operands.newTarget : site.operands.called;
        return ofOperandOfBuiltin(operands, builtin, BuiltinCallMode::Construct, BuiltinOperand::receiver(), callbacks);
    case PlaceAtCall::Kind::This:
        return ofOperandOfBuiltin(operands, builtin, mode, BuiltinOperand::receiver(), callbacks);
    case PlaceAtCall::Kind::Argument:
        if (place.argument > BuiltinOperand::maxArgumentIndex)
            return LinkedFate::anything();
        return ofOperandOfBuiltin(operands, builtin, mode, BuiltinOperand::argument(place.argument), callbacks);
    case PlaceAtCall::Kind::ElementOfList:
        return ofListOfArguments(operands, table.overloadsOf(table.canonicalID(builtin)), mode, callbacks);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LinkedFate FatesAtCalls::ofOperandOfBuiltinCall(const Function& caller, const Value& call, uint32_t operandIndex) const
{
    static_assert(numberOfNamedOperandsOf(Opcode::CallBuiltin) == 1 && numberOfNamedOperandsOf(Opcode::CallInline) == 1);

    const BuiltinTable& table = m_program.builtins();
    const BuiltinOverload& overload = table.at(call.overload());
    std::optional<uint32_t> argument = call.variadicOperandIndex(operandIndex);
    if (argument && *argument > BuiltinOperand::maxArgumentIndex)
        return LinkedFate::anything();
    BuiltinOperand asked = argument ? BuiltinOperand::argument(*argument) : BuiltinOperand::receiver();

    OperandsOfBuiltin operands { &caller, &call, std::nullopt, 1, call.numberOfVariadicOperands(), false };
    if (call.operand(0))
        operands.receiver = 0;

    LinkedFate fate;
    switch (builtinCallOf(m_program, call).form) {
    case BuiltinCallForm::AnyReceiver:
        // The function runs with a receiver that no overload was matched against, so it may behave as any of them.
        if (!asked.isReceiver())
            fate = ofOperandOfBuiltin(operands, overload.builtin, overload.mode, asked, Callbacks::AreFollowed);
        return fate;
    case BuiltinCallForm::List:
        operands.numberOfArguments = 0;
        operands.argumentsAreListed = true;
        if (!asked.isReceiver())
            return ofListOfArguments(operands, std::span(&overload, 1), overload.mode, Callbacks::AreFollowed);
        return ofOperandOfOverload(operands, overload, asked, Callbacks::AreFollowed);
    case BuiltinCallForm::Whole:
        return ofOperandOfOverload(operands, overload, asked, Callbacks::AreFollowed);
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
        break;
    }
    // The parts of what the built-in would have answered are the results.
    fate = ofOperandOfOverload(operands, overload, asked, Callbacks::AreFollowed);
    if (!fate.isAnything() && (fate.results || fate.storedIntoResults)) {
        fate.results = UINT32_MAX;
        fate.storedIntoResults = UINT32_MAX;
    }
    normalize(fate);
    return fate;
}

// Where all that the access may find is proven, the answer is what the accessors among it do: the reader sees the access itself.
LinkedFate FatesAtCalls::ofOperandOfAccess(const Function& caller, const Value& access, uint32_t operandIndex) const
{
    std::optional<uint32_t> receiver = receiverOf(access.opcode);
    std::optional<uint32_t> stored = storedOf(access.opcode);
    Access kind = accessOf(access.opcode);
    if (std::optional<Access> elementAccess = elementAccessOf(access.opcode)) {
        kind = *elementAccess;
        if (kind == Access::Store || kind == Access::Define)
            stored = OperandPosition::value(access.opcode);
    }
    if (!receiver)
        return LinkedFate::anything();
    if (receiver != operandIndex && stored != operandIndex) {
        // A key is converted, and neither an accessor nor a trap sees what it was before. A token is looked at.
        OperandBehavior behavior = declaredBehaviorOf(access.opcode, operandIndex);
        LinkedFate fate = LinkedFate::nothing();
        if (!behavior.uses.containsOnly(OperandUse::Inspects) || !addProtocols(fate, behavior, m_program.typeOf(caller, access.operand(operandIndex))))
            return LinkedFate::anything();
        return fate;
    }

    const BuiltinTable& table = m_program.builtins();
    ProvenAccess proven = provenAccessOf(m_program, caller, access);
    bool isReceiver = receiver == operandIndex;
    LinkedFate fate = LinkedFate::nothing();
    if (!proven.isComplete) {
        // A setter or a trap of the receiver, which nobody knows, takes the value.
        if (!isReceiver)
            return LinkedFate::anything();
        KeysOfMembers keys = KeysOfMembers::any();
        if (Value::opcodesWithKey.contains(access.opcode) && access.key()) {
            keys = { };
            keys.add(access.key());
        }
        switch (kind) {
        case Access::Read:
            fate.contents.add(ValueFate::IsReturned);
            [[fallthrough]];
        case Access::Has:
            fate.itself.add(ValueFate::HasMembersRead);
            fate.keysRead = keys;
            break;
        case Access::Store:
            fate.itself.add(ValueFate::HasMembersWritten);
            fate.keysWritten = keys;
            break;
        case Access::Define:
        case Access::None:
            fate.itself.add(ValueFate::HasMembersDeletedOrDefined);
            fate.keysWritten = keys;
            break;
        }
        return fate;
    }

    bool stores = kind == Access::Store || kind == Access::Define;
    if (isReceiver && kind == Access::Read)
        fate.contents.add(ValueFate::IsReturned);
    if (!isReceiver) {
        fate.storedIntoParameters |= 1u << *receiver;
        // A typed array converts what is stored into it.
        if (elementAccessOf(access.opcode))
            fate.itself.add(ValueFate::IsConvertedToPrimitive);
    }

    OperandsByPlace operandsOfAccessor;
    operandsOfAccessor.thisValue = receiver;
    if (stored) {
        operandsOfAccessor.firstArgument = *stored;
        operandsOfAccessor.numberOfArguments = 1;
    }
    Site site { caller, access, operandsOfAccessor, false };
    PlaceAtCall place { isReceiver ? PlaceAtCall::Kind::This : PlaceAtCall::Kind::Argument, false, 0 };
    for (FunctionID accessor : proven.accessors) {
        // A property with a getter alone has no function that a store could enter.
        if (!accessor)
            continue;
        for (EntryOfPlace entry : entriesAt(m_program.at(accessor), place)) {
            LinkedFate one = ofEntry(site, accessor, entry, operandIndex);
            if (stores)
                dropResults(one);
            fate.unite(one);
        }
    }
    BuiltinCallMode mode = stores ? BuiltinCallMode::Set : BuiltinCallMode::Get;
    OperandsOfBuiltin operandsOfBuiltin { &caller, &access, receiver, stored.value_or(0), stored ? 1u : 0u, false };
    for (BuiltinID builtin : proven.builtins) {
        // Any other member is a value that the access finds.
        if (!table.dispatchOf(table.canonicalID(builtin), mode))
            continue;
        LinkedFate one = ofOperandOfBuiltin(operandsOfBuiltin, builtin, mode, isReceiver ? BuiltinOperand::receiver() : BuiltinOperand::argument(0), Callbacks::AreFollowed);
        if (stores)
            dropResults(one);
        fate.unite(one);
    }
    normalize(fate);
    return fate;
}

// What the schema declares for the operand, and what the protocols find that the operation runs on it.
LinkedFate FatesAtCalls::ofOperandOfOperation(const Function& caller, const Value& user, uint32_t operandIndex) const
{
    // Where the test of a Check fails, a copy of the operations behind it runs that knows nothing about the value.
    if (user.opcode == Opcode::Check || maySuspend(user.opcode))
        return LinkedFate::anything();
    OperandBehavior behavior = declaredBehaviorOf(user.opcode, operandIndex);
    OperandUses uses = behavior.uses;
    if (uses.isEmpty() || uses.containsAny(callingUses | OperandUse::PassesToUserCode))
        return LinkedFate::anything();
    if (!uses.contains(OperandUse::RetainsContents) && declaredBoundOf(user.opcode).mayCallUnknownCode)
        return LinkedFate::anything();

    LinkedFate fate = LinkedFate::nothing();
    bool isHandedOn = uses.contains(OperandUse::Returns) || user.operandKind(operandIndex) == OperandKind::Results;
    if (isHandedOn) {
        std::optional<uint32_t> result = user.opcode == Opcode::Results ? user.variadicOperandIndex(operandIndex) : std::optional<uint32_t>(0);
        fate.results = result && *result < 32 ? 1u << *result : UINT32_MAX;
    }
    bool isHeldByResult = false;
    if (uses.contains(OperandUse::Retains)) {
        switch (behavior.retainedIn) {
        case RetainedIn::UnknownMemory:
            fate.itself.add(ValueFate::IsStoredInUnknownObject);
            break;
        case RetainedIn::Result:
            isHeldByResult = true;
            fate.storedIntoResults |= 1;
            break;
        case RetainedIn::Operand:
            if (behavior.retainingOperand < 32)
                fate.storedIntoParameters |= 1u << behavior.retainingOperand;
            else
                fate.itself.add(ValueFate::IsStoredInUnknownObject);
            break;
        }
    }
    if (uses.contains(OperandUse::RetainsContents))
        fate.contents.add(ValueFate::IsStoredInUnknownObject);
    // What an operation answers may be something that it has read out of an operand.
    if (!isHandedOn && !isHeldByResult && interpretedOf(user.opcode) != Interpretation::Allocation && (!user.type || mayBeReference(m_program.at(user.type))))
        fate.contents.add(ValueFate::IsReturned);
    if (user.opcode == Opcode::Eq || user.opcode == Opcode::StrictEq)
        fate.itself.add(ValueFate::IsCompared);
    addUses(fate, uses);
    if (!addProtocols(fate, behavior, m_program.typeOf(caller, user.operand(operandIndex))))
        return LinkedFate::anything();

    // The method that HasInstance runs on one operand takes the other operands as its arguments.
    bool isHandedToMethod = false;
    user.forEachOperand([&](ValueID other, uint32_t otherIndex) {
        if (otherIndex == operandIndex || !protocolsOf(user.opcode, otherIndex).contains(Protocol::HasInstance))
            return;
        isHandedToMethod |= mayRunCode(m_program.typeOf(caller, other), OperandBehavior { .uses = { }, .protocols = { } }, Protocol::HasInstance);
    });
    if (isHandedToMethod)
        return LinkedFate::anything();
    normalize(fate);
    return fate;
}

// A read of a callee may find a getter, which then runs with the operand as `this`.
void FatesAtCalls::enterGetters(const Function& caller, const Value& user, uint32_t operandIndex, LinkedFate& fate) const
{
    // Each round but the last adds a flag, a bit or a key, and more than a few keys count as any.
    constexpr unsigned mostRounds = 64;
    for (unsigned round = 0; round < mostRounds; ++round) {
        if (fate.isAnything() || !fate.itself.containsAny(fatesThatMayRunCodeOfTheObject))
            return;
        std::optional<GettersOfProgram> getters = gettersRunOnOperandAt(m_program, m_flow, caller, user, operandIndex, fate);
        if (!getters || getters->isEmpty())
            return;

        LinkedFate grown = fate;
        for (FunctionID getter : *getters) {
            std::optional<unsigned> thisParameter = m_program.at(getter).thisParameterIndex();
            if (!thisParameter)
                continue;
            LinkedFate ofThis = m_parameters.whole(getter, *thisParameter);
            // Its closure lies on the prototype of the class, which no operand leads to.
            giveBackToCaller(ofThis, nullptr, std::nullopt, fate.contents);
            grown.unite(ofThis);
        }
        normalize(grown);
        if (grown == fate)
            return;
        fate = WTF::move(grown);
    }
    fate = LinkedFate::anything();
}

LinkedFate FatesAtCalls::ofOperand(const Function& caller, const Value& user, uint32_t operandIndex) const
{
    if (operandIndex >= user.numberOfOperands() || !user.operand(operandIndex))
        return LinkedFate::anything();
    if (user.operandKind(operandIndex) == OperandKind::Guard)
        return LinkedFate::nothing();

    LinkedFate fate;
    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Call:
        fate = ofOperandOfCall(caller, user, operandIndex);
        break;
    case EffectRule::Builtin:
    case EffectRule::Inline:
        fate = ofOperandOfBuiltinCall(caller, user, operandIndex);
        break;
    case EffectRule::Access:
        fate = ofOperandOfAccess(caller, user, operandIndex);
        break;
    case EffectRule::Declared:
    case EffectRule::Operator:
        fate = ofOperandOfOperation(caller, user, operandIndex);
        break;
    }
    enterGetters(caller, user, operandIndex, fate);
    return fate;
}

LinkedFate fateOfOperandOfBuiltin(const RelationalLink& link, const Function& caller, BuiltinID builtin, const Value& call, uint32_t operandIndex, Callbacks callbacks)
{
    std::optional<OperandsByPlace> operands = operandsOfCallByValue(call);
    std::optional<PlaceAtCall> place = operands ? placeOf(*operands, operandIndex) : std::nullopt;
    if (!place)
        return LinkedFate::anything();
    FatesInLink parameters(link.linked);
    return FatesAtCalls(link.program, link.flow, parameters).ofOperandOfBuiltinAt(Site { caller, call, *operands, false }, builtin, *place, callbacks);
}

void forEachCallbackOfBuiltin(const Program& program, BuiltinID builtin, const Value& call, const ScopedLambda<void(const CallbackOfBuiltin&)>& functor)
{
    forEachCallbackOfBuiltin(program, nullptr, builtin, call, functor);
}

// With the caller only the overloads count that fateOfOperandOfBuiltin() counts: what `map` does with an object that is no array says nothing about a call on an array.
void forEachCallbackOfBuiltin(const Program& program, const Function* caller, BuiltinID builtin, const Value& call, const ScopedLambda<void(const CallbackOfBuiltin&)>& functor)
{
    std::optional<OperandsByPlace> byPlace = operandsOfCallByValue(call);
    // The elements of a list have no operand of their own. fateOfOperandOfBuiltin() leaves nothing out at such a call.
    if (!byPlace || byPlace->list)
        return;

    const BuiltinTable& table = program.builtins();
    OperandsOfBuiltin operands { caller, &call, byPlace->thisValue, byPlace->firstArgument, byPlace->numberOfArguments, false };
    BuiltinCallMode mode = byPlace->isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    auto forEachOperand = [&](const auto& visit) {
        if (operands.receiver)
            visit(BuiltinOperand::receiver(), *operands.receiver);
        for (uint32_t argument = 0; argument < std::min<uint32_t>(operands.numberOfArguments, BuiltinOperand::maxArgumentIndex + 1); ++argument)
            visit(BuiltinOperand::argument(argument), operands.firstArgument + argument);
    };

    forEachOverloadThatCallMayTake(program, operands, builtin, mode, [&](const BuiltinOverload& overload) {
        if (mayRunUnnamedCode(overload))
            return;
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            // A row cannot say that what the callback answers is handed to the callback again.
            OptionSet<ValueFate> resultOfCall = fatesOfResultInNativeCode(callback);
            forEachPlacePassedTo(callback, callback.callee, [&](const BuiltinPlace& passed, PlaceAtCall) {
                if (mentionOfCallbackResultIn(table, passed).mention != Mention::None)
                    resultOfCall = everyValueFate;
            });
            forEachOperand([&](BuiltinOperand called, uint32_t calledOperand) {
                if (!covers(callback.callee, called))
                    return;
                functor(CallbackOfBuiltin { calledOperand, calledOperand, true, PlaceAtCall { PlaceAtCall::Kind::Callee, callback.constructs, 0 }, resultOfCall });
                forEachOperand([&](BuiltinOperand handed, uint32_t handedOperand) {
                    forEachPlacePassedTo(callback, handed, [&](const BuiltinPlace& passed, PlaceAtCall place) {
                        // Of what lies in a new object, and of what is awaited, fateOfOperandOfBuiltin() has said the worst.
                        auto [mention, liesInNewObject, isAwaited] = mentionIn(table, passed, handed);
                        if (mention != Mention::None && !liesInNewObject && !isAwaited)
                            functor(CallbackOfBuiltin { calledOperand, handedOperand, mention == Mention::Contents, place, resultOfCall });
                    });
                });
            });
        }
    });
}

LinkedFate fateOfOperandAt(const Program& program, const FlowResults& flow, const FatesOfParameters& parameters, const Function& caller, const Value& user, uint32_t operandIndex)
{
    return FatesAtCalls(program, flow, parameters).ofOperand(caller, user, operandIndex);
}

LinkedFate FlowResults::fateOfOperandAt(const Function& caller, const Value& user, uint32_t operandIndex) const
{
    if (!m_relationalSummaries || !m_tables.program)
        return LinkedFate::anything();
    return Typed::fateOfOperandAt(*m_tables.program, *this, FatesBehindFlow(*this), caller, user, operandIndex);
}

LinkedFate FlowResults::fateApartFromCalledParameters(FunctionID function, uint32_t entryParameter) const
{
    if (!m_relationalSummaries)
        return LinkedFate::anything();
    FunctionID analyzed = function;
    std::optional<uint32_t> parameter = findAnalyzedParameter(analyzed, entryParameter);
    if (!parameter)
        return LinkedFate::anything();
    LinkedFate fate = m_relationalSummaries->fateApartFromCalledParameters(analyzed, *parameter);
    if (analyzed == function || fate.isAnything())
        return fate;

    // A version numbers its parameters and its results in its own way.
    uint32_t storedIntoParameters = 0;
    for (uint32_t rest = fate.storedIntoParameters; rest; rest &= rest - 1) {
        std::optional<uint32_t> derived = findDerivedParameter(function, static_cast<uint32_t>(std::countr_zero(rest)));
        if (derived && *derived < 32)
            storedIntoParameters |= 1u << *derived;
        else
            fate.itself.add(ValueFate::IsStoredInUnknownObject);
    }
    fate.storedIntoParameters = storedIntoParameters;
    if (fate.results)
        fate.results = UINT32_MAX;
    if (fate.storedIntoResults)
        fate.storedIntoResults = UINT32_MAX;
    return fate;
}

Vector<HandedToCalledParameter, 4> FlowResults::handedToCalledParameters(FunctionID function, bool& isComplete) const
{
    isComplete = false;
    Vector<HandedToCalledParameter, 4> rows;
    FunctionID analyzed = m_relationalSummaries ? findAnalyzedFunction(function) : FunctionID();
    if (!analyzed)
        return rows;

    isComplete = true;
    std::span<const HandedToCalledParameter> rowsAsRead = m_relationalSummaries->handedToCalledParameters(analyzed);
    if (analyzed == function) {
        rows.append(rowsAsRead);
        return rows;
    }
    if (rowsAsRead.empty())
        return rows;

    // By entry parameter of the version: the entry parameter as read that it is.
    uint32_t numberOfParameters = m_tables.program->at(function).derivation.parameterMap.size();
    Vector<std::optional<uint32_t>, 8> parametersAsRead(numberOfParameters, [&](size_t parameter) {
        FunctionID source = function;
        return findAnalyzedParameter(source, static_cast<uint32_t>(parameter));
    });
    for (const HandedToCalledParameter& rowAsRead : rowsAsRead) {
        for (uint32_t handed = 0; handed < numberOfParameters; ++handed) {
            if (parametersAsRead[handed] != rowAsRead.handed)
                continue;
            // A version that is no longer passed what it calls has it from somewhere that no caller sees.
            bool isPassedCalledValue = false;
            for (uint32_t called = 0; called < numberOfParameters; ++called) {
                if (parametersAsRead[called] != rowAsRead.called)
                    continue;
                isPassedCalledValue = true;
                HandedToCalledParameter row = rowAsRead;
                row.handed = handed;
                row.called = called;
                rows.append(row);
            }
            isComplete &= isPassedCalledValue;
        }
    }
    return rows;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
