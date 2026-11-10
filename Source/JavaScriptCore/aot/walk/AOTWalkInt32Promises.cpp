/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

static constexpr OpcodeSet opcodesFollowedForInt32Failure = valuePreservingOpcodes | allSums | OpcodeSet { Opcode::ToNumber, Opcode::ToNumeric, Opcode::Negate };
static constexpr OpcodeSet operatorsJudgedByWrittenTypes = allNumericOperators;

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
    TypeID type;
    bool restsOnWrittenType { false };
    bool leavesOutWhoCalls { false };
    bool standsBehindBrokenPromise { false };
    bool isBeingAsked { false };
    bool restsOnCastAlone { false };
    bool countsInItsAskingAlone { false };
    uint32_t asking { 0 };
};

struct ValueAtPlace {
    ValueID value;
    BlockID place;
};

} // anonymous namespace

struct Walker::KeptForInt32Promises {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(KeptForInt32Promises);

    struct WithBody {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WithBody);

        UncheckedKeyHashMap<uint64_t, TypeAsWritten, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> known;
        Vector<BlockID> placesBehindLastRelation;
    };

    WithBody& with(const BodyIndex& index)
    {
        std::unique_ptr<WithBody>& kept = withBodies.add(&index, nullptr).iterator->value;
        if (!kept)
            kept = makeUnique<WithBody>();
        return *kept;
    }

    UncheckedKeyHashMap<const BodyIndex*, std::unique_ptr<WithBody>> withBodies;
    uint32_t numberOfAskings { 0 };
};

void Walker::DeleterOfKeptForInt32Promises::operator()(KeptForInt32Promises* kept) const
{
    delete kept;
}

Walker::KeptForInt32Promises& Walker::keptForInt32Promises()
{
    if (!m_keptForInt32Promises)
        m_keptForInt32Promises.reset(new KeptForInt32Promises);
    return *m_keptForInt32Promises;
}

namespace {

class TypesAsWritten {
    WTF_MAKE_NONCOPYABLE(TypesAsWritten);
public:
    TypesAsWritten(Walker& walker, BlockID place)
        : m_walker(walker)
        , m_input(walker.currentBodyIndex())
        , m_place(place)
        , m_kept(walker.keptForInt32Promises().with(m_input))
        , m_asking(++walker.keptForInt32Promises().numberOfAskings)
    {
    }

    TypeAsWritten of(ValueID);
    Int32ProofFailure failureNamedByProducerOf(ValueID) const;
    bool walkKnowsWhoCalls() const;

private:
    using Parts = Vector<ValueAtPlace, 4>;

    BlockID placeBehindLastRelation(BlockID) const;
    uint64_t keyOf(ValueAtPlace) const;
    bool counts(const TypeAsWritten& kept) const { return !kept.countsInItsAskingAlone || kept.asking == m_asking; }
    bool isInTable(ValueAtPlace) const;
    TypeAsWritten known(ValueAtPlace) const;
    TypeAsWritten asWalkHasIt(ValueID) const;
    TypeAsWritten typeWrittenOf(ValueID) const;
    bool restsOnWordThatIsOnlyExpected(ValueID) const;
    TypeID typeStatedByCastsOf(ValueID) const;
    void forEachRelationInFront(ValueAtPlace, const BodyIncomingEdge* comesRoundOn, const Invocable<void(const BodyEdgeResult&, BlockID)> auto&) const;
    Parts otherSidesOfRelationsInFront(ValueAtPlace, const BodyIncomingEdge* comesRoundOn = nullptr) const;
    TypeAsWritten cutByRelationsInFront(ValueAtPlace, TypeAsWritten, const BodyIncomingEdge* comesRoundOn = nullptr) const;
    BuiltinID builtinWrittenAsCallee(ValueID) const;
    BuiltinID builtinBoundedByArguments(BodyValue) const;
    BuiltinID methodCalledOnWrittenText(BodyValue) const;
    Parts partsOf(ValueAtPlace) const;
    Parts valuesThatMeetIn(ValueID parameter) const;
    Parts valuesThatEnterLoopAt(ValueID parameter) const;
    Parts partsOfParameterOfHead(ValueID parameter) const;
    std::optional<BodyEdgeResult> testThatFills(ValueID parameter) const;
    TypeAsWritten typeOfParameter(BodyValue) const;
    TypeAsWritten typeWhereWaysMeet(ValueID parameter, const Parts&) const;
    TypeAsWritten typeAtHeadOfLoop(BodyValue parameter, const Parts& entering) const;
    TypeAsWritten typeBehindTest(ValueID parameter, const BodyEdgeResult&, const Parts& operandsOfTest) const;
    TypeAsWritten typeBehindConvert(BodyValue, BlockID place) const;
    TypeAsWritten typeBehindConvertOf(BodyValue, TypeAsWritten entering) const;
    TypeAsWritten compute(ValueAtPlace) const;

