/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The build proves that every value that enters a position declared `int32` is a whole number in range, and reports where it cannot. Nothing is built: to the code the declaration stays a guess.
// At a suspension, at a throw: nothing. It belongs to no mechanism and is never withheld.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTDiagnostics.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeNarrowing.h"
#include "AOTWalkNumberGuesses.h"
#include <cmath>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// Where the result of one of these may be no integer, the reason lies with the operand that may be none, if there is one.
static constexpr OpcodeSet opcodesFollowedForInt32Failure = opcodesThatGiveSameValue | allSums | OpcodeSet { Opcode::ToNumber, Opcode::ToNumeric, Opcode::Negate };
// Each makes a number of its operands by a rule that reads their types and nothing else.
static constexpr OpcodeSet operatorsJudgedByWrittenTypes = allNumericOperators;

// Whatever stands beside the numbers is the position's own affair.
static bool numbersKeepInt32Promise(const Type& type)
{
    return !(type.primitives & Primitive::Double) && type.integers.isSubsetOf(Range::int32());
}

static TypingContext typingForInt32Promises(Walker& walker)
{
    return { walker.program(), walker.types(), PositionTypes::knowingNothing() };
}

static bool writtenTypeKeepsInt32Promise(Walker& walker, TypeID written)
{
    if (!written)
        return false;
    Type type = walker.types().at(written);
    return (type.primitives & Primitive::Integer) && numbersKeepInt32Promise(type);
}

static std::optional<Int32ProofFailure> int32FailureSpecificTo(Opcode opcode)
{
    if (opcode == Opcode::Mul)
        return Int32ProofFailure::Product;
    if (opcode == Opcode::Div)
        return Int32ProofFailure::Quotient;
    if (opcode == Opcode::Mod)
        return Int32ProofFailure::Remainder;
    if (opcode == Opcode::Pow)
        return Int32ProofFailure::Power;
    return std::nullopt;
}

static Int32ProofFailure whyConstantBreaksInt32Promise(double number)
{
    if (std::isnan(number))
        return Int32ProofFailure::NotANumber;
    if (!number && std::signbit(number))
        return Int32ProofFailure::NegativeZero;
    if (std::isfinite(number) && number != std::trunc(number))
        return Int32ProofFailure::Fraction;
    return Int32ProofFailure::OutOfRange;
}

namespace {

struct TypeAsWritten {
    // Null: nobody can say.
    TypeID type;
    // A type that the program wrote stands in it, in place of what the walk has for the same value.
    bool restsOnWrittenType { false };
    // Of some parameter of the function the walk has less than is written: Link, a caller or an inlined call have said who calls.
    bool leavesOutWhoCalls { false };
};

// The block in which the run stands that asks: every relation in front of it holds of the value.
struct ValueAtPlace {
    ValueID value;
    BlockID place;
};

// Over the body being walked. A parameter of the function is what is written of it, and so is a call or a read of a place with a written type, with or without a cast. An operator and a built-in whose row bounds its result by its arguments go by their rules.
// A relation that the program asks in front of the promise cuts what is written of its operands: `if (limit > i) return i + 1`.
// Where ways meet again, what arrives is what each way brings, judged in the block that the way leaves: `a !== -2147483648 ? a - 1 : a`. Not at the head of a loop: typeOfParameter().
// For everything else the walk answers, and for whatever is made of such values alone.
// `function sum(a: int32, b: int32) { return (a + b) as int32; }` is reported though `sum(1, 2)` is the one call, and `Math.min(a.x, b.y)` of two int32s is not, whatever arrives in them.
class TypesAsWritten {
    WTF_MAKE_NONCOPYABLE(TypesAsWritten);
public:
    // `place`: the block in which the promise is made.
    TypesAsWritten(Walker& walker, BlockID place)
        : m_walker(walker)
        , m_input(walker.currentBodyIndex())
        , m_place(place)
    {
    }

    TypeAsWritten of(ValueID);
    // Behind of(), for a value that may be a double.
    Int32ProofFailure failureNamedByProducerOf(ValueID) const;
    // Of some parameter of the function the walk has less than is written. Then whatever it makes of the body may rest on who calls.
    bool walkKnowsWhoCalls() const;

private:
    using Parts = Vector<ValueAtPlace, 4>;

