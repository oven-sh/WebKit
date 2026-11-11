/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCountersOfLoops.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTResultRules.h"
#include "AOTWalkListReadsInLoops.h"
#include <limits>
#include <algorithm>
#include <cstdlib>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet stepsByOneOfCountersOfLoops { Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet stepsByNumberOfCountersOfLoops { Opcode::Add, Opcode::Sub };
static constexpr OpcodeSet stepsByNumberOrFactorOfCountersOfLoops = stepsByNumberOfCountersOfLoops | OpcodeSet { Opcode::Mul };
static constexpr OpcodeSet operatorsOnBitsOfCountersOfLoops { Opcode::BitOr, Opcode::BitAnd, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift };
static constexpr OpcodeSet accessesUnderKeysInCountersOfLoops { Opcode::ElemGet, Opcode::ElemSet };
static constexpr size_t mostNamesOfCounterOfLoop = 16;

using NamesOfCounterOfLoop = Vector<ValueID, mostNamesOfCounterOfLoop>;

class UsersOfValuesInCountersOfLoops {
public:
    static constexpr uint32_t none = std::numeric_limits<uint32_t>::max();

    struct Use {
        ValueID user;
        uint32_t next;
    };

    UsersOfValuesInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead)
    {
        std::span<const BlockID> members = index.membersOf(index.loopOf(inputHead));
        ASSERT(std::ranges::is_sorted(members, { }, &BlockID::index));
        ASSERT(members.front() == inputHead);
        uint32_t numberOfPlaces = 0;
        for (BlockID member : members) {
            BodyBlock block = index.block(member);
            uint32_t firstValue = block.firstParameter().index();
            uint32_t endOfValues = block.endOfValues().index();
            if (!m_runs.isEmpty() && m_runs.last().endOfValues == firstValue)
                m_runs.last().endOfValues = endOfValues;
            else
                m_runs.append({ firstValue, endOfValues, numberOfPlaces });
            numberOfPlaces += endOfValues - firstValue;
        }
        m_ends.fill({ none, none }, numberOfPlaces);
        for (BlockID member : members) {
            BodyBlock block = index.block(member);
            if (member != inputHead && !index.isEnteredByHandlerEdge(member)) {
                std::span<const BodyIncomingEdge> incoming = walker.incomingEdgesOf(index, member);
                for (uint32_t place = 0; place < block.numberOfParameters(); ++place) {
                    ValueID parameter = block.parameter(place);
                    if (ValueID narrowed = index.valueNarrowedInto(parameter)) {
                        note(narrowed, parameter);
                        continue;
                    }
                    for (const BodyIncomingEdge& edge : incoming) {
                        std::span<const ValueID> arguments = edge.target.arguments();
                        if (place < arguments.size())
                            note(arguments[place], parameter);
                    }
                }
            }
            for (BodyValue operation : block.values()) {
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    note(operand, operation.id());
                });
            }
        }
    }

    uint32_t firstUseOf(ValueID value) const
    {
        uint32_t place = placeOf(value);
        return place != none ? m_ends[place].first : none;
    }
    const Use& at(uint32_t use) const { return m_uses[use]; }

private:
    struct Ends {
        uint32_t first;
        uint32_t last;
    };

    struct RunOfMembers {
        uint32_t firstValue;
        uint32_t endOfValues;
        uint32_t firstPlace;
    };

    uint32_t placeOf(ValueID value) const
    {
        auto behind = std::ranges::upper_bound(m_runs, value.index(), { }, &RunOfMembers::firstValue);
        if (behind == m_runs.begin())
            return none;
        const RunOfMembers& run = *std::prev(behind);
        return value.index() < run.endOfValues ? run.firstPlace + (value.index() - run.firstValue) : none;
    }

    void note(ValueID used, ValueID user)
    {
        uint32_t place = used ? placeOf(used) : none;
        if (place == none)
            return;
        Ends& ends = m_ends[place];
        if (ends.last != none && m_uses[ends.last].user == user)
            return;
        uint32_t use = m_uses.size();
        m_uses.append({ user, none });
        (ends.last == none ? ends.first : m_uses[ends.last].next) = use;
        ends.last = use;
    }

    Vector<RunOfMembers, 4> m_runs;
    Vector<Ends, 64> m_ends;
    Vector<Use, 128> m_uses;
};

static std::shared_ptr<const UsersOfValuesInCountersOfLoops> usersOfValuesOfLoopInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead)
{
    Walker::UsersOfValuesOfLoop& kept = walker.usersOfValuesOfLoopOfFrame();
    if (!kept.users || kept.body != &index || kept.function != walker.currentFunction() || kept.head != inputHead)
        kept = { &index, walker.currentFunction(), inputHead, std::make_shared<const UsersOfValuesInCountersOfLoops>(walker, index, inputHead) };
    return kept.users;
}

class UsersOfNamesInCountersOfLoops {
public:
    explicit UsersOfNamesInCountersOfLoops(const UsersOfValuesInCountersOfLoops& users)
        : m_users(users)
    {
    }

    UsersOfNamesInCountersOfLoops(const UsersOfValuesInCountersOfLoops& users, const NamesOfCounterOfLoop& names)
        : m_users(users)
    {
        for (ValueID name : names) {
            if (uint32_t use = users.firstUseOf(name); use != UsersOfValuesInCountersOfLoops::none)
                m_next.append(use);
        }
    }

    void takeUsersBehind(ValueID name)
    {
        uint32_t use = m_users.firstUseOf(name);
        while (use != UsersOfValuesInCountersOfLoops::none && m_users.at(use).user.index() <= name.index())
            use = m_users.at(use).next;
        if (use != UsersOfValuesInCountersOfLoops::none)
            m_next.append(use);
    }

    ValueID next()
    {
        ValueID least;
        for (uint32_t use : m_next) {
            ValueID user = m_users.at(use).user;
            if (!least || user.index() < least.index())
                least = user;
        }
        size_t kept = 0;
        for (uint32_t use : m_next) {
            if (m_users.at(use).user == least)
                use = m_users.at(use).next;
            if (use != UsersOfValuesInCountersOfLoops::none)
                m_next[kept++] = use;
        }
        m_next.shrink(kept);
        return least;
    }

private:
    const UsersOfValuesInCountersOfLoops& m_users;
    Vector<uint32_t, mostNamesOfCounterOfLoop> m_next;
};

static bool mayBeInt32InCountersOfLoops(const Walker& walker, Reference value)
{
    if (!value || !value.isValue())
        return false;
    Type type = walker.types().at(walker.typeOf(value));
    if (!type.isJavaScriptValue() || !(type.primitives & Primitive::Integer))
        return false;
    bool isWideInteger = type.isInteger() && !type.integers.isSubsetOf(Range::int32()) && walker.representationOf(value) == Representation::Double;
    return !type.isInteger() || isWideInteger;
}

bool isNumberCarriedBesideTextOrBigInt(const Walker& walker, Reference enters, TypeID carried)
{
    if (!enters || !enters.isRecord() || !carried || isNeverType(carried))
        return false;
    Type round = walker.types().at(carried);
    return round.hasOnlyPrimitives(Primitive::Number | Primitive::AnyString | Primitive::BigInt) && round.primitives & Primitive::Number && !round.isNumber() && walker.types().at(walker.typeOf(enters)).primitives & Primitive::Number;
}

static bool isBoxOrDoubleThatMayBeInt32InCountersOfLoops(const Walker& walker, Reference value)
{
    if (!mayBeInt32InCountersOfLoops(walker, value))
        return false;
    Representation held = walker.representationOf(value);
    return held == Representation::JSValue || held == Representation::OptionalDouble || (held == Representation::Double && walker.types().at(walker.typeOf(value)).isNumber());
}

static VariableID findNumberInVariableThatNoTurnWritesInCountersOfLoops(Walker& walker, const BodyIndex& index, const Effects& ofTurns, ValueID written)
{
    ValueID read = index.stripCasts(written);
    if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = index.stripCasts(index.value(read).operand(0));
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
        return { };
    BodyValue definition = index.value(read);
    if (!numberReadOutOfVariableStays(walker, definition.variable()) && declaredEffectsOfOperation(walker.program(), Opcode::GetModuleVariable, definition).reads.overlaps(ofTurns.writes))
        return { };
    TypeID widest = widestTypeOfOperation(walker, definition);
    if (!widest)
        return { };
    Type type = walker.types().at(widest);
    bool mayBeInt32 = type.isJavaScriptValue() && type.primitives & Primitive::Integer && !type.isInteger();
    if (Reference known = walker.state().variable(definition.variable()); known && !isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, known))
        return { };
    return mayBeInt32 ? definition.variable() : VariableID();
}

static bool isWholeNumberAsWrittenInCountersOfLoops(const BodyIndex& index, ValueID value)
{
    ValueID written = index.stripCasts(value);
    return written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::Integer;
}

static TypeID typeByLookInCountersOfLoops(Walker& walker, const BodyIndex& index, const LoopPreview& preview, ValueID value)
{
    ValueID written = index.stripCasts(value);
    if (!written)
        return { };
    if (written.index() >= preview.firstValue) {
        size_t place = written.index() - preview.firstValue;
        return place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
    }
    Reference built = walker.referenceOf(written);
    return built ? walker.typeOf(built) : TypeID();
}

static bool hasNoFractionByLookInCountersOfLoops(Walker& walker, const BodyIndex& index, const LoopPreview& preview, ValueID value)
{
    TypeID type = typeByLookInCountersOfLoops(walker, index, preview, value);
    return type && !isNeverType(type) && walker.types().at(type).primitives & Primitive::Integer && walker.types().at(type).hasOnlyPrimitives(Primitive::NumberWithoutFraction);
}

static bool isIntegerInFrontOfLoopInCountersOfLoops(Walker& walker, const BodyIndex& index, uint32_t firstValueOfLoop, ValueID step)
{
    ValueID written = index.stripCasts(step);
    Reference held = written && written.index() < firstValueOfLoop ? walker.referenceOf(written) : Reference();
    return held && walker.types().at(walker.typeOf(held)).isInteger();
}

static bool headHoldsParameterAgainstBoundInCountersOfLoops(const BodyIndex& index, BlockID head, ValueID parameter)
{
    BodyTerminator end = index.block(head).terminator();
    ValueID asked = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!asked || index.isParameter(asked) || !allRelations.contains(index.opcodeOf(asked)))
        return false;
    BodyValue relation = index.value(asked);
    return index.stripCasts(relation.operand(0)) == parameter || index.stripCasts(relation.operand(1)) == parameter;
}

