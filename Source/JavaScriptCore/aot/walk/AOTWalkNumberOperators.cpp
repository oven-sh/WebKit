/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkNumberGuesses.h"
#include "JSGlobalObjectFunctions.h"
#include <array>
#include <cmath>


namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet arithmeticInNumberOperators { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow };
static constexpr OpcodeSet opcodesOfNumberOperators = arithmeticInNumberOperators | allBitwiseOperators | allEqualities | allRelations
    | OpcodeSet { Opcode::LogicalNot, Opcode::ToBoolean };

namespace {

struct OperandInNumberOperators {
    ValueID written;
    Reference reference;
    Type type;
    PrimitiveKnownToFolding known;
};

using OperandsInNumberOperators = std::array<OperandInNumberOperators, 2>;

} // anonymous namespace

static bool readOperandsInNumberOperators(Walker& walker, const BodyValue& value, OperandsInNumberOperators& operands)
{
    for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(value.opcode()); ++operandIndex) {
        OperandInNumberOperators& operand = operands[operandIndex];
        operand.written = value.operand(operandIndex);
        operand.reference = operand.written ? walker.referenceOf(operand.written) : Reference();
        TypeID type = operand.reference && operand.reference.isRecord() ? walker.typeOf(operand.reference) : TypeID();
        if (!type || isNeverType(type))
            return false;
        operand.type = walker.types().at(type);
        operand.known = primitiveKnownInNumberForms(walker, operand.reference, operand.type);
    }
    return true;
}

static bool isKnownIntegerInNumberOperators(PrimitiveKnownToFolding known, int64_t integer)
{
    if (known.kind() != PrimitiveKnownToFolding::Kind::Number || known.asNumber() != static_cast<double>(integer))
        return false;
    return integer || !std::signbit(known.asNumber());
}

static bool isNegativeZeroInNumberOperators(PrimitiveKnownToFolding known)
{
    return known.kind() == PrimitiveKnownToFolding::Kind::Number && !known.asNumber() && std::signbit(known.asNumber());
}

static bool bindInNumberOperators(Walker& walker, const BodyValue& value, Reference result)
{
    if (result)
        walker.bind(value.id(), result);
    return true;
}

static bool buildOnOperandsInNumberOperators(Walker& walker, const BodyValue& value, std::span<const Reference> operands)
{
    Draft draft = walker.draftOf(value);
    for (uint32_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
        draft.setOperand(operandIndex, operands[operandIndex]);
    return bindInNumberOperators(walker, value, buildInAllowedFormInNumberForms(walker, WTF::move(draft)));
}

static Origin originInFrontOfNumberOperator(Walker& walker, const BodyValue& value)
{
    Origin origin = walker.originOf(value);
    origin.isTailCall = false;
    return origin;
}

static bool bindWhatIsNegatedTwice(Walker& walker, const BodyValue& negation)
{
    if (!walker.has(Mechanism::Folding))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID inner = index.stripCasts(negation.valueOperand());
    if (inner && !index.isParameter(inner) && index.opcodeOf(inner) == Opcode::ToBoolean)
        inner = index.stripCasts(index.value(inner).valueOperand());
    if (!inner || index.isParameter(inner) || index.opcodeOf(inner) != Opcode::LogicalNot)
        return false;
    Reference negatedTwice = walker.referenceOf(index.value(inner).valueOperand());
    TypeID type = negatedTwice && negatedTwice.isRecord() ? walker.typeOf(negatedTwice) : TypeID();
    if (!type || !walker.types().at(type).isBoolean())
        return false;
    walker.bind(negation.id(), negatedTwice);
    return true;
}

static Reference valueThatArithmeticGives(Walker& walker, Opcode opcode, const OperandsInNumberOperators& operands)
{
    const OperandInNumberOperators& left = operands[0];
    const OperandInNumberOperators& right = operands[1];
    switch (opcode) {
    case Opcode::Add:
        for (uint32_t side = 0; side < 2; ++side) {
            const OperandInNumberOperators& other = operands[1 - side];
            if (isKnownIntegerInNumberOperators(operands[side].known, 0) && other.type.isInteger())
                return other.reference;
            if (isNegativeZeroInNumberOperators(operands[side].known) && other.type.isNumber())
                return other.reference;
        }
        return { };
    case Opcode::Sub:
        if (isKnownIntegerInNumberOperators(right.known, 0) && left.type.isNumber())
            return left.reference;
        if (isNegativeZeroInNumberOperators(right.known) && left.type.isInteger())
            return left.reference;
        return { };
    case Opcode::Mul:
        for (uint32_t side = 0; side < 2; ++side) {
            if (isKnownIntegerInNumberOperators(operands[side].known, 1) && operands[1 - side].type.isNumber())
                return operands[1 - side].reference;
        }
        return { };
    case Opcode::Div:
        return isKnownIntegerInNumberOperators(right.known, 1) && left.type.isNumber() ? left.reference : Reference();
    case Opcode::Pow:
        if (!left.type.isNumber())
            return { };
        if (isKnownIntegerInNumberOperators(right.known, 0) || isNegativeZeroInNumberOperators(right.known))
            return integerInNumberForms(walker, 1);
        return isKnownIntegerInNumberOperators(right.known, 1) ? left.reference : Reference();
    default:
        return { };
    }
}

static Reference buildMiddleOfBounds(Walker& walker, const BodyValue& sum, const OperandsInNumberOperators& operands)
{
    static constexpr Range halfOfInt32Range { -(1ll << 30), (1ll << 30) - 1 };
    const BodyIndex& index = walker.currentBodyIndex();
    auto findWritten = [&](ValueID operand, Opcode opcode) -> ValueID {
        ValueID written = operand ? index.stripCasts(operand) : ValueID();
        return written && !index.isParameter(written) && index.opcodeOf(written) == opcode ? written : ValueID();
    };
    auto staysInHalf = [&](Reference bound) {
        TypeID type = bound && bound.isRecord() ? walker.typeOf(bound) : TypeID();
        return type && walker.types().at(type).isInteger() && !walker.types().at(type).integers.isEmpty() && walker.types().at(type).integers.isSubsetOf(halfOfInt32Range);
    };
    for (uint32_t side = 0; side < 2; ++side) {
        ValueID shift = findWritten(operands[1 - side].written, Opcode::RShift);
        ValueID places = shift ? findWritten(index.value(shift).operand(1), Opcode::Integer) : ValueID();
        ValueID difference = places && index.value(places).as<Opcode::Integer>().integer() == 1 ? findWritten(index.value(shift).operand(0), Opcode::Sub) : ValueID();
        if (!difference)
            continue;
        Reference low = operands[side].reference;
        Reference high = walker.referenceOf(index.value(difference).operand(0));
        if (walker.referenceOf(index.value(difference).operand(1)) != low || !staysInHalf(low) || !staysInHalf(high))
            continue;
        Origin origin = walker.originOf(sum);
        Reference doubled = buildOperatorInNumberForms(walker, Opcode::Add, low, high, origin);
        return doubled ? buildOperatorInNumberForms(walker, Opcode::RShift, doubled, integerInNumberForms(walker, 1), origin) : Reference();
    }
    return { };
}

static Reference buildConversionInNumberOperators(Walker&, Opcode conversion, Reference number, Origin);

static Reference buildOptionalIntegerSum(Walker& walker, Opcode opcode, std::array<Reference, 2> terms, Origin origin)
{
    TypeArena& types = walker.types();
    uint32_t numberOfOptionalTerms = 0;
    for (Reference term : terms) {
        TypeID type = term && term.isRecord() ? walker.typeOf(term) : TypeID();
        if (type && types.isSubset(type, TypeTable::int32))
            continue;
        if (!type || !types.overlap(type, TypeTable::int32) || !types.isSubset(type, types.withPrimitives(TypeTable::int32, Primitive::Undefined)))
            return { };
        ++numberOfOptionalTerms;
    }
    if (!numberOfOptionalTerms)
        return { };

    Vector<BlockID, 2> undefinedBlocks;
    for (Reference& term : terms) {
        if (types.isSubset(walker.typeOf(term), TypeTable::int32))
            continue;
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(term);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Undefined));
        Reference isUndefined = walker.build(WTF::move(test));
        bool isTested = isUndefined && isUndefined.isValue() && !walker.code().isConstant(isUndefined.id()) && !walker.isUnreachable();
        if (!isTested)
            return { };
        BlockID integerBlock = walker.addBlock();
        undefinedBlocks.append(walker.addBlock(BlockFrequency::Unlikely));
        walker.branch(isUndefined, undefinedBlocks.last(), integerBlock);
        walker.continueIn(integerBlock);
        term = buildNarrowedInNumberForms(walker, term, isUndefined, OperandIndex::TypeTest::value, false, origin);
        if (!term)
            return { };
    }
    walker.count(StatisticID::SumsWithUndefinedTruncated);
    BlockID meeting = walker.addBlock();
    Reference added = buildOperatorInNumberForms(walker, opcode, terms[0], terms[1], origin);
    Reference bits = added ? buildConversionInNumberOperators(walker, Opcode::ToInt32, added, origin) : Reference();
    if (!bits)
        return { };
    Vector<WayInNumberForms, 3> ways;
    ways.append(WayInNumberForms { walker.currentBlock(), bits, { } });
    for (BlockID block : undefinedBlocks) {
        walker.continueIn(block);
        ways.append(WayInNumberForms { walker.currentBlock(), integerInNumberForms(walker, 0), { } });
    }
    return meetInNumberForms(walker, meeting, ways.span());
}

