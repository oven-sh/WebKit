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
#include "AOTWalkBlocksThatEveryTurnPasses.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkNumberForms.h"
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
    if (!type.isInteger())
        return true;
    if (type.integers.isSubsetOf(Range::int32()))
        return false;
    Representation held = walker.representationOf(value);
    return held == Representation::Double || (held == Representation::Int64 && std::ranges::contains(walker.code().at(entryBlock).parameters(), value.id()));
}

bool isNumberCarriedBesideTextOrBigInt(const Walker& walker, Reference enters, TypeID carried)
{
    if (!enters || !enters.isRecord() || !carried || isNeverType(carried))
        return false;
    Type round = walker.types().at(carried);
    return round.hasOnlyPrimitives(Primitive::Number | Primitive::AnyString | Primitive::BigInt) && round.primitives & Primitive::Number && !round.isNumber() && walker.types().at(walker.typeOf(enters)).primitives & Primitive::Number;
}

TypeID findCarriedNumberType(Walker& walker, BlockID inputHead, const LoopPreview& preview, Reference enters, uint32_t position)
{
    TypeArena& types = walker.types();
    if (!enters || !enters.isValue() || !types.isUninformative(walker.typeOf(enters)))
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    ValueID parameter = index.block(inputHead).parameter(position);
    bool carriesNumber = false;
    bool carriesIntegers = true;
    std::optional<unsigned> widthOfDigit;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID carried = position < arguments.size() ? index.stripCasts(arguments[position]) : ValueID();
        if (!carried)
            return { };
        if (carried == parameter)
            continue;
        widthOfDigit = widthOfCarriedDigit(index, carried, parameter);
        size_t place = carried.index() - preview.firstValue;
        TypeID found = carried.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
        if (!found || isNeverType(found) || !(types.at(found).primitives & Primitive::Number) || !types.at(found).hasOnlyPrimitives(Primitive::Number | Primitive::BigInt))
            return { };
        carriesNumber = true;
        carriesIntegers &= types.at(found).isInteger();
    }
    if (!carriesNumber)
        return { };
    std::shared_ptr<const UsersOfValuesInCountersOfLoops> users = usersOfValuesOfLoopInCountersOfLoops(walker, index, inputHead);
    UsersOfNamesInCountersOfLoops usersLeft(*users, { parameter });
    bool isComputedWith = false;
    while (ValueID user = usersLeft.next()) {
        if (index.isParameter(user))
            continue;
        BodyValue operation = index.value(user);
        bool isKey = (operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet) && index.stripCasts(operation.propertyOperand()) == parameter;
        if (!carriesIntegers && (isKey || allBitwiseOperators.contains(operation.opcode())))
            return { };
        isComputedWith |= allNumericOperators.contains(operation.opcode());
    }
    if (!isComputedWith)
        return { };
    if (carriesIntegers && widthOfDigit)
        return types.integer(Range { 0, (int64_t { 1 } << *widthOfDigit) - 1 });
    return carriesIntegers ? TypeTable::int32 : TypeTable::number;
}

static bool isBoxOrDoubleThatMayBeInt32InCountersOfLoops(const Walker& walker, Reference value)
{
    if (!mayBeInt32InCountersOfLoops(walker, value))
        return false;
    Representation held = walker.representationOf(value);
    return held == Representation::JSValue || held == Representation::OptionalDouble || ((held == Representation::Double || held == Representation::Int64) && walker.types().at(walker.typeOf(value)).isNumber());
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

static VariableID findViewInUnwrittenVariable(Walker& walker, const BodyIndex& index, const Effects& ofTurns, ValueID written)
{
    ValueID read = index.stripCasts(written);
    if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = index.stripCasts(index.value(read).operand(0));
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
        return { };
    BodyValue definition = index.value(read);
    VariableID variable = definition.variable();
    if (walker.program().at(variable).module != walker.program().at(walker.currentFunction()).module || !leavesDeadZoneForGood(walker.program(), variable) || declaredEffectsOfOperation(walker.program(), Opcode::GetModuleVariable, definition).reads.overlaps(ofTurns.writes))
        return { };
    TypeID widest = widestTypeOfOperation(walker, definition);
    TypeID behindDeadZone = widest ? walker.types().withoutPrimitives(widest, Primitive::Absent) : TypeID();
    return behindDeadZone && !isNeverType(behindDeadZone) && isViewOfNumbersReadInLine(walker, behindDeadZone) ? variable : VariableID();
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

static bool mayBeIntegerBeforeInnerLoop(Walker& walker, const BodyIndex& index, const LoopPreview& preview, ValueID value)
{
    TypeID type = typeByLookInCountersOfLoops(walker, index, preview, value);
    if (size_t place = value.index() - preview.firstValue; !type && value.index() >= preview.firstValue && place < preview.typesOfValues.size())
        type = preview.typesOfValues[place];
    return type && !isNeverType(type) && walker.types().at(type).primitives & Primitive::Integer;
}

static bool hasNoFractionByLookInCountersOfLoops(Walker& walker, const BodyIndex& index, const LoopPreview& preview, ValueID value)
{
    TypeID type = typeByLookInCountersOfLoops(walker, index, preview, value);
    return type && !isNeverType(type) && walker.types().at(type).primitives & Primitive::Integer && walker.types().at(type).hasOnlyPrimitives(Primitive::NumberWithoutFraction);
}

static bool isNumberByLookInCountersOfLoops(Walker& walker, const BodyIndex& index, const LoopPreview& preview, ValueID value)
{
    TypeID type = typeByLookInCountersOfLoops(walker, index, preview, value);
    return type && !isNeverType(type) && walker.types().at(type).isNumber();
}

static bool isIntegerInFrontOfLoopInCountersOfLoops(Walker& walker, const BodyIndex& index, uint32_t firstValueOfLoop, ValueID step)
{
    ValueID written = index.stripCasts(step);
    if (written && !walker.referenceOf(written))
        written = step;
    Reference held = written && written.index() < firstValueOfLoop ? walker.referenceOf(written) : Reference();
    TypeID type = held ? walker.typeOf(held) : TypeID();
    return type && walker.types().at(type).isInteger();
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

static bool isQuotient(const BodyIndex& index, ValueID value)
{
    return !index.isParameter(value) && index.opcodeOf(value) == Opcode::Div;
}

static bool isSteppedAllRoundInCountersOfLoops(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, BlockID inputHead, uint32_t position, NamesOfCounterOfLoop& names, const LoopPreview* typesOfSteps = nullptr, Vector<ValueID, 2>* stepsAskedInFront = nullptr, bool admitsWhatCallsGive = false, bool trustsHeadsOfLoopsInside = true, bool admitsStepsOfAnyNumber = false, bool admitsFractions = false)
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
                if (admitsStepsOfAnyNumber && other.index() < firstValueOfLoop && mayBeIntegerBeforeInnerLoop(walker, index, *typesOfSteps, other))
                    return true;
                if (admitsFractions && isNumberByLookInCountersOfLoops(walker, index, *typesOfSteps, other))
                    return true;
                ValueID written = stepsAskedInFront ? index.stripCasts(other) : ValueID();
                if (written && !walker.referenceOf(written))
                    written = other;
                if (!written || written.index() >= firstValueOfLoop || isQuotient(index, written) || !isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, walker.referenceOf(written)))
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
        return isSteppedAllRoundInCountersOfLoops(walker, index, users, inputHead, position, names, typesOfSteps, stepsAskedInFront, admitsWhatCallsGive, false, admitsStepsOfAnyNumber, admitsFractions);
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

static bool storesCounter(const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, const NamesOfCounterOfLoop& names)
{
    static constexpr OpcodeSet stores { Opcode::PropSet, Opcode::PrivateSet, Opcode::ElemSet, Opcode::PutModuleVariable, Opcode::PutEnvironmentField };
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (!index.isParameter(user) && stores.contains(index.opcodeOf(user)) && names.contains(index.value(user).valueOperand()))
            return true;
    }
    return false;
}

static bool isOnlyUsedInArithmetic(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, BlockID inputHead, const LoopPreview& preview, const NamesOfCounterOfLoop& names)
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

static bool isRotatedWord(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t position, std::span<const Reference> entering)
{
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstParameter = index.block(inputHead).firstParameter().index();
    Vector<uint32_t, 8> rotated { position };
    for (size_t place = 0; place < rotated.size(); ++place) {
        if (rotated[place] >= entering.size() || !entering[rotated[place]] || !(walker.types().at(walker.typeOf(entering[rotated[place]])).primitives & Primitive::Integer))
            return false;
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
            if (!index.isInLoop(edge.from, loop))
                continue;
            std::span<const ValueID> arguments = edge.target.arguments();
            ValueID brought = rotated[place] < arguments.size() ? index.stripCasts(arguments[rotated[place]]) : ValueID();
            if (!brought || !index.isParameter(brought) || index.blockOf(brought) != inputHead)
                return false;
            uint32_t from = brought.index() - firstParameter;
            if (from == rotated[place])
                return false;
            if (!rotated.contains(from))
                rotated.append(from);
        }
    }
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue user) {
            bool takesBits = false;
            if (allBitwiseOperators.contains(user.opcode())) {
                user.forEachOperand([&](ValueID operand, uint32_t) {
                    ValueID word = operand ? index.stripCasts(operand) : ValueID();
                    takesBits |= word && index.isParameter(word) && index.blockOf(word) == inputHead && rotated.contains(word.index() - firstParameter);
                });
            }
            return takesBits;
        });
    });
}

static bool isEqualityLoopTest(const BodyIndex& index, BlockID inputHead, ValueID test)
{
    static constexpr OpcodeSet equalities { Opcode::StrictEq, Opcode::Eq };
    if (!equalities.contains(index.opcodeOf(test)))
        return false;
    BodyTerminator end = index.terminator(index.blockOf(test));
    LoopID loop = index.loopOf(inputHead);
    return end.kind() == TerminatorKind::Branch && end.as<TerminatorKind::Branch>().conditionOperand() == test && !index.isInLoop(end.targets()[0].block(), loop) && index.isInLoop(end.targets()[1].block(), loop);
}