static bool isSteppedAllRoundInCountersOfLoops(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, BlockID inputHead, uint32_t position, NamesOfCounterOfLoop& names, const LoopPreview* typesOfSteps = nullptr, Vector<ValueID, 2>* stepsAskedInFront = nullptr, bool admitsWhatCallsGive = false, bool trustsHeadsOfLoopsInside = true)
{
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    bool isStepped = false;
    NamesOfCounterOfLoop namesBehindSteps;
    Vector<ValueID, 2> takenOnTrust;
    size_t numberOfStepsAskedInFrontBefore = stepsAskedInFront ? stepsAskedInFront->size() : 0;
    UsersOfNamesInCountersOfLoops usersLeft(users);
    auto add = [&](ValueID name, bool standsBehindStep) {
        if (names.size() >= mostNamesOfCounterOfLoop)
            return;
        names.append(name);
        usersLeft.takeUsersBehind(name);
        if (standsBehindStep)
            namesBehindSteps.append(name);
    };
    add(index.block(inputHead).parameter(position), false);
    while (ValueID user = usersLeft.next()) {
        if (index.isParameter(user)) {
            if (ValueID narrowed = index.valueNarrowedInto(user)) {
                if (names.contains(narrowed))
                    add(user, namesBehindSteps.contains(narrowed));
                continue;
            }
            BlockID member = index.blockOf(user);
            uint32_t place = user.index() - index.block(member).firstParameter().index();
            std::span<const BodyIncomingEdge> incoming = walker.incomingEdgesOf(index, member);
            bool isHeadOfLoopInside = trustsHeadsOfLoopsInside && member != inputHead && index.isLoopHead(member) && !headHoldsParameterAgainstBoundInCountersOfLoops(index, member, user);
            bool joinsNamesAlone = !incoming.empty() && std::ranges::all_of(incoming, [&](const BodyIncomingEdge& edge) {
                std::span<const ValueID> arguments = edge.target.arguments();
                return place < arguments.size() && ((isHeadOfLoopInside && index.isInLoop(edge.from, index.loopOf(member))) || names.contains(arguments[place]));
            });
            if (joinsNamesAlone) {
                if (isHeadOfLoopInside)
                    takenOnTrust.append(user);
                add(user, std::ranges::any_of(incoming, [&](const BodyIncomingEdge& edge) {
                    return namesBehindSteps.contains(edge.target.arguments()[place]);
                }));
            }
            continue;
        }
        BodyValue operation = index.value(user);
        Opcode opcode = operation.opcode();
        if (opcodesThatStateTypes.contains(opcode) || opcode == Opcode::ToNumeric || opcode == Opcode::ToPropertyKey) {
            if (names.contains(operation.operand(0)))
                add(operation.id(), namesBehindSteps.contains(operation.operand(0)));
            continue;
        }
        bool steps = false;
        if (stepsByOneOfCountersOfLoops.contains(opcode))
            steps = names.contains(operation.operand(0));
        else if ((typesOfSteps ? stepsByNumberOrFactorOfCountersOfLoops : stepsByNumberOfCountersOfLoops).contains(opcode)) {
            auto isStep = [&](ValueID other) {
                if (isWholeNumberAsWrittenInCountersOfLoops(index, other) || (typesOfSteps && hasNoFractionByLookInCountersOfLoops(walker, index, *typesOfSteps, other)))
                    return true;
                if (isIntegerInFrontOfLoopInCountersOfLoops(walker, index, firstValueOfLoop, other))
                    return true;
                ValueID written = stepsAskedInFront ? index.stripCasts(other) : ValueID();
                if (!written || written.index() >= firstValueOfLoop || !isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, walker.referenceOf(written)))
                    return false;
                if (!stepsAskedInFront->contains(written))
                    stepsAskedInFront->append(written);
                return true;
            };
            steps = (names.contains(operation.operand(0)) && isStep(operation.operand(1))) || (opcode != Opcode::Sub && names.contains(operation.operand(1)) && isStep(operation.operand(0)));
        }
        if (steps) {
            add(operation.id(), true);
            isStepped = true;
        }
    }
    for (ValueID trusted : takenOnTrust) {
        BlockID head = index.blockOf(trusted);
        uint32_t place = trusted.index() - index.block(head).firstParameter().index();
        bool everyWayBringsName = names.contains(trusted) && std::ranges::all_of(walker.incomingEdgesOf(index, head), [&](const BodyIncomingEdge& edge) {
            return names.contains(edge.target.arguments()[place]);
        });
        if (everyWayBringsName)
            continue;
        names.shrink(0);
        if (stepsAskedInFront)
            stepsAskedInFront->shrink(numberOfStepsAskedInFrontBefore);
        return isSteppedAllRoundInCountersOfLoops(walker, index, users, inputHead, position, names, typesOfSteps, stepsAskedInFront, admitsWhatCallsGive, false);
    }
    if (!isStepped)
        return false;
    bool someWayBringsStep = !admitsWhatCallsGive;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        if (position >= arguments.size())
            return false;
        if (names.contains(arguments[position])) {
            someWayBringsStep |= namesBehindSteps.contains(arguments[position]);
            continue;
        }
        ValueID brought = index.stripCasts(arguments[position]);
        if (!admitsWhatCallsGive || index.isParameter(brought) || (index.opcodeOf(brought) != Opcode::Call && index.opcodeOf(brought) != Opcode::CallDirect))
            return false;
    }
    return someWayBringsStep;
}

static bool turnsTakeCounterAsIntegerInCountersOfLoops(const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, const NamesOfCounterOfLoop& names)
{
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };
    static constexpr OpcodeSet calls { Opcode::Call, Opcode::CallDirect, Opcode::Construct };
    static constexpr OpcodeSet sumsAndProducts = stepsByNumberOfCountersOfLoops | OpcodeSet { Opcode::Mul };
    auto isName = [&](ValueID operand) {
        return names.contains(operand);
    };
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (index.isParameter(user))
            continue;
        BodyValue operation = index.value(user);
        Opcode opcode = operation.opcode();
        if (accessesByKey.contains(opcode) && isName(operation.propertyOperand()))
            return true;
        if (calls.contains(opcode) && std::ranges::any_of(operation.argumentsOperands(), isName))
            return true;
        if (operatorsOnBitsOfCountersOfLoops.contains(opcode) && (isName(operation.operand(0)) || (opcode != Opcode::BitNot && isName(operation.operand(1)))))
            return true;
        if (sumsAndProducts.contains(opcode) && !isName(operation.id()) && (isName(operation.operand(0)) || isName(operation.operand(1))))
            return true;
    }
    return false;
}

static bool isUsedInArithmeticAlone(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, BlockID inputHead, const LoopPreview& preview, const NamesOfCounterOfLoop& names)
{
    static constexpr OpcodeSet operators { Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Negate };
    auto isName = [&](ValueID operand) {
        return names.contains(operand);
    };
    auto isNumber = [&](ValueID operand) {
        TypeID type = typeByLookInCountersOfLoops(walker, index, preview, operand);
        return type && !isNeverType(type) && walker.types().at(type).isNumber();
    };
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (isName(user) || index.isParameter(user))
            continue;
        BodyValue operation = index.value(user);
        if (operators.contains(operation.opcode()))
            continue;
        if (operation.opcode() != Opcode::Add || !isNumber(isName(operation.operand(0)) ? operation.operand(1) : operation.operand(0)))
            return false;
    }
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        bool isTerminatorOperand = false;
        index.block(member).terminator().forEachOperand([&](ValueID operand, uint32_t) {
            isTerminatorOperand |= isName(operand);
        });
        if (isTerminatorOperand)
            return false;
    }
    return true;
}

static bool isIncreasingKey(const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, const NamesOfCounterOfLoop& names)
{
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet };
    auto isName = [&](ValueID operand) {
        return names.contains(operand);
    };
    bool isKey = false;
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (index.isParameter(user))
            continue;
        BodyValue operation = index.value(user);
        if (isName(operation.id()) && !opcodesThatStateTypes.contains(operation.opcode()) && operation.opcode() != Opcode::ToNumeric && operation.opcode() != Opcode::ToPropertyKey && operation.opcode() != Opcode::Increment)
            return false;
        isKey |= accessesByKey.contains(operation.opcode()) && isName(operation.propertyOperand());
    }
    return isKey;
}

struct BackEdgeInputs {
    Vector<ValueID, 4> elements;
    bool hasOtherInputs { false };
    Vector<ValueID, 8> joins;
};

static void collectBackEdgeInput(Walker& walker, const BodyIndex& index, BlockID inputHead, ValueID value, const NamesOfCounterOfLoop& names, BackEdgeInputs& inputs)
{
    ValueID input = index.stripCasts(value);
    if (names.contains(value) || names.contains(input) || inputs.joins.contains(input))
        return;
    LoopID loop = index.loopOf(inputHead);
    BlockID block = index.blockOf(input);
    if (!index.isParameter(input)) {
        if (index.opcodeOf(input) == Opcode::ElemGet && index.isInLoop(block, loop))
            inputs.elements.append(input);
        else
            inputs.hasOtherInputs = true;
        return;
    }
    if (block == inputHead || index.isLoopHead(block) || !index.isInLoop(block, loop) || inputs.joins.size() == 8) {
        inputs.hasOtherInputs = true;
        return;
    }
    inputs.joins.append(input);
    uint32_t position = input.index() - index.block(block).firstParameter().index();
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        if (position < arguments.size())
            collectBackEdgeInput(walker, index, inputHead, arguments[position], names, inputs);
        else
            inputs.hasOtherInputs = true;
    }
}

static BackEdgeInputs findBackEdgeInputs(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t position, const NamesOfCounterOfLoop& names)
{
    BackEdgeInputs inputs;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, index.loopOf(inputHead)))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        if (position < arguments.size())
            collectBackEdgeInput(walker, index, inputHead, arguments[position], names, inputs);
        else
            inputs.hasOtherInputs = true;
    }
    return inputs;
}

static bool hasElementBackEdgeInput(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t position)
{
    return findBackEdgeInputs(walker, index, inputHead, position, { index.block(inputHead).parameter(position) }).elements.size();
}

