/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include "AOTWalkKnown.h"
#include "AOTWalkNumberGuesses.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr size_t mostFactorsInWideProduct = 8;
constexpr size_t mostTermsInWideProduct = 12;
constexpr unsigned deepestValueInWideProduct = 12;

struct FactorInWideProduct {
    uint32_t key;
    uint32_t shift;
    Reference value;
    Range needed;
};

struct TermInWideProduct {
    int64_t coefficient;
    std::array<uint8_t, 2> factors;
};

struct SumInWideProduct {
    Vector<TermInWideProduct, 8> terms;
    Range range { Range::one(0) };
};

class WideProductReader {
public:
    WideProductReader(Walker& walker, unsigned digitBits, unsigned extraBits)
        : m_walker(walker)
        , m_index(walker.currentBodyIndex())
        , m_digitBits(digitBits)
        , m_extraBits(extraBits)
    {
    }

    std::optional<SumInWideProduct> read(ValueID, unsigned depth = 0);
    bool add(SumInWideProduct& to, const SumInWideProduct& from, int64_t scale);
    FactorInWideProduct& factorAt(uint8_t number) { return m_factors[number - 1]; }
    std::span<FactorInWideProduct> factors() { return m_factors.mutableSpan(); }

private:
    Type typeOf(Reference reference) const { return m_walker.types().at(m_walker.typeOf(reference)); }
    bool isExactInteger(Reference) const;
    std::optional<unsigned> knownShift(ValueID) const;
    std::optional<unsigned> widthOfKnownMask(ValueID) const;
    Reference boundTo(ValueID) const;
    uint8_t numberOfFactor(Reference, unsigned shift, Range needed);
    std::optional<SumInWideProduct> readFactor(Reference, unsigned shift = 0, bool isCarried = false);
    std::optional<SumInWideProduct> floorOf(Reference, unsigned shift, Range);

    Walker& m_walker;
    const BodyIndex& m_index;
    unsigned m_digitBits;
    unsigned m_extraBits;
    Vector<FactorInWideProduct, mostFactorsInWideProduct> m_factors;
};

} // anonymous namespace

bool WideProductReader::isExactInteger(Reference reference) const
{
    TypeID id = reference && reference.isRecord() ? m_walker.typeOf(reference) : TypeID();
    if (!id)
        return false;
    Type type = m_walker.types().at(id);
    return type.isInteger() && type.integers.isSubsetOf(Range::exact());
}

Reference WideProductReader::boundTo(ValueID value) const
{
    Reference reference = m_walker.referenceBoundTo(value);
    return reference && reference.isRecord() ? reference : Reference();
}

std::optional<unsigned> WideProductReader::knownShift(ValueID operand) const
{
    ValueID value = m_index.stripCasts(operand);
    if (m_index.isParameter(value) || m_index.opcodeOf(value) != Opcode::Integer)
        return std::nullopt;
    int64_t shift = m_index.value(value).as<Opcode::Integer>().integer();
    return shift > 0 && shift < 32 ? std::optional<unsigned>(shift) : std::nullopt;
}

std::optional<unsigned> WideProductReader::widthOfKnownMask(ValueID operand) const
{
    ValueID value = m_index.stripCasts(operand);
    if (m_index.isParameter(value) || m_index.opcodeOf(value) != Opcode::Integer)
        return std::nullopt;
    int64_t mask = m_index.value(value).as<Opcode::Integer>().integer();
    if (mask <= 0 || mask > std::numeric_limits<int32_t>::max() || (mask & (mask + 1)))
        return std::nullopt;
    return WTF::fastLog2(static_cast<uint64_t>(mask) + 1);
}

uint8_t WideProductReader::numberOfFactor(Reference value, unsigned shift, Range needed)
{
    const Code& code = m_walker.code();
    uint32_t key = value.isValue() ? code.valueBehindNames<valuePreservingOpcodes>(value.id()).index() << 1 : value.bits() << 1 | 1;
    for (size_t place = 0; place < m_factors.size(); ++place) {
        if (m_factors[place].key == key && m_factors[place].shift == shift)
            return place + 1;
    }
    if (m_factors.size() == mostFactorsInWideProduct)
        return 0;
    m_factors.append({ key, shift, value, needed });
    return m_factors.size();
}