namespace {

struct TermOfTruncatedSum {
    Reference value;
    bool isSubtracted { false };
};

} // anonymous namespace

static Reference buildSumOfOptionalIntegers(Walker& walker, Origin origin, ValueID sum)
{
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    static constexpr size_t mostTerms = 4;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    TypeID int32OrUndefined = types.withPrimitives(TypeTable::int32, Primitive::Undefined);
    BlockID block = index.blockOf(sum);
    Vector<TermOfTruncatedSum, mostTerms> terms;
    bool hasOtherTerm = false;
    auto gather = [&](auto& self, ValueID operand, bool isSubtracted, bool mayBeSum) -> void {
        ValueID written = operand;
        bool isUsedOnce = mayBeSum;
        while (written && !index.isParameter(written) && opcodesThatStateTypes.contains(index.opcodeOf(written))) {
            isUsedOnce &= index.hasOneUse(written);
            written = index.value(written).operand(0);
        }
        if (isUsedOnce && written && !index.isParameter(written) && sums.contains(index.opcodeOf(written)) && index.blockOf(written) == block && (written == sum || index.hasOneUse(written)) && terms.size() < mostTerms) {
            self(self, index.value(written).operand(0), isSubtracted, true);
            self(self, index.value(written).operand(1), isSubtracted ^ (index.opcodeOf(written) == Opcode::Sub), true);
            return;
        }
        Reference value = walker.referenceOf(operand);
        TypeID type = value && value.isRecord() ? walker.typeOf(value) : TypeID();
        hasOtherTerm |= !type || terms.size() == mostTerms || !types.overlap(type, TypeTable::int32) || !types.isSubset(type, int32OrUndefined);
        if (!hasOtherTerm)
            terms.append(TermOfTruncatedSum { value, isSubtracted });
    };
    gather(gather, sum, false, true);
    auto mayBeUndefined = [&](const TermOfTruncatedSum& term) {
        return !types.isSubset(walker.typeOf(term.value), TypeTable::int32);
    };
    size_t numberOfOptionalTerms = std::ranges::count_if(terms, mayBeUndefined);
    if (hasOtherTerm || terms.size() < 2 || !numberOfOptionalTerms || (terms.size() == 2 && numberOfOptionalTerms == 1))
        return { };

    Vector<BlockID, mostTerms> undefinedBlocks;
    for (size_t place = 0; place < terms.size(); ++place) {
        if (!mayBeUndefined(terms[place]))
            continue;
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(terms[place].value);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Undefined));
        Reference isUndefined = walker.build(WTF::move(test));
        if (!isUndefined || walker.isUnreachable())
            return { };
        if (!isUndefined.isValue() || walker.code().isConstant(isUndefined.id()))
            continue;
        BlockID integerBlock = walker.addBlock();
        undefinedBlocks.append(walker.addBlock(BlockFrequency::Unlikely));
        walker.branch(isUndefined, undefinedBlocks.last(), integerBlock);
        walker.continueIn(integerBlock);
        Reference before = terms[place].value;
        Reference integer = buildNarrowedInNumberForms(walker, before, isUndefined, OperandIndex::TypeTest::value, false, origin);
        if (!integer)
            return { };
        for (TermOfTruncatedSum& term : terms) {
            if (term.value == before)
                term.value = integer;
        }
    }
    walker.count(StatisticID::SumsWithUndefinedTruncated);
    BlockID meeting = walker.addBlock();
    if (terms[0].isSubtracted) {
        auto added = std::ranges::find_if(terms, [](const TermOfTruncatedSum& term) { return !term.isSubtracted; });
        if (added != terms.end())
            std::swap(terms[0], *added);
    }
    Reference total = terms[0].isSubtracted ? buildOperatorInNumberForms(walker, Opcode::Sub, integerInNumberForms(walker, 0), terms[0].value, origin) : terms[0].value;
    for (size_t place = 1; place < terms.size() && total; ++place)
        total = buildOperatorInNumberForms(walker, terms[place].isSubtracted ? Opcode::Sub : Opcode::Add, total, terms[place].value, origin);
    Reference bits = total ? buildConversionInNumberOperators(walker, Opcode::ToInt32, total, origin) : Reference();
    if (!bits)
        return { };
    Vector<WayInNumberForms, 5> ways;
    ways.append(WayInNumberForms { walker.currentBlock(), bits, { } });
    for (BlockID undefinedBlock : undefinedBlocks) {
        walker.continueIn(undefinedBlock);
        ways.append(WayInNumberForms { walker.currentBlock(), integerInNumberForms(walker, 0), { } });
    }
    return meetInNumberForms(walker, meeting, ways.span());
}

OperatorForm operatorFormFor(Walker&, const Draft&);

namespace {

struct TruncatedSumNode {
    Reference value;
    Opcode opcode { Opcode::Add };
    uint32_t left { UINT32_MAX };
    uint32_t right { UINT32_MAX };