    Walker& m_walker;
    const BodyIndex& m_input;
    BlockID m_place;
    Walker::KeptForInt32Promises::WithBody& m_kept;
    uint32_t m_asking;
};

} // anonymous namespace

uint64_t TypesAsWritten::keyOf(ValueAtPlace asked) const
{
    BlockID place = placeBehindLastRelation(asked.place);
    if (BlockID made = m_input.blockOf(asked.value); m_input.dominatorNumber(made) > m_input.dominatorNumber(place))
        place = made;
    return static_cast<uint64_t>(place.index()) << 32 | asked.value.index();
}

bool TypesAsWritten::isInTable(ValueAtPlace asked) const
{
    auto found = m_kept.known.find(keyOf(asked));
    return found != m_kept.known.end() && counts(found->value);
}

TypeAsWritten TypesAsWritten::known(ValueAtPlace asked) const
{
    auto found = m_kept.known.find(keyOf(asked));
    if (found != m_kept.known.end() && counts(found->value) && !found->value.isBeingAsked)
        return found->value;
    TypeAsWritten held;
    if (found != m_kept.known.end() && counts(found->value) && m_input.isParameter(asked.value) && m_input.isLoopHead(m_input.blockOf(asked.value)))
        held = typeOfParameter(m_input.value(asked.value));
    held.countsInItsAskingAlone = true;
    return held;
}

TypeAsWritten TypesAsWritten::asWalkHasIt(ValueID value) const
{
    Reference name = m_walker.referenceOf(value);
    TypeAsWritten walked;
    walked.type = name ? m_walker.typeOf(name) : TypeID();
    walked.countsInItsAskingAlone = true;
    return walked;
}

bool TypesAsWritten::restsOnWordThatIsOnlyExpected(ValueID value) const
{
    const Program& program = m_walker.program();
    while (value) {
        if (m_input.isParameter(value)) {
            value = m_input.valueNarrowedInto(value);
            continue;
        }
        BodyValue written = m_input.value(value);
        Opcode opcode = written.opcode();
        if (opcode == Opcode::Convert) {
            DeclaredType declared = written.as<Opcode::Convert>().declared();
            if (declared.written() && !declared.isExpected())
                return false;
            value = written.operand(0);
        } else if (opcode == Opcode::GetModuleVariable)
            return program.at(written.variable()).declared.isExpected();
        else if (opcode == Opcode::GetEnvironmentField) {
            const Layout& layout = program.at(written.layout());
            return written.field() < layout.fields.size() && layout.fields[written.field()].declared.isExpected();
        } else if (opcode == Opcode::ElemGet)
            value = written.objectOperand();
        else if (opcode == Opcode::ResultAt) {
            ValueID step = written.as<Opcode::ResultAt>().resultsOperand();
            if (m_input.isParameter(step) || m_input.opcodeOf(step) != Opcode::IteratorResume)
                return false;
            ValueID open = m_input.value(step).as<Opcode::IteratorResume>().iteratorOperand();
            if (m_input.isParameter(open) || m_input.opcodeOf(open) != Opcode::IteratorOpen)
                return false;
            value = m_input.value(open).as<Opcode::IteratorOpen>().iterableOperand();
        } else if (opcode == Opcode::CheckInitialized || opcode == Opcode::NarrowByTest || valuePreservingOpcodes.contains(opcode))
            value = written.operand(0);
        else
            return false;
    }
    return false;
}