std::optional<SumInWideProduct> WideProductReader::readFactor(Reference value, unsigned shift, bool isCarried)
{
    if (!isExactInteger(value) || shift >= m_digitBits)
        return std::nullopt;
    unsigned extraBits = isCarried || m_walker.isInvariantInSpecializedVersion(value) ? m_extraBits : 0;
    Range needed { 0, (int64_t { 1 } << (m_digitBits + extraBits - shift)) - 1 };
    Range range = typeOf(value).integers;
    Range told { std::max(range.minimum, needed.minimum), std::min(range.maximum, needed.maximum) };
    if (told.minimum > told.maximum)
        return std::nullopt;
    uint8_t number = numberOfFactor(value, 0, needed);
    if (!number)
        return std::nullopt;
    return SumInWideProduct { { TermInWideProduct { 1, { 0, number } } }, told };
}

std::optional<SumInWideProduct> WideProductReader::floorOf(Reference value, unsigned shift, Range range)
{
    uint8_t number = numberOfFactor(value, shift, Range::full());
    if (!number)
        return std::nullopt;
    return SumInWideProduct { { TermInWideProduct { 1, { 0, number } } }, Range { range.minimum >> shift, range.maximum >> shift } };
}

bool WideProductReader::add(SumInWideProduct& to, const SumInWideProduct& from, int64_t scale)
{
    for (const TermInWideProduct& term : from.terms) {
        CheckedInt64 coefficient = CheckedInt64(term.coefficient) * scale;
        size_t place = to.terms.findIf([&](const TermInWideProduct& other) { return other.factors == term.factors; });
        if (place != notFound)
            coefficient += to.terms[place].coefficient;
        if (coefficient.hasOverflowed())
            return false;
        if (place == notFound)
            to.terms.append({ coefficient.value(), term.factors });
        else if (coefficient.value())
            to.terms[place].coefficient = coefficient.value();
        else
            to.terms.removeAt(place);
    }
    return to.terms.size() <= mostTermsInWideProduct;
}