    bool isTerm() const { return left == UINT32_MAX; }
};

using TruncatedSumTree = Vector<TruncatedSumNode, 7>;

} // anonymous namespace

static std::optional<uint32_t> collectTruncatedSum(Walker& walker, Reference sum, unsigned depth, TruncatedSumTree& tree)
{
    static constexpr size_t mostNodes = 7;
    const Code& code = walker.code();
    auto nameOf = [&](ValueID operand) {
        return code.isConstant(operand) ? Reference::constant(operand) : Reference::value(operand);
    };
    while (sum && sum.isValue() && !code.isConstant(sum.id()) && conversionsOfForm.contains(code.at(sum.id()).opcode))
        sum = nameOf(code.at(sum.id()).operand(0));
    if (!sum || !sum.isRecord() || tree.size() == mostNodes)
        return std::nullopt;
    Type type = walker.types().at(walker.typeOf(sum));
    Representation representation = walker.representationOf(sum);
    bool isInteger = type.isInteger() || representation == Representation::Int32 || representation == Representation::Int64 || representation == Representation::OptionalInt64;
    if (isInteger) {
        if (!(type.primitives & Primitive::Integer) || !type.hasOnlyPrimitives(Primitive::Integer | Primitive::Undefined))
            return std::nullopt;
        tree.append(TruncatedSumNode { sum });
        return static_cast<uint32_t>(tree.size() - 1);
    }
    if (!depth || !sum.isValue() || code.isConstant(sum.id()) || representation != Representation::Double)
        return std::nullopt;
    const Operation& record = code.at(sum.id());
    if ((record.opcode != Opcode::Add && record.opcode != Opcode::Sub) || record.form() != OperatorForm::Numbers)
        return std::nullopt;
    Opcode opcode = record.opcode;
    std::array<Reference, 2> operands { nameOf(record.operand(0)), nameOf(record.operand(1)) };
    std::optional<uint32_t> left = collectTruncatedSum(walker, operands[0], depth - 1, tree);
    std::optional<uint32_t> right = left ? collectTruncatedSum(walker, operands[1], depth - 1, tree) : std::nullopt;
    if (!right || tree.size() == mostNodes)
        return std::nullopt;
    tree.append(TruncatedSumNode { { }, opcode, *left, *right });
    return static_cast<uint32_t>(tree.size() - 1);
}

static Reference buildTruncatedSumTree(Walker& walker, Origin origin, Opcode opcode, std::array<Reference, 2> operands)
{
    static constexpr unsigned mostNestedSums = 2;
    TypeArena& types = walker.types();
    TruncatedSumTree tree;
    std::optional<uint32_t> left = collectTruncatedSum(walker, operands[0], mostNestedSums, tree);
    std::optional<uint32_t> right = left ? collectTruncatedSum(walker, operands[1], mostNestedSums, tree) : std::nullopt;
    if (!right)
        return { };
    tree.append(TruncatedSumNode { { }, opcode, *left, *right });
    auto mayBeUndefined = [&](Reference term) {
        return types.at(walker.typeOf(term)).primitives & Primitive::Undefined;
    };
    bool hasOptionalTerm = std::ranges::any_of(tree, [&](const TruncatedSumNode& node) { return node.isTerm() && mayBeUndefined(node.value); });
    if (!hasOptionalTerm)
        return { };

    Vector<BlockID, 4> undefinedBlocks;
    for (uint32_t place = 0; place < tree.size(); ++place) {
        Reference before = tree[place].value;
        if (!tree[place].isTerm() || !mayBeUndefined(before))
            continue;
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(before);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Undefined));
        Reference isUndefined = walker.build(WTF::move(test));
        if (!isUndefined || walker.isUnreachable() || !isUndefined.isValue() || walker.code().isConstant(isUndefined.id()))
            return { };
        BlockID integerBlock = walker.addBlock();
        undefinedBlocks.append(walker.addBlock(BlockFrequency::Unlikely));
        walker.branch(isUndefined, undefinedBlocks.last(), integerBlock);
        walker.continueIn(integerBlock);
        Reference integer = buildNarrowedInNumberForms(walker, before, isUndefined, OperandIndex::TypeTest::value, false, origin);
        if (!integer)
            return { };
        for (TruncatedSumNode& node : tree) {
            if (node.isTerm() && node.value == before)
                node.value = integer;
        }
    }
    walker.count(StatisticID::SumsWithUndefinedTruncated);
    BlockID meeting = walker.addBlock();
    for (TruncatedSumNode& node : tree) {
        if (node.isTerm())
            continue;
        Draft draft(node.opcode, origin);
        draft.setOperand(0, tree[node.left].value);
        draft.setOperand(1, tree[node.right].value);
        walker.chooseConservativeFields(draft);
        draft.setForm(operatorFormFor(walker, draft));
        node.value = walker.buildAgain(WTF::move(draft));
        if (!node.value)
            return { };
    }
    Reference bits = buildConversionInNumberOperators(walker, Opcode::ToInt32, tree.last().value, origin);
    if (!bits)
        return { };
    Vector<WayInNumberForms, 5> ways;
    ways.append(WayInNumberForms { walker.currentBlock(), bits, { } });
    for (BlockID undefinedBlock : undefinedBlocks) {
        walker.continueIn(undefinedBlock);
        ways.append(WayInNumberForms { walker.currentBlock(), integerInNumberForms(walker, 0), { } });
    }
    return meetInNumberForms(walker, meeting, ways.span());
}

static Reference buildSumWithUndefined(Walker& walker, Origin origin, ValueID written)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID sum = index.stripCasts(written);
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::UnboxedNumbers) || index.isParameter(sum) || !sums.contains(index.opcodeOf(sum)))
        return { };
    std::array<Reference, 2> terms { walker.referenceOf(index.value(sum).operand(0)), walker.referenceOf(index.value(sum).operand(1)) };
    auto mayBeUndefined = [&](Reference term) {
        return !term || !term.isRecord() || walker.types().at(walker.typeOf(term)).primitives & Primitive::Undefined;
    };
    if (Reference bits = buildSumOfOptionalIntegers(walker, origin, sum); bits || walker.hasFailed() || walker.isUnreachable())
        return bits;
    Reference bits;
    if (!mayBeUndefined(terms[0]) || !mayBeUndefined(terms[1]))
        bits = buildOptionalIntegerSum(walker, index.opcodeOf(sum), terms, origin);
    if (bits || walker.hasFailed() || walker.isUnreachable())
        return bits;
    return buildTruncatedSumTree(walker, origin, index.opcodeOf(sum), terms);
}

Reference buildTruncatedSum(Walker& walker, Reference sum, Origin origin)
{
    const Code& code = walker.code();
    if (!walker.has(Mechanism::Folding) || !walker.has(Mechanism::UnboxedNumbers) || !sum.isValue() || code.isConstant(sum.id()))
        return { };
    const Operation& record = code.at(sum.id());
    if ((record.opcode != Opcode::Add && record.opcode != Opcode::Sub) || record.representation != Representation::Double || record.form() != OperatorForm::Numbers)
        return { };
    std::array<Reference, 2> terms;
    for (uint32_t side = 0; side < 2; ++side) {
        ValueID term = record.operand(side);
        while (!code.isConstant(term) && conversionsOfForm.contains(code.at(term).opcode))
            term = code.at(term).operand(0);
        bool isInteger = code.isConstant(term) || code.at(term).representation == Representation::Int32 || code.at(term).representation == Representation::Int64 || code.at(term).representation == Representation::OptionalInt64;
        if (!isInteger || (side && term == record.operand(0)))
            return { };
        terms[side] = Reference::value(term);
    }
    if (terms[0] == terms[1])
        return { };
    return buildOptionalIntegerSum(walker, record.opcode, terms, origin);
}