static void guessBoundsInCountersOfLoops(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, BlockID inputHead, const NamesOfCounterOfLoop& names, NamesBeforeLoop& namesBeforeLoop, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses)
{
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (index.isParameter(user) || (!allRelations.contains(index.opcodeOf(user)) && !isEqualityLoopTest(index, inputHead, user)))
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

static bool truncatesForNumber(const BodyIndex& index, const BodyValue& operation)
{
    if (operation.opcode() != Opcode::BitOr)
        return false;
    ValueID zero = operation.operand(1);
    return zero && !index.isParameter(zero) && index.opcodeOf(zero) == Opcode::Integer && !index.value(zero).as<Opcode::Integer>().integer();
}

static Vector<ValueID, 4> findSumsOfKey(const BodyIndex& index, uint32_t firstValueOfLoop, ValueID sum)
{
    Vector<ValueID, 4> sums { sum };
    while (!isKeyOfElementInCountersOfLoops(index, sums.last())) {
        ValueID user = index.onlyUsingValue(sums.last());
        if (!user || index.isParameter(user) || !stepsByNumberOfCountersOfLoops.contains(index.opcodeOf(user)))
            return { };
        ValueID beside = index.value(user).operand(index.value(user).operand(0) == sums.last() ? 1 : 0);
        beside = beside ? index.stripCasts(beside) : ValueID();
        if (!beside || (beside.index() >= firstValueOfLoop && (index.isParameter(beside) || index.opcodeOf(beside) != Opcode::Integer)))
            return { };
        sums.append(user);
    }
    return sums;
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
        if (index.isParameter(user) || !stepsByNumberOfCountersOfLoops.contains(index.opcodeOf(user)))
            continue;
        Vector<ValueID, 4> sums = findSumsOfKey(index, firstValueOfLoop, user);
        for (size_t sum = 0; sum < sums.size(); ++sum) {
            BodyValue operation = index.value(sums[sum]);
            for (uint32_t place : { 0u, 1u }) {
                if (sum ? operation.operand(place) != sums[sum - 1] : !names.contains(operation.operand(place)))
                    continue;
                ValueID addend = index.stripCasts(operation.operand(1 - place));
                if (!addend)
                    continue;
                Reference value = addend.index() < firstValueOfLoop ? walker.referenceOf(addend) : makeAddendThatNoTurnChangesInCountersOfLoops(walker, index, firstValueOfLoop, addend);
                if (!value)
                    continue;
                if (!addendMayBeGuessedInt32(walker, value) || std::ranges::find(entering, value) != entering.end())
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

static bool isComparedWithIntegerInHeader(const BodyIndex& index, BlockID inputHead, uint32_t position)
{
    BodyTerminator end = index.block(inputHead).terminator();
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!condition || index.isParameter(condition) || !allRelations.contains(index.opcodeOf(condition)))
        return false;
    ValueID bound = index.stripCasts(index.value(condition).operand(1));
    return parameterOfHeadNamedInCountersOfLoops(index, index.value(condition).operand(0), inputHead) == index.block(inputHead).parameter(position) && bound && !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::Integer;
}

static bool isTruthTestedInHeader(const BodyIndex& index, BlockID inputHead, uint32_t position)
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

static bool isNonNegativeByIterationTest(const BodyIndex& index, BlockID inputHead, BlockID block, ValueID number)
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

ValueID findUpdatedKey(Walker& walker, const BodyIndex& index, ValueID key)
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

AccessesNearCounter findAccessesNearCounter(Walker& walker, BlockID inputHead, NOESCAPE const ScopedLambda<TypeID(uint32_t position)>& typeThatEnters, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeInFront)
{
    static constexpr OpcodeSet conversions { Opcode::ToNumeric, Opcode::ToNumber, Opcode::ToPropertyKey };
    static constexpr int64_t mostAdded = 1 << 20;
    AccessesNearCounter found;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    if (end.kind() != TerminatorKind::Branch || head.numberOfParameters() < 2 || !index.holdsAnyOf(accessesUnderKeysInCountersOfLoops))
        return found;
    uint32_t firstValueOfLoop = head.firstParameter().index();
    ValueID asked = end.as<TerminatorKind::Branch>().conditionOperand();
    if (!asked || index.isParameter(asked) || !head.holds(asked) || !allRelations.contains(index.opcodeOf(asked)))
        return found;
    BodyTargets::Iterator target = end.targets().begin();
    if (!index.isInLoop((*target).block(), loop) || index.isInLoop((*++target).block(), loop))
        return found;
    auto isInteger = [&](TypeID type) {
        return type && walker.types().at(type).isInteger();
    };
    auto writtenInteger = [&](ValueID written) -> std::optional<int64_t> {
        ValueID value = index.stripCasts(written);
        if (!value || index.isParameter(value) || index.opcodeOf(value) != Opcode::Integer)
            return std::nullopt;
        int64_t integer = index.value(value).as<Opcode::Integer>().integer();
        return std::abs(integer) <= mostAdded ? std::optional { integer } : std::nullopt;
    };
    BodyValue relation = index.value(asked);
    bool isLess = relation.opcode() == Opcode::Less || relation.opcode() == Opcode::LessEq;
    for (uint32_t side : { 0u, 1u }) {
        ValueID counter = parameterOfHeadNamedInCountersOfLoops(index, relation.operand(side), inputHead);
        ValueID bound = relation.operand(1 - side);
        ValueID boundBehindCasts = index.stripCasts(bound);
        if (!counter || (boundBehindCasts.index() >= firstValueOfLoop ? !writtenInteger(bound) : !isInteger(typeInFront(bound.index() < firstValueOfLoop ? bound : boundBehindCasts))))
            continue;
        found.counter = counter.index() - firstValueOfLoop;
        found.bound = bound;
        found.counterRises = isLess == !side;
        found.admitsEqual = relation.opcode() == Opcode::LessEq || relation.opcode() == Opcode::GreaterEq;
        std::optional<Range> counted = findIterationSteps(walker, inputHead, found.counter);
        if (!counted || (found.counterRises ? counted->minimum <= 0 : counted->maximum >= 0) || !isInteger(typeThatEnters(found.counter)))
            continue;
        BackEdgeDominators backEdgeDominators(index, loop);
        UncheckedKeyHashMap<uint64_t, size_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> placesOfAccesses;
        UncheckedKeyHashMap<uint32_t, std::optional<Range>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> stepsOfPositions;
        for (BlockID member : index.membersOf(loop)) {
            if (!backEdgeDominators.contains(member))
                continue;
            for (BodyValue access : index.block(member).values()) {
                if (!accessesUnderKeysInCountersOfLoops.contains(access.opcode()))
                    continue;
                ValueID view = access.objectOperand().index() < firstValueOfLoop ? access.objectOperand() : index.stripCasts(access.objectOperand());
                if (view.index() >= firstValueOfLoop || !isViewOfNumbersReadInLine(walker, typeInFront(view)))
                    continue;
                int64_t addend = 0;
                ValueID key = index.stripCasts(findUpdatedKey(walker, index, access.propertyOperand()));
                while (key && !index.isParameter(key)) {
                    BodyValue made = index.value(key);
                    std::optional<int64_t> written = stepsByNumberOfCountersOfLoops.contains(made.opcode()) ? writtenInteger(made.operand(1)) : std::nullopt;
                    if (written)
                        addend += made.opcode() == Opcode::Add ? *written : -*written;
                    else if (made.opcode() == Opcode::Increment || made.opcode() == Opcode::Decrement)
                        addend += made.opcode() == Opcode::Increment ? 1 : -1;
                    else if (!conversions.contains(made.opcode()))
                        break;
                    key = index.stripCasts(made.operand(0));
                }
                ValueID stepped = key ? parameterOfHeadNamedInCountersOfLoops(index, key, inputHead) : ValueID();
                if (!stepped || stepped == counter || std::abs(addend) > mostAdded)
                    continue;
                uint32_t position = stepped.index() - firstValueOfLoop;
                std::optional<Range> steps = stepsOfPositions.ensure(position, [&] {
                    return isInteger(typeThatEnters(position)) ? findIterationSteps(walker, inputHead, position) : std::nullopt;
                }).iterator->value;
                std::optional<InvariantStep> invariant = !steps && isInteger(typeThatEnters(position)) ? findInvariantStep(walker, inputHead, position) : std::nullopt;
                if (!steps && !invariant)
                    continue;
                auto known = placesOfAccesses.add(static_cast<uint64_t>(view.index()) << 32 | position, found.accesses.size());
                size_t place = known.iterator->value;
                if (known.isNewEntry)
                    found.accesses.append({ view, position, Range::one(addend), steps ? *steps : invariant->steps, invariant ? invariant->step : ValueID(), invariant && invariant->isSubtracted });
                else
                    found.accesses[place].addends = found.accesses[place].addends.unionWith(Range::one(addend));
            }
        }
        return found;
    }
    return found;
}

CounterHeldBelowRoomOfViews findCounterHeldBelowRoomOfViews(Walker& walker, BlockID inputHead, NOESCAPE const ScopedLambda<TypeID(uint32_t position)>& typeThatEnters, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeInFront, NOESCAPE const ScopedLambda<TypeID(Reference)>& typeOfNameInFront, NOESCAPE const ScopedLambda<bool(ValueID)>& isListOnlyReadInLoop, NOESCAPE const ScopedLambda<std::optional<int64_t>(ValueID)>& integerInFront, const Effects& ofTurns)
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
            Reference field = fieldValueInPreheader(walker, index, value, firstValueOfLoop);
            TypeID typeOfField = field ? typeOfNameInFront(field) : TypeID();
            if (typeOfField && walker.types().at(typeOfField).isInteger())
                return walker.types().at(typeOfField).integers;
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
    if (!counter || (!found.viewWhoseLengthIsBound && !rangeOfIntegerFromInFront(relation.operand(1)) && !integerInFront(relation.operand(1))))
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
    std::optional<BackEdgeDominators> backEdgeDominators;
    for (BlockID member : index.membersOf(loop)) {
        if (member == inputHead)
            continue;
        for (BodyValue access : index.block(member).values()) {
            if (access.opcode() == Opcode::Call && std::ranges::any_of(access.as<Opcode::Call>().argumentsOperands(), [&](ValueID argument) { return argument && namesCounter(argument); })) {
                forEachElementAccessInInlinedCallee(walker, access, scopedLambda<void(ValueID, ValueID)>([&](ValueID argument, ValueID key) {
                    ValueID view = index.stripCasts(argument);
                    if (key && namesCounter(key) && view.index() < firstValueOfLoop && isViewOfNumbersReadInLine(walker, typeInFront(view)))
                        found.accesses.append({ access.id(), view, { }, { }, { }, false, false });
                }));
            }
            if (!accessesUnderKeysInCountersOfLoops.contains(access.opcode()))
                continue;
            Reference fieldView;
            VariableID variable;
            if (index.stripCasts(access.objectOperand()).index() >= firstValueOfLoop) {
                fieldView = index.loopOf(member) == loop ? nameOfListThatEntersLoopAround(walker, index, access) : Reference();
                if (!fieldView && index.loopOf(member) == loop)
                    variable = findViewInUnwrittenVariable(walker, index, ofTurns, access.objectOperand());
                if (!variable && (!fieldView || !fieldView.isValue() || !isViewOfNumbersReadInLine(walker, typeOfNameInFront(fieldView))))
                    continue;
            }
            Vector<AddendOfKey, 2> addends;
            ValueID key = index.stripCasts(findUpdatedKey(walker, index, access.propertyOperand()));
            while (key && !namesCounter(key) && !index.isParameter(key) && stepsByNumberOfCountersOfLoops.contains(index.opcodeOf(key))) {
                BodyValue sum = index.value(key);
                bool isDifference = sum.opcode() == Opcode::Sub;
                bool counterIsLeft = isDifference || !rangeOfIntegerFromInFront(sum.operand(0));
                if (!rangeOfIntegerFromInFront(sum.operand(counterIsLeft)))
                    break;
                addends.append({ sum.operand(counterIsLeft), isDifference });
                key = index.stripCasts(sum.operand(!counterIsLeft));
            }
            if (!key || !namesCounter(key))
                continue;
            bool mayPrecede = addends.size() > 1 || (!addends.isEmpty() && addends[0].isSubtracted);
            if (mayPrecede && !backEdgeDominators)
                backEdgeDominators.emplace(index, loop);
            if (mayPrecede && !backEdgeDominators->contains(member))
                continue;
            ValueID view = access.objectOperand().index() < firstValueOfLoop ? access.objectOperand() : index.stripCasts(access.objectOperand());
            bool isOfList = !fieldView && !variable && access.opcode() == Opcode::ElemGet && isListOnlyReadInLoop(view);
            if ((!isOfList && !fieldView && !variable && !isViewOfNumbersReadInLine(walker, typeInFront(view))) || testOfBodyHoldsPositionBelowLengthOfText(walker, index, member, access.objectOperand(), access.propertyOperand()))
                continue;
            bool iterationChecksAddendSign = addends.size() == 1 && !addends[0].isSubtracted && !isOfList && isNonNegativeByIterationTest(index, inputHead, member, addends[0].value);
            found.accesses.append({ access.id(), view, fieldView, variable, WTF::move(addends), isOfList, iterationChecksAddendSign });
        }
    }
    return found;
}

Vector<SteppedKey, 2> findSteppedKeys(Walker& walker, BlockID inputHead, NOESCAPE const ScopedLambda<std::optional<Range>(uint32_t)>& findSteps, NOESCAPE const ScopedLambda<Reference(ValueID)>& findTestedArray)
{
    Vector<SteppedKey, 2> found;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    if (end.kind() != TerminatorKind::Branch || head.numberOfParameters() < 2 || !index.holdsAnyOf(OpcodeSet { Opcode::ElemGet }) || index.loop(loop).numberOfLoopsWithin != 1)
        return found;
    ValueID asked = end.as<TerminatorKind::Branch>().conditionOperand();
    if (!asked || index.isParameter(asked) || !head.holds(asked) || !allRelations.contains(index.opcodeOf(asked)))
        return found;
    BodyTargets::Iterator target = end.targets().begin();
    if (!index.isInLoop((*target).block(), loop) || index.isInLoop((*++target).block(), loop))
        return found;
    for (BlockID member : index.membersOf(loop)) {
        if (member != inputHead && std::ranges::any_of(index.terminator(member).targets(), [&](const BodyTarget& target) { return !index.isInLoop(target.block(), loop); }))
            return found;
    }
    uint32_t firstValueOfLoop = head.firstParameter().index();
    BodyValue relation = index.value(asked);
    SteppedKey key;
    ValueID counter;
    for (uint32_t side = 0; side < 2 && !counter; ++side) {
        counter = parameterOfHeadNamedInCountersOfLoops(index, relation.operand(side), inputHead);
        bool isLess = relation.opcode() == Opcode::Less || relation.opcode() == Opcode::LessEq;
        key.counterRises = isLess == !side;
        key.bound = relation.operand(1 - side);
    }
    key.admitsBound = relation.opcode() == Opcode::LessEq || relation.opcode() == Opcode::GreaterEq;
    if (!counter)
        return found;
    key.counterPosition = counter.index() - firstValueOfLoop;
    ValueID bound = index.stripCasts(key.bound);
    bool isZero = bound && !index.isParameter(bound) && index.opcodeOf(bound) == Opcode::Integer && !index.value(bound).as<Opcode::Integer>().integer();
    bool isInvariant = bound && (bound.index() < firstValueOfLoop || (!index.isParameter(bound) && index.opcodeOf(bound) == Opcode::Integer));
    std::optional<Range> counted = findSteps(key.counterPosition);
    if (!counted || (key.counterRises ? !isInvariant || counted->minimum <= 0 : !isZero || counted->maximum >= 0))
        return found;
    std::optional<BackEdgeDominators> dominators;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue access : index.block(member).values()) {
            if (access.opcode() != Opcode::ElemGet || index.loopOf(member) != loop)
                continue;
            ValueID position = index.stripCasts(access.propertyOperand());
            while (position && !index.isParameter(position) && index.opcodeOf(position) == Opcode::ToNumeric)
                position = index.stripCasts(index.value(position).operand(0));
            ValueID stepped = position ? parameterOfHeadNamedInCountersOfLoops(index, position, inputHead) : ValueID();
            ValueID object = index.stripCasts(access.objectOperand());
            if (!stepped || stepped == counter || !object || object.index() >= firstValueOfLoop)
                continue;
            key.steppedPosition = stepped.index() - firstValueOfLoop;
            std::optional<Range> steps = findSteps(key.steppedPosition);
            if (!steps || steps->minimum < 0 || steps->maximum > 1)
                continue;
            key.list = findTestedArray(access.objectOperand());
            if (!key.list || found.containsIf([&](const SteppedKey& other) { return other.list == key.list && other.steppedPosition == key.steppedPosition; }))
                continue;
            if (!dominators)
                dominators.emplace(index, loop);
            if (member == inputHead || dominators->contains(member))
                found.append(key);
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

std::optional<InvariantStep> findInvariantStep(Walker& walker, BlockID inputHead, uint32_t position)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    ValueID from = index.block(inputHead).parameter(position);
    if (index.isEnteredByHandlerEdge(inputHead))
        return std::nullopt;
    InvariantStep found;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID next = position < arguments.size() && arguments[position] ? index.stripCasts(arguments[position]) : ValueID();
        if (!next || index.isParameter(next) || !stepsByNumberOfCountersOfLoops.contains(index.opcodeOf(next)))
            return std::nullopt;
        BodyValue sum = index.value(next);
        bool isSubtracted = sum.opcode() == Opcode::Sub;
        ValueID step = index.stripCasts(sum.operand(1));
        if (parameterOfHeadNamedInCountersOfLoops(index, sum.operand(0), inputHead) != from || step.index() >= firstValueOfLoop || (found.step && (index.stripCasts(found.step) != step || found.isSubtracted != isSubtracted)))
            return std::nullopt;
        found.step = sum.operand(1).index() < firstValueOfLoop ? sum.operand(1) : step;
        found.isSubtracted = isSubtracted;
    }
    Reference built = found.step ? walker.referenceOf(found.step) : Reference();
    TypeID type = built && built.isRecord() ? walker.typeOf(built) : TypeID();
    if (!type || !walker.types().at(type).isInteger() || !walker.types().at(type).integers.isSubsetOf(Range { 0, std::numeric_limits<int32_t>::max() }))
        return std::nullopt;
    Range range = walker.types().at(type).integers;
    found.steps = found.isSubtracted ? Range { -range.maximum, -range.minimum } : range;
    return found.steps.maximum <= std::numeric_limits<int32_t>::max() >> 2 ? std::optional { found } : std::nullopt;
}

std::optional<Range> findIterationSteps(Walker& walker, BlockID inputHead, uint32_t position)
{
    static constexpr size_t mostNames = 48;
    static constexpr int64_t mostAdded = 1 << 20;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    ValueID from = index.block(inputHead).parameter(position);
    if (index.isEnteredByHandlerEdge(inputHead))
        return std::nullopt;
    Range found = Range::empty();
    bool isBounded = true;
    Vector<std::pair<ValueID, int64_t>, 8> reached;
    auto follow = [&](ValueID next, int64_t added) {
        next = next ? index.stripCasts(next) : ValueID();
        isBounded &= next && reached.size() < mostNames && std::abs(added) <= mostAdded;
        if (isBounded && !reached.contains(std::pair(next, added)))
            reached.append({ next, added });
    };
    auto followEdges = [&](BlockID block, uint32_t place, int64_t added, bool comesRound) {
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
            if (comesRound && !index.isInLoop(edge.from, loop))
                continue;
            std::span<const ValueID> arguments = edge.target.arguments();
            follow(place < arguments.size() ? arguments[place] : ValueID(), added);
        }
    };
    followEdges(inputHead, position, 0, true);
    for (size_t next = 0; next < reached.size() && isBounded; ++next) {
        auto [value, added] = reached[next];
        if (value == from) {
            found = found.unionWith(Range::one(added));
            continue;
        }
        BlockID block = index.blockOf(value);
        if (!index.isInLoop(block, loop))
            return std::nullopt;
        if (index.isParameter(value)) {
            if (ValueID narrowed = index.valueNarrowedInto(value)) {
                follow(narrowed, added);
                continue;
            }
            if (index.isLoopHead(block) || index.isEnteredByHandlerEdge(block))
                return std::nullopt;
            followEdges(block, value.index() - index.block(block).firstParameter().index(), added, false);
            continue;
        }
        BodyValue made = index.value(value);
        switch (made.opcode()) {
        case Opcode::ToNumeric:
        case Opcode::ToNumber:
            follow(made.operand(0), added);
            break;
        case Opcode::Increment:
        case Opcode::Decrement:
            follow(made.operand(0), made.opcode() == Opcode::Increment ? added + 1 : added - 1);
            break;
        case Opcode::Add:
        case Opcode::Sub: {
            ValueID right = index.stripCasts(made.operand(1));
            if (!right || index.isParameter(right) || index.opcodeOf(right) != Opcode::Integer)
                return std::nullopt;
            int64_t by = index.value(right).as<Opcode::Integer>().integer();
            if (std::abs(by) > mostAdded)
                return std::nullopt;
            follow(made.operand(0), made.opcode() == Opcode::Add ? added + by : added - by);
            break;
        }
        default:
            return std::nullopt;
        }
    }
    if (!isBounded || found.isEmpty() || (!found.minimum && !found.maximum))
        return std::nullopt;
    auto isStepped = [&](ValueID key) {
        for (unsigned names = 0; names < 4 && key; ++names) {
            key = index.stripCasts(key);
            if (key == from || std::ranges::any_of(reached, [&](const auto& name) { return name.first == key; }))
                return true;
            if (index.isParameter(key) || (index.opcodeOf(key) != Opcode::ToNumeric && index.opcodeOf(key) != Opcode::ToPropertyKey && index.opcodeOf(key) != Opcode::Increment && index.opcodeOf(key) != Opcode::Decrement))
                return false;
            key = index.value(key).operand(0);
        }
        return false;
    };
    bool isCounter = false;
    BodyTerminator end = index.terminator(inputHead);
    if (ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID(); condition && !index.isParameter(condition) && allRelations.contains(index.opcodeOf(condition)))
        isCounter = index.stripCasts(index.value(condition).operand(0)) == from || index.stripCasts(index.value(condition).operand(1)) == from;
    for (BlockID member : index.membersOf(loop)) {
        if (isCounter)
            break;
        for (BodyValue operation : index.block(member).values()) {
            if ((operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet || operation.opcode() == Opcode::ElemDefine) && isStepped(operation.propertyOperand()))
                return found;
        }
    }
    return isCounter ? std::optional { found } : std::nullopt;
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

static Opcode mirroredRelation(Opcode relation)
{
    switch (relation) {
    case Opcode::Less:
        return Opcode::Greater;
    case Opcode::LessEq:
        return Opcode::GreaterEq;
    case Opcode::Greater:
        return Opcode::Less;
    default:
        return Opcode::LessEq;
    }
}

static std::optional<int64_t> findCounterStep(const BodyIndex& index, ValueID written, ValueID counter)
{
    ValueID value = index.stripCasts(written);
    if (index.isParameter(value))
        return value == counter ? std::optional<int64_t> { 0 } : std::nullopt;
    BodyValue step = index.value(value);
    if (step.opcode() == Opcode::Decrement)
        return index.stripCasts(step.operand(0)) == counter ? std::optional<int64_t> { -1 } : std::nullopt;
    if (step.opcode() != Opcode::Sub)
        return findConstantAddedToCounter(index, written, counter);
    ValueID subtrahend = index.stripCasts(step.operand(1));
    if (index.stripCasts(step.operand(0)) != counter || index.isParameter(subtrahend) || index.opcodeOf(subtrahend) != Opcode::Integer)
        return std::nullopt;
    int64_t constant = index.value(subtrahend).as<Opcode::Integer>().integer();
    return Range::one(constant).isSubsetOf(Range::int32()) ? std::optional { -constant } : std::nullopt;
}

bool counterOnlyFalls(Walker& walker, BlockID inputHead, uint32_t position)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isEnteredByHandlerEdge(inputHead))
        return false;
    ValueID counter = index.block(inputHead).parameter(position);
    LoopID loop = index.loopOf(inputHead);
    bool falls = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        std::optional<int64_t> step = position < arguments.size() && arguments[position] ? findCounterStep(index, arguments[position], counter) : std::nullopt;
        if (!step || *step > 0)
            return false;
        falls |= *step < 0;
    }
    return falls;
}