static bool isElementCarriedIndex(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, BlockID inputHead, uint32_t position, NamesOfCounterOfLoop& names, Vector<ValueID, 4>* elements = nullptr)
{
    isSteppedAllRoundInCountersOfLoops(walker, index, users, inputHead, position, names);
    BackEdgeInputs inputs = findBackEdgeInputs(walker, index, inputHead, position, names);
    if (inputs.elements.isEmpty() || inputs.hasOtherInputs)
        return false;
    if (elements)
        elements->appendVector(inputs.elements);
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (!index.isParameter(user) && accessesUnderKeysInCountersOfLoops.contains(index.opcodeOf(user)) && names.contains(index.value(user).propertyOperand()))
            return true;
    }
    return false;
}

enum class WhatComesRoundInCountersOfLoops : uint8_t { NotAsked, IsBeingAsked, Int32, Other };

static bool comesRoundAsInt32InCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, const LoopPreview& preview, uint32_t position, Vector<WhatComesRoundInCountersOfLoops, 8>& answers)
{
    if (answers[position] != WhatComesRoundInCountersOfLoops::NotAsked)
        return answers[position] == WhatComesRoundInCountersOfLoops::Int32;
    answers[position] = WhatComesRoundInCountersOfLoops::IsBeingAsked;
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstParameter = index.block(inputHead).firstParameter().index();
    auto everyWayRoundBringsInt32 = [&] {
        bool isInt32OnBackEdges = false;
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
            if (!index.isInLoop(edge.from, loop))
                continue;
            std::span<const ValueID> arguments = edge.target.arguments();
            ValueID brought = position < arguments.size() ? index.stripCasts(arguments[position]) : ValueID();
            if (!brought)
                return false;
            if (index.isParameter(brought) && index.blockOf(brought) == inputHead) {
                if (!comesRoundAsInt32InCountersOfLoops(walker, index, inputHead, preview, brought.index() - firstParameter, answers))
                    return false;
            } else {
                size_t place = brought.index() - preview.firstValue;
                TypeID found = brought.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
                TypeID number = found ? types.withoutPrimitives(found, Primitive::BigInt) : TypeID();
                if (!number || isNeverType(number) || !types.at(number).isInteger() || !types.at(number).integers.isSubsetOf(Range::int32().unionWith(Range::uint32())))
                    return false;
            }
            isInt32OnBackEdges = true;
        }
        return isInt32OnBackEdges;
    };
    bool isInt32OnBackEdges = everyWayRoundBringsInt32();
    answers[position] = isInt32OnBackEdges ? WhatComesRoundInCountersOfLoops::Int32 : WhatComesRoundInCountersOfLoops::Other;
    return isInt32OnBackEdges;
}

static void guessBoundsInCountersOfLoops(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, BlockID inputHead, const NamesOfCounterOfLoop& names, NamesBeforeLoop& namesBeforeLoop, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses)
{
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (index.isParameter(user) || !allRelations.contains(index.opcodeOf(user)))
            continue;
        BodyValue operation = index.value(user);
        for (uint32_t place : { 0u, 1u }) {
            if (!names.contains(operation.operand(place)))
                continue;
            ValueID bound = index.stripCasts(operation.operand(1 - place));
            if (!bound || bound.index() >= firstValueOfLoop)
                continue;
            Reference value = walker.referenceOf(bound);
            if (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, value) || std::ranges::find(entering, value) != entering.end())
                continue;
            size_t listed = guesses.findIf([&](const HoistedSpeculation& guess) {
                return guess.value == value;
            });
            if (listed == notFound)
                guesses.append(HoistedSpeculation { value, TypeTable::int32, { }, namesBeforeLoop.findNamesOf(value) });
            else if (walker.types().at(guesses[listed].tested).isNumber())
                guesses[listed].tested = TypeTable::int32;
        }
    }
}

static bool isKeyOfElementInCountersOfLoops(const BodyIndex& index, ValueID sum)
{
    ValueID name = sum;
    for (ValueID reader = index.onlyUsingValue(name); reader; reader = index.onlyUsingValue(name)) {
        Opcode opcode = index.opcodeOf(reader);
        if (accessesUnderKeysInCountersOfLoops.contains(opcode))
            return index.value(reader).propertyOperand() == name;
        if (!opcodesThatStateTypes.contains(opcode))
            return false;
        name = reader;
    }
    return false;
}

static Reference makeAddendThatNoTurnChangesInCountersOfLoops(Walker& walker, const BodyIndex& index, uint32_t firstValueOfLoop, ValueID addend)
{
    static constexpr OpcodeSet operators { Opcode::Mul, Opcode::Add, Opcode::Sub };
    if (Reference made = walker.nameOfOperatorMadeInFrontOfLoop(addend))
        return made;
    if (index.isParameter(addend) || !operators.contains(index.opcodeOf(addend)))
        return { };
    BodyValue written = index.value(addend);
    for (uint32_t place : { 0u, 1u }) {
        ValueID operand = written.operand(place);
        if (!operand || operand.index() >= firstValueOfLoop)
            return { };
        Reference value = walker.referenceOf(operand);
        if (!value || !walker.types().isSubset(walker.typeOf(value), TypeTable::number))
            return { };
    }
    if (walker.isVersioningDryRun())
        return walker.referenceOf(addend);
    Reference made;
    {
        Walker::ClosedArm takesNameBack(walker);
        if (buildTypedOperator(walker, written) && !walker.hasFailed())
            made = walker.referenceOf(addend);
    }
    if (made)
        walker.noteOperatorMadeInFrontOfLoop(addend, made);
    return made;
}

static void guessAddendsOfKeysInCountersOfLoops(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, BlockID inputHead, const NamesOfCounterOfLoop& names, NamesBeforeLoop& namesBeforeLoop, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses)
{
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (index.isParameter(user) || !stepsByNumberOfCountersOfLoops.contains(index.opcodeOf(user)) || !isKeyOfElementInCountersOfLoops(index, user))
            continue;
        BodyValue operation = index.value(user);
        for (uint32_t place : { 0u, 1u }) {
            if (!names.contains(operation.operand(place)))
                continue;
            ValueID addend = index.stripCasts(operation.operand(1 - place));
            if (!addend)
                continue;
            Reference value = addend.index() < firstValueOfLoop ? walker.referenceOf(addend) : makeAddendThatNoTurnChangesInCountersOfLoops(walker, index, firstValueOfLoop, addend);
            if (!value)
                continue;
            if (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, value) || std::ranges::find(entering, value) != entering.end())
                continue;
            if (guesses.containsIf([&](const HoistedSpeculation& guess) { return guess.value == value; }))
                continue;
            guesses.append(HoistedSpeculation { value, TypeTable::int32, { }, namesBeforeLoop.findNamesOf(value) });
            if (addend.index() >= firstValueOfLoop)
                guesses.last().names.append(addend);
            walker.count(StatisticID::InvariantNumberGuardsHoisted);
        }
    }
}

static void guessAddendsOfSumsCutToInt32InCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, NamesBeforeLoop& namesBeforeLoop, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses)
{
    static constexpr OpcodeSet cuts = operatorsOnBitsOfCountersOfLoops | OpcodeSet { Opcode::URShift };
    static constexpr unsigned deepestSum = 4;
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto ask = [&](ValueID written) {
        Reference value = walker.referenceOf(written);
        if (!value || !value.isValue() || std::ranges::find(entering, value) != entering.end())
            return;
        Type type = walker.types().at(walker.typeOf(value));
        if (!type.isNumberWithoutFraction() || type.isInt32())
            return;
        if (guesses.containsIf([&](const HoistedSpeculation& guess) { return guess.value == value; }))
            return;
        guesses.append(HoistedSpeculation { value, TypeTable::int32, { }, namesBeforeLoop.findNamesOf(value) });
        walker.count(StatisticID::InvariantNumberGuardsHoisted);
    };
    auto follow = [&](auto& self, ValueID operand, unsigned depth) -> void {
        ValueID written = index.stripCasts(operand);
        if (!written)
            return;
        if (written.index() < firstValueOfLoop) {
            if (depth)
                ask(written);
            return;
        }
        if (depth == deepestSum || index.isParameter(written) || !stepsByNumberOfCountersOfLoops.contains(index.opcodeOf(written)))
            return;
        self(self, index.value(written).operand(0), depth + 1);
        self(self, index.value(written).operand(1), depth + 1);
    };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!cuts.contains(operation.opcode()))
                continue;
            follow(follow, operation.operand(0), 0);
            if (operation.opcode() != Opcode::BitNot)
                follow(follow, operation.operand(1), 0);
        }
    }
}

static ValueID parameterOfHeadNamedInCountersOfLoops(const BodyIndex& index, ValueID value, BlockID inputHead)
{
    for (size_t steps = 0; value && steps < mostNamesOfCounterOfLoop; ++steps) {
        value = index.stripCasts(value);
        if (!index.isParameter(value))
            return { };
        if (index.blockOf(value) == inputHead)
            return value;
        value = index.valueNarrowedInto(value);
    }
    return { };
}

static bool isTruthAskedAtHead(const BodyIndex& index, BlockID inputHead, uint32_t position)
{
    BodyTerminator end = index.block(inputHead).terminator();
    ValueID asked = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!asked || index.isParameter(asked) || index.opcodeOf(asked) != Opcode::ToBoolean || !index.isInLoop(end.targets()[0].block(), index.loopOf(inputHead)))
        return false;
    ValueID subject = index.stripCasts(index.value(asked).operand(0));
    while (subject && !index.isParameter(subject) && index.opcodeOf(subject) == Opcode::ToNumeric)
        subject = index.stripCasts(index.value(subject).operand(0));
    return subject == index.block(inputHead).parameter(position);
}

static bool testOfTurnHoldsNumberFromZeroUpInCountersOfLoops(const BodyIndex& index, BlockID inputHead, BlockID block, ValueID number)
{
    for (number = index.stripCasts(number); block && block != inputHead; block = index.immediateDominator(block)) {
        std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(block);
        if (!edge || !index.terminator(edge->from).isConditionalBranch())
            continue;
        ValueID test = index.terminator(edge->from).as<TerminatorKind::Branch>().conditionOperand();
        bool holds = !edge->targetIndex;
        while (test && !index.isParameter(test) && (index.opcodeOf(test) == Opcode::LogicalNot || index.opcodeOf(test) == Opcode::ToBoolean)) {
            holds ^= index.opcodeOf(test) == Opcode::LogicalNot;
            test = index.value(test).operand(0);
        }
        if (!test || index.isParameter(test) || !allRelations.contains(index.opcodeOf(test)))
            continue;
        BodyValue relation = index.value(test);
        bool leftIsGreater = (relation.opcode() == Opcode::Greater || relation.opcode() == Opcode::GreaterEq) == holds;
        bool isStrict = (relation.opcode() == Opcode::Less || relation.opcode() == Opcode::Greater) == holds;
        ValueID bound = index.stripCasts(relation.operand(leftIsGreater));
        if (index.stripCasts(relation.operand(!leftIsGreater)) == number && !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::Integer && index.value(bound).as<Opcode::Integer>().integer() >= (isStrict ? -1 : 0))
            return true;
    }
    return false;
}