    static uint64_t keyOf(ValueAtPlace asked) { return static_cast<uint64_t>(asked.place.index()) << 32 | asked.value.index(); }
    TypeAsWritten known(ValueAtPlace asked) const { return m_known.get(keyOf(asked)); }
    TypeAsWritten asWalkHasIt(ValueID) const;
    // Of a value that is made of no other: by what is written of it alone.
    TypeAsWritten typeWrittenOf(ValueID) const;
    TypeID typeStatedByCastsOf(ValueID) const;
    Parts otherSidesOfRelationsInFront(ValueAtPlace) const;
    TypeAsWritten cutByRelationsInFront(ValueAtPlace, TypeAsWritten) const;
    BuiltinID builtinWrittenAsCallee(ValueID) const;
    BuiltinID builtinBoundedByArguments(BodyValue) const;
    // The values that the type is made of. None: the walk answers.
    Parts partsOf(ValueAtPlace) const;
    Parts valuesThatMeetIn(ValueID parameter) const;
    std::optional<BodyEdgeResult> testThatFills(ValueID parameter) const;
    TypeAsWritten typeOfParameter(BodyValue) const;
    TypeAsWritten typeWhereWaysMeet(ValueID parameter, const Parts&) const;
    TypeAsWritten typeBehindTest(ValueID parameter, const BodyEdgeResult&, const Parts& operandsOfTest) const;
    TypeAsWritten typeBehindConvert(BodyValue, BlockID place) const;
    TypeAsWritten compute(ValueAtPlace) const;

    Walker& m_walker;
    const BodyIndex& m_input;
    BlockID m_place;
    UncheckedKeyHashMap<uint64_t, TypeAsWritten, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_known;
};

} // anonymous namespace

TypeAsWritten TypesAsWritten::asWalkHasIt(ValueID value) const
{
    Reference name = m_walker.referenceOf(value);
    return { name ? m_walker.typeOf(name) : TypeID() };
}

TypeAsWritten TypesAsWritten::typeWrittenOf(ValueID value) const
{
    if (m_input.isParameter(value))
        return typeOfParameter(m_input.value(value));
    // Also in an arm that the walk has not entered, because it knows who calls.
    if (primitiveConstants.contains(m_input.opcodeOf(value)))
        return { typeOfConstant(typingForInt32Promises(m_walker), m_input.value(value)) };
    ExpectationOfValue expectation = findExpectation(m_walker, value);
    if (expectation.type && expectation.isDeclared)
        return { expectation.type, true };
    return asWalkHasIt(value);
}

// `if (a !== -2147483648) return a - 1`. As boundsProvenOnEdge(), which leaves equality out because the walk builds a name on every edge for which it answers.
static std::optional<std::array<BodyEdgeResult, 2>> sidesOfEqualityOnEdgeForInt32Promises(const BodyIndex& index, const BodyTerminator& terminator, uint32_t targetIndex)
{
    if (!terminator.isConditionalBranch() || index.isParameter(terminator.operand(0)))
        return std::nullopt;
    BodyValue comparison = index.value(terminator.operand(0));
    if (comparison.opcode() != Opcode::StrictEq && comparison.opcode() != Opcode::Eq)
        return std::nullopt;

    std::array<BodyEdgeResult, 2> sides;
    for (uint32_t operandIndex = 0; operandIndex < sides.size(); ++operandIndex) {
        sides[operandIndex].proof = Proof::Equality;
        sides[operandIndex].test = comparison.id();
        sides[operandIndex].subject = comparison.operand(operandIndex);
        sides[operandIndex].operandIndex = operandIndex;
        sides[operandIndex].isOnTakenEdge = !targetIndex;
    }
    return sides;
}

// Up the blocks that stand above the place, as far as the block that makes the value: every run that comes to the place has come through each of them, and by the edge where one edge alone enters.
// `for (let i: int32 = 0; i < to; i++) { if (c) .. }`: ways part and meet between the test and the step, and the test holds all the same.
// `i < point[key]`: the front end states behind a read that is an operand what the checker has for it, where that is an int32. As typeBehindConvert(): a cast into a position with the promise proves nothing,
// and what is only expected of a value is written nowhere.
TypeID TypesAsWritten::typeStatedByCastsOf(ValueID value) const
{
    while (value && !m_input.isParameter(value) && m_input.opcodeOf(value) == Opcode::Convert) {
        BodyValue convert = m_input.value(value);
        DeclaredType declared = convert.as<Opcode::Convert>().declared();
        TypeID stated = targetOfConvert(typingForInt32Promises(m_walker), declared);
        if (stated && !declared.has(PositionFlags::IsInt32Promise) && !declared.isExpected() && writtenTypeKeepsInt32Promise(m_walker, stated))
            return stated;
        value = convert.operand(0);
    }
    return { };
}