static Reference buildSimplerOperandOfBitOperator(Walker& walker, Origin origin, ValueID written)
{
    if (Reference bits = buildSumWithUndefined(walker, origin, written))
        return bits;
    if (walker.hasFailed() || walker.isUnreachable())
        return { };
    InputsOfTypedOperation inputs(walker, nullptr);
    if (std::optional<DivisionAsWritten> quotient = findQuotientOfIntegers(walker, written, inputs))
        return buildDivisionOfIntegers(walker, origin, quotient->dividend, quotient->divisor, QuotientRounding::ToInt32);
    if (std::optional<WholeRemainder> remainder = findWholeRemainder(walker, written, inputs))
        return buildWholeRemainder(walker, origin, *remainder);
    return { };
}

static bool isIdentityElementOfBitOperator(Opcode opcode, uint32_t operandIndex, int32_t bits)
{
    switch (opcode) {
    case Opcode::BitOr:
    case Opcode::BitXor:
        return !bits;
    case Opcode::BitAnd:
        return bits == -1;
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
        return operandIndex == 1 && !(bits & 31);
    default:
        return false;
    }
}

static Reference stripSumWithZero(const Walker& walker, Reference number)
{
    auto nameOf = [&](ValueID operand) {
        return walker.code().isConstant(operand) ? Reference::constant(operand) : Reference::value(operand);
    };
    while (number.isValue() && !walker.code().isConstant(number.id())) {
        const Operation& sum = walker.code().at(number.id());
        if (sum.opcode == Opcode::Unbox && walker.types().at(walker.typeOf(nameOf(sum.valueOperand()))).isNumber()) {
            number = nameOf(sum.valueOperand());
            continue;
        }
        if (sum.opcode != Opcode::Add || sum.form() != OperatorForm::Numbers)
            break;
        uint32_t term = integerKnownInNumberForms(walker, nameOf(sum.operand(1))) == std::optional<int64_t>(0) ? 0 : 1;
        if (term && integerKnownInNumberForms(walker, nameOf(sum.operand(0))) != std::optional<int64_t>(0))
            break;
        Reference other = nameOf(sum.operand(term));
        if (!walker.types().at(walker.typeOf(other)).isNumber())
            break;
        number = other;
    }
    return number;
}

static Reference buildConversionInNumberOperators(Walker& walker, Opcode conversion, Reference number, Origin origin)
{
    if (conversion == Opcode::ToInt32 && number && number.isValue()) {
        bool isSumWithZero = !walker.code().isConstant(number.id()) && walker.code().at(number.id()).opcode == Opcode::Add;
        if (isSumWithZero)
            number = stripSumWithZero(walker, number);
        if (std::optional<uint64_t> word = findKnown(walker, KnownNote::WordOfNumber, number))
            return Reference::fromBits(static_cast<uint32_t>(*word));
    }
    Draft draft(conversion, origin);
    walker.chooseConservativeFields(draft);
    draft.setValueOperand(number);
    return walker.build(WTF::move(draft));
}

static Reference buildBitsOfNumberInNumberOperators(Walker& walker, Opcode conversion, Reference number, Origin origin)
{
    Type type = walker.types().at(walker.typeOf(number));
    Range leftAsTheyAre = conversion == Opcode::ToUint32 ? Range::uint32() : Range::int32();
    if (type.isInteger() && type.integers.isSubsetOf(leftAsTheyAre) && walker.has(Mechanism::Folding)) {
        walker.checkRawNumberThatFoldRestsOn(number, origin);
        return number;
    }
    if (conversion == Opcode::ToUint32 && !type.isInteger()) {
        number = buildConversionInNumberOperators(walker, Opcode::ToInt32, number, origin);
        if (!number)
            return { };
    }
    return buildConversionInNumberOperators(walker, conversion, number, origin);
}

static bool isBoxedNumberOrNullish(Walker& walker, Reference operand)
{
    Type type = walker.types().at(walker.typeOf(operand));
    return operand.isValue() && !type.isNumber() && (type.primitives & Primitive::Integer) && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull)
        && walker.code().at(operand.id()).representation == Representation::JSValue;
}

static Reference buildBitsOfNumberOrNullish(Walker& walker, Reference operand, Origin origin)
{
    Reference number;
    BlockID whereNullish = walker.splitNumberFromContainer(operand, number);
    if (!whereNullish && !walker.hasFailed()) {
        Reference isNumber = buildTestOfNumber(walker, operand, walker.types().withoutNullish(walker.typeOf(operand)), origin);
        if (!isNumber || !isNumber.isValue())
            return { };
        whereNullish = walker.addBlock(BlockFrequency::Rare);
        continueIfTrue(walker, isNumber, whereNullish);
        number = buildProofOfNumber(walker, operand, isNumber, origin);
    }
    Reference bits = number ? buildConversionInNumberOperators(walker, Opcode::ToInt32, number, origin) : Reference();
    if (!bits)
        return { };
    WayInNumberForms underNumber { walker.currentBlock(), bits, { } };
    walker.continueIn(whereNullish);
    WayInNumberForms underNullish { walker.currentBlock(), walker.constant(PrimitiveKnownToFolding::number(0)), { } };
    std::array<WayInNumberForms, 2> ways { underNumber, underNullish };
    return meetInNumberForms(walker, walker.addBlock(), ways);
}

static bool isRawIterationNumber(Walker& walker, Reference number)
{
    Representation form = walker.representationOf(number);
    return (form == Representation::Double || form == Representation::OptionalDouble) && !walker.isInvariantInInnermostLoop(number);
}

bool takesBitsOfBoxedPrimitive(Walker&, Opcode, Reference operand);

static bool convertOperandsOfBitOperatorOnce(Walker& walker, Opcode opcode, Origin origin, std::span<Reference> operands, const std::array<bool, 2>& areTakenByOperatorAlone)
{
    TypeArena& types = walker.types();
    for (Reference operand : operands) {
        if (!types.at(walker.typeOf(operand)).isNumberLike())
            return false;
    }

    bool hasConverted = false;
    for (uint32_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex) {
        Reference number = operands[operandIndex];
        Type type = types.at(walker.typeOf(number));
        if (!(type.primitives & Primitive::Double))
            continue;
        bool isMasked = opcode == Opcode::BitAnd && isMaskOfLowestBitsOfInt32(types.at(walker.typeOf(operands[1 - operandIndex])));
        if (isMasked && (!walker.isInLoop() || (areTakenByOperatorAlone[operandIndex] && isRawIterationNumber(walker, number))))
            continue;
        if (opcode != Opcode::BitNot && !type.isNumber() && walker.representationOf(number) == Representation::JSValue && type.hasOnlyPrimitives(Primitive::NumberLike))
            continue;
        hasConverted = true;
        if (takesBitsOfBoxedPrimitive(walker, Opcode::BitOr, number)) {
            operands[operandIndex] = buildOperatorInNumberForms(walker, Opcode::BitOr, number, integerInNumberForms(walker, 0), origin);
            continue;
        }
        if (walker.isInLoop() && walker.opensGuesses() && isBoxedNumberOrNullish(walker, number)) {
            operands[operandIndex] = buildBitsOfNumberOrNullish(walker, number, origin);
            continue;
        }
        if (!type.isNumber())
            number = buildOperatorInNumberForms(walker, Opcode::ToNumber, number, { }, origin);
        if (!number) {
            operands[operandIndex] = { };
            continue;
        }
        operands[operandIndex] = buildConversionInNumberOperators(walker, Opcode::ToInt32, number, origin);
    }
    return hasConverted;
}