static ValueID findUpdatedKey(Walker& walker, const BodyIndex& index, ValueID key)
{
    ValueID value = index.stripCasts(key);
    if (!index.isParameter(value) || index.valueNarrowedInto(value) || index.isLoopHead(index.blockOf(value)))
        return key;
    uint32_t position = value.index() - index.block(index.blockOf(value)).firstParameter().index();
    ValueID updated;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, index.blockOf(value))) {
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID argument = position < arguments.size() ? index.stripCasts(arguments[position]) : ValueID();
        if (argument && !index.isParameter(argument) && index.opcodeOf(argument) == Opcode::ToPropertyKey)
            argument = index.stripCasts(index.value(argument).operand(0));
        while (argument && index.isParameter(argument) && index.valueNarrowedInto(argument))
            argument = index.stripCasts(index.valueNarrowedInto(argument));
        if (!argument || (updated && updated != argument))
            return key;
        updated = argument;
    }
    return updated ? updated : key;
}

CounterHeldBelowRoomOfViews findCounterHeldBelowRoomOfViews(Walker& walker, BlockID inputHead, NOESCAPE const ScopedLambda<TypeID(uint32_t position)>& typeThatEnters, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeInFront, NOESCAPE const ScopedLambda<bool(ValueID)>& isListOnlyReadInLoop)
{
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq };
    CounterHeldBelowRoomOfViews found;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    uint32_t firstValueOfLoop = head.firstParameter().index();
    BodyTerminator end = head.terminator();
    if (end.kind() != TerminatorKind::Branch || !head.numberOfParameters() || !index.holdsAnyOf(accessesUnderKeysInCountersOfLoops | OpcodeSet { Opcode::Call }))
        return found;
    ValueID asked = end.as<TerminatorKind::Branch>().conditionOperand();
    if (!asked || index.isParameter(asked) || !head.holds(asked) || asked.index() + 1 != head.endOfValues().index() || !relations.contains(index.opcodeOf(asked)))
        return found;
    BodyTargets::Iterator target = end.targets().begin();
    if (!index.isInLoop((*target).block(), loop) || index.isInLoop((*++target).block(), loop))
        return found;
    auto rangeOfIntegerFromInFront = [&](ValueID written) -> std::optional<Range> {
        ValueID value = index.stripCasts(written);
        if (value.index() >= firstValueOfLoop) {
            if (index.isParameter(value) || index.opcodeOf(value) != Opcode::Integer)
                return std::nullopt;
            int64_t integer = index.value(value).as<Opcode::Integer>().integer();
            return Range { integer, integer };
        }
        TypeID type = typeInFront(written.index() < firstValueOfLoop ? written : value);
        if (!type || !walker.types().at(type).isInteger())
            return std::nullopt;
        return walker.types().at(type).integers;
    };
    BodyValue relation = index.value(asked);
    ValueID counter = parameterOfHeadNamedInCountersOfLoops(index, relation.operand(0), inputHead);
    if (ValueID read = index.stripCasts(relation.operand(1)); read.index() >= firstValueOfLoop && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName() && index.value(read).key() == walker.program().keyOf(BuiltinNames::length)) {
        ValueID view = index.value(read).objectOperand();
        if (view.index() >= firstValueOfLoop)
            view = index.stripCasts(view);
        if (view.index() < firstValueOfLoop && isViewOfNumbersReadInLine(walker, typeInFront(view)))
            found.viewWhoseLengthIsBound = view;
    }
    if (!counter || (!found.viewWhoseLengthIsBound && !rangeOfIntegerFromInFront(relation.operand(1))))
        return found;
    found.position = counter.index() - firstValueOfLoop;
    found.relation = asked;
    found.bound = relation.operand(1);
    TypeID enters = typeThatEnters(found.position);
    if (!enters || !walker.types().at(enters).isInteger())
        return found;
    auto namesCounter = [&](ValueID value) {
        return parameterOfHeadNamedInCountersOfLoops(index, value, inputHead) == counter;
    };
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID next = found.position < arguments.size() ? index.stripCasts(arguments[found.position]) : ValueID();
        if (!next || index.isParameter(next))
            return found;
        BodyValue step = index.value(next);
        if (step.opcode() == Opcode::Increment && namesCounter(step.operand(0)))
            continue;
        if (step.opcode() != Opcode::Add)
            return found;
        ValueID by = namesCounter(step.operand(0)) ? step.operand(1) : namesCounter(step.operand(1)) ? step.operand(0) : ValueID();
        std::optional<Range> range = by ? rangeOfIntegerFromInFront(by) : std::nullopt;
        if (!range)
            return found;
        if (range->minimum >= 0)
            continue;
        ValueID written = index.stripCasts(by);
        if (found.stepThatMayBeNegative && index.stripCasts(found.stepThatMayBeNegative) != written)
            return found;
        found.stepThatMayBeNegative = by;
    }
    for (BlockID member : index.membersOf(loop)) {
        if (member == inputHead)
            continue;
        for (BodyValue access : index.block(member).values()) {
            if (access.opcode() == Opcode::Call && std::ranges::any_of(access.as<Opcode::Call>().argumentsOperands(), [&](ValueID argument) { return argument && namesCounter(argument); })) {
                forEachElementAccessInInlinedCallee(walker, access, scopedLambda<void(ValueID, ValueID)>([&](ValueID argument, ValueID key) {
                    ValueID view = index.stripCasts(argument);
                    if (key && namesCounter(key) && view.index() < firstValueOfLoop && isViewOfNumbersReadInLine(walker, typeInFront(view)))
                        found.accesses.append({ access.id(), view, { }, { }, false, false });
                }));
            }
            if (!accessesUnderKeysInCountersOfLoops.contains(access.opcode()))
                continue;
            Reference fieldView;
            if (index.stripCasts(access.objectOperand()).index() >= firstValueOfLoop) {
                fieldView = index.loopOf(member) == loop ? nameOfListThatEntersLoopAround(walker, index, access) : Reference();
                if (!fieldView || !fieldView.isValue() || !isViewOfNumbersReadInLine(walker, walker.typeOf(fieldView)))
                    continue;
            }
            ValueID key = index.stripCasts(findUpdatedKey(walker, index, access.propertyOperand()));
            ValueID addend;
            if (!namesCounter(key)) {
                if (index.isParameter(key) || index.opcodeOf(key) != Opcode::Add)
                    continue;
                BodyValue sum = index.value(key);
                addend = namesCounter(sum.operand(0)) ? sum.operand(1) : namesCounter(sum.operand(1)) ? sum.operand(0) : ValueID();
                if (!addend || !rangeOfIntegerFromInFront(addend))
                    continue;
            }
            ValueID view = access.objectOperand().index() < firstValueOfLoop ? access.objectOperand() : index.stripCasts(access.objectOperand());
            bool isOfList = !fieldView && access.opcode() == Opcode::ElemGet && isListOnlyReadInLoop(view);
            if ((!isOfList && !fieldView && !isViewOfNumbersReadInLine(walker, typeInFront(view))) || testOfBodyHoldsPositionBelowLengthOfText(walker, index, member, access.objectOperand(), access.propertyOperand()))
                continue;
            bool turnAsksSignOfAddend = addend && !isOfList && testOfTurnHoldsNumberFromZeroUpInCountersOfLoops(index, inputHead, member, addend);
            found.accesses.append({ access.id(), view, fieldView, addend, isOfList, turnAsksSignOfAddend });
        }
    }
    return found;
}

static constexpr OpcodeSet divisionsOfCountersOfLoops { Opcode::Mod, Opcode::Div };

static TypeID typeOfDivisorInCountersOfLoops(Walker& walker, const BodyIndex& index, uint32_t firstValueOfLoop, ValueID written)
{
    ValueID divisor = index.stripCasts(written);
    TypeID type;
    if (divisor.index() < firstValueOfLoop) {
        Reference built = walker.referenceOf(divisor);
        type = built && built.isRecord() ? walker.typeOf(built) : TypeID();
    } else if (isWholeNumberAsWrittenInCountersOfLoops(index, divisor))
        type = walker.types().integer(Range::one(index.value(divisor).as<Opcode::Integer>().integer()));
    if (!type || !walker.types().at(type).isInteger() || walker.types().at(type).integers.contains(0))
        return { };
    return type;
}

static Range remaindersFromZeroUpInCountersOfLoops(const TypeArena& types, TypeID divisor)
{
    Range divisors = types.at(divisor).integers;
    return { 0, std::max(std::abs(divisors.minimum), std::abs(divisors.maximum)) - 1 };
}

static ValueID numberThatRoundedQuotientDividesInCountersOfLoops(Walker& walker, const BodyIndex& index, uint32_t firstValueOfLoop, const BodyValue& made)
{
    ValueID quotient;
    if (made.opcode() == Opcode::BitOr && isWholeNumberAsWrittenInCountersOfLoops(index, made.operand(1)) && !index.value(index.stripCasts(made.operand(1))).as<Opcode::Integer>().integer())
        quotient = index.stripCasts(made.operand(0));
    else if (made.opcode() == Opcode::Call && made.argumentsOperands().size() == 1 && made.argumentsOperands()[0]) {
        ValueID callee = made.as<Opcode::Call>().calleeOperand();
        callee = callee ? index.stripCasts(callee) : ValueID();
        BuiltinID called = callee && !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::Builtin ? walker.program().builtins().canonicalID(index.value(callee).as<Opcode::Builtin>().builtin()) : BuiltinID();
        if (called == Builtins::MathFloor || called == Builtins::MathTrunc || called == Builtins::MathCeil || called == Builtins::MathRound)
            quotient = index.stripCasts(made.argumentsOperands()[0]);
    }
    if (!quotient || quotient.index() < firstValueOfLoop || index.isParameter(quotient) || index.opcodeOf(quotient) != Opcode::Div)
        return { };
    TypeID divisor = typeOfDivisorInCountersOfLoops(walker, index, firstValueOfLoop, index.value(quotient).operand(1));
    return divisor && walker.types().at(divisor).integers.minimum >= 1 ? index.value(quotient).operand(0) : ValueID();
}