std::optional<EqualityLoopTest> findEqualityLoopTest(Walker& walker, BlockID inputHead)
{
    static constexpr OpcodeSet tests { Opcode::StrictEq, Opcode::Eq, Opcode::ToBoolean };
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!condition || index.isParameter(condition) || !head.holds(condition) || !tests.contains(index.opcodeOf(condition)) || !index.isLoopHead(inputHead) || index.isEnteredByHandlerEdge(inputHead))
        return std::nullopt;
    BodyValue test = index.value(condition);
    bool testsTruth = test.opcode() == Opcode::ToBoolean;
    LoopID loop = index.loopOf(inputHead);
    uint32_t exitTarget = testsTruth ? 1 : 0;
    if (index.isInLoop(end.targets()[exitTarget].block(), loop) || !index.isInLoop(end.targets()[1 - exitTarget].block(), loop))
        return std::nullopt;

    for (uint32_t place = 0; place < (testsTruth ? 1u : 2u); ++place) {
        ValueID counter = index.stripCasts(test.operand(place));
        if (!counter || !index.isParameter(counter) || index.blockOf(counter) != inputHead)
            continue;
        EqualityLoopTest found;
        found.position = counter.index() - head.firstParameter().index();
        found.bound = testsTruth ? ValueID() : test.operand(1 - place);
        found.exitTarget = exitTarget;
        int64_t least = 0;
        int64_t most = 0;
        bool isStepped = true;
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
            if (!index.isInLoop(edge.from, loop))
                continue;
            std::span<const ValueID> arguments = edge.target.arguments();
            std::optional<int64_t> step = found.position < arguments.size() && arguments[found.position] ? findCounterStep(index, arguments[found.position], counter) : std::nullopt;
            isStepped &= step.has_value();
            least = std::min(least, step.value_or(0));
            most = std::max(most, step.value_or(0));
        }
        if (!isStepped || !least == !most)
            continue;
        found.countsUp = !least;
        found.largestStep = found.countsUp ? most : -least;
        return found;
    }
    return std::nullopt;
}