// `i <= point[key] - 1`: what bounds the value is made of other values, as the value that enters a promise is. Each other side, in the block of its relation: of() judges it in front of the value that it cuts.
// That block stands above the place that asks, so the places climb and this ends. The climb is that of cutByRelationsInFront().
TypesAsWritten::Parts TypesAsWritten::otherSidesOfRelationsInFront(ValueAtPlace asked) const
{
    Parts otherSides;
    BlockID made = m_input.blockOf(asked.value);
    for (BlockID below = asked.place, block = m_input.immediateDominator(below); below != made && block; below = block, block = m_input.immediateDominator(below)) {
        std::optional<BodyIncomingEdge> edge = m_input.onlyIncomingEdge(below);
        if (!edge)
            continue;
        std::optional<std::array<BodyEdgeResult, 2>> bounds = boundsProvenOnEdge(m_input, m_input.terminator(block), edge->targetIndex);
        if (!bounds)
            bounds = sidesOfEqualityOnEdgeForInt32Promises(m_input, m_input.terminator(block), edge->targetIndex);
        if (!bounds)
            continue;
        for (const BodyEdgeResult& bound : *bounds) {
            if (m_input.valueBehindCasts(bound.subject) != asked.value)
                continue;
            if (ValueID otherSide = m_input.value(bound.test).operand(1 - bound.operandIndex))
                otherSides.append({ otherSide, block });
        }
    }
    return otherSides;
}

TypeAsWritten TypesAsWritten::cutByRelationsInFront(ValueAtPlace asked, TypeAsWritten written) const
{
    if (!written.type)
        return written;
    TypingContext typing = typingForInt32Promises(m_walker);
    BlockID made = m_input.blockOf(asked.value);
    for (BlockID below = asked.place, block = m_input.immediateDominator(below); below != made && block; below = block, block = m_input.immediateDominator(below)) {
        std::optional<BodyIncomingEdge> edge = m_input.onlyIncomingEdge(below);
        if (!edge)
            continue;
        std::optional<std::array<BodyEdgeResult, 2>> bounds = boundsProvenOnEdge(m_input, m_input.terminator(block), edge->targetIndex);
        if (!bounds)
            bounds = sidesOfEqualityOnEdgeForInt32Promises(m_input, m_input.terminator(block), edge->targetIndex);
        if (!bounds)
            continue;
        for (const BodyEdgeResult& bound : *bounds) {
            if (m_input.valueBehindCasts(bound.subject) != asked.value)
                continue;
            BodyValue relation = m_input.value(bound.test);
            ValueID otherSide = relation.operand(1 - bound.operandIndex);
            TypeAsWritten judged = known({ otherSide, block });
            TypeID other = judged.restsOnWrittenType ? judged.type : TypeID();
            if (!other)
                other = typeStatedByCastsOf(otherSide);
            if (!other)
                other = typeWrittenOf(m_input.valueBehindCasts(otherSide)).type;
            if (!other)
                continue;
            TypeID cut = narrowByComparison(typing, relation.opcode(), !bound.operandIndex, written.type, other, bound.isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken);
            if (!cut || isNeverType(cut) || cut == written.type)
                continue;
            // The relation is written, and the walk has no name for what it leaves: whatever is made of the value is made by the rules.
            written.type = cut;
            written.restsOnWrittenType = true;
        }
    }
    return written;
}

// The front end names a member where the table locks it. Where the program may put something else in its place, as a run of tests that replaces `Math`, it says which built-in the callee is
// declared to be, as what is expected of the read. That is what is written of the callee.
BuiltinID TypesAsWritten::builtinWrittenAsCallee(ValueID callee) const
{
    if (!callee || m_input.isParameter(callee))
        return { };
    BodyValue written = m_input.value(callee);
    if (written.opcode() == Opcode::Convert) {
        DeclaredType declared = written.as<Opcode::Convert>().declared();
        if (declared.isExpected() && declared.written()) {
            if (BuiltinID expected = singleBuiltinOf(m_walker.types(), m_walker.types().at(declared.written())))
                return expected;
        }
        return builtinWrittenAsCallee(written.operand(0));
    }
    if (written.opcode() != Opcode::Builtin)
        return { };
    BuiltinID named = written.as<Opcode::Builtin>().builtin();
    return m_walker.program().builtins().at(named).mutability == BuiltinMutability::AlwaysLocked ? named : BuiltinID();
}