std::optional<NumberThatDivisionsBringRound> findNumberThatDivisionsBringRound(Walker& walker, BlockID inputHead, uint32_t position, std::span<const TypeID> entering, WhatEntersHead whatEnters)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.holdsAnyOf(divisionsOfCountersOfLoops) || index.isEnteredByHandlerEdge(inputHead))
        return std::nullopt;
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    NumberThatDivisionsBringRound found { Range::empty(), true, { } };
    bool someDivisionComesRound = false;
    Vector<ValueID, 16> reached;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> isReached;
    auto follow = [&](ValueID next) {
        next = next ? index.stripCasts(next) : ValueID();
        if (!next)
            return false;
        if (isReached.add(next.index()).isNewEntry)
            reached.append(next);
        return true;
    };
    follow(index.block(inputHead).parameter(position));
    for (size_t next = 0; next < reached.size(); ++next) {
        ValueID value = reached[next];
        if (value.index() < firstValueOfLoop) {
            Reference built = walker.referenceOf(value);
            TypeID type = built && built.isRecord() ? walker.typeOf(built) : TypeID();
            if (!type || !types.at(type).isInteger())
                return std::nullopt;
            found.integers = found.integers.unionWith(types.at(type).integers);
            continue;
        }
        if (!index.isParameter(value)) {
            BodyValue made = index.value(value);
            if (made.opcode() == Opcode::Integer) {
                found.integers = found.integers.unionWith(Range::one(made.as<Opcode::Integer>().integer()));
                continue;
            }
            if (made.opcode() == Opcode::Mod) {
                TypeID divisor = typeOfDivisorInCountersOfLoops(walker, index, firstValueOfLoop, made.operand(1));
                if (!divisor)
                    return std::nullopt;
                found.integers = found.integers.unionWith(remaindersFromZeroUpInCountersOfLoops(types, divisor));
                found.remainders.append(value);
            } else {
                ValueID divided = numberThatRoundedQuotientDividesInCountersOfLoops(walker, index, firstValueOfLoop, made);
                if (!divided || !follow(divided))
                    return std::nullopt;
                found.integers = found.integers.unionWith(Range::one(0));
            }
            someDivisionComesRound = true;
            continue;
        }
        if (ValueID narrowed = index.valueNarrowedInto(value)) {
            if (!follow(narrowed))
                return std::nullopt;
            continue;
        }
        BlockID block = index.blockOf(value);
        if (index.isEnteredByHandlerEdge(block))
            return std::nullopt;
        uint32_t place = value.index() - index.block(block).firstParameter().index();
        bool entersAsInteger = false;
        if (block == inputHead) {
            TypeID enters = place < entering.size() ? entering[place] : TypeID();
            entersAsInteger = enters && types.at(enters).isInteger() && (whatEnters == WhatEntersHead::IsInteger || types.at(enters).integers.minimum >= 0);
            if (entersAsInteger)
                found.integers = found.integers.unionWith(types.at(enters).integers);
            else if (whatEnters == WhatEntersHead::IsInteger)
                return std::nullopt;
        }
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
            std::span<const ValueID> arguments = edge.target.arguments();
            ValueID brought = place < arguments.size() ? arguments[place] : ValueID();
            if (block != inputHead || index.isInLoop(edge.from, loop)) {
                if (!follow(brought))
                    return std::nullopt;
                continue;
            }
            if (entersAsInteger)
                continue;
            ValueID made = brought ? index.stripCasts(brought) : ValueID();
            TypeID divisor = made && !index.isParameter(made) && index.opcodeOf(made) == Opcode::Mod ? typeOfDivisorInCountersOfLoops(walker, index, firstValueOfLoop, index.value(made).operand(1)) : TypeID();
            if (divisor)
                found.integers = found.integers.unionWith(remaindersFromZeroUpInCountersOfLoops(types, divisor));
            else
                found.isDividedInFront = false;
        }
    }
    if (!someDivisionComesRound || found.integers.isEmpty() || !found.integers.isSubsetOf(Range::exact()))
        return std::nullopt;
    return found;
}

static std::optional<Range> findLoopBoundRange(Walker& walker, const BodyIndex& index, ValueID firstValueOfLoop, ValueID written, unsigned operatorsLeft = 2)
{
    ValueID value = index.stripCasts(written);
    if (!index.isParameter(value) && index.opcodeOf(value) == Opcode::Integer)
        return Range::one(index.value(value).as<Opcode::Integer>().integer());
    if (value.index() < firstValueOfLoop.index()) {
        Reference built = walker.referenceOf(value);
        if (!built || !built.isRecord())
            return std::nullopt;
        Type type = walker.types().at(walker.typeOf(built));
        return type.isInteger() ? std::optional { type.integers } : std::nullopt;
    }
    if (index.isParameter(value) || !operatorsLeft || (index.opcodeOf(value) != Opcode::Add && index.opcodeOf(value) != Opcode::Sub))
        return std::nullopt;
    std::optional<Range> left = findLoopBoundRange(walker, index, firstValueOfLoop, index.value(value).operand(0), operatorsLeft - 1);
    std::optional<Range> right = findLoopBoundRange(walker, index, firstValueOfLoop, index.value(value).operand(1), operatorsLeft - 1);
    if (!left || !right || !left->isSubsetOf(Range::exact()) || !right->isSubsetOf(Range::exact()))
        return std::nullopt;
    if (index.opcodeOf(value) == Opcode::Add)
        return Range { left->minimum + right->minimum, left->maximum + right->maximum };
    return Range { left->minimum - right->maximum, left->maximum - right->minimum };
}

static std::optional<int64_t> findConstantAddedToCounter(const BodyIndex& index, ValueID written, ValueID counter)
{
    ValueID value = index.stripCasts(written);
    if (value == counter)
        return 0;
    if (index.isParameter(value))
        return std::nullopt;
    BodyValue sum = index.value(value);
    if (sum.opcode() == Opcode::Increment)
        return index.stripCasts(sum.operand(0)) == counter ? std::optional<int64_t> { 1 } : std::nullopt;
    if (sum.opcode() != Opcode::Add)
        return std::nullopt;
    bool counterIsFirst = index.stripCasts(sum.operand(0)) == counter;
    ValueID addend = counterIsFirst || index.stripCasts(sum.operand(1)) == counter ? index.stripCasts(sum.operand(counterIsFirst ? 1 : 0)) : ValueID();
    if (!addend || index.isParameter(addend) || index.opcodeOf(addend) != Opcode::Integer)
        return std::nullopt;
    int64_t constant = index.value(addend).as<Opcode::Integer>().integer();
    return Range::one(constant).isSubsetOf(Range::int32()) ? std::optional { constant } : std::nullopt;
}

std::optional<Range> findCounterRange(Walker& walker, BlockID inputHead, uint32_t position, int64_t start)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!condition || index.isParameter(condition) || !head.holds(condition) || index.isEnteredByHandlerEdge(inputHead))
        return std::nullopt;
    LoopID loop = index.loopOf(inputHead);
    if (index.loop(loop).numberOfLoopsWithin != 1)
        return std::nullopt;
    if (!index.isInLoop(end.targets()[0].block(), loop) || index.isInLoop(end.targets()[1].block(), loop))
        return std::nullopt;

    ValueID counter = head.parameter(position);
    Opcode opcode = index.opcodeOf(condition);
    if (opcode != Opcode::Less && opcode != Opcode::LessEq && opcode != Opcode::Greater && opcode != Opcode::GreaterEq)
        return std::nullopt;
    bool counterIsFirst = opcode == Opcode::Less || opcode == Opcode::LessEq;
    BodyValue relation = index.value(condition);
    std::optional<int64_t> offset = findConstantAddedToCounter(index, relation.operand(counterIsFirst ? 0 : 1), counter);
    if (!offset)
        return std::nullopt;
    int64_t step = 1;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        std::optional<int64_t> added = position < arguments.size() && arguments[position] ? findConstantAddedToCounter(index, arguments[position], counter) : std::nullopt;
        if (!added || *added < 0)
            return std::nullopt;
        step = std::max(step, *added);
    }

    ValueID written = relation.operand(counterIsFirst ? 1 : 0);
    std::optional<Range> bound = findLoopBoundRange(walker, index, head.firstParameter(), written);
    if (bool isStrided = *offset || step > 1; !bound && isStrided) {
        ValueID read = index.stripCasts(written);
        bool isLength = !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName() && index.value(read).key() == walker.program().keyOf(BuiltinNames::length);
        if (isLength)
            bound = Range { 0, maxTypedArrayLength };
    }
    if (!bound || !bound->isSubsetOf(Range::exact()))
        return std::nullopt;
    int64_t last = bound->maximum - !(opcode == Opcode::LessEq || opcode == Opcode::GreaterEq) - *offset + step;
    if (last <= start || !Range { start, last }.isSubsetOf(Range::exact()))
        return std::nullopt;
    return Range { start, last };
}