static Reference buildSumOfHalves(Walker& walker, Origin origin, const OperandsInNumberOperators& operands)
{
    static constexpr OpcodeSet shifts { Opcode::RShift, Opcode::URShift };
    static constexpr int64_t lowHalf = 0xffff;
    static constexpr int64_t halfWidth = 16;
    const Code& code = walker.code();
    auto strip = [&](ValueID id) {
        return code.isConstant(id) ? id : code.valueBehindNames<valuePreservingOpcodes>(id);
    };
    auto integerOf = [&](ValueID id) -> std::optional<int64_t> {
        return code.isConstant(id) && code.at(id).opcode == Opcode::Integer ? std::optional(code.at(id).integer()) : std::nullopt;
    };
    auto isInt32 = [&](ValueID id) {
        return !code.isConstant(id) && walker.types().isSubset(code.at(id).type, TypeTable::int32);
    };
    auto operandBeside = [&](ValueID id, OpcodeSet opcodes, int64_t integer) -> ValueID {
        id = strip(id);
        if (code.isConstant(id) || !opcodes.contains(code.at(id).opcode))
            return { };
        const Operation& operation = code.at(id);
        if (integerOf(operation.operand(1)) == integer)
            return operation.operand(0);
        return operation.opcode == Opcode::BitAnd && integerOf(operation.operand(0)) == integer ? operation.operand(1) : ValueID();
    };
    for (uint32_t place : { 0u, 1u }) {
        Reference low = operands[place].reference;
        Reference high = operands[1 - place].reference;
        if (!low || !low.isValue() || !high || !high.isValue())
            continue;
        ValueID sumOfLows = operandBeside(low.id(), { Opcode::BitAnd }, lowHalf);
        ValueID sumOfHighs = sumOfLows ? operandBeside(high.id(), { Opcode::LShift }, halfWidth) : ValueID();
        if (!sumOfHighs)
            continue;
        ValueID lows = strip(sumOfLows);
        if (code.isConstant(lows) || code.at(lows).opcode != Opcode::Add || !isInt32(lows))
            continue;
        Vector<ValueID, 4> highs { strip(sumOfHighs) };
        for (size_t term = 0; term < highs.size() && highs.size() < 4; ++term) {
            while (!code.isConstant(highs[term]) && code.at(highs[term]).opcode == Opcode::Add && isInt32(highs[term]) && highs.size() < 4) {
                highs.append(strip(code.at(highs[term]).operand(1)));
                highs[term] = strip(code.at(highs[term]).operand(0));
            }
        }
        size_t carry = highs.findIf([&](ValueID term) {
            ValueID shifted = operandBeside(term, shifts, halfWidth);
            return shifted && strip(shifted) == lows;
        });
        if (highs.size() != 3 || carry == notFound)
            continue;
        highs.removeAt(carry);
        auto wordOf = [&](ValueID lowTerm, size_t place) -> Reference {
            if (std::optional<int64_t> bits = integerOf(strip(lowTerm)); bits && *bits >= 0 && *bits <= lowHalf) {
                std::optional<int64_t> above = integerOf(highs[place]);
                if (!above || *above < -(lowHalf + 1) || *above > lowHalf)
                    return { };
                Draft word(Opcode::Integer, origin);
                word.setInteger(static_cast<int32_t>(static_cast<uint32_t>(*above) << halfWidth | static_cast<uint32_t>(*bits)));
                return walker.constant(WTF::move(word));
            }
            ValueID word = operandBeside(lowTerm, { Opcode::BitAnd }, lowHalf);
            ValueID shifted = word ? operandBeside(highs[place], shifts, halfWidth) : ValueID();
            return shifted && strip(shifted) == strip(word) && isInt32(word) ? Reference::value(word) : Reference();
        };
        std::array<ValueID, 2> lowTerms { code.at(lows).operand(0), code.at(lows).operand(1) };
        for (size_t first : { 0u, 1u }) {
            Reference left = wordOf(lowTerms[0], first);
            Reference right = left ? wordOf(lowTerms[1], 1 - first) : Reference();
            if (!right || (left.isConstant() && right.isConstant()))
                continue;
            Reference sum = buildIntegerOperator(walker, Opcode::Add, left, right, origin);
            if (!sum)
                return { };
            walker.count(StatisticID::HalvesAddedAsWords);
            return buildConversionInNumberOperators(walker, Opcode::ToInt32, sum, origin);
        }
    }
    return { };
}

static bool buildBitOperator(Walker& walker, const BodyValue& value, OperandsInNumberOperators& operands)
{
    Opcode opcode = value.opcode();
    uint32_t numberOfOperands = numberOfNamedOperandsOf(opcode);
    Origin origin = originInFrontOfNumberOperator(walker, value);
    if (opcode == Opcode::BitOr && walker.has(Mechanism::Folding)) {
        if (Reference sum = buildSumOfHalves(walker, origin, operands))
            return bindInNumberOperators(walker, value, sum);
    }

    bool takesOtherOperands = false;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        OperandInNumberOperators& operand = operands[operandIndex];
        if (operand.known.kind() == PrimitiveKnownToFolding::Kind::Number && numberOfOperands == 2 && !walker.types().isSubset(walker.typeOf(operand.reference), TypeTable::int32)) {
            Draft word(Opcode::Integer, origin);
            word.setInteger(toInt32(operand.known.asNumber()));
            takesOtherOperands = true;
            operand.reference = walker.constant(WTF::move(word));
            operand.type = walker.types().at(walker.typeOf(operand.reference));
            operand.known = primitiveKnownInNumberForms(walker, operand.reference);
            walker.count(StatisticID::ConstantsCutToInt32);
            continue;
        }
        Reference simpler = buildSimplerOperandOfBitOperator(walker, origin, operand.written);
        if (!simpler)
            simpler = integerThatCharCodeIsKnownToBe(walker, value, operand.written);
        if (walker.isUnreachable())
            return true;
        if (!simpler)
            continue;
        takesOtherOperands = true;
        operand.reference = simpler;
        operand.type = walker.types().at(walker.typeOf(simpler));
        operand.known = primitiveKnownInNumberForms(walker, simpler);
    }

    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands && numberOfOperands == 2; ++operandIndex) {
        const OperandInNumberOperators& other = operands[1 - operandIndex];
        PrimitiveKnownToFolding known = operands[operandIndex].known;
        if (!other.type.isNumber() || known.kind() != PrimitiveKnownToFolding::Kind::Number)
            continue;
        if (isIdentityElementOfBitOperator(opcode, operandIndex, toInt32(known.asNumber())))
            return bindInNumberOperators(walker, value, buildBitsOfNumberInNumberOperators(walker, opcode == Opcode::URShift ? Opcode::ToUint32 : Opcode::ToInt32, other.reference, origin));
    }

    std::array<Reference, 2> taken { operands[0].reference, operands[1].reference };
    std::span<Reference> takenByOperator = std::span(taken).first(numberOfOperands);
    std::array<bool, 2> areTakenByOperatorAlone { };
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
        areTakenByOperatorAlone[operandIndex] = walker.currentBodyIndex().onlyUsingValue(operands[operandIndex].written) == value.id();
    takesOtherOperands |= convertOperandsOfBitOperatorOnce(walker, opcode, origin, takenByOperator, areTakenByOperatorAlone);
    if (!takesOtherOperands)
        return false;
    return buildOnOperandsInNumberOperators(walker, value, takenByOperator);
}