// `Math.min(a.x, b.y)`.
BuiltinID TypesAsWritten::builtinBoundedByArguments(BodyValue written) const
{
    if (written.opcode() != Opcode::Call)
        return { };
    auto call = written.as<Opcode::Call>();
    BuiltinID named = builtinWrittenAsCallee(call.calleeOperand());
    if (!named || std::ranges::contains(call.argumentsOperands(), ValueID()))
        return { };

    const BuiltinTable& table = m_walker.program().builtins();
    BuiltinID builtin = table.canonicalID(named);
    bool isBounded = std::ranges::any_of(table.overloadsOf(builtin), [](const BuiltinOverload& overload) {
        return overload.mode == BuiltinCallMode::Call && !overload.receiver && overload.result.rangeRule != BuiltinRangeRule::None;
    });
    return isBounded ? builtin : BuiltinID();
}

TypesAsWritten::Parts TypesAsWritten::partsOf(ValueAtPlace asked) const
{
    BodyValue written = m_input.value(asked.value);
    Parts parts;
    if (written.isParameter()) {
        std::optional<BodyEdgeResult> filled = testThatFills(asked.value);
        if (!filled)
            return valuesThatMeetIn(asked.value);
        BodyValue test = m_input.value(filled->test);
        BlockID from = m_input.onlyIncomingEdge(m_input.blockOf(asked.value))->from;
        for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(test.opcode()); ++operandIndex) {
            if (!test.operand(operandIndex))
                return { };
            parts.append({ test.operand(operandIndex), from });
        }
        return parts;
    }
    Opcode opcode = written.opcode();
    if (opcode == Opcode::Convert || operatorsJudgedByWrittenTypes.contains(opcode)) {
        for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(opcode); ++operandIndex) {
            if (ValueID operand = written.operand(operandIndex))
                parts.append({ operand, asked.place });
        }
    } else if (builtinBoundedByArguments(written)) {
        for (ValueID argument : written.as<Opcode::Call>().argumentsOperands())
            parts.append({ argument, asked.place });
    }
    return parts;
}

// What each edge into the block of the parameter hands to it, with the block that the edge leaves. None: the entry, the head of a loop, a block that a handler edge enters, a parameter that a test fills.
TypesAsWritten::Parts TypesAsWritten::valuesThatMeetIn(ValueID parameter) const
{
    BlockID block = m_input.blockOf(parameter);
    BlockID above = m_input.immediateDominator(block);
    uint32_t numberOfEdges = m_input.numberOfIncomingEdges(block);
    if (!above || m_input.isLoopHead(block) || m_input.isEnteredByHandlerEdge(block) || m_input.hasIrreducibleFlow() || numberOfEdges == BodyIndex::maxCountedIncomingEdges)
        return { };
    uint32_t index = *m_input.block(block).indexOfParameter(parameter);

    // Whoever jumps here stands at or below the block above. In the order of the walk the arms come in front of the block in which they meet: an edge that does not is missed by the count, and then the walk answers.
    Parts arriving;
    std::span<const BlockID> order = m_input.blocksInDominatorOrder();
    for (uint32_t number = m_input.dominatorNumber(above); number < m_input.dominatorNumber(block); ++number) {
        for (BodyTarget target : m_input.terminator(order[number]).targets()) {
            if (target.block() != block)
                continue;
            if (index >= target.arguments().size() || !target.arguments()[index])
                return { };
            arriving.append({ target.arguments()[index], order[number] });
        }
    }
    if (arriving.size() != numberOfEdges)
        return { };
    return arriving;
}