TypeAsWritten TypesAsWritten::typeWrittenOf(ValueID value) const
{
    if (m_input.isParameter(value))
        return typeOfParameter(m_input.value(value));
    if (primitiveConstants.contains(m_input.opcodeOf(value)))
        return { typeOfConstant(typingForInt32Promises(m_walker), m_input.value(value)) };
    ExpectationOfValue expectation = findExpectation(m_walker, value);
    Walker::RememberedExpectation remembered = m_walker.expectationRememberedOf(value);
    if (expectation.type && expectation.isDeclared && !(remembered.isKnown && remembered.isHeld)) {
        TypeAsWritten walked = asWalkHasIt(value);
        bool proofKeepsWhatWordDoesNot = walked.type && writtenTypeKeepsInt32Promise(m_walker, walked.type) && !writtenTypeKeepsInt32Promise(m_walker, expectation.type);
        if (proofKeepsWhatWordDoesNot && restsOnWordThatIsOnlyExpected(value))
            return walked;
        return { .type = expectation.type, .restsOnWrittenType = true, .restsOnCastAlone = true, .countsInItsAskingAlone = true };
    }
    return asWalkHasIt(value);
}

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

BlockID TypesAsWritten::placeBehindLastRelation(BlockID place) const
{
    Vector<BlockID>& places = m_kept.placesBehindLastRelation;
    if (places.isEmpty())
        places.fill(BlockID(), m_input.numberOfBlocks());
    Vector<BlockID, 16> climbed;
    BlockID found;
    for (BlockID block = place; !found; block = m_input.immediateDominator(block)) {
        found = places[block.index()];
        if (found)
            break;
        climbed.append(block);
        std::optional<BodyIncomingEdge> edge = m_input.onlyIncomingEdge(block);
        if (!m_input.immediateDominator(block) || (edge && (boundsProvenOnEdge(m_input, m_input.terminator(edge->from), edge->targetIndex) || sidesOfEqualityOnEdgeForInt32Promises(m_input, m_input.terminator(edge->from), edge->targetIndex))))
            found = block;
    }
    for (BlockID block : climbed)
        places[block.index()] = found;
    return found;
}

void TypesAsWritten::forEachRelationInFront(ValueAtPlace asked, const BodyIncomingEdge* comesRoundOn, const Invocable<void(const BodyEdgeResult&, BlockID)> auto& functor) const
{
    auto lookAtEdge = [&](BlockID block, uint32_t targetIndex) {
        std::optional<std::array<BodyEdgeResult, 2>> bounds = boundsProvenOnEdge(m_input, m_input.terminator(block), targetIndex);
        if (!bounds)
            bounds = sidesOfEqualityOnEdgeForInt32Promises(m_input, m_input.terminator(block), targetIndex);
        if (!bounds)
            return;
        for (const BodyEdgeResult& bound : *bounds) {
            if (m_input.stripCasts(bound.subject) == asked.value)
                functor(bound, block);
        }
    };
    if (comesRoundOn)
        lookAtEdge(comesRoundOn->from, comesRoundOn->targetIndex);
    BlockID made = m_input.blockOf(asked.value);
    for (BlockID below = asked.place, block = m_input.immediateDominator(below); below != made && block; below = block, block = m_input.immediateDominator(below)) {
        if (std::optional<BodyIncomingEdge> edge = m_input.onlyIncomingEdge(below))
            lookAtEdge(block, edge->targetIndex);
    }
}

