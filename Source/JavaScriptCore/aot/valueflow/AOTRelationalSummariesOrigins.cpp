/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelationalSummariesInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTFlowResults.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/DataLog.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

bool throwsBeforeEntry(const Function& callee, bool isConstruction)
{
    if (isConstruction)
        return callee.constructorForm() == ConstructorForm::NotConstructor;
    return callee.kind == FunctionKind::BaseConstructor || callee.kind == FunctionKind::DerivedConstructor;
}

bool isParameterWithoutArgument(const Function& callee, const Value& call, uint32_t entryParameter)
{
    if (!VariadicOperandPosition::arguments(call.opcode) || !Value::opcodesWithCallees.contains(call.opcode))
        return false;
    if (callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }) || callee.entryParameterKind(entryParameter) != EntryParameter::Parameter)
        return false;
    uint32_t position = entryParameter - callee.firstParameterIndex();
    return position < callee.numberOfPositionalParameters() && position >= call.numberOfVariadicOperands();
}

namespace {

// A result with more origins counts as anything.
constexpr unsigned mostOrigins = 32;
// An operator of an operator of an operator. A function that hands `n + 1` to itself and returns what it was handed adds a level at each visit of the link.
constexpr unsigned deepestOrigin = 8;
constexpr unsigned mostCallsOnTheWay = 8;
constexpr unsigned mostStores = 64;
constexpr unsigned mostArgumentsListed = 6;
// A helper that calls what it is handed has hundreds of callees. What it answers is said in terms of the value that it calls.
constexpr unsigned mostCalleesFollowed = 4;
constexpr unsigned mostChanges = 16;
// In place of the value of a constant: the undefined of `return;`.
constexpr uint32_t noValue = UINT32_MAX;

constexpr OptionSet<ValueFate> fatesThatKeepBesideTheResult = fatesThatKeep - ValueFate::IsReturned;

// The origins that a value may be: indexes, ascending.
using Choice = Vector<uint32_t, 4>;

// A result while it is made. What the table keeps has room for what it holds and no more.
struct ResultInTheMaking {
    Choice mayBe;
    Vector<LinkedOrigin, 8> origins;
    Vector<uint32_t, 16> operands;
    Vector<CallOnTheWay, 4> calls;
    Vector<StoreIntoOrigin, 8> stores;
};

// Flow's results are applied to the bodies in front of the link, which puts another operation in the place of some values.
constexpr OpcodeSet reads { Opcode::PropGet, Opcode::PrivateGet, Opcode::GetEnvironmentField, Opcode::ElemGet, Opcode::GetContiguousIndex };
constexpr OpcodeSet literals { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt };

// The operation says what it answers, whatever its operands are.
bool hasNaturalResult(const Value& operation)
{
    return effectRuleOf(operation.opcode) != EffectRule::Call && flowOf(operation.opcode) != ValueFlowEdgeClass::Allocates && tryWidestNaturalTypeOf(operation);
}

// What flow has proven of all that the function returns. Of a coroutine that is not what a call answers. A type that is only expected holds behind a test.
TypeID provenResultTypeOf(const Function& callee)
{
    if (callee.isUnsplitCoroutine() || callee.results.size() != 1 || callee.results[0].flags.contains(PositionFlags::IsExpected))
        return { };
    return callee.results[0].type;
}

// A template object is made once for its place in the source.
bool makesAnotherObjectEachTime(Opcode opcode)
{
    return flowOf(opcode) == ValueFlowEdgeClass::Allocates && boundToOf(opcode) != BoundTo::Site;
}

// The operations that put nothing into the object but their operands, each of which has a row.
constexpr OpcodeSet fillsObjectFromOperandsAlone { Opcode::NewObject, Opcode::NewArray, Opcode::NewDictionary };

// One result of one function.
class Composer {
public:
    // Without a link: before flow has run. No callee is followed, nothing is said of who holds an object, and the stores are those that the function makes itself.
    Composer(const Program& program, const RelationalLink* link, FunctionID id, const FunctionRelations& record, std::span<const LinkedFate> fates, bool listsStores)
        : m_program(program)
        , m_link(link)
        , m_id(id)
        , m_function(program.at(id))
        , m_record(record)
        , m_fates(fates)
        , m_listsStores(listsStores)
        , m_choices(record.subjects.size())
    {
        m_states.fill(State::Unvisited, record.subjects.size());
    }

    // Nothing: there are more origins than a result may have.
    std::optional<ResultOrigin> compose(uint32_t result);

private:
    enum class State : uint8_t { Unvisited, InProgress, Done };

    // What one callee answers at one call.
    struct Import {
        uint32_t subject;
        ValueID callID;
        const Value& call;
        const Function& callee;
        const ResultOrigin& answered;
        // By origin of the callee.
        Vector<std::optional<Choice>, mostOrigins> imported;
    };

    const Program& program() const { return m_program; }
    const Value* valueAt(uint32_t index) const { return index < m_function.values.size() ? &m_function.at(ValueID(index)) : nullptr; }
    const Value* operationOf(const LinkedOrigin& origin) const { return origin.value ? &program().at(origin.function).at(origin.value) : nullptr; }

    bool hasSamePayload(const LinkedOrigin&, const LinkedOrigin&) const;
    bool isSame(const LinkedOrigin& listed, const LinkedOrigin&, std::span<const uint32_t> operands, std::span<const CallOnTheWay> calls) const;
    uint32_t add(const LinkedOrigin&, std::span<const uint32_t> operands = { }, std::span<const CallOnTheWay> calls = { });
    uint32_t anything();
    uint32_t unlistedValueOf(TypeID);
    uint32_t undefinedConstant();
    uint32_t oneOf(const Choice&);
    bool isAnything(const Choice& choice) const { return m_anything && choice.size() == 1 && choice[0] == *m_anything; }
    void unite(Choice& into, const Choice&);
    Choice computed(const LinkedOrigin&, std::span<const Choice> operands);
    Choice readFrom(LinkedOrigin&&, const Choice& source, const Choice* computedKey);

    const Choice& choiceOf(uint32_t subject);
    Choice choiceOfOperand(uint32_t call, uint32_t operandIndex);
    Choice choiceOfConstant(uint32_t value);
    Choice choiceOfComputed(uint32_t subject);
    Choice choiceOfRead(uint32_t subject);
    Choice choiceOfComputedKey(uint32_t subject, const Value& read);
    Choice choiceOfCallResult(uint32_t subject);
    uint32_t objectMadeByConstruction(uint32_t subject, const Value& construction);
    bool appendReceiverAndArguments(uint32_t callIndex, const Value& call, Vector<Choice, 4>&);
    Choice choiceOfBuiltinResult(uint32_t callIndex, const Value& call, BuiltinID);
    Choice choiceOfResultOfCalledValue(uint32_t callIndex, const Value& call);
    void visitAllocation(uint32_t subject);