static bool headerTestsParameter(const BodyIndex& index, BlockID inputHead)
{
    BodyTerminator end = index.terminator(inputHead);
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!condition || index.isParameter(condition) || !allRelations.contains(index.opcodeOf(condition)))
        return false;
    ValueID counter = index.stripCasts(index.value(condition).operand(0));
    LoopID loop = index.loopOf(inputHead);
    return counter && index.isParameter(counter) && index.blockOf(counter) == inputHead && end.targets()[0].block() != inputHead && index.isInLoop(end.targets()[0].block(), loop) && !index.isInLoop(end.targets()[1].block(), loop);
}

std::optional<EndLoopTest> findEndLoopTest(Walker& walker, BlockID inputHead)
{
    static constexpr OpcodeSet equalities { Opcode::StrictEq, Opcode::Eq, Opcode::ToBoolean };
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.isLoopHead(inputHead))
        return std::nullopt;
    LoopID loop = index.loopOf(inputHead);
    if (index.loop(loop).numberOfLoopsWithin != 1 || headerTestsParameter(index, inputHead) || index.isEnteredByHandlerEdge(inputHead))
        return std::nullopt;
    EndLoopTest found;
    std::span<const ValueID> arguments;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        if (found.block)
            return std::nullopt;
        found.block = edge.from;
        arguments = edge.target.arguments();
    }
    BlockID next = inputHead;
    if (std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(found.block); edge && found.block != inputHead && index.terminator(found.block).kind() == TerminatorKind::Jump && !index.block(found.block).numberOfValues())
        next = std::exchange(found.block, edge->from);
    BodyTerminator end = index.terminator(found.block);
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!condition || index.isParameter(condition) || !index.block(found.block).holds(condition))
        return std::nullopt;
    BodyValue test = index.value(condition);
    found.isRelation = allRelations.contains(test.opcode());
    bool testsTruth = test.opcode() == Opcode::ToBoolean;
    if (!found.isRelation && !equalities.contains(test.opcode()))
        return std::nullopt;
    found.exitTarget = found.isRelation || testsTruth ? 1 : 0;
    if (index.isInLoop(end.targets()[found.exitTarget].block(), loop) || end.targets()[1 - found.exitTarget].block() != next)
        return std::nullopt;

    BodyBlock head = index.block(inputHead);
    for (uint32_t place = 0; place < (testsTruth ? 1u : 2u); ++place) {
        ValueID tested = index.stripCasts(test.operand(place));
        found.bound = testsTruth ? ValueID() : test.operand(1 - place);
        for (found.position = 0; found.position < arguments.size(); ++found.position) {
            ValueID stepped = arguments[found.position];
            if (ValueID narrowed = stepped && index.isParameter(stepped) ? index.valueNarrowedInto(stepped) : ValueID())
                stepped = narrowed;
            if (!stepped || index.stripCasts(stepped) != tested)
                continue;
            found.stepped = stepped;
            std::optional<int64_t> step = findCounterStep(index, stepped, head.parameter(found.position));
            if (!step || !*step || (!found.isRelation && std::abs(*step) != 1) || (found.bound && !findLoopBoundRange(walker, index, head.firstParameter(), found.bound)))
                continue;
            found.relation = !found.isRelation ? (*step > 0 ? Opcode::Less : Opcode::Greater) : place ? mirroredRelation(test.opcode()) : test.opcode();
            if ((*step > 0) == (found.relation == Opcode::Less || found.relation == Opcode::LessEq))
                return found;
        }
    }
    return std::nullopt;
}

static bool isElementKey(Walker& walker, const BodyIndex& index, const UsersOfValuesInCountersOfLoops& users, const NamesOfCounterOfLoop& names)
{
    const Program& program = walker.program();
    auto isName = [&](ValueID operand) {
        return names.contains(operand);
    };
    UsersOfNamesInCountersOfLoops usersLeft(users, names);
    while (ValueID user = usersLeft.next()) {
        if (index.isParameter(user))
            continue;
        BodyValue operation = index.value(user);
        if (accessesUnderKeysInCountersOfLoops.contains(operation.opcode()) && isName(operation.propertyOperand()))
            return true;
        if (operation.opcode() != Opcode::Call || std::ranges::none_of(operation.argumentsOperands(), isName))
            continue;
        ValueID callee = index.stripCasts(operation.calleeOperand());
        if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::PropGet || index.value(callee).isFreeName())
            continue;
        PropertyKeyID key = index.value(callee).key();
        if (key == program.keyOf(BuiltinNames::charCodeAt) || key == program.keyOf(BuiltinNames::charAt) || key == program.keyOf(BuiltinNames::codePointAt))
            return true;
    }
    return false;
}

bool boundsPositions(const Walker& walker, const EndLoopTest& test, Reference bound)
{
    static constexpr OpcodeSet lengths { Opcode::GetArrayLength, Opcode::GetTypedArrayLength, Opcode::GetStringLength };
    if (test.relation == Opcode::Greater || test.relation == Opcode::GreaterEq)
        return true;
    if (!bound || !bound.isValue())
        return false;
    const Code& code = walker.code();
    ValueID made = bound.id();
    while (conversionsOfForm.contains(code.at(made).opcode) || code.at(made).opcode == Opcode::Narrowed)
        made = code.at(made).operand(0);
    return lengths.contains(code.at(made).opcode);
}

bool benefitsFromHeaderGuard(Walker& walker, BlockID inputHead, std::span<const Reference> entering)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::UnboxedNumbers) || index.loop(index.loopOf(inputHead)).numberOfLoopsWithin != 1)
        return false;
    std::optional<EqualityLoopTest> equality = findEqualityLoopTest(walker, inputHead);
    std::optional<EndLoopTest> atEnd = equality ? std::nullopt : findEndLoopTest(walker, inputHead);
    if (!equality && !atEnd)
        return false;
    uint32_t position = equality ? equality->position : atEnd->position;
    ValueID bound = equality ? equality->bound : atEnd->bound;
    Reference enters = position < entering.size() ? entering[position] : Reference();
    if (!enters || !enters.isRecord() || !walker.types().at(walker.typeOf(enters)).isInteger() || (bound && !findLoopBoundRange(walker, index, index.block(inputHead).firstParameter(), bound)))
        return false;
    if (equality && findCounterRange(walker, inputHead, *equality, walker.types().at(walker.typeOf(enters)).integers, false))
        return false;
    if (atEnd && !boundsPositions(walker, *atEnd, bound ? walker.referenceOf(index.stripCasts(bound)) : Reference()))
        return false;
    NamesOfCounterOfLoop names;
    std::shared_ptr<const UsersOfValuesInCountersOfLoops> users = usersOfValuesOfLoopInCountersOfLoops(walker, index, inputHead);
    return isSteppedAllRoundInCountersOfLoops(walker, index, *users, inputHead, position, names) && isElementKey(walker, index, *users, names);
}