static constexpr OpcodeSet relationsOfCountersOfLoops { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
static constexpr OpcodeSet arithmeticOfCountersOfLoops { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement };

static void collectMultipliedParameters(const BodyIndex& index, ValueID written, unsigned operatorsLeft, bool isMultiplied, Vector<ValueID, 8>& parameters)
{
    ValueID value = index.stripCasts(written);
    if (!value)
        return;
    if (index.isParameter(value)) {
        if (isMultiplied && index.isLoopHead(index.blockOf(value)) && !parameters.contains(value))
            parameters.append(value);
        return;
    }
    BodyValue made = index.value(value);
    bool isArithmetic = arithmeticOfCountersOfLoops.contains(made.opcode());
    if (made.opcode() != Opcode::ToNumeric && (!isArithmetic || !operatorsLeft))
        return;
    made.forEachOperand([&](ValueID operand, uint32_t) {
        collectMultipliedParameters(index, operand, operatorsLeft - isArithmetic, isMultiplied || made.opcode() == Opcode::Mul, parameters);
    });
}

std::span<const Walker::ComputedComparison> Walker::computedComparisonsOf(ValueID parameter)
{
    static constexpr unsigned mostOperators = 4;
    static constexpr OpcodeSet holdsLeftBelow { Opcode::Less, Opcode::LessEq };
    const BodyIndex& index = currentBodyIndex();
    const Vector<ComputedComparison>& listed = m_computedComparisons.ensure(&index, [&] {
        Vector<ComputedComparison> found;
        if (!index.holdsAnyOf(relationsOfCountersOfLoops) || !index.holdsAnyOf(OpcodeSet { Opcode::Mul }))
            return found;
        Vector<ValueID, 8> parameters;
        for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
            if (!index.loopOf(BlockID(block)))
                continue;
            for (BodyValue relation : index.block(BlockID(block)).values()) {
                if (!relationsOfCountersOfLoops.contains(relation.opcode()))
                    continue;
                uint32_t below = holdsLeftBelow.contains(relation.opcode()) ? 0 : 1;
                parameters.shrink(0);
                collectMultipliedParameters(index, relation.operand(below), mostOperators, false, parameters);
                for (ValueID multiplied : parameters)
                    found.append({ multiplied, relation.id(), index.stripCasts(relation.operand(1 - below)) });
            }
        }
        std::ranges::stable_sort(found, { }, &ComputedComparison::parameter);
        return found;
    }).iterator->value;
    auto range = std::ranges::equal_range(listed, parameter, { }, &ComputedComparison::parameter);
    return { range.begin(), range.end() };
}

bool isMultipliedBelowLength(Walker& walker, BlockID inputHead, uint32_t position)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto isLength = [&](ValueID bound) {
        if (bound.index() >= firstValueOfLoop)
            return !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::PropGet && !index.value(bound).isFreeName() && index.value(bound).key() == walker.program().keyOf(BuiltinNames::length);
        Reference built = walker.referenceOf(bound);
        if (!built || !built.isRecord())
            return false;
        Type type = walker.types().at(walker.typeOf(built));
        return type.isInteger() && type.integers.maximum <= Range::uint32().maximum;
    };
    return std::ranges::any_of(walker.computedComparisonsOf(index.block(inputHead).parameter(position)), [&](const Walker::ComputedComparison& comparison) {
        return index.isInLoop(index.blockOf(comparison.relation), loop) && isLength(comparison.bound);
    });
}

bool operatorsOnBitsAloneBringRound(Walker& walker, BlockID inputHead, uint32_t position, std::span<const TypeID> entering)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.holdsAnyOf(operatorsOnBitsOfCountersOfLoops) || index.isEnteredByHandlerEdge(inputHead))
        return false;
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto isInt32 = [&](TypeID type) {
        return type && types.at(type).isInteger() && types.at(type).integers.isSubsetOf(Range::int32());
    };
    bool someOperatorComesRound = false;
    Vector<ValueID, 16> reached;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> isReached;
    auto follow = [&](ValueID next) {
        next = next ? index.stripCasts(next) : ValueID();
        if (!next)
            return false;
        if (isReached.add(next.index()).isNewEntry)
            reached.append(next);
        return true;
    };
    follow(index.block(inputHead).parameter(position));
    for (size_t next = 0; next < reached.size(); ++next) {
        ValueID value = reached[next];
        if (value.index() < firstValueOfLoop) {
            Reference built = walker.referenceOf(value);
            if (!built || !built.isRecord() || !isInt32(walker.typeOf(built)))
                return false;
            continue;
        }
        if (!index.isParameter(value)) {
            BodyValue made = index.value(value);
            if (made.opcode() == Opcode::Integer && Range::one(made.as<Opcode::Integer>().integer()).isSubsetOf(Range::int32()))
                continue;
            if (!operatorsOnBitsOfCountersOfLoops.contains(made.opcode()))
                return false;
            someOperatorComesRound = true;
            continue;
        }
        if (ValueID narrowed = index.valueNarrowedInto(value)) {
            if (!follow(narrowed))
                return false;
            continue;
        }
        BlockID block = index.blockOf(value);
        if (index.isEnteredByHandlerEdge(block))
            return false;
        uint32_t place = value.index() - index.block(block).firstParameter().index();
        if (block == inputHead && !isInt32(place < entering.size() ? entering[place] : TypeID()))
            return false;
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
            std::span<const ValueID> arguments = edge.target.arguments();
            if ((block != inputHead || index.isInLoop(edge.from, loop)) && !follow(place < arguments.size() ? arguments[place] : ValueID()))
                return false;
        }
    }
    return someOperatorComesRound;
}

static TypeID typeWhereDivisionsBringIntegersInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, const LoopPreview& preview, std::span<const TypeID> carried, ValueID written, unsigned depth)
{
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Mod, Opcode::Increment, Opcode::Decrement };
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    if (ValueID parameter = parameterOfHeadNamedInCountersOfLoops(index, written, inputHead); parameter && carried[parameter.index() - firstValueOfLoop])
        return carried[parameter.index() - firstValueOfLoop];
    ValueID value = written ? index.stripCasts(written) : ValueID();
    if (!value || !depth || value.index() < firstValueOfLoop || index.isParameter(value) || !arithmetic.contains(index.opcodeOf(value)))
        return value ? typeByLookInCountersOfLoops(walker, index, preview, value) : TypeID();
    BodyValue operation = index.value(value);
    Vector<TypeID, 2> operands;
    for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(operation.opcode()); ++operandIndex) {
        operands.append(typeWhereDivisionsBringIntegersInCountersOfLoops(walker, index, inputHead, preview, carried, operation.operand(operandIndex), depth - 1));
        if (!operands.last() || isNeverType(operands.last()))
            return { };
    }
    return resultTypeOfOperator(walker.typing(), operation.opcode(), operands.span());
}

static void appendNumbersThatDivisionsBringRoundInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, const Vector<HoistedSpeculation, 4>& guesses, Vector<HoistedCounterCheck, 2>& counters)
{
    static constexpr unsigned depthOfDividend = 4;
    if (!guesses.isEmpty() || !counters.isEmpty())
        return;
    TypeArena& types = walker.types();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    Vector<TypeID, 8> typesEntering;
    for (Reference enters : entering)
        typesEntering.append(enters && enters.isRecord() ? walker.typeOf(enters) : TypeID());
    Vector<TypeID, 8> carried;
    Vector<ValueID, 4> remainders;
    bool somethingIsAsked = false;
    for (uint32_t position = 0; position < index.block(inputHead).numberOfParameters(); ++position) {
        carried.append(TypeID());
        TypeID enters = position < typesEntering.size() ? typesEntering[position] : TypeID();
        if (!enters || !types.at(enters).isJavaScriptValue())
            continue;
        std::optional<NumberThatDivisionsBringRound> found = findNumberThatDivisionsBringRound(walker, inputHead, position, typesEntering.span(), WhatEntersHead::MayBeAskedInFront);
        if (!found)
            continue;
        carried.last() = types.integer(found->isDividedInFront ? found->integers : found->integers.unionWith(Range::length()));
        somethingIsAsked |= !types.isSubset(enters, carried.last());
        remainders.appendVector(found->remainders);
    }
    if (!somethingIsAsked)
        return;
    for (ValueID remainder : remainders) {
        BodyValue operation = index.value(remainder);
        TypeID dividend = typeWhereDivisionsBringIntegersInCountersOfLoops(walker, index, inputHead, preview, carried.span(), operation.operand(0), depthOfDividend);
        TypeID divisor = typeOfDivisorInCountersOfLoops(walker, index, firstValueOfLoop, operation.operand(1));
        if (!dividend || isNeverType(dividend) || !divisor || chooseFormOfOperator(walker.program(), types, Opcode::Mod, OperatorTypes { dividend, divisor }) != OperatorForm::Integers)
            return;
    }
    for (uint32_t position = 0; position < carried.size() && position < entering.size(); ++position) {
        Reference enters = entering[position];
        if (!carried[position] || !enters || !enters.isValue() || types.isSubset(typesEntering[position], carried[position]) || !types.overlap(typesEntering[position], carried[position]))
            continue;
        if (!types.at(typesEntering[position]).isInteger() && !isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, enters))
            continue;
        bool isTestedAlready = std::ranges::find(entering.first(position), enters) != entering.first(position).end()
            || counters.containsIf([&](const HoistedCounterCheck& counter) { return counter.position == position; })
            || guesses.containsIf([&](const HoistedSpeculation& guess) { return guess.value == enters; });
        if (isTestedAlready)
            continue;
        counters.append({ position, carried[position], false, true });
        walker.count(StatisticID::InvariantNumberGuardsHoisted);
    }
}

Vector<ValueID, 4> findElementCarriedIndexes(Walker& walker, BlockID inputHead)
{
    Vector<ValueID, 4> found;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return found;
    for (uint32_t position = 0; position < index.block(inputHead).numberOfParameters(); ++position) {
        NamesOfCounterOfLoop names;
        Vector<ValueID, 4> elements;
        if (hasElementBackEdgeInput(walker, index, inputHead, position) && isElementCarriedIndex(walker, index, *usersOfValuesOfLoopInCountersOfLoops(walker, index, inputHead), inputHead, position, names, &elements))
            found.appendVector(elements);
    }
    return found;
}

BitVector namesOfCountersAskedForInt32InFrontOfLoop(Walker& walker, BlockID inputHead, std::span<const Reference> entering)
{
    BitVector found;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !index.holdsAnyOf(stepsByOneOfCountersOfLoops | stepsByNumberOfCountersOfLoops | OpcodeSet { Opcode::ElemGet }))
        return found;
    std::shared_ptr<const UsersOfValuesInCountersOfLoops> users;
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    uint32_t numberOfParameters = index.block(inputHead).numberOfParameters();
    for (uint32_t position = 0; position < numberOfParameters && position < entering.size(); ++position) {
        bool entersAsInt32 = entering[position] && walker.types().at(walker.typeOf(entering[position])).isInt32();
        bool bringsElement = hasElementBackEdgeInput(walker, index, inputHead, position);
        if (entersAsInt32 ? !bringsElement : !isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, entering[position]) || std::ranges::find(entering.first(position), entering[position]) != entering.first(position).end())
            continue;
        if (!users)
            users = usersOfValuesOfLoopInCountersOfLoops(walker, index, inputHead);
        NamesOfCounterOfLoop names;
        if (bringsElement ? !isElementCarriedIndex(walker, index, *users, inputHead, position, names) : !isSteppedAllRoundInCountersOfLoops(walker, index, *users, inputHead, position, names))
            continue;
        for (ValueID name : names)
            found.set(name.index() - firstValueOfLoop);
    }
    return found;
}