static bool buildComparisonOfWholeRemainders(Walker& walker, const BodyValue& comparison, const OperandsInNumberOperators& operands)
{
    InputsOfTypedOperation inputs(walker, nullptr);
    std::array<std::optional<WholeRemainder>, 2> remainders { findWholeRemainder(walker, operands[0].written, inputs), findWholeRemainder(walker, operands[1].written, inputs) };
    if (!remainders[0] && !remainders[1])
        return false;
    for (uint32_t index = 0; index < 2; ++index) {
        if (!remainders[index] && !operands[index].type.isNumber())
            return false;
    }

    bool isEquality = allEqualities.contains(comparison.opcode());
    Origin origin = originInFrontOfNumberOperator(walker, comparison);
    std::array<Reference, 2> compared { operands[0].reference, operands[1].reference };
    Vector<ValueID, 2> divisorsThatMayBeZero;
    for (uint32_t index = 0; index < 2; ++index) {
        if (!remainders[index])
            continue;
        const OperandInNumberOperators& other = operands[1 - index];
        std::optional<int64_t> knownOther = isEquality && !remainders[1 - index] ? integerKnownInNumberForms(walker, other.reference) : std::nullopt;
        if (knownOther) {
            if (std::optional<BitsOfDividendCompared> bits = tryCompareBitsOfDividend(walker, origin, *remainders[index], *knownOther)) {
                compared[index] = bits->bits;
                compared[1 - index] = bits->comparedWith;
                continue;
            }
        }
        compared[index] = buildWholeRemainder(walker, origin, *remainders[index]);
        bool differsFromZeroAnyway = isEquality && other.type.isInteger() && !other.type.integers.contains(0);
        if (remainders[index]->mayBeNaN && !differsFromZeroAnyway)
            divisorsThatMayBeZero.append(remainders[index]->divisor);
    }

    Draft draft = walker.draftOf(comparison);
    draft.setOperand(0, compared[0]);
    draft.setOperand(1, compared[1]);
    return bindInNumberOperators(walker, comparison, buildComparisonWhereDivisorsAreNotZero(walker, WTF::move(draft), divisorsThatMayBeZero.span()));
}

static bool standsInFrontOfLoopAroundInNumberOperators(Walker& walker, const BodyValue& user, Reference reference)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(user.id()));
    if (!loop || !reference.isValue())
        return false;
    BodyBlock head = index.block(index.loop(loop).head);
    if (!head.numberOfParameters()) {
        std::optional<uint32_t> open = walker.innermostOpenLoop();
        return open && walker.standsInFrontOfOpenLoop(*open, reference);
    }
    Reference first = walker.referenceBoundTo(head.parameter(0));
    if (!first.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& parameter = code.at(first.id());
    if (parameter.opcode != Opcode::Parameter || !parameter.block() || !code.at(parameter.block()).isLoopHead)
        return false;
    if (reference.id().index() < first.id().index())
        return true;
    BlockID block = code.blockOf(reference.id());
    return block && block.index() < parameter.block().index();
}

Reference roundBoundBeforeLoop(Walker&, Opcode relation, Origin, Reference bound, bool boundIsOnLeft);

static bool buildRelationWithNumberRoundedInFrontOfLoop(Walker& walker, const BodyValue& relation, const OperandsInNumberOperators& operands)
{
    if (!walker.isInLoop() || !walker.has(Mechanism::LoopInvariants))
        return false;
    for (uint32_t numberIndex = 0; numberIndex < 2; ++numberIndex) {
        const OperandInNumberOperators& number = operands[numberIndex];
        if (!operands[1 - numberIndex].type.isInteger() || !number.type.isNumber() || number.type.isInteger())
            continue;
        if (operands[1 - numberIndex].reference.isValue() && walker.representationOf(operands[1 - numberIndex].reference) == Representation::Double) {
            Representation held = walker.representationOf(number.reference);
            if (held == Representation::Double || !standsInFrontOfLoopAroundInNumberOperators(walker, relation, number.reference))
                return false;
            std::optional<ConversionPath> path = findConversion(walker.types(), walker.typeOf(number.reference), held, Representation::Double);
            if (!path)
                return false;
            Reference unboxed = number.reference;
            for (ConversionStep step : *path) {
                Draft conversion(step.opcode, originInFrontOfNumberOperator(walker, relation));
                conversion.setOperand(0, unboxed);
                conversion.representation = step.to;
                unboxed = walker.build(WTF::move(conversion));
                if (!unboxed)
                    return true;
            }
            std::array<Reference, 2> compared { operands[0].reference, operands[1].reference };
            compared[numberIndex] = unboxed;
            return buildOnOperandsInNumberOperators(walker, relation, compared);
        }
        if (!standsInFrontOfLoopAroundInNumberOperators(walker, relation, number.reference))
            return false;

        std::array<Reference, 2> compared { operands[0].reference, operands[1].reference };
        bool canCheckBound = number.type.primitives & Primitive::Integer && walker.has(Mechanism::Guesses) && walker.opensGuesses() && walker.isInvariantInSpecializedVersion(number.reference, Walker::IterationPurity::KnownEffectsOnly);
        if (Reference integer = canCheckBound ? roundBoundBeforeLoop(walker, relation.opcode(), originInFrontOfNumberOperator(walker, relation), number.reference, !numberIndex) : Reference()) {
            compared[numberIndex] = integer;
            return buildOnOperandsInNumberOperators(walker, relation, compared);
        }
        if (walker.hasFailed())
            return true;
        Draft bound(Opcode::IntegerBound, originInFrontOfNumberOperator(walker, relation));
        walker.chooseConservativeFields(bound);
        bound.setNumberOperand(number.reference);
        bound.setComparison(relation.opcode());
        bound.setNumberIsOnLeft(!numberIndex);
        Reference rounded = walker.build(WTF::move(bound));
        if (!rounded)
            return true;
        if (walker.code().blockOf(rounded.id()) != walker.currentBlock())
            compared[numberIndex] = rounded;
        return buildOnOperandsInNumberOperators(walker, relation, compared);
    }
    return false;
}