TypesAsWritten::Parts TypesAsWritten::otherSidesOfRelationsInFront(ValueAtPlace asked, const BodyIncomingEdge* comesRoundOn) const
{
    Parts otherSides;
    forEachRelationInFront(asked, comesRoundOn, [&](const BodyEdgeResult& bound, BlockID block) {
        if (ValueID otherSide = m_input.value(bound.test).operand(1 - bound.operandIndex))
            otherSides.append({ otherSide, block });
    });
    return otherSides;
}

TypeAsWritten TypesAsWritten::cutByRelationsInFront(ValueAtPlace asked, TypeAsWritten written, const BodyIncomingEdge* comesRoundOn) const
{
    if (!written.type)
        return written;
    TypingContext typing = typingForInt32Promises(m_walker);
    forEachRelationInFront(asked, comesRoundOn, [&](const BodyEdgeResult& bound, BlockID block) {
        BodyValue relation = m_input.value(bound.test);
        ValueID otherSide = relation.operand(1 - bound.operandIndex);
        bool hasFailed = !bound.isOnTakenEdge;
        TypeAsWritten judged = known({ otherSide, block });
        written.countsInItsAskingAlone |= judged.countsInItsAskingAlone;
        TypeID other = judged.restsOnWrittenType && !(hasFailed && judged.restsOnCastAlone) ? judged.type : TypeID();
        if (!other && !hasFailed)
            other = typeStatedByCastsOf(otherSide);
        if (ValueID made = m_input.stripCasts(otherSide); !other && made) {
            TypeAsWritten ofOtherSide = typeWrittenOf(made);
            written.countsInItsAskingAlone |= ofOtherSide.countsInItsAskingAlone;
            bool isConstant = !m_input.isParameter(made) && primitiveConstants.contains(m_input.opcodeOf(made));
            if ((!comesRoundOn || ofOtherSide.restsOnWrittenType || isConstant) && !(hasFailed && ofOtherSide.restsOnCastAlone))
                other = ofOtherSide.type;
        }
        if (!other)
            return;
        TypeID cut = narrowByComparison(typing, relation.opcode(), !bound.operandIndex, written.type, other, bound.isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken);
        if (!cut || isNeverType(cut) || cut == written.type)
            return;
        written.type = cut;
        written.restsOnWrittenType = true;
    });
    return written;
}

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
        return overload.mode == BuiltinCallMode::Call && !overload.receiver;
    });
    return isBounded ? builtin : BuiltinID();
}

BuiltinID TypesAsWritten::methodCalledOnWrittenText(BodyValue written) const
{
    if (written.opcode() != Opcode::Call)
        return { };
    auto call = written.as<Opcode::Call>();
    ValueID text = call.thisValueOperand() ? m_input.stripCasts(call.thisValueOperand()) : ValueID();
    ValueID read = call.calleeOperand() ? m_input.stripCasts(call.calleeOperand()) : ValueID();
    if (!text || !read || m_input.isParameter(read) || m_input.opcodeOf(read) != Opcode::PropGet || std::ranges::contains(call.argumentsOperands(), ValueID()))
        return { };
    BodyValue readOfMethod = m_input.value(read);
    if (readOfMethod.isFreeName() || m_input.stripCasts(readOfMethod.objectOperand()) != text)
        return { };
    TypeAsWritten ofText = typeWrittenOf(text);
    bool isWritten = ofText.restsOnWrittenType || (!m_input.isParameter(text) && primitiveConstants.contains(m_input.opcodeOf(text)));
    if (!isWritten || !ofText.type || isNeverType(ofText.type) || !m_walker.types().at(ofText.type).hasOnlyPrimitives(Primitive::AnyString))
        return { };
    BuiltinID method = findConstantMemberReadOff(std::as_const(m_walker), ofText.type, readOfMethod.key());
    return method ? m_walker.program().builtins().canonicalID(method) : BuiltinID();
}