std::optional<Range> findCounterRange(Walker& walker, BlockID inputHead, const EqualityLoopTest& test, Range entering, bool isExitChecked)
{
    const BodyIndex& index = walker.currentBodyIndex();
    std::optional<Range> bound = test.bound ? findLoopBoundRange(walker, index, index.block(inputHead).firstParameter(), test.bound) : Range::one(0);
    if (!bound || !bound->isSubsetOf(Range::exact()) || !entering.isSubsetOf(Range::exact()))
        return std::nullopt;
    Range range = entering;
    if (!isExitChecked) {
        if (test.largestStep != 1 || (test.countsUp ? entering.maximum > bound->minimum : entering.minimum < bound->maximum))
            return std::nullopt;
        (test.countsUp ? range.maximum : range.minimum) = test.countsUp ? bound->maximum : bound->minimum;
    } else if (test.countsUp)
        range.maximum = std::max(range.maximum, bound->maximum - 1 + test.largestStep);
    else
        range.minimum = std::min(range.minimum, bound->minimum + 1 - test.largestStep);
    return range.isSubsetOf(Range::exact()) ? std::optional { range } : std::nullopt;
}

bool counterStaysWithinBound(Walker& walker, BlockID inputHead, const EqualityLoopTest& test, TypeID entering)
{
    if (!entering || isNeverType(entering) || !walker.types().at(entering).isInteger() || walker.types().at(entering).integers.isEmpty())
        return false;
    return findCounterRange(walker, inputHead, test, walker.types().at(entering).integers, false).has_value();
}

static constexpr OpcodeSet counterRelations { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
static constexpr OpcodeSet counterArithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement };

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
    bool isArithmetic = counterArithmetic.contains(made.opcode());
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
        if (!index.holdsAnyOf(counterRelations) || !index.holdsAnyOf(OpcodeSet { Opcode::Mul }))
            return found;
        Vector<ValueID, 8> parameters;
        for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
            if (!index.loopOf(BlockID(block)))
                continue;
            for (BodyValue relation : index.block(BlockID(block)).values()) {
                if (!counterRelations.contains(relation.opcode()))
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

struct MeetingBounds {
    uint32_t low { 0 };
    uint32_t high { 0 };
};

static constexpr Range rangeOfHalvedBounds { -(1ll << 30), (1ll << 30) - 1 };

static std::optional<MeetingBounds> findMeetingBounds(Walker& walker, BlockID inputHead)
{
    static constexpr OpcodeSet relationsOfLow { Opcode::Less, Opcode::LessEq };
    static constexpr OpcodeSet relationsOfHigh { Opcode::Greater, Opcode::GreaterEq };
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.holdsAnyOf(OpcodeSet { Opcode::RShift, Opcode::URShift }))
        return std::nullopt;
    BodyTerminator end = index.terminator(inputHead);
    ValueID asked = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!asked || index.isParameter(asked) || !(relationsOfLow | relationsOfHigh).contains(index.opcodeOf(asked)))
        return std::nullopt;
    LoopID loop = index.loopOf(inputHead);
    bool staysWhereTaken = index.isInLoop(end.targets()[0].block(), loop);
    if (staysWhereTaken == index.isInLoop(end.targets()[1].block(), loop) || index.isEnteredByHandlerEdge(inputHead))
        return std::nullopt;
    BodyValue relation = index.value(asked);
    bool namesLowFirst = (staysWhereTaken ? relationsOfLow : relationsOfHigh).contains(relation.opcode());
    ValueID low = parameterOfHeadNamedInCountersOfLoops(index, relation.operand(namesLowFirst ? 0 : 1), inputHead);
    ValueID high = parameterOfHeadNamedInCountersOfLoops(index, relation.operand(namesLowFirst ? 1 : 0), inputHead);
    if (!low || !high || low == high)
        return std::nullopt;
    uint32_t firstParameter = index.block(inputHead).firstParameter().index();
    auto changes = [&](ValueID bound) {
        return std::ranges::any_of(walker.incomingEdgesOf(index, inputHead), [&](const BodyIncomingEdge& edge) {
            std::span<const ValueID> arguments = edge.target.arguments();
            uint32_t place = bound.index() - firstParameter;
            return index.isInLoop(edge.from, loop) && place < arguments.size() && parameterOfHeadNamedInCountersOfLoops(index, arguments[place], inputHead) != bound;
        });
    };
    auto halves = [&](BodyValue operation) {
        if (operation.opcode() != Opcode::RShift && operation.opcode() != Opcode::URShift)
            return false;
        ValueID places = index.stripCasts(operation.operand(1));
        return !index.isParameter(places) && index.opcodeOf(places) == Opcode::Integer && index.value(places).as<Opcode::Integer>().integer() == 1;
    };
    bool loopHalves = std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), halves);
    });
    if (!loopHalves || !changes(low) || !changes(high))
        return std::nullopt;
    return MeetingBounds { low.index() - firstParameter, high.index() - firstParameter };
}

bool searchesNumbersByHalves(Walker& walker, BlockID inputHead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.holdsAnyOf(OpcodeSet { Opcode::ElemGet }) || !findMeetingBounds(walker, inputHead))
        return false;
    TypeArena& types = walker.types();
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto isReadOffRow = [&](ValueID holder) {
        holder = index.stripCasts(holder);
        return !holder || (holder.index() >= firstValueOfLoop && !index.isParameter(holder) && index.opcodeOf(holder) == Opcode::ElemGet);
    };
    return std::ranges::any_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue read) {
            if (read.opcode() != Opcode::ElemGet || isReadOffRow(read.objectOperand()))
                return false;
            TypeID seenByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
            return seenByLink && types.overlap(seenByLink, TypeTable::number) && types.at(seenByLink).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent);
        });
    });
}

std::optional<Range> findRangeOfMeetingBound(Walker& walker, BlockID inputHead, uint32_t position, std::span<const TypeID> entering)
{
    std::optional<MeetingBounds> bounds = findMeetingBounds(walker, inputHead);
    if (!bounds || (position != bounds->low && position != bounds->high) || std::max(bounds->low, bounds->high) >= entering.size())
        return std::nullopt;
    auto rangeOf = [&](uint32_t place) -> std::optional<Range> {
        TypeID type = entering[place];
        if (!type || !walker.types().at(type).isInteger() || walker.types().at(type).integers.isEmpty() || !walker.types().at(type).integers.isSubsetOf(rangeOfHalvedBounds))
            return std::nullopt;
        return walker.types().at(type).integers;
    };
    std::optional<Range> low = rangeOf(bounds->low);
    std::optional<Range> high = rangeOf(bounds->high);
    if (!low || !high)
        return std::nullopt;
    if (position == bounds->low)
        return Range { low->minimum, std::max(low->maximum, high->maximum + 1) };
    return Range { std::min(high->minimum, low->minimum - 1), high->maximum };
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

static bool callsCharCodeAt(const Program& program, const BodyIndex& index, ValueID value)
{
    if (index.isParameter(value) || index.opcodeOf(value) != Opcode::Call)
        return false;
    ValueID callee = index.stripCasts(index.value(value).calleeOperand());
    return callee && !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::PropGet && !index.value(callee).isFreeName() && index.value(callee).key() == program.keyOf(BuiltinNames::charCodeAt);
}

static TypeID typeWhereDivisionsBringIntegersInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, const LoopPreview& preview, std::span<const TypeID> carried, ValueID written, unsigned depth)
{
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Mod, Opcode::Increment, Opcode::Decrement };
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    if (ValueID parameter = parameterOfHeadNamedInCountersOfLoops(index, written, inputHead); parameter && carried[parameter.index() - firstValueOfLoop])
        return carried[parameter.index() - firstValueOfLoop];
    ValueID value = written ? index.stripCasts(written) : ValueID();
    if (!value || !depth || value.index() < firstValueOfLoop || index.isParameter(value) || !arithmetic.contains(index.opcodeOf(value))) {
        TypeID byLook = value ? typeByLookInCountersOfLoops(walker, index, preview, value) : TypeID();
        bool isUnknown = !byLook || isNeverType(byLook) || walker.types().at(byLook).isAnything();
        return isUnknown && value && callsCharCodeAt(walker.program(), index, value) ? walker.types().integer(Range { 0, std::numeric_limits<uint16_t>::max() }) : byLook;
    }
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
    if (!counters.isEmpty())
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
        counters.append({ position, carried[position], false, guesses.isEmpty() });
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

BitVector findParametersCheckedForBits(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering)
{
    BitVector found;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !index.holdsAnyOf(operatorsOnBitsOfCountersOfLoops | OpcodeSet { Opcode::URShift }))
        return found;
    uint32_t numberOfParameters = index.block(inputHead).numberOfParameters();
    Vector<WhatComesRoundInCountersOfLoops, 8> whatComesRound;
    whatComesRound.fill(WhatComesRoundInCountersOfLoops::NotAsked, numberOfParameters);
    for (uint32_t position = 0; position < numberOfParameters && position < entering.size(); ++position) {
        if (mayBeInt32InCountersOfLoops(walker, entering[position]) && comesRoundAsInt32InCountersOfLoops(walker, index, inputHead, preview, position, whatComesRound))
            found.set(position);
    }
    return found;
}

bool addendMayBeGuessedInt32(const Walker& walker, Reference addend)
{
    if (isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, addend))
        return true;
    if (mayBeInt32InCountersOfLoops(walker, addend) && walker.representationOf(addend) == Representation::OptionalInt64)
        return true;
    if (!addend || !addend.isValue() || walker.representationOf(addend) != Representation::Int64)
        return false;
    constexpr int64_t minNonIntegerStep = int64_t(1) << 52;
    Type type = walker.types().at(walker.typeOf(addend));
    return type.isInteger() && type.integers.minimum >= 0 && type.integers.maximum >= minNonIntegerStep;
}

BitVector namesOfInnerLoopCounters(Walker& walker, BlockID inputHead, const LoopPreview& preview)
{
    BitVector found;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !index.holdsAnyOf(stepsByOneOfCountersOfLoops | stepsByNumberOfCountersOfLoops))
        return found;
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    uint32_t numberOfLoopsLeft = index.loop(loop).numberOfLoopsWithin - 1;
    for (uint32_t inner = loop.index() + 1; numberOfLoopsLeft && inner < index.numberOfLoops(); ++inner) {
        BlockID head = index.loop(LoopID(inner)).head;
        if (!index.isInLoop(head, loop))
            continue;
        --numberOfLoopsLeft;
        std::shared_ptr<const UsersOfValuesInCountersOfLoops> users;
        for (uint32_t position = 0; position < index.block(head).numberOfParameters(); ++position) {
            TypeID inTurns = typeByLookInCountersOfLoops(walker, index, preview, index.block(head).parameter(position));
            if (!inTurns || isNeverType(inTurns) || !(walker.types().at(inTurns).primitives & Primitive::Integer))
                continue;
            if (!users)
                users = usersOfValuesOfLoopInCountersOfLoops(walker, index, head);
            NamesOfCounterOfLoop names;
            if (!isSteppedAllRoundInCountersOfLoops(walker, index, *users, head, position, names, &preview, nullptr, false, true, true))
                continue;
            for (ValueID name : names)
                found.set(name.index() - firstValueOfLoop);
        }
    }
    return found;
}