int64_t tightenBoundOfSteppedCounter(Walker& walker, const BodyIndex& index, Opcode relation, ValueID counter, int64_t bound)
{
    constexpr int64_t greatest = std::numeric_limits<int32_t>::max();
    if ((relation != Opcode::Less && relation != Opcode::LessEq) || !counter || !index.isParameter(counter) || std::abs(bound) > greatest)
        return bound;
    BlockID head = index.blockOf(counter);
    LoopID loop = index.loopOf(head);
    if (!loop || index.loop(loop).head != head)
        return bound;
    uint32_t place = counter.index() - index.block(head).firstParameter().index();
    auto writtenInteger = [&](ValueID value) -> std::optional<int64_t> {
        value = index.stripCasts(value);
        if (!value || index.isParameter(value) || index.opcodeOf(value) != Opcode::Integer)
            return std::nullopt;
        int64_t integer = index.value(value).as<Opcode::Integer>().integer();
        return std::abs(integer) <= greatest ? std::optional(integer) : std::nullopt;
    };
    std::optional<int64_t> start;
    std::optional<int64_t> step;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, head)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID passed = place < arguments.size() ? index.stripCasts(arguments[place]) : ValueID();
        if (!passed || index.isParameter(passed))
            return bound;
        std::optional<int64_t>& known = index.isInLoop(edge.from, loop) ? step : start;
        std::optional<int64_t> found;
        if (&known == &start)
            found = writtenInteger(passed);
        else if (index.opcodeOf(passed) == Opcode::Add && index.stripCasts(index.value(passed).operand(0)) == counter)
            found = writtenInteger(index.value(passed).operand(1));
        if (!found || known.value_or(*found) != *found)
            return bound;
        known = found;
    }
    int64_t last = bound - (relation == Opcode::Less);
    if (!start || !step || *step < 2 || last < *start)
        return bound;
    return *start + (last - *start) / *step * *step + (relation == Opcode::Less);
}

static bool loopStoresBeyondCounter(const BodyIndex& index, ValueID counter)
{
    LoopID loop = index.loopOf(index.blockOf(counter));
    return loop && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            ValueID key = operation.opcode() == Opcode::ElemSet ? index.stripCasts(operation.propertyOperand()) : ValueID();
            return key && !index.isParameter(key) && index.opcodeOf(key) == Opcode::Add && index.stripCasts(index.value(key).operand(0)) == counter;
        });
    });
}

static bool buildRelationOfSteppedCounter(Walker& walker, const BodyValue& relation, const OperandsInNumberOperators& operands)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::LoopInvariants) || !operands[0].type.isInteger() || !operands[1].reference.isConstant())
        return false;
    ValueID counter = index.stripCasts(operands[0].written);
    std::optional<int64_t> bound = integerKnownInNumberForms(walker, operands[1].reference);
    int64_t tight = bound ? tightenBoundOfSteppedCounter(walker, index, relation.opcode(), counter, *bound) : 0;
    if (!bound || tight == *bound || !loopStoresBeyondCounter(index, counter))
        return false;
    Draft lesser(Opcode::Integer, originInFrontOfNumberOperator(walker, relation));
    lesser.setInteger(tight);
    std::array<Reference, 2> compared { operands[0].reference, walker.constant(WTF::move(lesser)) };
    return buildOnOperandsInNumberOperators(walker, relation, compared);
}

static_assert(opcodesThatBuildNumberFormTakes == (opcodesOfNumberOperators));

static Reference buildProductWithoutNegativeZero(Walker& walker, const BodyValue& value, const OperandsInNumberOperators& operands)
{
    TypeArena& types = walker.types();
    if (!operands[0].type.isInteger() || !operands[1].type.isInteger())
        return { };
    Draft product(Opcode::MultiplyIntegers, walker.originOf(value));
    product.setMultiplicandOperand(operands[0].reference);
    product.setMultiplierOperand(operands[1].reference);
    const Type& products = types.at(walker.resultTypeOf(product));
    if (!products.isInteger() || !products.integers.isSubsetOf(Range::exact()) || products.integers.isSingleValue())
        return { };
    Range multiplicands = operands[0].type.integers;
    Range multipliers = operands[1].type.integers;
    bool mayBeNegativeZero = (multiplicands.minimum < 0 && multipliers.contains(0)) || (multipliers.minimum < 0 && multiplicands.contains(0));
    if (!mayBeNegativeZero)
        return { };
    bool isSignIgnored = isSignOfZeroIgnored(walker, value.id()) || (walker.isInLoop() && isZeroSignUnused(walker, value.id()));
    bool canGuard = walker.isInLoop() && walker.canBuildIterationGuard() && isTermOfRemainder(walker.currentBodyIndex(), value.id());
    if (!isSignIgnored && !canGuard)
        return { };
    walker.chooseConservativeFields(product);
    Reference whole = walker.build(WTF::move(product));
    if (isSignIgnored || !whole || !whole.isValue() || walker.isUnreachable())
        return whole;
    Origin origin = walker.originOf(value);
    origin.isTailCall = false;
    Reference isNotZero = buildOperatorInNumberForms(walker, Opcode::ToBoolean, whole, { }, origin);
    if (!isNotZero)
        return { };
    BlockID isZero = walker.addBlock(BlockFrequency::Unlikely);
    BlockID restarts = walker.addBlock(BlockFrequency::Rare);
    BlockID passed = walker.addBlock();
    walker.branch(isNotZero, passed, isZero);
    walker.continueIn(isZero);
    for (const OperandInNumberOperators& factor : operands) {
        if (walker.isUnreachable() || factor.type.integers.minimum >= 0)
            continue;
        Reference isFromZeroUp = buildOperatorInNumberForms(walker, Opcode::GreaterEq, factor.reference, integerInNumberForms(walker, 0), origin);
        if (!isFromZeroUp)
            return { };
        BlockID next = walker.addBlock();
        walker.branch(isFromZeroUp, next, restarts);
        walker.continueIn(next);
    }
    if (!walker.isUnreachable())
        walker.jumpTo(passed);
    walker.continueIn(restarts);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(passed);
    walker.recordSpeculation();
    return walker.isUnreachable() || walker.hasFailed() ? Reference() : whole;
}