    const Choice& import(Import&, uint32_t origin);
    Choice importParameter(Import&, uint32_t entryParameter);
    Choice importCapture(Import&, uint32_t capture);
    void importAllocation(Import&, uint32_t origin);

    bool isReturned(uint32_t subject) const { return m_record.subjects[subject].fate.contains(ValueFate::IsReturned) && m_record.subjects[subject].results >> m_resultIndex & 1; }
    bool runsNoCode(const LinkedFate&, const Type& ofOperation, bool isResult) const;
    bool isHeldByResultAlone(const LinkedFate&, const Type&, bool isResult) const;
    bool isOnlyLookedAtByCallees(uint32_t subject) const;
    bool holdsWhatWasStored(uint32_t subject, const Type& ofOperation, bool isResult) const;
    bool isWrittenByListedStoresAlone(uint32_t subject, const Type&, bool isResult) const;
    void listStores(uint32_t origin, uint32_t subject, const Type&);
    void listStores(uint32_t origin, Import&, uint32_t originBelow, const Type&, bool isResult);
    void setStores(uint32_t origin, const Vector<StoreIntoOrigin, 8>&, bool areAllListed);
    void appendStoresInto(uint32_t subject, Vector<StoreIntoOrigin, 8>&);
    std::optional<PropertyKeyID> keyFilledAtCreation(uint32_t subject, uint32_t operandIndex) const;
    ResultOrigin finish() const;

    const Program& m_program;
    const RelationalLink* m_link;
    FunctionID m_id;
    const Function& m_function;
    const FunctionRelations& m_record;
    std::span<const LinkedFate> m_fates;
    bool m_listsStores;
    uint32_t m_resultIndex { 0 };