static bool isComparedWithPossibleInt32(Walker& walker, const BodyIndex& index, BlockID inputHead, ValueID counter)
{
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!condition || index.isParameter(condition) || !head.holds(condition) || !allRelations.contains(index.opcodeOf(condition)))
        return false;
    BodyValue relation = index.value(condition);
    if (index.stripCasts(relation.operand(0)) != counter)
        return false;
    ValueID bound = index.stripCasts(relation.operand(1));
    return bound.index() < head.firstParameter().index() && isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, walker.referenceOf(bound));
}

static bool accessesViewUnderCounter(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, BlockID inputHead, const NamesOfCounterOfLoop& names)
{
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (index.isParameter(user) || !accessesUnderKeysInCountersOfLoops.contains(index.opcodeOf(user)) || !names.contains(index.value(user).propertyOperand()))
            continue;
        BodyValue access = index.value(user);
        ValueID object = index.stripCasts(access.objectOperand());
        bool isBeforeLoop = object.index() < index.block(inputHead).firstParameter().index();
        Reference view = isBeforeLoop ? walker.referenceOf(object) : index.loopOf(index.blockOf(user)) == index.loopOf(inputHead) ? nameOfListThatEntersLoopAround(walker, index, access) : Reference();
        if (view && view.isValue() && isViewOfNumbersReadInLine(walker, walker.typeOf(view)))
            return true;
    }
    return false;
}

static Vector<ValueID, 4> readsOffListsFoundInTurnInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, const LoopPreview& preview)
{
    TypeArena& types = walker.types();
    Vector<ValueID, 4> reads;
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue read : index.block(member).values()) {
            if (read.opcode() != Opcode::ElemGet)
                continue;
            ValueID list = index.stripCasts(read.objectOperand());
            if (!list || list.index() < preview.firstValue || index.isParameter(list))
                continue;
            Opcode finds = index.opcodeOf(list);
            if (finds != Opcode::PrivateGet && finds != Opcode::ElemGet && (finds != Opcode::PropGet || index.value(list).isFreeName()))
                continue;
            TypeID held = typeByLookInCountersOfLoops(walker, index, preview, list);
            const ClassPart* onlyClass = held && !isNeverType(held) && types.at(types.withoutNullish(held)).isObject() ? singleBuiltinClassOf(types, types.at(types.withoutNullish(held))) : nullptr;
            if (!onlyClass || walker.program().builtins().typedArrayTypeOf(onlyClass->builtinClass) == NotTypedArray)
                reads.append(read.id());
        }
    }
    return reads;
}

std::optional<HoistedCounterCheck> findCounterOfAnyKind(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !index.opcodes().contains(Opcode::ElemGet) || !index.holdsAnyOf(stepsByOneOfCountersOfLoops | stepsByNumberOfCountersOfLoops))
        return std::nullopt;
    std::optional<Vector<ValueID, 4>> reads;
    std::shared_ptr<const UsersOfValuesInCountersOfLoops> users;
    uint32_t numberOfParameters = index.block(inputHead).numberOfParameters();
    for (uint32_t position = 0; position < numberOfParameters && position < entering.size(); ++position) {
        Reference enters = entering[position];
        if (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, enters) || walker.types().at(walker.typeOf(enters)).isNumber())
            continue;
        if (!reads)
            reads = readsOffListsFoundInTurnInCountersOfLoops(walker, index, inputHead, preview);
        if (reads->isEmpty())
            return std::nullopt;
        if (!users)
            users = usersOfValuesOfLoopInCountersOfLoops(walker, index, inputHead);
        NamesOfCounterOfLoop names;
        bool isKey = isSteppedAllRoundInCountersOfLoops(walker, index, *users, inputHead, position, names) && std::ranges::any_of(*reads, [&](ValueID read) {
            return names.contains(index.value(read).propertyOperand());
        });
        if (isKey)
            return HoistedCounterCheck { position, isIncreasingKey(index, *users, names) ? walker.types().integer(Range { 0, std::numeric_limits<int32_t>::max() }) : TypeID(TypeTable::int32) };
    }
    return std::nullopt;
}

Vector<HoistedCounterCheck, 2> findHoistedCounterChecks(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses)
{
    Vector<HoistedCounterCheck, 2> counters;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return counters;
    const BodyIndex& index = walker.currentBodyIndex();
    bool holdsOperatorsOnBits = index.holdsAnyOf(operatorsOnBitsOfCountersOfLoops | OpcodeSet { Opcode::URShift });
    bool holdsElementReads = index.holdsAnyOf(OpcodeSet { Opcode::ElemGet });
    if (!holdsOperatorsOnBits && !holdsElementReads && !index.holdsAnyOf(stepsByOneOfCountersOfLoops | stepsByNumberOfCountersOfLoops | divisionsOfCountersOfLoops))
        return counters;
    bool holdsAccessesUnderKeys = index.holdsAnyOf(accessesUnderKeysInCountersOfLoops) && index.holdsAnyOf(stepsByNumberOfCountersOfLoops);
    uint32_t numberOfParameters = index.block(inputHead).numberOfParameters();
    Vector<WhatComesRoundInCountersOfLoops, 8> whatComesRound;
    whatComesRound.fill(WhatComesRoundInCountersOfLoops::NotAsked, numberOfParameters);
    std::shared_ptr<const UsersOfValuesInCountersOfLoops> usersOnceAsked;
    auto users = [&]() -> const UsersOfValuesInCountersOfLoops& {
        if (!usersOnceAsked)
            usersOnceAsked = usersOfValuesOfLoopInCountersOfLoops(walker, index, inputHead);
        return *usersOnceAsked;
    };
    NamesBeforeLoop namesBeforeLoop(walker, inputHead);
    for (uint32_t position = 0; position < numberOfParameters && position < entering.size(); ++position) {
        Reference enters = entering[position];
        bool entersAsInt32 = enters && walker.types().at(walker.typeOf(enters)).isInt32();
        TypeID inTurns = position < preview.typesOfParameters.size() ? preview.typesOfParameters[position] : TypeID();
        if (holdsElementReads && inTurns && !isNeverType(inTurns) && !walker.types().at(inTurns).isInteger() && (walker.types().at(inTurns).primitives & Primitive::Integer) && (entersAsInt32 || isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, enters)) && hasElementBackEdgeInput(walker, index, inputHead, position)) {
            bool isTested = !entersAsInt32 && (std::ranges::find(entering.first(position), enters) != entering.first(position).end() || guesses.containsIf([&](const HoistedSpeculation& guess) {
                return guess.value == enters;
            }));
            NamesOfCounterOfLoop names;
            if (!isTested && isElementCarriedIndex(walker, index, users(), inputHead, position, names)) {
                counters.append({ position, TypeTable::int32, true });
                walker.count(StatisticID::CarriedIndexGuardsHoisted);
                continue;
            }
        }
        if (entersAsInt32 && index.holdsAnyOf(accessesUnderKeysInCountersOfLoops) && isComparedWithPossibleInt32(walker, index, inputHead, index.block(inputHead).parameter(position))) {
            NamesOfCounterOfLoop names;
            if (isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names) && accessesViewUnderCounter(walker, index, users(), inputHead, names))
                guessBoundsInCountersOfLoops(walker, index, users(), inputHead, names, namesBeforeLoop, entering, guesses);
        }
        if (entersAsInt32 && (!guesses.isEmpty() || holdsAccessesUnderKeys)) {
            NamesOfCounterOfLoop names;
            if (isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names)) {
                if (holdsAccessesUnderKeys)
                    guessAddendsOfKeysInCountersOfLoops(walker, index, users(), inputHead, names, namesBeforeLoop, entering, guesses);
                if (!guesses.isEmpty())
                    guessBoundsInCountersOfLoops(walker, index, users(), inputHead, names, namesBeforeLoop, entering, guesses);
            }
            TypeID byLook = position < preview.typesOfParameters.size() ? preview.typesOfParameters[position] : TypeID();
            if (byLook && !isNeverType(byLook) && !walker.types().at(byLook).isInteger() && walker.types().at(byLook).isNumberWithoutFraction()) {
                names.shrink(0);
                if (isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names, &preview) && turnsTakeCounterAsIntegerInCountersOfLoops(index, users(), names))
                    counters.append({ position, TypeTable::int32, true });
            }
            continue;
        }
        if (!mayBeInt32InCountersOfLoops(walker, enters))
            continue;
        bool isTestedAlready = std::ranges::find(entering.first(position), enters) != entering.first(position).end() || guesses.containsIf([&](const HoistedSpeculation& guess) {
            return guess.value == enters;
        });
        if (isTestedAlready)
            continue;
        if (holdsOperatorsOnBits && comesRoundAsInt32InCountersOfLoops(walker, index, inputHead, preview, position, whatComesRound)) {
            Type entering = walker.types().at(walker.typeOf(enters));
            bool entersAsWiderInteger = !(entering.primitives & Primitive::Double) && !entering.integers.isSubsetOf(Range::int32());
            BodyTerminator end = index.block(inputHead).terminator();
            ValueID asked = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
            auto isThisWord = [&](ValueID operand) {
                return parameterOfHeadNamedInCountersOfLoops(index, operand, inputHead) == index.block(inputHead).parameter(position);
            };
            bool isComparedByHead = asked && !index.isParameter(asked) && allRelations.contains(index.opcodeOf(asked)) && (isThisWord(index.value(asked).operand(0)) || isThisWord(index.value(asked).operand(1)));
            TypeID tested = entersAsWiderInteger ? TypeID(TypeTable::number) : TypeID(TypeTable::int32);
            if (entering.primitives & Primitive::Double && !isComparedByHead) {
                tested = walker.types().integer(Range::int32().unionWith(Range::uint32()));
                walker.count(StatisticID::Int32OrUint32GuardsHoisted);
            }
            counters.append({ position, tested });
            continue;
        }
        NamesOfCounterOfLoop names;
        if (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, enters))
            continue;
        bool isCheckedOnBackEdge = !isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names);
        Vector<ValueID, 2> stepsAskedInFront;
        if (isCheckedOnBackEdge) {
            names.shrink(0);
            if (!isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names, &preview)) {
                names.shrink(0);
                if (!holdsAccessesUnderKeys || !isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names, &preview, &stepsAskedInFront)) {
                    names.shrink(0);
                    stepsAskedInFront.shrink(0);
                    if (!isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names, &preview, nullptr, true))
                        continue;
                }
            }
        }
        if ((isCheckedOnBackEdge || walker.types().at(walker.typeOf(enters)).isNumber()) && !turnsTakeCounterAsIntegerInCountersOfLoops(index, users(), names))
            continue;
        if (!isCheckedOnBackEdge && !walker.types().at(walker.typeOf(enters)).isNumber() && isUsedInArithmeticAlone(walker, index, users(), inputHead, preview, names)) {
            counters.append({ position, TypeTable::number });
            walker.count(StatisticID::InvariantNumberGuardsHoisted);
            continue;
        }
        bool stepEnters = std::ranges::any_of(stepsAskedInFront, [&](ValueID step) {
            return std::ranges::find(entering, walker.referenceOf(step)) != entering.end();
        });
        if (stepEnters)
            continue;
        bool isFromZeroUp = !isCheckedOnBackEdge && (isIncreasingKey(index, users(), names) || isTruthAskedAtHead(index, inputHead, position));
        counters.append({ position, isFromZeroUp ? walker.types().integer(Range { 0, std::numeric_limits<int32_t>::max() }) : TypeID(TypeTable::int32), isCheckedOnBackEdge });
        bool loopInsideStepsIt = std::ranges::any_of(names, [&](ValueID name) {
            return index.isParameter(name) && index.blockOf(name) != inputHead && index.isLoopHead(index.blockOf(name));
        });
        if (loopInsideStepsIt)
            walker.count(StatisticID::NestedLoopCounterGuardsHoisted);
        for (ValueID step : stepsAskedInFront) {
            Reference value = walker.referenceOf(step);
            if (guesses.containsIf([&](const HoistedSpeculation& guess) { return guess.value == value; }))
                continue;
            guesses.append(HoistedSpeculation { value, TypeTable::int32, { }, namesBeforeLoop.findNamesOf(value) });
            walker.count(StatisticID::InvariantNumberGuardsHoisted);
        }
        guessBoundsInCountersOfLoops(walker, index, users(), inputHead, names, namesBeforeLoop, entering, guesses);
        if (holdsAccessesUnderKeys)
            guessAddendsOfKeysInCountersOfLoops(walker, index, users(), inputHead, names, namesBeforeLoop, entering, guesses);
    }
    if (index.holdsAnyOf(divisionsOfCountersOfLoops))
        appendNumbersThatDivisionsBringRoundInCountersOfLoops(walker, index, inputHead, preview, entering, guesses, counters);
    if (index.holdsAnyOf(stepsByNumberOfCountersOfLoops))
        guessAddendsOfSumsCutToInt32InCountersOfLoops(walker, index, inputHead, namesBeforeLoop, entering, guesses);
    for (uint32_t position = 0; !walker.sumsAreNotAskedInRow() && position < numberOfParameters && position < entering.size() && position < preview.typesOfParameters.size(); ++position) {
        Reference enters = entering[position];
        if (!isNumberCarriedBesideTextOrBigInt(walker, enters, preview.typesOfParameters[position]))
            continue;
        bool isTestedAlready = std::ranges::find(entering.first(position), enters) != entering.first(position).end()
            || counters.containsIf([&](const HoistedCounterCheck& counter) { return counter.position == position; })
            || guesses.containsIf([&](const HoistedSpeculation& guess) { return guess.value == enters; });
        if (isTestedAlready)
            continue;
        counters.append({ position, TypeTable::number, true, true, true });
        walker.count(StatisticID::SumsGuardedAsNumbers);
    }
    return counters;
}