// BodyIndex::valueNarrowedInto(), with the test.
std::optional<BodyEdgeResult> TypesAsWritten::testThatFills(ValueID parameter) const
{
    std::optional<BodyIncomingEdge> edge = m_input.onlyIncomingEdge(m_input.blockOf(parameter));
    if (!edge)
        return std::nullopt;
    uint32_t numberOfProofs = static_cast<uint32_t>(edge->target.proofs().size());
    for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
        std::optional<BodyEdgeResult> result = m_input.edgeResult(edge->from, edge->targetIndex, edge->target, proofIndex);
        if (result && result->parameter == parameter)
            return result->test ? result : std::nullopt;
    }
    return std::nullopt;
}

// Of the function, and of a variable with the promise at the head of a loop: whatever comes in there has been held to the promise where it was stored, the first value and that of each turn.
// So a store in the loop is judged with the variable as it is written and the tests of the program in front: `for (let i: int32 = 0; i < to; i++)` has room for the step, `i <= to` has not.
TypeAsWritten TypesAsWritten::typeOfParameter(BodyValue parameter) const
{
    TypeAsWritten walked = asWalkHasIt(parameter.id());
    TypeID written = parameter.parameterType();
    if (!written || !walked.type)
        return walked;
    if (m_input.blockOf(parameter.id()) != entryBlock)
        return writtenTypeKeepsInt32Promise(m_walker, written) ? TypeAsWritten { written, true } : walked;
    return { written, true, !m_walker.types().isSubset(written, walked.type) };
}

bool TypesAsWritten::walkKnowsWhoCalls() const
{
    BodyBlock entry = m_input.block(entryBlock);
    for (uint32_t index = 0; index < entry.numberOfParameters(); ++index) {
        if (typeOfParameter(entry.parameterValue(index)).leavesOutWhoCalls)
            return true;
    }
    return false;
}

// Every run has come by one of the edges.
TypeAsWritten TypesAsWritten::typeWhereWaysMeet(ValueID parameter, const Parts& arriving) const
{
    TypeAsWritten met;
    for (ValueAtPlace part : arriving) {
        TypeAsWritten typeOfPart = known(part);
        if (!typeOfPart.type)
            return asWalkHasIt(parameter);
        met.type = met.type ? m_walker.types().unionOf(met.type, typeOfPart.type) : typeOfPart.type;
        met.restsOnWrittenType |= typeOfPart.restsOnWrittenType;
        met.leavesOutWhoCalls |= typeOfPart.leavesOutWhoCalls;
    }
    return met.restsOnWrittenType ? met : asWalkHasIt(parameter);
}

// `a ?? 0`: the parameter is the value that was tested, on the edge on which the test has said what it is.
TypeAsWritten TypesAsWritten::typeBehindTest(ValueID parameter, const BodyEdgeResult& filled, const Parts& operandsOfTest) const
{
    Vector<TypeID, 2> types;
    for (ValueAtPlace operand : operandsOfTest)
        types.append(known(operand).type);
    TypeAsWritten tested = filled.operandIndex < operandsOfTest.size() ? known(operandsOfTest[filled.operandIndex]) : TypeAsWritten();
    if (!tested.restsOnWrittenType || types.contains(TypeID()))
        return asWalkHasIt(parameter);
    tested.type = typeOfEdgeResult(typingForInt32Promises(m_walker), m_input.value(filled.test), OperandTypes { types.span() }, filled.operandIndex, filled.isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken);
    return tested.type && !isNeverType(tested.type) ? tested : asWalkHasIt(parameter);
}

TypeAsWritten TypesAsWritten::typeBehindConvert(BodyValue convert, BlockID place) const
{
    DeclaredType declared = convert.as<Opcode::Convert>().declared();
    TypeAsWritten entering = known({ convert.operand(0), place });
    TypeID stated = targetOfConvert(typingForInt32Promises(m_walker), declared);
    // A cast into a position with the promise proves nothing, and what is only expected of a value is written nowhere.
    bool statesNothing = !stated || declared.has(PositionFlags::IsInt32Promise) || declared.isExpected();
    if (!statesNothing && writtenTypeKeepsInt32Promise(m_walker, stated)) {
        TypeID both = entering.type && entering.restsOnWrittenType ? m_walker.types().intersectionOf(entering.type, stated) : TypeID();
        return both && !isNeverType(both) ? TypeAsWritten { both, true, entering.leavesOutWhoCalls } : TypeAsWritten { stated, true };
    }
    if (!entering.type || !entering.restsOnWrittenType)
        return asWalkHasIt(convert.id());
    if (statesNothing)
        return entering;
    TypeID both = m_walker.types().intersectionOf(entering.type, stated);
    return { isNeverType(both) ? stated : both, true, entering.leavesOutWhoCalls };
}