std::optional<SumInWideProduct> WideProductReader::read(ValueID written, unsigned depth)
{
    if (depth > deepestValueInWideProduct)
        return std::nullopt;
    ValueID value = m_index.stripCasts(written);
    Reference bound = boundTo(value);
    if (!bound)
        return std::nullopt;
    if (m_index.isParameter(value))
        return readFactor(bound, 0, true);
    BodyValue operation = m_index.value(value);
    switch (operation.opcode()) {
    case Opcode::Integer:
        return SumInWideProduct { { TermInWideProduct { operation.as<Opcode::Integer>().integer(), { 0, 0 } } }, Range::one(operation.as<Opcode::Integer>().integer()) };
    case Opcode::Add:
    case Opcode::Sub: {
        if (!isExactInteger(bound))
            return std::nullopt;
        std::optional<SumInWideProduct> left = read(operation.operand(0), depth + 1);
        std::optional<SumInWideProduct> right = left ? read(operation.operand(1), depth + 1) : std::nullopt;
        bool isSum = operation.opcode() == Opcode::Add;
        if (!right || !add(*left, *right, isSum ? 1 : -1))
            return std::nullopt;
        left->range = isSum ? Range { left->range.minimum + right->range.minimum, left->range.maximum + right->range.maximum } : Range { left->range.minimum - right->range.maximum, left->range.maximum - right->range.minimum };
        return left->range.isSubsetOf(Range::exact()) ? left : std::nullopt;
    }
    case Opcode::Mul: {
        if (!isExactInteger(bound))
            return std::nullopt;
        std::optional<SumInWideProduct> left = read(operation.operand(0), depth + 1);
        std::optional<SumInWideProduct> right = left ? read(operation.operand(1), depth + 1) : std::nullopt;
        if (!right)
            return std::nullopt;
        SumInWideProduct product;
        for (const TermInWideProduct& term : left->terms) {
            SumInWideProduct row;
            for (const TermInWideProduct& other : right->terms) {
                if (term.factors[0] || other.factors[0])
                    return std::nullopt;
                row.terms.append({ other.coefficient, { std::min(term.factors[1], other.factors[1]), std::max(term.factors[1], other.factors[1]) } });
            }
            if (!add(product, row, term.coefficient))
                return std::nullopt;
        }
        product.range = Range::empty();
        for (int64_t multiplicand : { left->range.minimum, left->range.maximum }) {
            for (int64_t multiplier : { right->range.minimum, right->range.maximum }) {
                CheckedInt64 corner = CheckedInt64(multiplicand) * multiplier;
                if (corner.hasOverflowed())
                    return std::nullopt;
                product.range = product.range.unionWith(Range::one(corner.value()));
            }
        }
        return product.range.isSubsetOf(Range::exact()) ? std::optional<SumInWideProduct>(WTF::move(product)) : std::nullopt;
    }
    case Opcode::BitAnd: {
        uint32_t maskIndex = widthOfKnownMask(operation.operand(1)) ? 1 : 0;
        std::optional<unsigned> width = widthOfKnownMask(operation.operand(maskIndex));
        Reference masked = boundTo(m_index.stripCasts(operation.operand(1 - maskIndex)));
        if (!width || !masked || !isExactInteger(masked))
            return readFactor(bound);
        std::optional<SumInWideProduct> bits = read(operation.operand(1 - maskIndex), depth + 1);
        Range mask { 0, (int64_t { 1 } << *width) - 1 };
        if (!bits || bits->range.isSubsetOf(mask))
            return bits;
        std::optional<SumInWideProduct> floor = floorOf(masked, *width, bits->range);
        if (!floor || !add(*bits, *floor, -(int64_t { 1 } << *width)))
            return std::nullopt;
        bits->range = mask;
        return bits;
    }
    case Opcode::RShift: {
        std::optional<unsigned> shift = knownShift(operation.operand(1));
        Reference shifted = boundTo(m_index.stripCasts(operation.operand(0)));
        if (!shift || !shifted)
            return readFactor(bound);
        if (!isExactInteger(shifted))
            return readFactor(bound, *shift);
        std::optional<SumInWideProduct> whole = read(operation.operand(0), depth + 1);
        if (!whole || !whole->range.isSubsetOf(Range::int32()))
            return std::nullopt;
        return floorOf(shifted, *shift, whole->range);
    }
    case Opcode::LShift: {
        std::optional<unsigned> shift = knownShift(operation.operand(1));
        std::optional<SumInWideProduct> whole = shift && isExactInteger(boundTo(m_index.stripCasts(operation.operand(0)))) ? read(operation.operand(0), depth + 1) : std::nullopt;
        if (!whole)
            return std::nullopt;
        SumInWideProduct raised;
        raised.range = { whole->range.minimum * (int64_t { 1 } << *shift), whole->range.maximum * (int64_t { 1 } << *shift) };
        if (!whole->range.isSubsetOf(Range::int32()) || !raised.range.isSubsetOf(Range::int32()) || !add(raised, *whole, int64_t { 1 } << *shift))
            return std::nullopt;
        return raised;
    }
    default:
        return readFactor(bound);
    }
}

static constexpr unsigned fewestDigitBitsOfWideProduct = 16;
static constexpr unsigned mostDigitBitsOfWideProduct = 30;
static constexpr size_t mostTermsOfCarryInWideProduct = 6;

static bool collectTermsOfCarry(Walker& walker, ValueID written, Vector<ValueID, mostTermsOfCarryInWideProduct>& terms)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID value = index.stripCasts(written);
    Reference bound = walker.referenceBoundTo(value);
    if (!bound || !bound.isRecord() || !walker.types().at(walker.typeOf(bound)).isInteger())
        return false;
    if (!index.isParameter(value) && index.opcodeOf(value) == Opcode::Add) {
        BodyValue sum = index.value(value);
        return collectTermsOfCarry(walker, sum.operand(0), terms) && collectTermsOfCarry(walker, sum.operand(1), terms);
    }
    if (terms.size() == mostTermsOfCarryInWideProduct)
        return false;
    terms.append(value);
    return true;
}