void guessDivisorsOfIntegersInFrontOfLoop(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses, Vector<HoistedVariableCheck, 2>& variables)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !index.holdsAnyOf(OpcodeSet { Opcode::Mod }))
        return;
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    TypeID positiveInt32 = types.integer(Range { 1, std::numeric_limits<int32_t>::max() });
    NamesBeforeLoop namesBeforeLoop(walker, inputHead);
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::Mod)
                continue;
            ValueID divisor = index.stripCasts(operation.operand(OperandIndex::Mod::right));
            if (!divisor)
                continue;
            VariableID variable = divisor.index() >= firstValueOfLoop ? findNumberInVariableThatNoTurnWritesInCountersOfLoops(walker, index, preview.effects, divisor) : VariableID();
            if (divisor.index() >= firstValueOfLoop && !variable)
                continue;
            Reference value = variable ? Reference() : walker.referenceOf(divisor);
            if (!variable && (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, value) || std::ranges::find(entering, value) != entering.end()))
                continue;
            TypeID dividend = typeByLookInCountersOfLoops(walker, index, preview, operation.operand(OperandIndex::Mod::left));
            if (!dividend || isNeverType(dividend) || chooseFormOfOperator(walker.program(), types, Opcode::Mod, OperatorTypes { dividend, positiveInt32 }) != OperatorForm::Integers)
                continue;
            if (variable) {
                if (!variables.containsIf([&](const HoistedVariableCheck& asked) { return asked.variable == variable; })) {
                    variables.append({ variable, positiveInt32, StatisticID::DivisorVariableReadsHoisted });
                    walker.count(StatisticID::InvariantNumberGuardsHoisted);
                }
                continue;
            }
            size_t listed = guesses.findIf([&](const HoistedSpeculation& guess) {
                return guess.value == value;
            });
            if (listed == notFound) {
                guesses.append(HoistedSpeculation { value, positiveInt32, { }, namesBeforeLoop.findNamesOf(value) });
                walker.count(StatisticID::InvariantNumberGuardsHoisted);
            } else if (types.isSubset(positiveInt32, guesses[listed].tested))
                guesses[listed].tested = positiveInt32;
        }
    }
}

static Reference makeBoundOutOfNumberFromInFrontInCountersOfLoops(Walker& walker, const BodyIndex& index, const BodyBlock& head, ValueID bound)
{
    static constexpr OpcodeSet operators { Opcode::Add, Opcode::Sub };
    if (Reference made = walker.nameOfOperatorMadeInFrontOfLoop(bound))
        return made;
    if (index.isParameter(bound) || !head.holds(bound) || !operators.contains(index.opcodeOf(bound)))
        return { };
    uint32_t firstValueOfLoop = head.firstParameter().index();
    BodyValue written = index.value(bound);
    ValueID writtenInteger;
    for (uint32_t place : { 0u, 1u }) {
        ValueID operand = written.operand(place);
        if (!operand)
            return { };
        if (operand.index() >= firstValueOfLoop) {
            if (writtenInteger || index.isParameter(operand) || index.opcodeOf(operand) != Opcode::Integer)
                return { };
            writtenInteger = operand;
            continue;
        }
        Reference value = walker.referenceOf(operand);
        if (!value || !walker.types().isSubset(walker.typeOf(value), TypeTable::number))
            return { };
    }
    if (!writtenInteger)
        return { };
    if (walker.isVersioningDryRun())
        return walker.referenceOf(bound);
    Reference made;
    {
        Walker::ClosedArm takesNameBack(walker);
        Draft integer(Opcode::Integer, walker.originOf(index.value(writtenInteger)));
        integer.setInteger(index.value(writtenInteger).as<Opcode::Integer>().integer());
        walker.bind(writtenInteger, walker.constant(WTF::move(integer)));
        if (buildTypedOperator(walker, written) && !walker.hasFailed())
            made = walker.referenceOf(bound);
    }
    if (made)
        walker.noteOperatorMadeInFrontOfLoop(bound, made);
    return made;
}

LoopBoundComparison findLoopBoundComparison(Walker& walker, BlockID inputHead, const Effects& ofTurns, std::span<const Reference> entering, std::span<const HoistedCounterCheck> askedInFront)
{
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq };
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !walker.has(Mechanism::LoopInvariants))
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.holdsAnyOf(stepsByOneOfCountersOfLoops | stepsByNumberOfCountersOfLoops))
        return { };
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    if (end.kind() != TerminatorKind::Branch || !head.numberOfParameters())
        return { };
    uint32_t firstValueOfLoop = head.firstParameter().index();
    ValueID asked = end.as<TerminatorKind::Branch>().conditionOperand();
    if (!asked || index.isParameter(asked) || !head.holds(asked) || asked.index() + 1 != head.endOfValues().index() || !relations.contains(index.opcodeOf(asked)))
        return { };
    BodyTargets::Iterator target = end.targets().begin();
    if (!index.isInLoop((*target).block(), loop) || index.isInLoop((*++target).block(), loop))
        return { };
    BodyValue relation = index.value(asked);
    ValueID counter = parameterOfHeadNamedInCountersOfLoops(index, relation.operand(0), inputHead);
    ValueID number = index.stripCasts(relation.operand(1));
    if (!counter || !number)
        return { };
    VariableID variable = number.index() >= firstValueOfLoop ? findNumberInVariableThatNoTurnWritesInCountersOfLoops(walker, index, ofTurns, number) : VariableID();
    Reference madeInFront = number.index() >= firstValueOfLoop && !variable ? makeBoundOutOfNumberFromInFrontInCountersOfLoops(walker, index, head, number) : Reference();
    if (number.index() >= firstValueOfLoop && !variable && !madeInFront)
        return { };
    uint32_t position = counter.index() - firstValueOfLoop;
    Reference enters = position < entering.size() ? entering[position] : Reference();
    bool isAskedForInt32 = std::ranges::any_of(askedInFront, [&](const HoistedCounterCheck& counter) {
        return counter.position == position && counter.tested == TypeTable::int32 && !counter.isCheckedOnBackEdge && !counter.testMayWellFail;
    });
    if (!enters || !enters.isRecord() || (!isAskedForInt32 && !walker.types().at(walker.typeOf(enters)).isInteger()))
        return { };
    Reference value = variable ? Reference() : madeInFront ? madeInFront : walker.referenceOf(number);
    if (!variable && (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, value) || std::ranges::find(entering, value) != entering.end()))
        return { };
    NamesOfCounterOfLoop names;
    std::shared_ptr<const UsersOfValuesInCountersOfLoops> users = usersOfValuesOfLoopInCountersOfLoops(walker, index, inputHead);
    if (!isSteppedAllRoundInCountersOfLoops(walker, index, *users, inputHead, position, names) || !turnsTakeCounterAsIntegerInCountersOfLoops(index, *users, names))
        return { };
    return { asked, number, variable };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