static bool isComparedWithPossibleInt32(Walker& walker, const BodyIndex& index, BlockID inputHead, ValueID counter)
{
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!condition || index.isParameter(condition) || !head.holds(condition) || (!allRelations.contains(index.opcodeOf(condition)) && !isEqualityLoopTest(index, inputHead, condition)))
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

static Vector<ValueID, 4> arrayReadsInIteration(Walker& walker, const BodyIndex& index, BlockID inputHead, const LoopPreview& preview)
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

static void guessAddendsOfCarriedSum(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t position, NamesBeforeLoop& namesBeforeLoop, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses)
{
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    auto entersElsewhere = [&](Reference value) {
        size_t place = std::ranges::find(entering, value) - entering.begin();
        return place < entering.size() && place != position;
    };
    Vector<ValueID, 8> sums { index.block(inputHead).parameter(position) };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::Add || sums.size() >= mostNamesOfCounterOfLoop)
                continue;
            for (uint32_t side : { 0u, 1u }) {
                if (!sums.contains(index.stripCasts(operation.operand(1 - side))))
                    continue;
                sums.append(operation.id());
                ValueID written = index.stripCasts(operation.operand(side));
                Reference value = written && written.index() < firstValueOfLoop ? walker.referenceOf(written) : Reference();
                if (!value || !value.isValue() || !walker.types().isUninformative(walker.typeOf(value)) || entersElsewhere(value))
                    break;
                if (guesses.containsIf([&](const HoistedSpeculation& guess) { return guess.value == value; }))
                    break;
                guesses.append(HoistedSpeculation { value, TypeTable::number, { }, namesBeforeLoop.findNamesOf(value) });
                walker.count(StatisticID::InvariantNumberGuardsHoisted);
                break;
            }
        }
    }
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
            reads = arrayReadsInIteration(walker, index, inputHead, preview);
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
    std::optional<MeetingBounds> bounds = holdsOperatorsOnBits ? findMeetingBounds(walker, inputHead) : std::nullopt;
    for (uint32_t position = 0; position < numberOfParameters && position < entering.size(); ++position) {
        Reference enters = entering[position];
        if (bounds && position == bounds->high && enters && enters.isValue()) {
            Type record = walker.types().at(walker.typeOf(enters));
            bool isWideInteger = record.isInteger() && record.integers.minimum >= rangeOfHalvedBounds.minimum && record.integers.maximum > rangeOfHalvedBounds.maximum;
            TypeID halved = walker.types().integer(Range { isWideInteger ? record.integers.minimum : rangeOfHalvedBounds.minimum, rangeOfHalvedBounds.maximum });
            size_t guessed = guesses.findIf([&](const HoistedSpeculation& guess) { return guess.value == enters; });
            bool isAskedElsewhere = (record.isInteger() && !isWideInteger) || guessed != notFound;
            if (guessed != notFound && guesses[guessed].tested == TypeID(TypeTable::number)) {
                guesses[guessed].tested = halved;
                continue;
            }
            if ((record.isNumber() || isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, enters)) && !isAskedElsewhere) {
                counters.append({ position, halved });
                continue;
            }
        }
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
        size_t placeOfGuess = guesses.findIf([&](const HoistedSpeculation& guess) { return guess.value == enters; });
        if (NamesOfCounterOfLoop names; placeOfGuess != notFound && guesses[placeOfGuess].tested == TypeID(TypeTable::number) && isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, enters) && isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names) && isElementKey(walker, index, users(), names)) {
            TypeID asked = isIncreasingKey(index, users(), names) ? walker.types().integer(Range { 0, std::numeric_limits<int32_t>::max() }) : TypeID(TypeTable::int32);
            guesses[placeOfGuess].tested = asked;
            counters.append({ position, asked });
            continue;
        }
        bool isTestedAlready = placeOfGuess != notFound || std::ranges::find(entering.first(position), enters) != entering.first(position).end();
        if (isTestedAlready)
            continue;
        if (holdsOperatorsOnBits && (comesRoundAsInt32InCountersOfLoops(walker, index, inputHead, preview, position, whatComesRound) || isRotatedWord(walker, index, inputHead, position, entering))) {
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
                    if (!isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names, &preview, nullptr, true)) {
                        names.shrink(0);
                        if (!walker.types().at(walker.typeOf(enters)).isNumber() && isSteppedAllRoundInCountersOfLoops(walker, index, users(), inputHead, position, names, &preview, nullptr, false, true, false, true)) {
                            counters.append({ position, TypeTable::number });
                            walker.count(StatisticID::InvariantNumberGuardsHoisted);
                        }
                        continue;
                    }
                }
            }
        }
        bool countsIterations = holdsAccessesUnderKeys && !isCheckedOnBackEdge && isComparedWithIntegerInHeader(index, inputHead, position);
        if ((isCheckedOnBackEdge || walker.types().at(walker.typeOf(enters)).isNumber()) && !countsIterations && !turnsTakeCounterAsIntegerInCountersOfLoops(index, users(), names))
            continue;
        if (!isCheckedOnBackEdge && !walker.types().at(walker.typeOf(enters)).isNumber() && isOnlyUsedInArithmetic(walker, index, users(), inputHead, preview, names)) {
            counters.append({ position, TypeTable::number });
            walker.count(StatisticID::InvariantNumberGuardsHoisted);
            continue;
        }
        bool stepEnters = std::ranges::any_of(stepsAskedInFront, [&](ValueID step) {
            return std::ranges::find(entering, walker.referenceOf(step)) != entering.end();
        });
        if (stepEnters)
            continue;
        bool isFromZeroUp = !isCheckedOnBackEdge && (isIncreasingKey(index, users(), names) || isTruthTestedInHeader(index, inputHead, position));
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
        TypeID carriedType = findCarriedNumberType(walker, inputHead, preview, enters, position);
        if (!carriedType && !isNumberCarriedBesideTextOrBigInt(walker, enters, preview.typesOfParameters[position]))
            continue;
        bool isTestedAlready = std::ranges::find(entering.first(position), enters) != entering.first(position).end()
            || counters.containsIf([&](const HoistedCounterCheck& counter) { return counter.position == position; })
            || guesses.containsIf([&](const HoistedSpeculation& guess) { return guess.value == enters; });
        if (!isTestedAlready) {
            counters.append({ position, carriedType ? carriedType : TypeID(TypeTable::number), !carriedType || !walker.types().at(carriedType).isInt32(), !carriedType, true });
            walker.count(StatisticID::SumsGuardedAsNumbers);
        }
        guessAddendsOfCarriedSum(walker, index, inputHead, position, namesBeforeLoop, entering, guesses);
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

namespace {

enum class KeyTermRole : uint8_t { Addend, Factor, Divisor };

struct KeyTerm {
    KeyTermRole role { KeyTermRole::Addend };
    ValueID value;
    ValueID holder;
    PropertyKeyID field;
    VariableID variable;
    bool decidesForm { false };
    Reference known { };
    Reference scope { };
    FieldReference captured { };
};

struct KeyScope {
    const BodyIndex& body;
    FunctionID function;
    std::span<const ValueID> arguments;
    ValueID thisValue;
    const KeyScope* caller { nullptr };
};

static constexpr int64_t largestKeyFactor = 1 << 20;
static constexpr Range rangeOfKeyAddends { -(int64_t { 1 } << 33), int64_t { 1 } << 33 };

class KeyTermCollector {
public:
    KeyTermCollector(Walker& walker, BlockID inputHead, const LoopPreview* preview)
        : m_walker(walker)
        , m_index(walker.currentBodyIndex())
        , m_preview(preview)
        , m_firstValueOfLoop(m_index.block(inputHead).firstParameter().index())
        , m_inputHead(inputHead)
    {
    }

    bool callsOnlyLeaves() const
    {
        static constexpr OpcodeSet otherCalls { Opcode::CallDirect, Opcode::CallList, Opcode::Construct, Opcode::SuperConstruct };
        for (BlockID member : m_index.membersOf(m_index.loopOf(m_inputHead))) {
            for (BodyValue operation : m_index.block(member).values()) {
                if (otherCalls.contains(operation.opcode()))
                    return false;
                if (operation.opcode() == Opcode::Call && !callsLeaf({ m_index, m_walker.currentFunction(), { }, { }, nullptr }, operation, deepestCall))
                    return false;
            }
        }
        return true;
    }

    template<typename Functor>
    void forEachKey(const Functor& visit)
    {
        KeyScope loop { m_index, m_walker.currentFunction(), { }, { }, nullptr };
        for (BlockID member : m_index.membersOf(m_index.loopOf(m_inputHead))) {
            m_isInInnerLoop = m_index.loopOf(member) != m_index.loopOf(m_inputHead);
            visitBlock(loop, m_index.block(member), deepestCall, visit);
        }
    }

private:
    static constexpr unsigned deepestCall = 2;
    static constexpr unsigned deepestTerm = 8;
    static constexpr OpcodeSet accesses { Opcode::ElemGet, Opcode::ElemSet };
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement };

    bool callsLeaf(const KeyScope& scope, const BodyValue& call, unsigned callsLeft) const
    {
        FunctionID callee = m_walker.facts().linked().calleeAt(scope.function, call.id());
        if (!callee) {
            ValueID read = scope.body.stripCasts(call.as<Opcode::Call>().calleeOperand());
            bool isPush = read && !scope.body.isParameter(read) && scope.body.opcodeOf(read) == Opcode::PropGet && !scope.body.value(read).isFreeName() && scope.body.value(read).key() == m_walker.program().keyOf(BuiltinNames::push);
            return isPush || isMathCall(m_walker, scope.body, call);
        }
        const BodyIndex* body = m_walker.facts().bodies().find(callee);
        if (!body || !callsLeft || !(body->hasLoops() ? canInlineSingleCallLoop(m_walker, callee, { scope.function, true, &scope.body, call.id() }, nullptr) : copyOfCalleeCalledInLoopPays(m_walker, callee, call.as<Opcode::Call>().argumentsOperands().size())))
            return false;
        KeyScope inner { *body, callee, { }, { }, &scope };
        for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
            for (BodyValue operation : body->block(BlockID(number)).values()) {
                if (operation.opcode() == Opcode::Call && !callsLeaf(inner, operation, callsLeft - 1))
                    return false;
            }
        }
        return true;
    }

    template<typename Functor>
    void visitBlock(const KeyScope& scope, BodyBlock block, unsigned callsLeft, const Functor& visit)
    {
        for (BodyValue operation : block.values()) {
            if (accesses.contains(operation.opcode())) {
                m_terms.shrink(0);
                m_hasProduct = false;
                ValueID key = scope.body.stripCasts(operation.propertyOperand());
                if (key && !scope.body.isParameter(key) && collectKey(scope, key) && m_hasProduct && !m_terms.isEmpty())
                    visit(m_terms.span());
                continue;
            }
            if (operation.opcode() != Opcode::Call || !callsLeft)
                continue;
            FunctionID callee = m_walker.facts().linked().calleeAt(scope.function, operation.id());
            const BodyIndex* body = callee ? m_walker.facts().bodies().find(callee) : nullptr;
            auto call = operation.as<Opcode::Call>();
            if (!body || !(body->hasLoops() ? canInlineSingleCallLoop(m_walker, callee, { scope.function, true, &scope.body, operation.id() }, nullptr) : body->holdsAnyOf(accesses) && body->holdsAnyOf(sums | OpcodeSet { Opcode::Mul, Opcode::Mod }) && copyOfCalleeCalledInLoopPays(m_walker, callee, call.argumentsOperands().size())))
                continue;
            KeyScope inner { *body, callee, call.argumentsOperands(), call.thisValueOperand(), &scope };
            for (uint32_t number = 0; number < body->numberOfBlocks(); ++number)
                visitBlock(inner, body->block(BlockID(number)), callsLeft - 1, visit);
        }
    }

    ValueID argumentNamed(const KeyScope& scope, ValueID parameter) const
    {
        if (scope.body.blockOf(parameter) != entryBlock)
            return { };
        const Function& function = m_walker.program().at(scope.function);
        uint32_t place = parameter.index() - scope.body.block(entryBlock).firstParameter().index();
        if (function.entryParameterKind(place) == EntryParameter::This)
            return scope.thisValue;
        if (function.entryParameterKind(place) != EntryParameter::Parameter || place < function.firstParameterIndex())
            return { };
        place -= function.firstParameterIndex();
        return place < scope.arguments.size() ? scope.arguments[place] : ValueID();
    }

    ValueID valueBeforeLoop(const KeyScope& scope, ValueID value) const
    {
        value = value ? scope.body.stripCasts(value) : ValueID();
        if (!value)
            return { };
        if (!scope.caller)
            return value.index() < m_firstValueOfLoop ? value : ValueID();
        if (!scope.body.isParameter(value))
            return { };
        return valueBeforeLoop(*scope.caller, argumentNamed(scope, value));
    }

    static ValueID sumTruncated(const BodyIndex& body, ValueID value)
    {
        if (body.opcodeOf(value) != Opcode::BitOr)
            return { };
        ValueID zero = body.value(value).operand(1);
        bool isZero = zero && !body.isParameter(zero) && body.opcodeOf(zero) == Opcode::Integer && !body.value(zero).as<Opcode::Integer>().integer();
        ValueID number = isZero ? body.stripCasts(body.value(value).operand(0)) : ValueID();
        if (!number || body.isParameter(number) || !sums.contains(body.opcodeOf(number)))
            return { };
        ValueID added = body.stripCasts(body.value(number).operand(1));
        return added && (body.isParameter(added) || body.opcodeOf(added) != Opcode::Integer) ? number : ValueID();
    }

    VariableID variableRead(const KeyScope& scope, ValueID value) const
    {
        if (m_preview)
            return findNumberInVariableThatNoTurnWritesInCountersOfLoops(m_walker, scope.body, m_preview->effects, value);
        if (scope.body.opcodeOf(value) == Opcode::CheckInitialized)
            value = scope.body.stripCasts(scope.body.value(value).operand(0));
        return !scope.body.isParameter(value) && scope.body.opcodeOf(value) == Opcode::GetModuleVariable ? scope.body.value(value).variable() : VariableID();
    }

    bool collectKey(const KeyScope& scope, ValueID key)
    {
        if (ValueID sum = sumTruncated(scope.body, key))
            key = sum;
        Opcode opcode = scope.body.opcodeOf(key);
        m_hasProduct |= opcode == Opcode::Mod;
        if (opcode == Opcode::Mod) {
            BodyValue remainder = scope.body.value(key);
            m_divisorDecidesForm = !scope.caller && m_preview && isRemainderOfIntegers(remainder);
            return collect(scope, remainder.operand(OperandIndex::Mod::left), KeyTermRole::Addend, 0) && collect(scope, remainder.operand(OperandIndex::Mod::right), KeyTermRole::Divisor, 0);
        }
        return (sums.contains(opcode) || opcode == Opcode::Mul) && collect(scope, key, KeyTermRole::Addend, 0);
    }

    bool collect(const KeyScope& scope, ValueID value, KeyTermRole role, unsigned depth)
    {
        value = value ? scope.body.stripCasts(value) : ValueID();
        if (!value || depth > deepestTerm)
            return false;
        if (!scope.caller && value.index() < m_firstValueOfLoop) {
            m_terms.append({ role, value, { }, { }, { }, decidesForm(role) });
            return true;
        }
        if (scope.body.isParameter(value)) {
            if (scope.caller) {
                if (scope.body.isLoopHead(scope.body.blockOf(value)))
                    return role == KeyTermRole::Addend;
                ValueID argument = argumentNamed(scope, value);
                return argument && collect(*scope.caller, argument, role, depth + 1);
            }
            return role == KeyTermRole::Addend && isIntegerInLoop(value);
        }
        BodyValue made = scope.body.value(value);
        Opcode opcode = made.opcode();
        if (opcode == Opcode::Integer)
            return role == KeyTermRole::Addend || made.as<Opcode::Integer>().integer() > 0;
        if (opcode == Opcode::PropGet && !made.isFreeName()) {
            ValueID holder = valueBeforeLoop(scope, made.objectOperand());
            if (holder)
                m_terms.append({ role, { }, holder, made.key(), { }, decidesForm(role) });
            return !!holder;
        }
        if (opcode == Opcode::GetModuleVariable || opcode == Opcode::GetEnvironmentField || opcode == Opcode::CheckInitialized) {
            VariableID variable = variableRead(scope, value);
            if (!variable && collectVariable(scope, value, role))
                return true;
            if (!variable)
                return role == KeyTermRole::Addend && !scope.caller && isIntegerInLoop(value);
            m_terms.append({ role, { }, { }, { }, variable });
            return true;
        }
        if (role != KeyTermRole::Addend)
            return false;
        if (ValueID sum = sumTruncated(scope.body, value))
            return collect(scope, sum, KeyTermRole::Addend, depth + 1);
        if (steps.contains(opcode))
            return collect(scope, made.operand(0), KeyTermRole::Addend, depth + 1);
        if (sums.contains(opcode))
            return collect(scope, made.operand(0), KeyTermRole::Addend, depth + 1) && collect(scope, made.operand(1), KeyTermRole::Addend, depth + 1);
        m_hasProduct |= opcode == Opcode::Mul;
        if (opcode == Opcode::Mul) {
            for (uint32_t factor : { 1u, 0u }) {
                size_t kept = m_terms.size();
                if (collect(scope, made.operand(factor), KeyTermRole::Factor, depth + 1) && collect(scope, made.operand(1 - factor), KeyTermRole::Addend, depth + 1))
                    return true;
                m_terms.shrink(kept);
            }
            return false;
        }
        return !scope.caller && isIntegerInLoop(value);
    }

    bool collectVariable(const KeyScope& scope, ValueID value, KeyTermRole role)
    {
        if (scope.caller || !m_preview || role == KeyTermRole::Divisor || (m_isInInnerLoop && m_preview->effects.mayRunOtherCode()))
            return false;
        if (m_index.opcodeOf(value) == Opcode::CheckInitialized)
            value = m_index.stripCasts(m_index.value(value).operand(0));
        if (m_index.isParameter(value) || (m_index.opcodeOf(value) != Opcode::GetEnvironmentField && m_index.opcodeOf(value) != Opcode::GetModuleVariable))
            return false;
        BodyValue read = m_index.value(value);
        if (outermostLoopStoresWhatIsRead(m_index, m_index.loop(m_index.loopOf(m_inputHead)).outermost, read))
            return false;
        bool isCaptured = read.opcode() == Opcode::GetEnvironmentField;
        Reference environment = isCaptured ? loopInvariantScope(m_walker, m_index, read.environmentOperand(), m_firstValueOfLoop) : Reference();
        if (isCaptured && !environment)
            return false;
        FieldReference captured = isCaptured ? FieldReference { read.layout(), read.field() } : FieldReference { };
        Reference known = isCaptured ? m_walker.state().environmentField(environment, captured) : m_walker.state().variable(read.variable());
        if (known && !known.isValue())
            return false;
        TypeID type = known ? m_walker.typeOf(known) : widestTypeOfOperation(m_walker, read);
        if (!type || isNeverType(type) || !m_walker.types().at(type).isJavaScriptValue() || !(m_walker.types().at(type).primitives & Primitive::Integer))
            return false;
        KeyTerm term { .role = role, .decidesForm = decidesForm(role), .known = known };
        if (!known && isCaptured) {
            term.scope = environment;
            term.captured = captured;
        } else if (!known)
            term.variable = read.variable();
        m_terms.append(term);
        return true;
    }

    bool decidesForm(KeyTermRole role) const { return role == KeyTermRole::Factor || (role == KeyTermRole::Divisor && m_divisorDecidesForm); }

    bool isRemainderOfIntegers(const BodyValue& remainder) const
    {
        TypeID dividend = typeByLookInCountersOfLoops(m_walker, m_index, *m_preview, remainder.operand(OperandIndex::Mod::left));
        TypeID divisor = m_walker.types().integer(Range { 1, std::numeric_limits<int32_t>::max() });
        return dividend && !isNeverType(dividend) && chooseFormOfOperator(m_walker.program(), m_walker.types(), Opcode::Mod, OperatorTypes { dividend, divisor }) == OperatorForm::Integers;
    }

    bool isIntegerInLoop(ValueID value) const
    {
        if (!m_preview)
            return true;
        TypeID type = typeByLookInCountersOfLoops(m_walker, m_index, *m_preview, value);
        return type && !isNeverType(type) && m_walker.types().at(type).isInteger();
    }

    Walker& m_walker;
    const BodyIndex& m_index;
    const LoopPreview* m_preview;
    uint32_t m_firstValueOfLoop;
    BlockID m_inputHead;
    Vector<KeyTerm, 8> m_terms;
    bool m_hasProduct { false };
    bool m_divisorDecidesForm { false };
    bool m_isInInnerLoop { false };
};

} // anonymous namespace

static bool computesKeys(Walker& walker)
{
    const BodyIndex& index = walker.currentBodyIndex();
    static constexpr OpcodeSet accessesAndCalls { Opcode::ElemGet, Opcode::ElemSet, Opcode::Call };
    return walker.has(Mechanism::TypedOperations) && walker.has(Mechanism::UnboxedNumbers) && walker.has(Mechanism::Folding) && index.holdsAnyOf(accessesAndCalls);
}