static bool isShiftOfSum(const BodyIndex& index, ValueID written)
{
    ValueID value = index.stripCasts(written);
    if (index.isParameter(value) || index.opcodeOf(value) != Opcode::RShift)
        return false;
    ValueID shifted = index.stripCasts(index.value(value).operand(0));
    return !index.isParameter(shifted) && index.opcodeOf(shifted) == Opcode::Add;
}

static bool holdsShiftOfSum(const BodyIndex& index, ValueID written, unsigned depth = 0)
{
    if (isShiftOfSum(index, written))
        return true;
    ValueID value = index.stripCasts(written);
    if (depth == 2 || index.isParameter(value) || index.opcodeOf(value) != Opcode::Add)
        return false;
    BodyValue sum = index.value(value);
    return holdsShiftOfSum(index, sum.operand(0), depth + 1) || holdsShiftOfSum(index, sum.operand(1), depth + 1);
}

static bool isTermOfSum(const BodyIndex& index, ValueID written, ValueID term, unsigned depth = 0)
{
    ValueID value = index.stripCasts(written);
    if (value == term)
        return true;
    if (depth == 4 || index.isParameter(value) || index.opcodeOf(value) != Opcode::Add)
        return false;
    BodyValue sum = index.value(value);
    return isTermOfSum(index, sum.operand(0), term, depth + 1) || isTermOfSum(index, sum.operand(1), term, depth + 1);
}

static unsigned bitsShiftedOffCarry(const BodyIndex& index, ValueID written, ValueID parameter, unsigned depth = 0)
{
    ValueID value = index.stripCasts(written);
    if (isShiftOfSum(index, value)) {
        BodyValue shift = index.value(value);
        ValueID bits = index.stripCasts(shift.operand(1));
        bool isWritten = !index.isParameter(bits) && index.opcodeOf(bits) == Opcode::Integer && isTermOfSum(index, shift.operand(0), parameter);
        int64_t written = isWritten ? index.value(bits).as<Opcode::Integer>().integer() : 0;
        return written > 0 && written <= mostDigitBitsOfWideProduct ? written : 0;
    }
    if (depth == 2 || index.isParameter(value) || index.opcodeOf(value) != Opcode::Add)
        return 0;
    BodyValue sum = index.value(value);
    return std::max(bitsShiftedOffCarry(index, sum.operand(0), parameter, depth + 1), bitsShiftedOffCarry(index, sum.operand(1), parameter, depth + 1));
}

std::optional<unsigned> widthOfCarriedDigit(const BodyIndex& index, ValueID carried, ValueID parameter)
{
    unsigned bits = bitsShiftedOffCarry(index, carried, parameter);
    return bits >= fewestDigitBitsOfWideProduct ? std::optional<unsigned>(std::min(bits + 2, 31u)) : std::nullopt;
}

static Reference guardFactorOfWideProduct(Walker& walker, const FactorInWideProduct& factor, Origin origin)
{
    if (walker.types().at(walker.typeOf(factor.value)).integers.isSubsetOf(factor.needed))
        return factor.value;
    bool isInvariant = walker.isInvariantInSpecializedVersion(factor.value);
    Reference test;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isInvariant);
        test = buildTestOfNumber(walker, factor.value, walker.types().integer(factor.needed), origin);
    }
    if (!test || !test.isValue() || walker.isUnreachable() || walker.hasFailed())
        return { };
    bool isInPreheader = isInvariant && walker.tryBranchInPreheader(test, true);
    if (!isInPreheader) {
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
        walker.letGuessFailUnasked(failed, origin);
        walker.branch(test, passed, failed, true);
        walker.continueIn(failed);
        if (!walker.isUnreachable())
            walker.restartIteration();
        walker.continueIn(passed);
    }
    walker.recordSpeculation();
    if (walker.isUnreachable() || walker.hasFailed())
        return { };
    Walker::SpecializedPreheaderScope preheaderScope(walker, isInPreheader);
    return buildProofOfNumber(walker, factor.value, test, origin);
}