    ResultInTheMaking m_result;
    Vector<uint8_t, mostOrigins> m_depths;
    std::optional<uint32_t> m_anything;
    Vector<State, 64> m_states;
    Vector<Choice, 64> m_choices;
    Choice m_choiceOfUnlistedValue;
    bool m_isOverLimit { false };
};

bool Composer::hasSamePayload(const LinkedOrigin& first, const LinkedOrigin& second) const
{
    const Value* firstOperation = operationOf(first);
    const Value* secondOperation = operationOf(second);
    // Only undefined comes without an operation, and it has no payload.
    return !firstOperation || !secondOperation || firstOperation == secondOperation || firstOperation->hasSamePayloadAs(*secondOperation);
}

bool Composer::isSame(const LinkedOrigin& listed, const LinkedOrigin& origin, std::span<const uint32_t> operands, std::span<const CallOnTheWay> calls) const
{
    if (listed.kind != origin.kind)
        return false;
    auto hasSameOperands = [&] {
        return std::ranges::equal(m_result.operands.span().subspan(listed.firstOperand, listed.operandCount), operands);
    };
    switch (origin.kind) {
    case LinkedOrigin::Kind::Parameter:
        return listed.parameter == origin.parameter;
    case LinkedOrigin::Kind::Allocation:
        return listed.function == origin.function && listed.value == origin.value && listed.hasUnlistedCalls == origin.hasUnlistedCalls
            && std::ranges::equal(m_result.calls.span().subspan(listed.firstCall, listed.callCount), calls);
    case LinkedOrigin::Kind::Constant:
        return listed.opcode == origin.opcode && hasSamePayload(listed, origin);
    case LinkedOrigin::Kind::Computed:
        return listed.opcode == origin.opcode && hasSameOperands() && hasSamePayload(listed, origin);
    case LinkedOrigin::Kind::ReadFrom:
        return listed.opcode == origin.opcode && listed.source == origin.source && listed.key == origin.key && listed.field == origin.field && hasSameOperands();
    case LinkedOrigin::Kind::BuiltinResult:
        return listed.builtin == origin.builtin && listed.isConstruction == origin.isConstruction && hasSameOperands();
    case LinkedOrigin::Kind::CallResult:
        return listed.isConstruction == origin.isConstruction && hasSameOperands();
    case LinkedOrigin::Kind::OneOf:
        return hasSameOperands();
    case LinkedOrigin::Kind::Anything:
        return listed.type == origin.type;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t Composer::add(const LinkedOrigin& origin, std::span<const uint32_t> operands, std::span<const CallOnTheWay> calls)
{
    for (uint32_t index = 0; index < m_result.origins.size(); ++index) {
        if (isSame(m_result.origins[index], origin, operands, calls))
            return index;
    }
    if (m_result.origins.size() == mostOrigins) {
        m_isOverLimit = true;
        return 0;
    }

    unsigned depth = 0;
    for (uint32_t operand : operands)
        depth = std::max<unsigned>(depth, m_depths[operand]);
    if (origin.kind == LinkedOrigin::Kind::ReadFrom)
        depth = std::max<unsigned>(depth, m_depths[origin.source]);
    m_depths.append(depth + (origin.kind != LinkedOrigin::Kind::OneOf));

    LinkedOrigin added = origin;
    added.firstOperand = static_cast<uint16_t>(m_result.operands.size());
    added.operandCount = static_cast<uint8_t>(operands.size());
    m_result.operands.append(operands);
    added.firstCall = static_cast<uint16_t>(m_result.calls.size());
    added.callCount = static_cast<uint8_t>(calls.size());
    m_result.calls.append(calls);
    added.firstStore = 0;
    added.storeCount = 0;
    m_result.origins.append(WTF::move(added));
    return m_result.origins.size() - 1;
}

uint32_t Composer::anything()
{
    if (!m_anything) {
        LinkedOrigin origin;
        origin.kind = LinkedOrigin::Kind::Anything;
        m_anything = add(origin);
    }
    return *m_anything;
}

// Nobody can list the value, but flow has proven a type of it that holds whoever calls.
uint32_t Composer::unlistedValueOf(TypeID type)
{
    if (!type || program().types.at(type).isAnything())
        return anything();
    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::Anything;
    origin.type = type;
    return add(origin);
}

uint32_t Composer::undefinedConstant()
{
    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::Constant;
    origin.opcode = Opcode::Undefined;
    origin.type = TypeTable::undefined;
    return add(origin);
}

uint32_t Composer::oneOf(const Choice& choice)
{
    RELEASE_ASSERT(!choice.isEmpty());
    if (choice.size() == 1)
        return choice[0];
    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::OneOf;
    return add(origin, choice.span());
}

void Composer::unite(Choice& into, const Choice& other)
{
    if (isAnything(into))
        return;
    if (isAnything(other)) {
        into = other;
        return;
    }
    for (uint32_t origin : other) {
        auto position = std::ranges::lower_bound(into, origin);
        if (position == into.end() || *position != origin)
            into.insert(position - into.begin(), origin);
    }
}

Choice Composer::computed(const LinkedOrigin& origin, std::span<const Choice> operands)
{
    Vector<uint32_t, 2> indexes;
    for (const Choice& operand : operands) {
        if (operand.isEmpty())
            return { };
        uint32_t index = oneOf(operand);
        indexes.append(m_depths[index] < deepestOrigin ? index : anything());
    }
    return { add(origin, indexes.span()) };
}

Choice Composer::readFrom(LinkedOrigin&& origin, const Choice& source, const Choice* computedKey)
{
    if (source.isEmpty() || (computedKey && computedKey->isEmpty()))
        return { };
    origin.source = static_cast<uint8_t>(oneOf(source));
    if (isAnything(source) || m_depths[origin.source] >= deepestOrigin) {
        // The layout says what a field of an environment and a private field hold, whichever object they are read out of.
        if (!origin.field.layout)
            return { anything() };
        origin.source = static_cast<uint8_t>(anything());
    }
    if (!computedKey)
        return { add(origin) };
    uint32_t key = oneOf(*computedKey);
    if (m_depths[key] >= deepestOrigin)
        key = anything();
    return { add(origin, singleElementSpan(key)) };
}

const Choice& Composer::choiceOf(uint32_t index)
{
    // The front end names no subject where a value may be one that it does not list. A value that is computed from itself, around a loop, is not followed.
    if (index >= m_record.subjects.size() || m_states[index] == State::InProgress) {
        m_choiceOfUnlistedValue = { anything() };
        return m_choiceOfUnlistedValue;
    }
    if (m_states[index] == State::Done)
        return m_choices[index];

    m_states[index] = State::InProgress;
    const RelationSubject& subject = m_record.subjects[index];
    switch (subject.kind) {
    case RelationSubjectKind::Parameter: {
        LinkedOrigin origin;
        origin.kind = LinkedOrigin::Kind::Parameter;
        origin.parameter = subject.index;
        m_choices[index] = { subject.index < m_function.numberOfEntryParameters() ? add(origin) : anything() };
        break;
    }
    case RelationSubjectKind::Allocation:
        visitAllocation(index);
        break;
    case RelationSubjectKind::CallResult:
        m_choices[index] = choiceOfCallResult(index);
        break;
    case RelationSubjectKind::ReadFrom:
        m_choices[index] = choiceOfRead(index);
        break;
    case RelationSubjectKind::Constant:
        m_choices[index] = choiceOfConstant(subject.index);
        break;
    case RelationSubjectKind::Computed:
        m_choices[index] = choiceOfComputed(index);
        break;
    }
    m_states[index] = State::Done;
    return m_choices[index];
}

Choice Composer::choiceOfOperand(uint32_t call, uint32_t operandIndex)
{
    Choice choice;
    bool isListed = false;
    for (const HandedTo& handing : m_record.handed) {
        if (handing.call != call || handing.argument != operandIndex)
            continue;
        isListed = true;
        unite(choice, Choice(choiceOf(handing.source)));
    }
    if (!isListed)
        return { anything() };
    return choice;
}

Choice Composer::choiceOfConstant(uint32_t value)
{
    if (value == noValue)
        return { undefinedConstant() };
    const Value* literal = valueAt(value);
    if (!literal || literal->numberOfOperands())
        return { anything() };
    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::Constant;
    origin.opcode = literal->opcode;
    origin.type = literal->type;
    origin.function = m_id;
    origin.value = ValueID(value);
    return { add(origin) };
}

Choice Composer::choiceOfComputed(uint32_t index)
{
    const Value* operation = valueAt(m_record.subjects[index].index);
    if (!operation || !hasNaturalResult(*operation))
        return { anything() };

    // The parts of a long template say nothing that the operation does not say.
    Vector<Choice, 2> operands(operation->numberOfOperands() <= mostArgumentsListed ? operation->numberOfOperands() : 0);
    Vector<bool, 2> isListed;
    isListed.fill(false, operands.size());
    for (const OperandOf& operand : m_record.operands) {
        if (operand.user != index || operand.position >= operands.size())
            continue;
        isListed[operand.position] = true;
        unite(operands[operand.position], Choice(choiceOf(operand.source)));
    }
    for (unsigned position = 0; position < operands.size(); ++position) {
        if (!isListed[position])
            operands[position] = { anything() };
    }

    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::Computed;
    origin.opcode = operation->opcode;
    origin.function = m_id;
    origin.value = ValueID(m_record.subjects[index].index);
    return computed(origin, operands.span());
}

// The front end may list what the key is as it lists the operands of an operator.
Choice Composer::choiceOfComputedKey(uint32_t index, const Value& read)
{
    Choice choice;
    bool isListed = false;
    for (const OperandOf& operand : m_record.operands) {
        if (operand.user != index)
            continue;
        isListed = true;
        unite(choice, Choice(choiceOf(operand.source)));
    }
    if (isListed)
        return choice;
    ValueID key = read.propertyOperand();
    if (literals.contains(m_function.at(key).opcode))
        return choiceOfConstant(key.index());
    return { anything() };
}

Choice Composer::choiceOfRead(uint32_t index)
{
    const RelationSubject& subject = m_record.subjects[index];
    const Value* read = valueAt(subject.index);
    if (!read || !reads.contains(read->opcode) || (read->opcode == Opcode::PropGet && read->key() != subject.key))
        return { anything() };

    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::ReadFrom;
    origin.opcode = read->opcode;
    origin.function = m_id;
    origin.value = ValueID(subject.index);
    origin.key = subject.key;
    if (Value::opcodesWithField.contains(read->opcode))
        origin.field = { read->layout(), read->field() };
    Choice source = choiceOf(subject.source);
    if (read->opcode != Opcode::ElemGet)
        return readFrom(WTF::move(origin), source, nullptr);
    Choice key = choiceOfComputedKey(index, *read);
    return readFrom(WTF::move(origin), source, &key);
}

Choice Composer::choiceOfCallResult(uint32_t index)
{
    const Value* call = valueAt(m_record.subjects[index].index);
    if (!call || effectRuleOf(call->opcode) != EffectRule::Call || maySuspend(call->opcode))
        return { anything() };
    uint32_t callIndex = m_record.subjects[index].index;
    if (!m_link)
        return choiceOfResultOfCalledValue(callIndex, *call);
    CalleeList callees = calleesOfCall(program(), *call);
    if (!callees.isComplete || callees.functions.size() + callees.builtins.size() > mostCalleesFollowed)
        return choiceOfResultOfCalledValue(callIndex, *call);
    // A bound function enters its target with a receiver and with arguments of its own.
    if (Value::opcodesWithCallees.contains(call->opcode) && (!Value::opcodesWithAccessSite.contains(call->opcode) || m_link->flow.mayCallBoundFunction(call->accessSite())))
        return choiceOfResultOfCalledValue(callIndex, *call);

    bool isConstruction = OperandPosition::newTarget(call->opcode).has_value();
    Choice choice;
    for (FunctionID id : callees.functions) {
        const Function& callee = program().at(id);
        if (throwsBeforeEntry(callee, isConstruction))
            continue;
        // Whoever constructs such a function makes the object, and answers it unless the function returns another. For another new.target the object is of another class.
        if (isConstruction && callee.kind == FunctionKind::Plain) {
            bool isItsOwnNewTarget = !call->operand(*OperandPosition::newTarget(call->opcode));
            if (isItsOwnNewTarget && m_link->linked.originOfResult(id, 0).isFreshWhenConstructed)
                unite(choice, Choice { objectMadeByConstruction(index, *call) });
            else
                unite(choice, Choice { unlistedValueOf(TypeTable::object) });
            continue;
        }
        const ResultOrigin& answered = m_link->linked.originOfResult(id, 0);
        if (!answered.isFullyListed) {
            unite(choice, Choice { unlistedValueOf(provenResultTypeOf(callee)) });
            continue;
        }

        Import context { index, ValueID(callIndex), *call, callee, answered, { } };
        context.imported.grow(answered.origins.size());
        for (uint32_t origin : answered.mayBe)
            unite(choice, Choice(import(context, origin)));
    }
    for (BuiltinID builtin : callees.builtins)
        unite(choice, choiceOfBuiltinResult(callIndex, *call, builtin));
    return choice;
}

// The constructor has kept `this` nowhere, so the object is this function's own, as if it had made it where the construction stands.
uint32_t Composer::objectMadeByConstruction(uint32_t index, const Value& construction)
{
    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::Allocation;
    origin.opcode = construction.opcode;
    origin.type = construction.type;
    origin.function = m_id;
    origin.value = ValueID(m_record.subjects[index].index);
    origin.isHeldByNobodyElse = isHeldByResultAlone(m_fates[index], program().types.at(construction.type), isReturned(index));
    return add(origin);
}

// Answers false where the operands cannot be told apart, or mean something else than a receiver and arguments.
bool Composer::appendReceiverAndArguments(uint32_t callIndex, const Value& call, Vector<Choice, 4>& operands)
{
    // A construction for another new.target makes an object of another class. A call with a typed result runs code that is derived from what the result is expected to be.
    bool isPlainCall = call.opcode == Opcode::Call && !call.expectedResult();
    bool isPlainConstruction = call.opcode == Opcode::Construct && !call.newTargetOperand();
    if ((!isPlainCall && !isPlainConstruction) || call.numberOfVariadicOperands() > mostArgumentsListed)
        return false;

    if (isPlainCall && call.thisValueOperand())
        operands.append(choiceOfOperand(callIndex, *OperandPosition::thisValue(call.opcode)));
    else
        operands.append(Choice { undefinedConstant() });
    for (uint32_t argument = 0; argument < call.numberOfVariadicOperands(); ++argument)
        operands.append(choiceOfOperand(callIndex, call.numberOfNamedOperands() + argument));
    return true;
}

Choice Composer::choiceOfBuiltinResult(uint32_t callIndex, const Value& call, BuiltinID builtin)
{
    Vector<Choice, 4> operands;
    if (!appendReceiverAndArguments(callIndex, call, operands))
        return { anything() };

    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::BuiltinResult;
    origin.opcode = call.opcode;
    origin.function = m_id;
    origin.value = ValueID(callIndex);
    origin.builtin = builtin;
    origin.isConstruction = call.opcode == Opcode::Construct;
    return computed(origin, operands.span());
}

// Nobody can say here what the call enters. Who knows what a caller passes may know. A construction answers an object whatever it enters.
Choice Composer::choiceOfResultOfCalledValue(uint32_t callIndex, const Value& call)
{
    bool answersObject = call.opcode == Opcode::Construct || call.opcode == Opcode::ConstructList;
    Choice unlisted { answersObject ? unlistedValueOf(TypeTable::object) : anything() };
    std::optional<unsigned> callee = OperandPosition::callee(call.opcode);
    if (!callee)
        return unlisted;
    Vector<Choice, 4> operands;
    operands.append(choiceOfOperand(callIndex, *callee));
    if (isAnything(operands[0]) || !appendReceiverAndArguments(callIndex, call, operands))
        return unlisted;

    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::CallResult;
    origin.opcode = call.opcode;
    origin.function = m_id;
    origin.value = ValueID(callIndex);
    origin.isConstruction = call.opcode == Opcode::Construct;
    return computed(origin, operands.span());
}

const Choice& Composer::import(Import& context, uint32_t index)
{
    if (context.imported[index])
        return *context.imported[index];

    const ResultOrigin& answered = context.answered;
    const LinkedOrigin& origin = answered.origins[index];
    std::span<const uint8_t> operandsBelow = answered.operands.span().subspan(origin.firstOperand, origin.operandCount);
    Choice choice;
    switch (origin.kind) {
    case LinkedOrigin::Kind::Parameter:
        choice = importParameter(context, origin.parameter);
        break;
    case LinkedOrigin::Kind::Allocation:
        importAllocation(context, index);
        return *context.imported[index];
    case LinkedOrigin::Kind::Constant:
        choice = { add(origin) };
        break;
    case LinkedOrigin::Kind::Computed:
    case LinkedOrigin::Kind::BuiltinResult:
    case LinkedOrigin::Kind::CallResult: {
        Vector<Choice, 2> operands;
        for (uint32_t operand : operandsBelow)
            operands.append(import(context, operand));
        bool callsUnlistedValue = origin.kind == LinkedOrigin::Kind::CallResult && isAnything(operands[0]);
        choice = callsUnlistedValue ? Choice { anything() } : computed(origin, operands.span());
        break;
    }
    case LinkedOrigin::Kind::ReadFrom: {
        Choice source = import(context, origin.source);
        Choice key;
        if (!operandsBelow.empty())
            key = import(context, operandsBelow[0]);
        choice = readFrom(LinkedOrigin(origin), source, operandsBelow.empty() ? nullptr : &key);
        break;
    }
    case LinkedOrigin::Kind::OneOf:
        for (uint32_t operand : operandsBelow)
            unite(choice, Choice(import(context, operand)));
        break;
    case LinkedOrigin::Kind::Anything:
        choice = { unlistedValueOf(origin.type) };
        break;
    }
    context.imported[index] = WTF::move(choice);
    return *context.imported[index];
}

Choice Composer::importParameter(Import& context, uint32_t entryParameter)
{
    if (std::optional<uint32_t> operandIndex = findOperandPassedFor(context.callee, context.call, entryParameter))
        return choiceOfOperand(context.callID.index(), *operandIndex);
    if (isParameterWithoutArgument(context.callee, context.call, entryParameter))
        return { undefinedConstant() };
    if (context.callee.entryParameterKind(entryParameter) == EntryParameter::Capture)
        return importCapture(context, entryParameter);
    return { anything() };
}

// A closure that this function has made holds what the function has put into it. The front end lists that like the operands of a call, and the captures are the first entry parameters.
Choice Composer::importCapture(Import& context, uint32_t capture)
{
    std::optional<unsigned> calledOperand = OperandPosition::callee(context.call.opcode);
    if (!calledOperand)
        return { anything() };
    Choice choice;
    for (uint32_t index : choiceOfOperand(context.callID.index(), *calledOperand)) {
        LinkedOrigin called = m_result.origins[index];
        if (called.kind != LinkedOrigin::Kind::Allocation || called.opcode != Opcode::NewClosure || called.function != m_id || called.callCount || called.hasUnlistedCalls)
            return { anything() };
        // A closure of another function does not enter this one.
        if (&program().at(m_function.at(called.value).function()) == &context.callee)
            unite(choice, choiceOfOperand(called.value.index(), capture));
    }
    return choice;
}

void Composer::importAllocation(Import& context, uint32_t index)
{
    const LinkedOrigin& below = context.answered.origins[index];
    std::span<const CallOnTheWay> callsBelow = context.answered.calls.span().subspan(below.firstCall, below.callCount);
    CallOnTheWay callHere { m_id, context.callID, Value::opcodesWithAccessSite.contains(context.call.opcode) ? context.call.accessSite() : AccessSiteID() };
    const Type& type = program().types.at(below.type);

    LinkedOrigin origin = below;
    origin.hasUnlistedCalls = below.hasUnlistedCalls || callsBelow.size() == mostCallsOnTheWay || std::ranges::find(callsBelow, callHere) != callsBelow.end();
    bool isResult = context.answered.mayBe.contains(index) && isReturned(context.subject);
    origin.isHeldByNobodyElse = below.isHeldByNobodyElse && isHeldByResultAlone(m_fates[context.subject], type, isResult);
    origin.hasOnlyListedStores = false;
    Vector<CallOnTheWay, mostCallsOnTheWay> calls;
    if (!origin.hasUnlistedCalls) {
        calls.append(callHere);
        calls.append(callsBelow);
    }

    unsigned numberOfOriginsBefore = m_result.origins.size();
    uint32_t added = add(origin, { }, calls.span());
    // The stores may name the object itself.
    context.imported[index] = Choice { added };
    if (m_isOverLimit)
        return;
    if (m_result.origins.size() == numberOfOriginsBefore) {
        // Several ways that are not listed lead to one operation.
        m_result.origins[added].isHeldByNobodyElse &= origin.isHeldByNobodyElse;
        return;
    }
    if (!origin.hasUnlistedCalls)
        listStores(added, context, index, type, isResult);
}

void Composer::visitAllocation(uint32_t index)
{
    const RelationSubject& subject = m_record.subjects[index];
    const Value* operation = valueAt(subject.index);
    if (!operation || !makesAnotherObjectEachTime(operation->opcode)) {
        m_choices[index] = { anything() };
        return;
    }
    const Type& type = program().types.at(operation->type);

    LinkedOrigin origin;
    origin.kind = LinkedOrigin::Kind::Allocation;
    origin.opcode = operation->opcode;
    origin.type = operation->type;
    origin.function = m_id;
    origin.value = ValueID(subject.index);
    if (Value::opcodesWithAllocationSite.contains(operation->opcode))
        origin.allocationSite = operation->allocationSite();
    origin.isHeldByNobodyElse = m_link && isHeldByResultAlone(m_fates[index], type, isReturned(index));
    uint32_t added = add(origin);
    // The stores may name the object itself.
    m_choices[index] = { added };
    m_states[index] = State::Done;
    if (!m_isOverLimit)
        listStores(added, index, type);
}

// Any type that the object lies in will do. Flow says more of a position than the operation says of what it makes: that arrays are as they were made, which class an object with a prototype is of.
bool Composer::runsNoCode(const LinkedFate& fate, const Type& ofOperation, bool isResult) const
{
    if (runsNoCodeOn(program(), m_link->flow, ofOperation, fate))
        return true;
    const DeclaredType& result = m_function.results[m_resultIndex];
    return isResult && !result.flags.contains(PositionFlags::IsExpected) && runsNoCodeOn(program(), m_link->flow, program().types.at(result.type), fate);
}

// No access has run code, and each store has made or filled a member of the object's own. What the operation that makes the object puts in is there from birth.
bool Composer::holdsWhatWasStored(uint32_t subject, const Type& ofOperation, bool isResult) const
{
    LinkedFate stores = LinkedFate::nothing();
    for (const StoredInto& store : m_record.stores) {
        if (store.target != subject || store.creationOperand)
            continue;
        stores.itself.add(ValueFate::HasMembersWritten);
        if (store.key)
            stores.keysWritten.add(store.key);
        else
            stores.keysWritten = KeysOfMembers::any();
    }
    auto holds = [&](const Type& type) {
        return runsNoCodeOn(program(), m_link->flow, type, m_fates[subject]) && storesLandInOwnFields(program(), m_link->flow, type, stores);
    };
    if (holds(ofOperation))
        return true;
    const DeclaredType& result = m_function.results[m_resultIndex];
    return isResult && !result.flags.contains(PositionFlags::IsExpected) && holds(program().types.at(result.type));
}

bool Composer::isHeldByResultAlone(const LinkedFate& fate, const Type& type, bool isResult) const
{
    return !fate.itself.containsAny(fatesThatKeepBesideTheResult) && !fate.storedIntoParameters && !fate.storedIntoResults && runsNoCode(fate, type, isResult);
}

// A callee that only looks at the object leaves it as it is, and has let go of it when it returns.
bool Composer::isOnlyLookedAtByCallees(uint32_t subject) const
{
    for (const HandedTo& handing : m_record.handed) {
        if (handing.source != subject)
            continue;
        const Value* call = valueAt(handing.call);
        if (!call || effectRuleOf(call->opcode) != EffectRule::Call || maySuspend(call->opcode))
            return false;
        CalleeList callees = calleesOfCall(program(), *call);
        if (!callees.isComplete || !callees.builtins.isEmpty())
            return false;
        if (Value::opcodesWithCallees.contains(call->opcode) && (!Value::opcodesWithAccessSite.contains(call->opcode) || m_link->flow.mayCallBoundFunction(call->accessSite())))
            return false;
        for (FunctionID id : callees.functions) {
            std::optional<uint32_t> parameter = entryParameterOf(program().at(id), *call, handing.argument);
            if (!parameter)
                return false;
            const LinkedFate& fate = m_link->linked.fateOfParameter(id, *parameter);
            if (!isKeptNowhere(fate) || fate.itself.containsAny({ ValueFate::HasMembersWritten, ValueFate::HasMembersDeletedOrDefined }))
                return false;
        }
    }
    return true;
}

// The front end has a row for every store into a subject. What it cannot see is a store by somebody else who has got hold of the object while the call ran.
bool Composer::isWrittenByListedStoresAlone(uint32_t subject, const Type& type, bool isResult) const
{
    const LinkedFate& fate = m_fates[subject];
    if (fate.itself.containsAny(fatesThatKeepBesideTheResult | OptionSet<ValueFate> { ValueFate::HasMembersDeletedOrDefined, ValueFate::HasKeysEnumerated }))
        return false;
    if (fate.storedIntoParameters || fate.storedIntoResults)
        return false;
    if (std::ranges::find(m_record.stores, subject, &StoredInto::source) != m_record.stores.end() || !isOnlyLookedAtByCallees(subject))
        return false;
    return holdsWhatWasStored(subject, type, isResult);
}

// Where the operation that makes the object puts one of its operands. No key: an element, a capture. Nothing: the prototype, which is a link and no member.
std::optional<PropertyKeyID> Composer::keyFilledAtCreation(uint32_t subject, uint32_t operandIndex) const
{
    const Value* operation = valueAt(m_record.subjects[subject].index);
    if (!operation || operation->opcode != Opcode::NewObject)
        return PropertyKeyID();
    std::optional<uint32_t> position = operation->variadicOperandIndex(operandIndex);
    if (!position)
        return std::nullopt;
    if (*position >= operation->fields().size())
        return PropertyKeyID();
    return fieldAt(program(), { operation->layout(), operation->fields()[*position] }).key;
}

// A value that never comes about is never stored.
void Composer::appendStoresInto(uint32_t subject, Vector<StoreIntoOrigin, 8>& stores)
{
    for (const StoredInto& store : m_record.stores) {
        if (store.target != subject)
            continue;
        std::optional<PropertyKeyID> key = store.creationOperand ? keyFilledAtCreation(subject, *store.creationOperand) : store.key;
        if (!key)
            continue;
        Choice stored = choiceOf(store.source);
        if (!stored.isEmpty())
            stores.append({ *key, store.isOnEveryPath, store.creationOperand.has_value(), static_cast<uint8_t>(oneOf(stored)) });
    }
}

void Composer::setStores(uint32_t origin, const Vector<StoreIntoOrigin, 8>& stores, bool areAllListed)
{
    if (m_result.stores.size() + stores.size() > mostStores)
        return;
    LinkedOrigin& listed = m_result.origins[origin];
    listed.hasOnlyListedStores = areAllListed;
    listed.firstStore = static_cast<uint8_t>(m_result.stores.size());
    listed.storeCount = static_cast<uint8_t>(stores.size());
    m_result.stores.appendVector(stores);
}

// Of an object that this function makes.
void Composer::listStores(uint32_t origin, uint32_t subject, const Type& type)
{
    if (!m_listsStores)
        return;
    // Before flow has run nobody knows what the callees do that the object is handed to.
    bool areAllListed = m_link && fillsObjectFromOperandsAlone.contains(m_result.origins[origin].opcode) && isWrittenByListedStoresAlone(subject, type, isReturned(subject));
    if (m_link && !areAllListed)
        return;
    Vector<StoreIntoOrigin, 8> stores;
    appendStoresInto(subject, stores);
    setStores(origin, stores, areAllListed);
}

// Of an object that a callee has answered, or that lies in what the callee has answered.
void Composer::listStores(uint32_t origin, Import& context, uint32_t originBelow, const Type& type, bool isResult)
{
    const LinkedOrigin& below = context.answered.origins[originBelow];
    if (!m_listsStores || !below.hasOnlyListedStores || !isWrittenByListedStoresAlone(context.subject, type, isResult))
        return;
    // What this function does with the contents of the result it may do with this object.
    const LinkedFate& fate = m_fates[context.subject];
    if (fate.contents.contains(ValueFate::IsIncomplete) || mayRunCodeOfContents(fate))
        return;

    Vector<StoreIntoOrigin, 8> stores;
    for (const StoreIntoOrigin& store : context.answered.stores.span().subspan(below.firstStore, below.storeCount)) {
        Choice stored = import(context, store.origin);
        if (!stored.isEmpty())
            stores.append({ store.key, store.isOnEveryPath, store.isAtCreation, static_cast<uint8_t>(oneOf(stored)) });
    }
    if (context.answered.mayBe.contains(originBelow))
        appendStoresInto(context.subject, stores);
    setStores(origin, stores, true);
}

// Leaves out what nothing names any more: the other origins of a value that turned out to be anything.
ResultOrigin Composer::finish() const
{
    unsigned count = m_result.origins.size();
    Vector<bool, mostOrigins> isNamed;
    isNamed.fill(false, count);
    Vector<uint32_t, mostOrigins> worklist(m_result.mayBe.span());
    auto name = [&](uint32_t index) {
        if (std::exchange(isNamed[index], true))
            return;
        worklist.append(index);
    };
    for (uint32_t root : m_result.mayBe)
        isNamed[root] = true;
    unsigned numberOfOrigins = 0, numberOfOperands = 0, numberOfCalls = 0, numberOfStores = 0;
    while (!worklist.isEmpty()) {
        const LinkedOrigin& origin = m_result.origins[worklist.takeLast()];
        ++numberOfOrigins;
        numberOfOperands += origin.operandCount;
        numberOfCalls += origin.callCount;
        numberOfStores += origin.storeCount;
        for (uint32_t operand : m_result.operands.span().subspan(origin.firstOperand, origin.operandCount))
            name(operand);
        if (origin.kind == LinkedOrigin::Kind::ReadFrom)
            name(origin.source);
        for (const StoreIntoOrigin& store : m_result.stores.span().subspan(origin.firstStore, origin.storeCount))
            name(store.origin);
    }

    Vector<uint8_t, mostOrigins> newIndexes(count);
    uint8_t numberOfEarlierOrigins = 0;
    for (unsigned index = 0; index < count; ++index) {
        newIndexes[index] = numberOfEarlierOrigins;
        numberOfEarlierOrigins += isNamed[index];
    }

    ResultOrigin finished;
    finished.isFullyListed = true;
    finished.isFresh = std::ranges::all_of(m_result.mayBe, [&](uint32_t root) {
        return m_result.origins[root].kind == LinkedOrigin::Kind::Allocation && m_result.origins[root].isHeldByNobodyElse;
    });
    finished.mayBe.reserveInitialCapacity(m_result.mayBe.size());
    finished.origins.reserveInitialCapacity(numberOfOrigins);
    finished.operands.reserveInitialCapacity(numberOfOperands);
    finished.calls.reserveInitialCapacity(numberOfCalls);
    finished.stores.reserveInitialCapacity(numberOfStores);
    for (uint32_t root : m_result.mayBe)
        finished.mayBe.append(newIndexes[root]);
    for (unsigned index = 0; index < count; ++index) {
        if (!isNamed[index])
            continue;
        LinkedOrigin origin = m_result.origins[index];
        std::span<const uint32_t> operands = m_result.operands.span().subspan(origin.firstOperand, origin.operandCount);
        std::span<const CallOnTheWay> calls = m_result.calls.span().subspan(origin.firstCall, origin.callCount);
        std::span<const StoreIntoOrigin> stores = m_result.stores.span().subspan(origin.firstStore, origin.storeCount);
        origin.firstOperand = static_cast<uint16_t>(finished.operands.size());
        for (uint32_t operand : operands)
            finished.operands.append(newIndexes[operand]);
        if (origin.kind == LinkedOrigin::Kind::ReadFrom)
            origin.source = newIndexes[origin.source];
        origin.firstCall = static_cast<uint16_t>(finished.calls.size());
        finished.calls.append(calls);
        origin.firstStore = static_cast<uint8_t>(finished.stores.size());
        for (const StoreIntoOrigin& store : stores)
            finished.stores.append({ store.key, store.isOnEveryPath, store.isAtCreation, newIndexes[store.origin] });
        finished.origins.append(WTF::move(origin));
    }
    return finished;
}

std::optional<ResultOrigin> Composer::compose(uint32_t result)
{
    m_resultIndex = result;
    Choice roots;
    for (uint32_t index = 0; index < m_record.subjects.size() && !m_isOverLimit; ++index) {
        if (isReturned(index))
            unite(roots, Choice(choiceOf(index)));
    }
    if (m_isOverLimit)
        return std::nullopt;
    if (isAnything(roots))
        return ResultOrigin { };

    m_result.mayBe = WTF::move(roots);
    return finish();
}

ResultOrigin composeResult(const RelationalLink& link, FunctionID id, const FunctionRelations& record, std::span<const LinkedFate> fates, uint32_t result)
{
    // What was stored into the objects comes second. Where it does not fit, the result is listed without.
    for (bool listsStores : { true, false }) {
        if (std::optional<ResultOrigin> origin = Composer(link.program, &link, id, record, fates, listsStores).compose(result))
            return WTF::move(*origin);
    }
    return { };
}

// Whoever constructs the function gets the object that was made for the construction, unless the function returns another. Nobody else holds it where the function has kept `this` nowhere.
bool answersFreshObjectWhenConstructed(const RelationalLink& link, const Function& function, std::span<const LinkedFate> fates, const ResultOrigin& returned)
{
    // Only a function that the front end has seen constructed has a class of objects.
    if (function.kind != FunctionKind::Plain || function.constructorForm() != ConstructorForm::Base || !function.constructedClass || !returned.isFullyListed)
        return false;
    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    bool returnsNoOtherObject = std::ranges::all_of(returned.mayBe, [&](uint32_t root) {
        const LinkedOrigin& origin = returned.origins[root];
        if (origin.kind == LinkedOrigin::Kind::Parameter)
            return thisParameter && origin.parameter == *thisParameter;
        return origin.kind == LinkedOrigin::Kind::Constant && literals.contains(origin.opcode);
    });
    if (!returnsNoOtherObject)
        return false;
    if (!thisParameter)
        return true;

    const Vector<RelationSubject>& subjects = function.relations.subjects;
    for (uint32_t index = 0; index < subjects.size(); ++index) {
        if (subjects[index].kind != RelationSubjectKind::Parameter || subjects[index].index != *thisParameter)
            continue;
        const LinkedFate& fate = fates[index];
        if (fate.itself.containsAny(fatesThatKeepBesideTheResult) || fate.storedIntoParameters || fate.storedIntoResults)
            return false;
        return !function.thisType.flags.contains(PositionFlags::IsExpected) && runsNoCodeOn(link.program, link.flow, link.program.types.at(function.thisType.type), fate);
    }
    return true;
}

} // anonymous namespace

bool linkResultOrigins(const RelationalLink& link, FunctionID id, std::span<const LinkedFate> fatesOfSubjects, Vector<ResultOrigin>& origins)
{
    const Function& function = link.program.at(id);
    const FunctionRelations& record = function.relations;
    Vector<ResultOrigin> found(function.results.size());
    // A call of a coroutine answers a promise or a generator. What the body returns goes into that.
    if (record.subjects.size() == fatesOfSubjects.size() && !function.isUnsplitCoroutine()) {
        for (uint32_t result = 0; result < std::min<size_t>(found.size(), 32); ++result) {
            // The limits make a result that has grown too large "anything", and a result that is made of such a result small again.
            bool wasFullyListed = result < origins.size() && origins[result].isFullyListed;
            if (!wasFullyListed || !(record.resultsFullyListed >> result & 1))
                continue;
            ResultOrigin composed = composeResult(link, id, record, fatesOfSubjects, result);
            composed.numberOfChanges = origins[result].numberOfChanges;
            composed.isFreshWhenConstructed = !result && origins[result].isFreshWhenConstructed && answersFreshObjectWhenConstructed(link, function, fatesOfSubjects, composed);
            if (!composed.isFullyListed || (composed != origins[result] && ++composed.numberOfChanges > mostChanges))
                continue;
            found[result] = WTF::move(composed);
        }
    }
    // No entry says the same as an entry that lists nothing, and most functions have no more to say.
    if (std::ranges::none_of(found, &ResultOrigin::isFullyListed))
        found.clear();
    if (found == origins)
        return false;
    origins = WTF::move(found);
    return true;
}

ResultOrigin originOfResultBeforeFlow(const Program& program, FunctionID id, uint32_t result)
{
    const Function& function = program.at(id);
    if (result >= 32 || !(function.relations.resultsFullyListed >> result & 1) || function.isUnsplitCoroutine())
        return { };
    for (bool listsStores : { true, false }) {
        if (std::optional<ResultOrigin> origin = Composer(program, nullptr, id, function.relations, { }, listsStores).compose(result))
            return WTF::move(*origin);
    }
    return { };
}

void keepOriginsThatDependOnTheCall(Vector<ResultOrigin>& origins)
{
    for (ResultOrigin& result : origins) {
        bool namesParameter = std::ranges::any_of(result.origins, [](const LinkedOrigin& origin) {
            return origin.kind == LinkedOrigin::Kind::Parameter;
        });
        bool mayBeHeldByNobodyElse = std::ranges::any_of(result.mayBe, [&](uint32_t root) {
            return result.origins[root].isHeldByNobodyElse;
        });
        if (!namesParameter && !mayBeHeldByNobodyElse && !result.isFreshWhenConstructed)
            result = { };
    }
    if (std::ranges::none_of(origins, &ResultOrigin::isFullyListed))
        origins = { };
}

bool mayBeTypedByOrigins(const Function& function)
{
    if (!function.hasBody() || function.results.size() != 1)
        return false;
    const DeclaredType& position = function.results[0];
    if (position.site || !position.flags.contains(PositionFlags::IsExpected))
        return false;
    return (function.relations.resultsFullyListed & 1) && !function.isUnsplitCoroutine();
}

void typeResultsByOrigins(Program& program, const FlowResults& flow, BuildContext& build)
{
    const RelationalSummaries* summaries = flow.relationalSummaries();
    RELEASE_ASSERT(summaries);
    TypeArena types(program.types);
    unsigned numberOfResults = 0;
    for (uint32_t index : findIndicesInParallel(build.parallelLoops(), flow.functionCountAsRead(), [&](unsigned index) {
        return mayBeTypedByOrigins(program.functions[index]);
    })) {
        Function& function = program.functions[index];
        DeclaredType& position = function.results[0];
        const ResultOrigin& origin = summaries->originOfResult(FunctionID(index), 0);
        // An object that a callee has made may come out of a tail call, whose result nothing tests: the position of this function may not say more than the callee's.
        bool areAllObjectsMadeHere = origin.isFullyListed && !origin.mayBe.isEmpty() && std::ranges::all_of(origin.mayBe, [&](uint32_t root) {
            const LinkedOrigin& object = origin.origins[root];
            return object.kind == LinkedOrigin::Kind::Allocation && object.type && !object.callCount && !object.hasUnlistedCalls;
        });
        if (!areAllObjectsMadeHere)
            continue;
        Type type = Type::never();
        for (uint32_t root : origin.mayBe)
            type = type.unionWith(program.types.at(origin.origins[root].type), types);
        position.type = types.intern(WTF::move(type));
        position.flags.remove(PositionFlags::IsExpected);
        ++numberOfResults;
    }
    if (build.options().logging.logsPhaseTimes) [[unlikely]]
        dataLogLn("AOT relational summaries: result positions typed by their origins ", numberOfResults);
}

void ResultOrigin::dump(PrintStream& out) const
{
    if (!isFullyListed) {
        out.print("anything"_s);
        return;
    }
    out.print(isFresh ? "fresh "_s : ""_s, "one of ["_s);
    CommaPrinter rootComma(", "_s);
    for (uint32_t root : mayBe)
        out.print(rootComma, "o"_s, root);
    out.print("]"_s);
    if (isFreshWhenConstructed)
        out.print(", constructed: fresh"_s);

    for (uint32_t index = 0; index < origins.size(); ++index) {
        const LinkedOrigin& origin = origins[index];
        out.print(" | o"_s, index, " = "_s);
        auto printOperands = [&] {
            out.print("("_s);
            CommaPrinter comma(", "_s);
            for (uint32_t operand : operands.span().subspan(origin.firstOperand, origin.operandCount))
                out.print(comma, "o"_s, operand);
            out.print(")"_s);
        };
        switch (origin.kind) {
        case LinkedOrigin::Kind::Parameter:
            out.print("parameter "_s, origin.parameter);
            break;
        case LinkedOrigin::Kind::Allocation: {
            out.print(nameOf(origin.opcode), " f"_s, origin.function.index(), " v"_s, origin.value.index());
            if (origin.isHeldByNobodyElse)
                out.print(" held-by-nobody-else"_s);
            if (origin.hasUnlistedCalls)
                out.print(" through-unlisted-calls"_s);
            for (const CallOnTheWay& call : calls.span().subspan(origin.firstCall, origin.callCount))
                out.print(" through f"_s, call.function.index(), " v"_s, call.call.index());
            if (!origin.hasOnlyListedStores && !origin.storeCount)
                break;
            out.print(origin.hasOnlyListedStores ? " holds {"_s : " holds at least {"_s);
            CommaPrinter comma(", "_s);
            for (const StoreIntoOrigin& store : stores.span().subspan(origin.firstStore, origin.storeCount)) {
                out.print(comma);
                if (store.key)
                    out.print("k"_s, store.key.index());
                else
                    out.print("computed"_s);
                out.print(store.isAtCreation ? " = o"_s : store.isOnEveryPath ? ": o"_s : "?: o"_s, static_cast<unsigned>(store.origin));
            }
            out.print("}"_s);
            break;
        }
        case LinkedOrigin::Kind::Constant:
            out.print(nameOf(origin.opcode));
            if (origin.value)
                out.print(" f"_s, origin.function.index(), " v"_s, origin.value.index());
            break;
        case LinkedOrigin::Kind::Computed:
            out.print(nameOf(origin.opcode));
            printOperands();
            break;
        case LinkedOrigin::Kind::ReadFrom:
            out.print(nameOf(origin.opcode), " o"_s, static_cast<unsigned>(origin.source));
            if (origin.key)
                out.print(" k"_s, origin.key.index());
            else if (origin.field.layout)
                out.print(" L"_s, origin.field.layout.index(), "."_s, origin.field.field);
            else if (origin.operandCount)
                printOperands();
            break;
        case LinkedOrigin::Kind::BuiltinResult:
            out.print(origin.isConstruction ? "new "_s : ""_s, "builtin "_s, origin.builtin.index());
            printOperands();
            break;
        case LinkedOrigin::Kind::CallResult:
            out.print(origin.isConstruction ? "new"_s : "call"_s);
            printOperands();
            break;
        case LinkedOrigin::Kind::OneOf:
            out.print("one of "_s);
            printOperands();
            break;
        case LinkedOrigin::Kind::Anything:
            out.print("anything"_s);
            if (origin.type)
                out.print(" of t"_s, origin.type.index());
            break;
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