void hoistKeyFields(Walker& walker, BlockID inputHead)
{
    if (!computesKeys(walker) || walker.isUnreachable() || !walker.framesKeepFields())
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    Origin origin = walker.originOf(*index.block(inputHead).values().begin());
    origin.isTailCall = false;
    KeyTermCollector collector(walker, inputHead, nullptr);
    if (!collector.callsOnlyLeaves())
        return;
    collector.forEachKey([&](std::span<const KeyTerm> terms) {
        for (const KeyTerm& term : terms) {
            if (!term.holder || walker.isUnreachable())
                continue;
            readFilledFieldOfProvenObject(walker, walker.referenceOf(term.holder), term.field, origin);
        }
    });
}

void speculateOnKeyTerms(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses, Vector<HoistedVariableCheck, 2>& variables)
{
    if (!computesKeys(walker))
        return;
    TypeArena& types = walker.types();
    NamesBeforeLoop namesBeforeLoop(walker, inputHead);
    TypeID factors = types.integer(Range { 1, largestKeyFactor });
    TypeID divisors = types.integer(Range { 1, std::numeric_limits<int32_t>::max() });
    struct Asked {
        Reference value;
        TypeID tested;
        ValueID holder;
        PropertyKeyID field;
        VariableID variable;
        bool isName { false };
        Reference scope { };
        FieldReference captured { };
    };
    KeyTermCollector collector(walker, inputHead, &preview);
    if (!collector.callsOnlyLeaves())
        return;
    collector.forEachKey([&](std::span<const KeyTerm> terms) {
        Vector<Asked, 8> asked;
        bool someTermIsNumber = false;
        for (const KeyTerm& term : terms) {
            if (term.variable || term.scope) {
                someTermIsNumber |= term.role == KeyTermRole::Factor;
                asked.append({ { }, term.role == KeyTermRole::Addend ? TypeID(TypeTable::int32) : term.role == KeyTermRole::Factor ? factors : divisors, { }, { }, term.variable, false, term.scope, term.captured });
                continue;
            }
            Reference value = term.known ? term.known : term.holder ? findFieldValue(walker, walker.referenceOf(term.holder), term.field) : walker.referenceOf(term.value);
            TypeID type = value ? walker.typeOf(value) : TypeID();
            if (!type || isNeverType(type))
                return;
            TypeID tested = term.role == KeyTermRole::Addend ? TypeID(TypeTable::int32) : term.role == KeyTermRole::Factor ? factors : divisors;
            if (types.isSubset(type, tested) || (term.role == KeyTermRole::Addend && types.at(type).isInteger() && types.at(type).integers.isSubsetOf(rangeOfKeyAddends) && walker.representationOf(value) != Representation::Double))
                continue;
            if (!value.isValue() || !types.overlap(type, tested) || std::ranges::find(entering, value) != entering.end())
                return;
            if (!types.at(type).isInteger() && !addendMayBeGuessedInt32(walker, value))
                return;
            bool isInteger = walker.representationOf(value) == Representation::Int32 || walker.representationOf(value) == Representation::Int64;
            bool mayBeNegativeDivisor = term.role == KeyTermRole::Divisor && isInteger && types.at(type).integers.minimum < 0;
            someTermIsNumber |= (!isInteger || term.decidesForm || mayBeNegativeDivisor || term.role == KeyTermRole::Addend) && types.at(type).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
            asked.append({ value, tested, term.holder, term.field, { } });
        }
        if (!someTermIsNumber)
            return;
        for (const Asked& term : asked) {
            if (term.variable || term.scope) {
                size_t listed = variables.findIf([&](const HoistedVariableCheck& check) { return check.variable == term.variable && check.scope == term.scope && check.field == term.captured; });
                if (listed == notFound) {
                    variables.append({ term.variable, term.tested, StatisticID::DivisorVariableReadsHoisted, false, term.scope, term.captured });
                    walker.count(StatisticID::InvariantNumberGuardsHoisted);
                } else if (TypeID both = types.intersectionOf(variables[listed].tested, term.tested); both && !isNeverType(both) && types.at(both).isInteger())
                    variables[listed].tested = both;
                continue;
            }
            size_t listed = guesses.findIf([&](const HoistedSpeculation& guess) {
                return guess.value == term.value;
            });
            if (listed != notFound) {
                TypeID both = types.intersectionOf(guesses[listed].tested, term.tested);
                if (both && !isNeverType(both) && types.at(both).isInteger())
                    guesses[listed].tested = both;
                continue;
            }
            guesses.append(HoistedSpeculation { term.value, term.tested, { }, term.holder ? Vector<ValueID, 2>() : namesBeforeLoop.findNamesOf(term.value) });
            guesses.last().isTermOfKey = true;
            if (term.holder)
                guesses.last().fieldInPreheader = HoistedSpeculation::FieldInPreheader { walker.referenceOf(term.holder), term.field };
            walker.count(StatisticID::InvariantNumberGuardsHoisted);
        }
    });
}

void guessFactorsInVariables(Walker& walker, BlockID inputHead, const LoopPreview& preview, Vector<HoistedVariableCheck, 2>& variables)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !index.holdsAnyOf(OpcodeSet { Opcode::Mul }) || !index.holdsAnyOf(OpcodeSet { Opcode::GetModuleVariable }))
        return;
    LoopID loop = index.loopOf(inputHead);
    for (BlockID member : index.membersOf(loop)) {
        if (index.loopOf(member) != loop)
            continue;
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::Mul)
                continue;
            for (uint32_t place : { 0u, 1u }) {
                VariableID variable = findNumberInVariableThatNoTurnWritesInCountersOfLoops(walker, index, preview.effects, operation.operand(place));
                if (!variable || variables.containsIf([&](const HoistedVariableCheck& check) { return check.variable == variable; }))
                    continue;
                TypeID proven = typeByLookInCountersOfLoops(walker, index, preview, operation.operand(place));
                if (proven && !isNeverType(proven) && walker.types().at(proven).isNumber())
                    continue;
                variables.append({ variable, TypeTable::number, StatisticID::DivisorVariableReadsHoisted });
                walker.count(StatisticID::InvariantNumberGuardsHoisted);
            }
        }
    }
}

void guessViewsInVariables(Walker& walker, BlockID inputHead, Vector<HoistedVariableCheck, 2>& variables)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::TypedOperations) || !walker.opensGuesses() || !index.holdsAnyOf(accessesUnderKeysInCountersOfLoops) || !index.holdsAnyOf(OpcodeSet { Opcode::GetModuleVariable }))
        return;
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    Vector<VariableID, 4> stored;
    Vector<VariableID, 4> accessed;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            Opcode opcode = operation.opcode();
            if (effectRuleOf(opcode) == EffectRule::Call || opcode == Opcode::Await || opcode == Opcode::Yield)
                return;
            if (opcode == Opcode::PutModuleVariable)
                stored.append(operation.variable());
            if (!accessesUnderKeysInCountersOfLoops.contains(opcode))
                continue;
            ValueID read = index.stripCasts(operation.objectOperand());
            if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::GetModuleVariable)
                accessed.append(index.value(read).variable());
        }
    }
    auto typeStoredIntoUnknown = [&](const BodyValue& read) -> TypeID {
        VariableID variable = read.variable();
        if (stored.contains(variable) || walker.program().at(variable).module != walker.program().at(walker.currentFunction()).module || variables.containsIf([&](const HoistedVariableCheck& check) { return check.variable == variable; }))
            return { };
        TypeID widest = widestTypeOfOperation(walker, read);
        TypeID held = walker.facts().beforeEvaluation().typeOf(variable);
        if (!held)
            held = walker.facts().held().variable(variable).proven;
        if (!widest || !held || !types.isUninformative(widest))
            return { };
        TypeID found = types.withoutPrimitives(held, Primitive::Absent | Primitive::Undefined | Primitive::Null);
        return isNeverType(found) ? TypeID() : found;
    };
    Vector<HoistedVariableCheck, 2> numbers;
    bool findsView = false;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue read : index.block(member).values()) {
            TypeID found = read.opcode() == Opcode::GetModuleVariable ? typeStoredIntoUnknown(read) : TypeID();
            if (!found)
                continue;
            if (accessed.contains(read.variable()) && isViewOfNumbersReadInLine(walker, found)) {
                variables.append({ read.variable(), found, StatisticID::CapturedArrayReadsHoisted });
                findsView = true;
            } else if (types.at(found).isNumber() && !numbers.containsIf([&](const HoistedVariableCheck& check) { return check.variable == read.variable(); }))
                numbers.append({ read.variable(), types.isSubset(found, TypeTable::int32) ? found : TypeID(TypeTable::number), StatisticID::CapturedArrayReadsHoisted });
        }
    }
    if (findsView)
        variables.appendVector(numbers);
}

void findNumbersOnlyReckonedWith(Walker& walker, BlockID inputHead, Vector<HoistedSpeculation, 4>& guesses, Vector<HoistedVariableCheck, 2>& variables)
{
    static constexpr OpcodeSet reckoning { Opcode::Mul, Opcode::Div, Opcode::Sub };
    bool someIsNumber = false;
    for (HoistedSpeculation& guess : guesses)
        someIsNumber |= guess.isOnlyReckonedWith = guess.tested == TypeTable::number && !guess.names.isEmpty() && guess.value.isValue() && walker.representationOf(guess.value) == Representation::JSValue;
    for (HoistedVariableCheck& check : variables)
        someIsNumber |= check.isOnlyReckonedWith = check.tested == TypeTable::number;
    if (!someIsNumber)
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    bool holdsHandler = std::ranges::any_of(index.membersOf(index.loopOf(inputHead)), [&](BlockID member) {
        return index.block(member).hasHandler();
    });
    if (holdsHandler) {
        for (HoistedSpeculation& guess : guesses)
            guess.isOnlyReckonedWith = false;
        for (HoistedVariableCheck& check : variables)
            check.isOnlyReckonedWith = false;
        return;
    }
    auto takeOtherwise = [&](ValueID operand) {
        if (!operand)
            return;
        for (HoistedSpeculation& guess : guesses)
            guess.isOnlyReckonedWith &= !guess.names.contains(operand);
        ValueID read = index.stripCasts(operand);
        if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
            read = index.stripCasts(index.value(read).operand(0));
        if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
            return;
        for (HoistedVariableCheck& check : variables)
            check.isOnlyReckonedWith &= check.variable != index.value(read).variable();
    };
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values()) {
            Opcode opcode = operation.opcode();
            if (reckoning.contains(opcode) || allRelations.contains(opcode) || truncatesForNumber(index, operation) || opcodesThatStateTypes.contains(opcode) || opcode == Opcode::CheckInitialized)
                continue;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                takeOtherwise(operand);
            });
        }
        block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
            takeOtherwise(operand);
        });
        for (BodyTarget target : block.terminator().targets()) {
            for (ValueID argument : target.arguments())
                takeOtherwise(argument);
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                takeOtherwise(argument);
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
    if (!isSteppedAllRoundInCountersOfLoops(walker, index, *users, inputHead, position, names) || (!turnsTakeCounterAsIntegerInCountersOfLoops(index, *users, names) && !storesCounter(index, *users, names)))
        return { };
    return { asked, number, variable };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