Reference buildWideProduct(Walker& walker, const BodyValue& sum)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.isInLoop() || !walker.opensGuesses() || !(holdsShiftOfSum(index, sum.operand(0)) || holdsShiftOfSum(index, sum.operand(1))))
        return { };
    if (!walker.has(Mechanism::Guesses) || !walker.has(Mechanism::Folding) || !walker.canRestartIteration() || !walker.canBuildIterationGuard())
        return { };
    Vector<ValueID, mostTermsOfCarryInWideProduct> terms;
    if (!collectTermsOfCarry(walker, sum.operand(0), terms) || !collectTermsOfCarry(walker, sum.operand(1), terms))
        return { };

    ValueID shiftOfLow;
    unsigned digitBits = 0;
    for (ValueID term : terms) {
        if (!isShiftOfSum(index, term))
            continue;
        ValueID shift = index.stripCasts(index.value(term).operand(1));
        int64_t bits = !index.isParameter(shift) && index.opcodeOf(shift) == Opcode::Integer ? index.value(shift).as<Opcode::Integer>().integer() : 0;
        if (bits > static_cast<int64_t>(digitBits) && bits <= mostDigitBitsOfWideProduct) {
            digitBits = bits;
            shiftOfLow = term;
        }
    }
    if (digitBits < fewestDigitBitsOfWideProduct)
        return { };

    ValueID low = index.stripCasts(index.value(shiftOfLow).operand(0));
    auto readWhole = [&](WideProductReader& reader) -> std::optional<SumInWideProduct> {
        std::optional<SumInWideProduct> whole = reader.read(low);
        if (!whole || !whole->range.isSubsetOf(Range::int32()))
            return std::nullopt;
        Range carries { whole->range.minimum >> digitBits, whole->range.maximum >> digitBits };
        for (ValueID term : terms) {
            if (term == shiftOfLow)
                continue;
            std::optional<SumInWideProduct> high = reader.read(term);
            if (!high || !reader.add(*whole, *high, int64_t { 1 } << digitBits))
                return std::nullopt;
            carries = { carries.minimum + high->range.minimum, carries.maximum + high->range.maximum };
        }
        return carries.isSubsetOf(Range::exact()) ? whole : std::nullopt;
    };
    std::optional<WideProductReader> foundReader;
    std::optional<SumInWideProduct> whole;
    for (unsigned extraBits : { 2u, 1u, 0u }) {
        foundReader.emplace(walker, digitBits, extraBits);
        whole = readWhole(*foundReader);
        if (whole)
            break;
    }
    if (!whole)
        return { };
    WideProductReader& reader = *foundReader;

    Vector<TermInWideProduct, 2> products;
    Vector<uint8_t, 2> addends;
    for (const TermInWideProduct& term : whole->terms) {
        bool namesFloor = !term.factors[1] || reader.factorAt(term.factors[1]).shift || (term.factors[0] && reader.factorAt(term.factors[0]).shift);
        if (namesFloor)
            return { };
        if (term.factors[0] && products.size() < 2)
            products.append(term);
        else if (!term.factors[0] && term.coefficient == 1 && addends.size() < 2)
            addends.append(term.factors[1]);
        else
            return { };
    }
    if (products.isEmpty() || addends.isEmpty())
        return { };
    uint8_t multiplier = 0;
    std::array<uint8_t, 2> halves { };
    unsigned shiftOfHighHalf = 0;
    if (products.size() == 1) {
        if (products[0].coefficient != 1)
            return { };
        halves[0] = products[0].factors[0];
        multiplier = products[0].factors[1];
    } else {
        if (products[0].coefficient != 1)
            std::swap(products[0], products[1]);
        uint64_t scale = products[1].coefficient;
        if (products[0].coefficient != 1 || products[1].coefficient <= 1 || !hasOneBitSet(scale))
            return { };
        for (uint32_t place : { 0u, 1u }) {
            uint8_t shared = products[0].factors[place];
            if (shared != products[1].factors[0] && shared != products[1].factors[1])
                continue;
            multiplier = shared;
            halves = { products[0].factors[1 - place], shared == products[1].factors[0] ? products[1].factors[1] : products[1].factors[0] };
        }
        shiftOfHighHalf = WTF::fastLog2(scale);
        if (!multiplier || shiftOfHighHalf >= digitBits)
            return { };
        const FactorInWideProduct& high = reader.factorAt(halves[1]);
        Range told = walker.types().at(walker.typeOf(high.value)).integers.intersectWith(high.needed);
        Range raised { told.minimum * (int64_t { 1 } << shiftOfHighHalf), told.maximum * (int64_t { 1 } << shiftOfHighHalf) };
        if (told.isEmpty() || !raised.isSubsetOf(Range::int32()))
            return { };
    }

    Origin origin = walker.originOf(sum);
    origin.isTailCall = false;
    for (FactorInWideProduct& factor : reader.factors()) {
        if (factor.shift)
            continue;
        factor.value = guardFactorOfWideProduct(walker, factor, origin);
        if (!factor.value)
            return { };
    }
    Reference multiplicand = reader.factorAt(halves[0]).value;
    if (halves[1]) {
        bool isInvariant = walker.isInvariantInSpecializedVersion(multiplicand) && walker.isInvariantInSpecializedVersion(reader.factorAt(halves[1]).value);
        Walker::SpecializedPreheaderScope preheaderScope(walker, isInvariant);
        Reference raised = buildOperatorInNumberForms(walker, Opcode::LShift, reader.factorAt(halves[1]).value, integerInNumberForms(walker, shiftOfHighHalf), origin);
        multiplicand = raised ? buildOperatorInNumberForms(walker, Opcode::Add, multiplicand, raised, origin) : Reference();
    }
    if (!multiplicand || !walker.types().at(walker.typeOf(multiplicand)).isInteger())
        return { };
    Draft wide(Opcode::MultiplyAddIntegers, origin);
    walker.chooseConservativeFields(wide);
    wide.setMultiplicandOperand(multiplicand);
    wide.setMultiplierOperand(reader.factorAt(multiplier).value);
    wide.setAddendOperand(reader.factorAt(addends[0]).value);
    if (addends.size() == 2)
        wide.setCarryOperand(reader.factorAt(addends[1]).value);
    const Type& sums = walker.types().at(walker.resultTypeOf(wide));
    if (!sums.isInteger() || !sums.integers.isSubsetOf({ -(int64_t { 1 } << 61), int64_t { 1 } << 61 }))
        return { };
    Reference built = walker.build(WTF::move(wide));
    if (!built || !built.isValue())
        return { };
    Draft carry(Opcode::DivideIntegers, origin);
    walker.chooseConservativeFields(carry);
    carry.setDividendOperand(built);
    carry.setDivisorOperand(integerInNumberForms(walker, int64_t { 1 } << digitBits));
    carry.setRounding(QuotientRounding::Floor);
    Reference result = walker.build(WTF::move(carry));
    if (!result)
        return { };
    walker.count(StatisticID::SplitProductsWidened);
    Reference narrow = walker.referenceBoundTo(low);
    noteKnown(walker, KnownNote::WideSum, narrow, { }, 0, LocationMasks(), built.bits());
    noteKnown(walker, KnownNote::LowBitsOfWideSum, narrow, { }, 0, LocationMasks(), digitBits);
    return result;
}

Reference buildLowBitsOfWideProduct(Walker& walker, Reference narrow, Reference mask, Origin origin)
{
    if (!narrow || !narrow.isValue())
        return { };
    std::optional<uint64_t> wide = findKnown(walker, KnownNote::WideSum, narrow);
    std::optional<uint64_t> bits = wide ? findKnown(walker, KnownNote::LowBitsOfWideSum, narrow) : std::nullopt;
    std::optional<int64_t> known = bits ? integerKnownInNumberForms(walker, mask) : std::nullopt;
    if (!known || *known < 0 || *known >= int64_t { 1 } << *bits)
        return { };
    return buildOperatorInNumberForms(walker, Opcode::BitAnd, Reference::fromBits(static_cast<uint32_t>(*wide)), mask, origin);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