TypesAsWritten::Parts TypesAsWritten::partsOf(ValueAtPlace asked) const
{
    BodyValue written = m_input.value(asked.value);
    Parts parts;
    if (written.isParameter()) {
        std::optional<BodyEdgeResult> filled = testThatFills(asked.value);
        if (!filled)
            return m_input.isLoopHead(m_input.blockOf(asked.value)) ? partsOfParameterOfHead(asked.value) : valuesThatMeetIn(asked.value);
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
    } else if (methodCalledOnWrittenText(written)) {
        parts.append({ written.as<Opcode::Call>().thisValueOperand(), asked.place });
        for (ValueID argument : written.as<Opcode::Call>().argumentsOperands())
            parts.append({ argument, asked.place });
    }
    return parts;
}

TypesAsWritten::Parts TypesAsWritten::valuesThatMeetIn(ValueID parameter) const
{
    BlockID block = m_input.blockOf(parameter);
    BlockID above = m_input.immediateDominator(block);
    uint32_t numberOfEdges = m_input.numberOfIncomingEdges(block);
    if (!above || m_input.isLoopHead(block) || m_input.isEnteredByHandlerEdge(block) || m_input.hasIrreducibleFlow() || numberOfEdges == BodyIndex::maxCountedIncomingEdges)
        return { };
    uint32_t index = *m_input.block(block).indexOfParameter(parameter);

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

TypesAsWritten::Parts TypesAsWritten::valuesThatEnterLoopAt(ValueID parameter) const
{
    BlockID head = m_input.blockOf(parameter);
    if (m_input.isEnteredByHandlerEdge(head) || m_input.hasIrreducibleFlow() || !writtenTypeKeepsInt32Promise(m_walker, m_input.value(parameter).parameterType()))
        return { };
    uint32_t index = *m_input.block(head).indexOfParameter(parameter);
    Parts entering;
    for (const BodyIncomingEdge& edge : m_walker.incomingEdgesOf(m_input, head)) {
        if (edge.from >= head)
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        if (index >= arguments.size() || !arguments[index])
            return { };
        entering.append({ arguments[index], edge.from });
    }
    return entering;
}

TypesAsWritten::Parts TypesAsWritten::partsOfParameterOfHead(ValueID parameter) const
{
    Parts parts = valuesThatEnterLoopAt(parameter);
    if (parts.isEmpty())
        return parts;
    BlockID head = m_input.blockOf(parameter);
    uint32_t index = *m_input.block(head).indexOfParameter(parameter);
    for (const BodyIncomingEdge& edge : m_walker.incomingEdgesOf(m_input, head)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        if (edge.from >= head && index < arguments.size() && arguments[index])
            parts.appendVector(otherSidesOfRelationsInFront({ m_input.stripCasts(arguments[index]), edge.from }, &edge));
    }
    return parts;
}

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

TypeAsWritten TypesAsWritten::typeWhereWaysMeet(ValueID parameter, const Parts& arriving) const
{
    TypeAsWritten met;
    for (ValueAtPlace part : arriving) {
        TypeAsWritten typeOfPart = known(part);
        if (!typeOfPart.type)
            return asWalkHasIt(parameter);
        met.type = met.type ? m_walker.types().unionOf(met.type, typeOfPart.type) : typeOfPart.type;
        met.restsOnWrittenType |= typeOfPart.restsOnWrittenType;
        met.countsInItsAskingAlone |= typeOfPart.countsInItsAskingAlone;
        met.leavesOutWhoCalls |= typeOfPart.leavesOutWhoCalls;
        met.standsBehindBrokenPromise |= typeOfPart.standsBehindBrokenPromise;
        met.restsOnCastAlone |= typeOfPart.restsOnCastAlone;
    }
    return met.restsOnWrittenType ? met : asWalkHasIt(parameter);
}

TypeAsWritten TypesAsWritten::typeAtHeadOfLoop(BodyValue parameter, const Parts& entering) const
{
    TypeAsWritten held = typeOfParameter(parameter);
    if (!held.restsOnWrittenType)
        return held;
    TypeArena& types = m_walker.types();
    TypingContext typing = typingForInt32Promises(m_walker);
    auto isConstant = [&](ValueID value) {
        return value && !m_input.isParameter(value) && primitiveConstants.contains(m_input.opcodeOf(value));
    };
    TypeAsWritten met = held;
    met.type = { };
    for (ValueAtPlace part : entering) {
        ValueID made = m_input.stripCasts(part.value);
        TypeAsWritten typeOfPart = isConstant(made) ? TypeAsWritten { typeOfConstant(typing, m_input.value(made)), true } : known(part);
        TypeID both = typeOfPart.type && typeOfPart.restsOnWrittenType ? types.intersectionOf(typeOfPart.type, held.type) : TypeID();
        held.countsInItsAskingAlone |= typeOfPart.countsInItsAskingAlone;
        met.countsInItsAskingAlone |= typeOfPart.countsInItsAskingAlone;
        if (!both || isNeverType(both))
            return held;
        met.type = met.type ? types.unionOf(met.type, both) : both;
        met.leavesOutWhoCalls |= typeOfPart.leavesOutWhoCalls;
        met.standsBehindBrokenPromise |= typeOfPart.standsBehindBrokenPromise;
    }
    BlockID head = m_input.blockOf(parameter.id());
    uint32_t index = *m_input.block(head).indexOfParameter(parameter.id());
    for (const BodyIncomingEdge& edge : m_walker.incomingEdgesOf(m_input, head)) {
        if (edge.from < head)
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID loopCarriedValue = index < arguments.size() ? m_input.stripCasts(arguments[index]) : ValueID();
        TypeAsWritten cut = loopCarriedValue ? cutByRelationsInFront({ loopCarriedValue, edge.from }, held, &edge) : held;
        TypeID round = cut.type;
        held.countsInItsAskingAlone |= cut.countsInItsAskingAlone;
        met.countsInItsAskingAlone |= cut.countsInItsAskingAlone;
        if (round == held.type)
            return held;
        met.type = met.type ? types.unionOf(met.type, round) : round;
    }
    return met.type ? met : held;
}

TypeAsWritten TypesAsWritten::typeBehindTest(ValueID parameter, const BodyEdgeResult& filled, const Parts& operandsOfTest) const
{
    Vector<TypeID, 2> types;
    bool countsInItsAskingAlone = false;
    for (ValueAtPlace operand : operandsOfTest) {
        types.append(known(operand).type);
        countsInItsAskingAlone |= known(operand).countsInItsAskingAlone;
    }
    TypeAsWritten tested = filled.operandIndex < operandsOfTest.size() ? known(operandsOfTest[filled.operandIndex]) : TypeAsWritten();
    tested.countsInItsAskingAlone |= countsInItsAskingAlone;
    if (!tested.restsOnWrittenType || types.contains(TypeID()))
        return asWalkHasIt(parameter);
    tested.type = typeOfEdgeResult(typingForInt32Promises(m_walker), m_input.value(filled.test), OperandTypes { types.span() }, filled.operandIndex, filled.isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken);
    return tested.type && !isNeverType(tested.type) ? tested : asWalkHasIt(parameter);
}

TypeAsWritten TypesAsWritten::typeBehindConvert(BodyValue convert, BlockID place) const
{
    TypeAsWritten behind = typeBehindConvertOf(convert, known({ convert.operand(0), place }));
    behind.countsInItsAskingAlone |= known({ convert.operand(0), place }).countsInItsAskingAlone;
    return behind;
}

TypeAsWritten TypesAsWritten::typeBehindConvertOf(BodyValue convert, TypeAsWritten entering) const
{
    DeclaredType declared = convert.as<Opcode::Convert>().declared();
    TypeID stated = targetOfConvert(typingForInt32Promises(m_walker), declared);
    bool statesNothing = !stated || declared.has(PositionFlags::IsInt32Promise) || declared.isExpected();
    if (!statesNothing && writtenTypeKeepsInt32Promise(m_walker, stated)) {
        TypeID both = entering.type && entering.restsOnWrittenType ? m_walker.types().intersectionOf(entering.type, stated) : TypeID();
        if (both && !isNeverType(both)) {
            entering.type = both;
            return entering;
        }
        ValueID made = m_input.stripCasts(convert.id());
        return { .type = stated, .restsOnWrittenType = true, .restsOnCastAlone = !made || m_input.isParameter(made) || !primitiveConstants.contains(m_input.opcodeOf(made)) };
    }
    bool isPromise = declared.has(PositionFlags::IsInt32Promise) && writtenTypeKeepsInt32Promise(m_walker, stated);
    if (isPromise && (!entering.type || !entering.restsOnWrittenType))
        return { stated, true };
    if (!entering.type || !entering.restsOnWrittenType)
        return asWalkHasIt(convert.id());
    if (isPromise && !numbersKeepInt32Promise(m_walker.types().at(entering.type)))
        return { stated, true, entering.leavesOutWhoCalls, true };
    if (isPromise)
        entering.restsOnCastAlone = false;
    if (statesNothing) {
        if (declared.isExpected() && !writtenTypeKeepsInt32Promise(m_walker, entering.type)) {
            TypeAsWritten walked = asWalkHasIt(convert.id());
            if (walked.type && writtenTypeKeepsInt32Promise(m_walker, walked.type) && restsOnWordThatIsOnlyExpected(convert.id()))
                return walked;
        }
        return entering;
    }
    TypeID both = m_walker.types().intersectionOf(entering.type, stated);
    entering.type = isNeverType(both) ? stated : both;
    return entering;
}

TypeAsWritten TypesAsWritten::compute(ValueAtPlace asked) const
{
    BodyValue written = m_input.value(asked.value);
    Parts parts = partsOf(asked);
    if (written.isParameter()) {
        if (parts.isEmpty())
            return cutByRelationsInFront(asked, typeOfParameter(written));
        std::optional<BodyEdgeResult> filled = testThatFills(asked.value);
        if (!filled && m_input.isLoopHead(m_input.blockOf(asked.value)))
            return cutByRelationsInFront(asked, typeAtHeadOfLoop(written, valuesThatEnterLoopAt(asked.value)));
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
        made.countsInItsAskingAlone |= typeOfPart.countsInItsAskingAlone;
        made.leavesOutWhoCalls |= typeOfPart.leavesOutWhoCalls;
        made.standsBehindBrokenPromise |= typeOfPart.standsBehindBrokenPromise;
        made.restsOnCastAlone |= typeOfPart.restsOnCastAlone;
        typesOfParts.append(typeOfPart.type);
    }
    if (!made.restsOnWrittenType)
        return asWalkHasIt(written.id());

    if (BuiltinID builtin = builtinBoundedByArguments(written)) {
        BuiltinOperandTypes passed;
        passed.arguments.appendVector(typesOfParts);
        made.type = resultTypeOfReachableOverloads(BuiltinTypeContext { m_walker.program(), m_walker.types() }, builtin, BuiltinCallMode::Call, passed);
    } else if (BuiltinID method = methodCalledOnWrittenText(written)) {
        BuiltinOperandTypes passed;
        passed.receiver = typesOfParts[0];
        passed.arguments.append(typesOfParts.subspan(1));
        made.type = resultTypeOfReachableOverloads(BuiltinTypeContext { m_walker.program(), m_walker.types() }, method, BuiltinCallMode::Call, passed);
    } else if (typesOfParts.size() == numberOfNamedOperandsOf(opcode))
        made.type = resultTypeOfOperator(typingForInt32Promises(m_walker), opcode, typesOfParts.span());
    return made.type ? cutByRelationsInFront(asked, made) : asWalkHasIt(written.id());
}

TypeAsWritten TypesAsWritten::of(ValueID root)
{
    Vector<ValueAtPlace, 16> pending { { root, m_place } };
    while (!pending.isEmpty()) {
        ValueAtPlace asked = pending.last();
        uint64_t key = keyOf(asked);
        auto entry = m_kept.known.add(key, TypeAsWritten());
        bool isFirstVisit = entry.isNewEntry || !counts(entry.iterator->value);
        if (!isFirstVisit && !entry.iterator->value.isBeingAsked) {
            pending.removeLast();
            continue;
        }
        if (isFirstVisit) {
            entry.iterator->value = { .isBeingAsked = true, .countsInItsAskingAlone = true, .asking = m_asking };
            size_t numberPending = pending.size();
            for (ValueAtPlace part : partsOf(asked)) {
                if (!isInTable(part))
                    pending.append(part);
            }
            for (ValueAtPlace otherSide : otherSidesOfRelationsInFront(asked)) {
                if (!isInTable(otherSide))
                    pending.append(otherSide);
            }
            if (pending.size() != numberPending)
                continue;
        }
        TypeAsWritten computed = compute(asked);
        computed.asking = m_asking;
        m_kept.known.set(key, computed);
        pending.removeLast();
    }
    return known({ root, m_place });
}

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

static bool mayLieInPositionOfInt32Promise(Walker& walker, TypeID judged, const DeclaredType& position)
{
    if (walker.types().at(judged).isInt32OrNullish())
        return true;
    TypeID target = targetOfConvert(typingForInt32Promises(walker), position);
    return !target || walker.types().overlap(judged, target);
}

void checkInt32Promise(Walker& walker, const DeclaredType& position, ValueID entering, Origin origin)
{
    if (!position.has(PositionFlags::IsInt32Promise) || !entering || !walker.judgesInt32Promises())
        return;
    Reference value = walker.referenceOf(entering);
    TypeID walked = value ? walker.typeOf(value) : TypeID();
    if (!walked)
        return;

    auto keepsPromise = [&](TypeID type) {
        return numbersKeepInt32Promise(walker.types().at(type)) && mayLieInPositionOfInt32Promise(walker, type, position);
    };
    TypesAsWritten typesAsWritten(walker, walker.inputBlockBeingWalked());
    TypeAsWritten asWritten = typesAsWritten.of(entering);
    bool isJudgedAsWritten = asWritten.type && asWritten.restsOnWrittenType;
    if (keepsPromise(walked) && (!isJudgedAsWritten || !(typesAsWritten.walkKnowsWhoCalls() || asWritten.standsBehindBrokenPromise)))
        return;
    if (isJudgedAsWritten && keepsPromise(asWritten.type))
        return;

    TypeID judgedID = isJudgedAsWritten ? asWritten.type : walked;
    Type judged = walker.types().at(judgedID);
    const BodyIndex& input = walker.currentBodyIndex();
    ValueID producer = input.stripCasts(entering);
    bool isConstant = producer && !input.isParameter(producer) && primitiveConstants.contains(input.opcodeOf(producer));
    bool hasOnlyIntegersAmongNumbers = (judged.primitives & Primitive::Number) == Primitive::Integer;
    bool isOneNumber = (hasOnlyIntegersAmongNumbers && judged.integers.isSingleValue()) || (isConstant && judged.isNumber());

    UserDiagnostic diagnostic {
        .kind = position.has(PositionFlags::IsDeclaredByDependency) ? UserDiagnosticKind::UnprovenDependencyInt32 : isOneNumber ? UserDiagnosticKind::ConstantNotInt32 : UserDiagnosticKind::UnprovenInt32,
        .function = walker.currentFunction(),
        .location = walker.code().locationOf(walker.program(), origin),
    };
    if (position.site) {
        const TypeSite& site = walker.program().at(position.site);
        diagnostic.origin = { site.module.index(), site.position };
        diagnostic.siteKind = site.kind;
    }
    if (diagnostic.kind != UserDiagnosticKind::UnprovenDependencyInt32) {
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