TypeAsWritten TypesAsWritten::compute(ValueAtPlace asked) const
{
    BodyValue written = m_input.value(asked.value);
    Parts parts = partsOf(asked);
    if (written.isParameter()) {
        if (parts.isEmpty())
            return cutByRelationsInFront(asked, typeOfParameter(written));
        std::optional<BodyEdgeResult> filled = testThatFills(asked.value);
        return cutByRelationsInFront(asked, filled ? typeBehindTest(asked.value, *filled, parts) : typeWhereWaysMeet(asked.value, parts));
    }
    Opcode opcode = written.opcode();
    if (opcode == Opcode::Convert)
        return typeBehindConvert(written, asked.place);
    if (parts.isEmpty())
        return cutByRelationsInFront(asked, typeWrittenOf(asked.value));

    TypeAsWritten made;
    Vector<TypeID, 4> typesOfParts;
    for (ValueAtPlace part : parts) {
        TypeAsWritten typeOfPart = known(part);
        if (!typeOfPart.type)
            return asWalkHasIt(written.id());
        made.restsOnWrittenType |= typeOfPart.restsOnWrittenType;
        made.leavesOutWhoCalls |= typeOfPart.leavesOutWhoCalls;
        typesOfParts.append(typeOfPart.type);
    }
    if (!made.restsOnWrittenType)
        return asWalkHasIt(written.id());

    if (BuiltinID builtin = builtinBoundedByArguments(written)) {
        BuiltinOperandTypes passed;
        passed.arguments.appendVector(typesOfParts);
        made.type = resultTypeOfReachableOverloads(BuiltinTypeContext { m_walker.program(), m_walker.types() }, builtin, BuiltinCallMode::Call, passed);
    } else if (typesOfParts.size() == numberOfNamedOperandsOf(opcode))
        made.type = resultTypeOfOperator(typingForInt32Promises(m_walker), opcode, typesOfParts.span());
    // `const low = a & 255; return low !== 0 ? -low : 0;`
    return made.type ? cutByRelationsInFront(asked, made) : asWalkHasIt(written.id());
}

TypeAsWritten TypesAsWritten::of(ValueID root)
{
    // An operation stands behind its operands, so this ends. How deep an expression is written is no depth of the stack.
    // Nor does a way that meets another lead back to itself: valuesThatMeetIn() follows no turn of a loop.
    Vector<ValueAtPlace, 16> pending { { root, m_place } };
    while (!pending.isEmpty()) {
        ValueAtPlace asked = pending.last();
        if (m_known.contains(keyOf(asked))) {
            pending.removeLast();
            continue;
        }
        size_t numberPending = pending.size();
        for (ValueAtPlace part : partsOf(asked)) {
            if (!m_known.contains(keyOf(part)))
                pending.append(part);
        }
        for (ValueAtPlace otherSide : otherSidesOfRelationsInFront(asked)) {
            if (!m_known.contains(keyOf(otherSide)))
                pending.append(otherSide);
        }
        if (pending.size() != numberPending)
            continue;
        m_known.add(keyOf(asked), compute(asked));
        pending.removeLast();
    }
    return known({ root, m_place });
}

// It proves nothing: it only finds the words for what the types have decided.
Int32ProofFailure TypesAsWritten::failureNamedByProducerOf(ValueID value) const
{
    auto typeOf = [&](ValueID part) {
        TypeID type = known({ part, m_place }).type;
        return type ? m_walker.types().at(type) : Type();
    };
    while (true) {
        BodyValue written = m_input.value(value);
        if (written.isParameter())
            break;
        Opcode opcode = written.opcode();
        if (std::optional<Int32ProofFailure> failure = int32FailureSpecificTo(opcode))
            return *failure;
        if (opcode == Opcode::Double)
            return whyConstantBreaksInt32Promise(written.as<Opcode::Double>().number());
        if (opcode != Opcode::Convert && !opcodesFollowedForInt32Failure.contains(opcode))
            break;

        Parts parts = partsOf({ value, m_place });
        auto found = std::ranges::find_if(parts, [&](ValueAtPlace part) {
            return typeOf(part.value).primitives & Primitive::Double;
        });
        if (found == parts.end()) {
            if (opcode == Opcode::Negate && parts.size() == 1 && typeOf(parts[0].value).isInteger() && typeOf(parts[0].value).integers.contains(0))
                return Int32ProofFailure::NegatedZero;
            break;
        }
        value = found->value;
    }
    return typeOf(value).isAnything() ? Int32ProofFailure::Untyped : Int32ProofFailure::AnyNumber;
}