static bool buildIntegerRemainder(Walker& walker, const BodyValue& value, const OperandsInNumberOperators& operands)
{
    TypeArena& types = walker.types();
    Type dividend = operands[0].type;
    Type divisor = operands[1].type;
    Reference divisorReference = operands[1].reference;
    if (std::optional<uint64_t> noted = divisorReference.isValue() && divisor.isInteger() && divisor.integers.contains(0) ? findKnown(walker, KnownNote::PositionBelowLength, divisorReference) : std::nullopt) {
        divisorReference = Reference::fromBits(static_cast<uint32_t>(*noted));
        divisor = types.at(walker.typeOf(divisorReference));
    }
    bool divisorMayBeZero = divisor.isInteger() && divisor.integers.contains(0);
    if (!dividend.isInteger() || !divisor.isInteger() || (dividend.integers.minimum >= 0 && !divisorMayBeZero && divisorReference == operands[1].reference) || !dividend.integers.isSubsetOf(Range::exact()) || !divisor.integers.isSubsetOf(Range::exact()) || divisor.integers.isSubsetOf(Range::one(1)))
        return false;
    bool isSignIgnored = dividend.integers.minimum >= 0 || isSignOfZeroIgnored(walker, value.id()) || (walker.isInLoop() && isZeroSignUnused(walker, value.id()));
    bool canGuard = walker.isInLoop() && walker.canBuildIterationGuard();
    if (!isSignIgnored && !canGuard)
        return false;
    int64_t magnitude = divisor.integers.isSingleValue() ? std::abs(divisor.integers.minimum) : 0;
    if (isSignIgnored && magnitude && !(magnitude & (magnitude - 1)) && isOnlyComparedWithIntegers(walker, value.id()))
        return false;
    Origin origin = walker.originOf(value);
    origin.isTailCall = false;
    if (divisorMayBeZero) {
        if (!walker.isInLoop())
            return false;
        Reference isNotZero = buildOperatorInNumberForms(walker, Opcode::ToBoolean, divisorReference, { }, origin);
        if (!isNotZero || walker.isUnreachable())
            return false;
        auto isShown = [&](Reference test, bool answer) {
            PrimitiveKnownToFolding known = test ? primitiveKnownInNumberForms(walker, test) : PrimitiveKnownToFolding();
            if (known.kind() == PrimitiveKnownToFolding::Kind::Boolean)
                return known.asBoolean() == answer;
            return test && test.isValue() && !!walker.findPassedTest(test, answer);
        };
        bool isKnown = isShown(isNotZero, true);
        if (!isKnown) {
            Reference isZero = buildOperatorInNumberForms(walker, Opcode::StrictEq, operands[1].reference, integerInNumberForms(walker, 0), origin);
            if (walker.isUnreachable())
                return false;
            isKnown = isShown(isZero, false);
        }
        if (!isKnown && (!canGuard || isShown(isNotZero, false)))
            return false;
        if (!isKnown) {
            BlockID passed = walker.addBlock();
            BlockID restarts = walker.addBlock(BlockFrequency::Rare);
            walker.branch(isNotZero, passed, restarts);
            walker.continueIn(restarts);
            if (!walker.isUnreachable())
                walker.restartIteration();
            walker.continueIn(passed);
            walker.recordSpeculation();
            if (walker.isUnreachable() || walker.hasFailed())
                return true;
        }
    }
    Draft whole(Opcode::RemainderOfIntegers, origin);
    walker.chooseConservativeFields(whole);
    whole.setDividendOperand(operands[0].reference);
    whole.setDivisorOperand(divisorReference);
    const Type& remainders = types.at(walker.resultTypeOf(whole));
    if (!remainders.isInteger() || remainders.integers.isSingleValue())
        return walker.isUnreachable() || walker.hasFailed();
    Reference remainder = walker.build(WTF::move(whole));
    if (!remainder || !remainder.isValue() || walker.isUnreachable())
        return false;
    if (!isSignIgnored) {
        BlockID isZero = walker.addBlock(BlockFrequency::Unlikely);
        BlockID restarts = walker.addBlock(BlockFrequency::Rare);
        BlockID passed = walker.addBlock();
        Reference isNotZero = buildOperatorInNumberForms(walker, Opcode::ToBoolean, remainder, { }, origin);
        if (!isNotZero)
            return false;
        walker.branch(isNotZero, passed, isZero);
        walker.continueIn(isZero);
        if (!walker.isUnreachable()) {
            Reference isFromZeroUp = buildOperatorInNumberForms(walker, Opcode::GreaterEq, operands[0].reference, integerInNumberForms(walker, 0), origin);
            if (!isFromZeroUp)
                return false;
            walker.branch(isFromZeroUp, passed, restarts);
            walker.continueIn(restarts);
            if (!walker.isUnreachable())
                walker.restartIteration();
        }
        walker.continueIn(passed);
        walker.recordSpeculation();
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
    }
    return bindInNumberOperators(walker, value, remainder);
}

bool buildNumberFormOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfNumberOperators.contains(opcode) || !hasMechanismsOfNumberForms(walker))
        return false;

    if (opcode == Opcode::LogicalNot)
        return bindWhatIsNegatedTwice(walker, value);

    if (allEqualities.contains(opcode) || allRelations.contains(opcode) || opcode == Opcode::ToBoolean) {
        bool somethingIsProven = false;
        for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(opcode) && !somethingIsProven; ++operandIndex)
            somethingIsProven |= !walker.hasUninformativeType(walker.referenceOf(value.operand(operandIndex)));
        if (!somethingIsProven)
            return false;
    }

    OperandsInNumberOperators operands;
    if (!readOperandsInNumberOperators(walker, value, operands))
        return false;

    if (arithmeticInNumberOperators.contains(opcode) && walker.has(Mechanism::Folding)) {
        if (Reference given = valueThatArithmeticGives(walker, opcode, operands)) {
            walker.noteValueThatFoldedArithmeticGives(given);
            return bindInNumberOperators(walker, value, given);
        }
    }
    if (opcode == Opcode::Add) {
        if (Reference middle = buildMiddleOfBounds(walker, value, operands))
            return bindInNumberOperators(walker, value, middle);
        if (Reference carry = buildWideProduct(walker, value))
            return bindInNumberOperators(walker, value, carry);
    }
    if (opcode == Opcode::BitAnd) {
        for (uint32_t operandIndex : { 0u, 1u }) {
            if (Reference bits = buildLowBitsOfWideProduct(walker, operands[operandIndex].reference, operands[1 - operandIndex].reference, originInFrontOfNumberOperator(walker, value)))
                return bindInNumberOperators(walker, value, bits);
        }
    }
    if (opcode == Opcode::Mul) {
        if (Reference product = buildProductWithoutNegativeZero(walker, value, operands))
            return bindInNumberOperators(walker, value, product);
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
    }
    if (opcode == Opcode::Pow && operands[0].type.isNumber() && isKnownIntegerInNumberOperators(operands[1].known, 2))
        return bindInNumberOperators(walker, value, buildOperatorInNumberForms(walker, Opcode::Mul, operands[0].reference, operands[0].reference, walker.originOf(value)));

    if (allBitwiseOperators.contains(opcode))
        return buildBitOperator(walker, value, operands);
    if (opcode == Opcode::Mod)
        return buildIntegerRemainder(walker, value, operands) || buildNumberRemainderWithItsTest(walker, value);

    InputsOfTypedOperation inputs(walker, nullptr);
    if (allEqualities.contains(opcode)) {
        if (buildComparisonOfWholeRemainders(walker, value, operands))
            return true;
        return buildComparisonByCodeUnit(walker, value, inputs);
    }

    if (allRelations.contains(opcode)) {
        if (buildRelationOfSteppedCounter(walker, value, operands))
            return true;
        if (buildRelationWithNumberRoundedInFrontOfLoop(walker, value, operands))
            return true;
        if (buildComparisonOfWholeRemainders(walker, value, operands))
            return true;
        return buildComparisonByCodeUnit(walker, value, inputs);
    }

    if (opcode == Opcode::ToBoolean) {
        std::optional<WholeRemainder> remainder = findWholeRemainder(walker, operands[0].written, inputs);
        if (!remainder)
            return false;
        Reference integer = buildZeroWhereWholeRemainderIsZero(walker, originInFrontOfNumberOperator(walker, value), *remainder);
        return buildOnOperandsInNumberOperators(walker, value, singleElementSpan(integer));
    }

    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