// The list of a spread argument enters the position in place of elements that nobody has looked at: a type without numbers proves nothing unless a value of it can lie there.
static bool mayLieInPositionOfInt32Promise(Walker& walker, TypeID judged, const DeclaredType& position)
{
    if (walker.types().at(judged).isInt32OrNullish())
        return true;
    TypeID target = targetOfConvert(typingForInt32Promises(walker), position);
    return !target || walker.types().overlap(judged, target);
}

void checkInt32Promise(Walker& walker, const DeclaredType& position, ValueID entering, Origin origin)
{
    if (!position.has(PositionFlags::IsInt32Promise) || !entering)
        return;
    Reference value = walker.referenceOf(entering);
    TypeID walked = value ? walker.typeOf(value) : TypeID();
    if (!walked)
        return;

    auto keepsPromise = [&](TypeID type) {
        return numbersKeepInt32Promise(walker.types().at(type)) && mayLieInPositionOfInt32Promise(walker, type, position);
    };
    // Where the promise is made, not where the value is: `for (let i = 0; i < point[key]; i++) take(i)`: `i` is a parameter of the head of the loop, and the relation stands on the way from there into the body.
    TypesAsWritten typesAsWritten(walker, walker.inputBlockBeingWalked());
    TypeAsWritten asWritten = typesAsWritten.of(entering);
    bool isJudgedAsWritten = asWritten.type && asWritten.restsOnWrittenType;
    // Of what is written the walk may make more than the rules do one by one: a read that it has seen tested.
    if (keepsPromise(walked) && (!isJudgedAsWritten || !typesAsWritten.walkKnowsWhoCalls()))
        return;
    if (isJudgedAsWritten && keepsPromise(asWritten.type))
        return;

    TypeID judgedID = isJudgedAsWritten ? asWritten.type : walked;
    Type judged = walker.types().at(judgedID);
    const BodyIndex& input = walker.currentBodyIndex();
    ValueID producer = input.valueBehindCasts(entering);
    bool isConstant = producer && !input.isParameter(producer) && primitiveConstants.contains(input.opcodeOf(producer));
    bool hasOnlyIntegersAmongNumbers = (judged.primitives & Primitive::Number) == Primitive::Integer;
    bool isOneNumber = (hasOnlyIntegersAmongNumbers && judged.integers.isSingleValue()) || (isConstant && judged.isNumber());

    UserDiagnostic diagnostic {
        // Whoever builds the program cannot change what a dependency wrote.
        .kind = position.has(PositionFlags::IsDeclaredByDependency) ? UserDiagnosticKind::Int32OfDependencyNotProven : isOneNumber ? UserDiagnosticKind::ConstantNotInt32 : UserDiagnosticKind::UnprovenInt32,
        .function = walker.currentFunction(),
        .location = walker.code().locationOf(walker.program(), origin),
    };
    if (position.site) {
        const TypeSite& site = walker.program().at(position.site);
        diagnostic.origin = { site.module.index(), site.position };
        diagnostic.siteKind = site.kind;
    }
    // The warning words neither a reason nor a range.
    if (diagnostic.kind != UserDiagnosticKind::Int32OfDependencyNotProven) {
        // Unknown: no number is among what may come, and none of it can lie in the position.
        Int32ProofFailure failure = Int32ProofFailure::Unknown;
        if (!numbersKeepInt32Promise(judged))
            failure = judged.primitives & Primitive::Double ? typesAsWritten.failureNamedByProducerOf(entering) : Int32ProofFailure::OutOfRange;
        diagnostic.count = static_cast<uint32_t>(failure);
        if (hasOnlyIntegersAmongNumbers) {
            diagnostic.minimum = static_cast<double>(judged.integers.minimum);
            diagnostic.maximum = static_cast<double>(judged.integers.maximum);
        }
    }
    walker.report(diagnostic);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
