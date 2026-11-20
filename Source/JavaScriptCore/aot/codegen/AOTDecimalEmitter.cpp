/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDecimalEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTGlobalBindings.h"
#include "AOTRuntimeCallEmitter.h"
#include "DecimalObject.h"

namespace JSC { namespace AOT { namespace Typed {

static ptrdiff_t offsetOfDecimalMethod(DecimalOperation operation)
{
    return AOT_INSTANCE_OFFSET(decimals.methods) + static_cast<unsigned>(operation) * sizeof(EncodedJSValue);
}

LBasicBlock DecimalEmitter::FastPathGenerator::newBlock()
{
    // The block that is being built is in front of every way to it.
    Block block = out().newBlock();
    scopesOfBlocksToCome.append({ block, out().forkScope() });
    return block;
}

void DecimalEmitter::FastPathGenerator::appendTo(Block block)
{
    size_t place = scopesOfBlocksToCome.findIf([&](auto& each) { return each.first == block; });
    ScopeFork scope = scopesOfBlocksToCome[place].second;
    scopesOfBlocksToCome.removeAt(place);
    out().appendToJoin(block, scope);
}

LValue DecimalEmitter::FastPathGenerator::addressInTables(ptrdiff_t offset, Value index)
{
    return out().add(out().add(emitter.loadImmutablePointer(AOT_INSTANCE_OFFSET(decimalTables.tables)).value(), index), offset);
}

LValue DecimalEmitter::FastPathGenerator::loadFromTables(Value address, ptrdiff_t offset)
{
    return out().load64(out().address(emitter.m_heaps.AOTImage_tables, address, offset));
}

DecimalEmitter::DecimalEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
    , m_runs(m_program, m_plan, m_facts, m_code, types())
{
}

std::optional<DecimalRun> DecimalEmitter::findRun(BlockID block, std::span<const ValueID> values)
{
    const Terminator& terminator = m_code.at(block).terminator;
    bool mayReturn = terminator.kind == TerminatorKind::ReturnCall && emitter<AccessEmitter>().returnsOneBoxedValue();
    return m_runs.find(block, values, mayReturn ? terminator.callOperand() : ValueID(), m_reused);
}

const ReusedDecimals& DecimalEmitter::findReusedDecimals(std::span<const BlockID> order, const ScopedLambda<bool(BlockID, BlockID)>& dominates)
{
    m_reused = ReusedDecimals(m_code, m_runs, order, dominates);
    if (m_reused.numberOfObjects())
        m_reusedObjects = m_proc.addStackSlot(m_reused.numberOfObjects() * sizeof(EncodedJSValue));
    return m_reused;
}

TypedPointer DecimalEmitter::wordThatNamesObjectOf(unsigned object)
{
    return m_out.address(m_heaps.reusedDecimals, m_out.slotBase(m_reusedObjects), object * sizeof(EncodedJSValue));
}

TypedPointer DecimalEmitter::wordOfReusedObject(CellPointer cell, ptrdiff_t offset)
{
    return m_out.address(m_heaps.reusedDecimals, cell.value(), offset);
}

void DecimalEmitter::beginWithoutReusedObjects()
{
    for (unsigned object = 0; object < m_reused.numberOfObjects(); ++object)
        m_out.store64(m_out.int64Zero, wordThatNamesObjectOf(object));
}

void DecimalEmitter::letGo(ValueID value)
{
    Machine::DeclaredAccessesSuspension suspension(m_out);
    std::array<Operand, 2> arguments { Pointer(m_out.valueOf(wordThatNamesObjectOf(m_reused.objectOf(value)))), generator().getReusedDecimal(value) };
    callLeaf(RuntimeOperation::LetGoOfReusedDecimal, arguments, Representation::Void);
}

void DecimalEmitter::beginRun(const DecimalRun& run, BlockID block, LBasicBlock asWritten)
{
    // By this the instance fills its words.
    m_records.totals.entriesUsed.set(static_cast<size_t>(AOT::Entry::operationAOTNewDecimal));
    m_run = &run;
    m_asWritten = asWritten;
    m_computesInLine = generator().runsRepeatedly(block) || !function().isModuleBody();
    m_classIsKnownToBeAsMade = false;
    m_decimals.shrink(0);
    m_truths.shrink(0);
}

void DecimalEmitter::leaveUnless(Condition condition)
{
    LBasicBlock holds = newBlock();
    m_out.branch(condition.value(), normalEdge(holds), m_run->isLikely ? rareEdge(m_asWritten) : normalEdge(m_asWritten));
    m_out.appendToContinuation(holds);
}

void DecimalEmitter::requireClassAsMade()
{
    if (!std::exchange(m_classIsKnownToBeAsMade, true))
        leaveUnless(isNotZero(loadMutable32(AOT_INSTANCE_OFFSET(decimals.structureID))));
}

void DecimalEmitter::requireWordOfClass(Boxed value, ptrdiff_t offsetInInstance)
{
    leaveUnless(Condition(m_out.equal(value.value(), m_out.load64(mutableInstanceField(offsetInInstance)))));
    m_classIsKnownToBeAsMade = true;
}

auto DecimalEmitter::decimalIn(ValueID value) -> KnownDecimal
{
    ValueID origin = m_runs.originOf(value);
    if (auto known = std::ranges::find(m_decimals, origin, &KnownDecimal::origin); known != m_decimals.end())
        return *known;

    bool isReused = m_reused.contains(origin);
    if (isReused)
        value = origin;
    Boxed word = isReused ? generator().getReusedDecimal(value) : boxed(value);
    if (uint16_t primitives = typeOf(value).primitives)
        leaveUnless(isCellAmong(word, primitives));
    CellPointer cell = unboxCell(word);
    leaveUnless(equal(loadStructureID(cell), loadMutable32(AOT_INSTANCE_OFFSET(decimals.structureID))));
    m_classIsKnownToBeAsMade = true;
    if (isReused)
        m_decimals.append({ origin, { m_out.load64(wordOfReusedObject(cell, DecimalObject::offsetOfLow())), m_out.load64(wordOfReusedObject(cell, DecimalObject::offsetOfUpper())) }, TrailingZeros::Some });
    else
        m_decimals.append({ origin, { m_out.load64(cell.value(), m_heaps.DecimalObject_low), m_out.load64(cell.value(), m_heaps.DecimalObject_upper) }, TrailingZeros::None });
    return m_decimals.last();
}

void DecimalEmitter::stripTrailingZerosOf(ValueID value)
{
    KnownDecimal& known = *std::ranges::find(m_decimals, m_runs.originOf(value), &KnownDecimal::origin);
    if (std::exchange(known.trailingZeros, TrailingZeros::None) == TrailingZeros::Some)
        known.words = m_fastPaths.withoutTrailingZeros(known.words);
}

Operand DecimalEmitter::argument(ValueID value)
{
    if (const Operation& constant = operationAt(m_runs.originOf(value)); constant.opcode == Opcode::Integer && isRepresentableAs<int32_t>(constant.integer()))
        return word32(static_cast<int32_t>(constant.integer()));
    if (!m_run->has(value, DecimalRun::Step::IsReadThrough))
        return get(value);
    ValueID integer = operationAt(value).valueOperand();
    if (std::optional<Boxed> word = generator().findBoxed(integer))
        return *word;
    return get(integer);
}

Boxed DecimalEmitter::boxedArgument(const ValueContext& context, ValueID value)
{
    Operand operand = argument(value);
    return operand.is<Boxed>() ? operand.as<Boxed>() : emitter<ConversionEmitter>().box(context, operand, typeOf(value));
}

auto DecimalEmitter::callForWords(const ValueContext& context, RuntimeOperation operation, std::initializer_list<Operand> arguments) -> Words
{
    StubCallResult result = emitter<RuntimeCallEmitter>().callOperationReturningPair(context, operation, std::span<const Operand>(arguments.begin(), arguments.size()), { });
    return { result.first, result.second };
}

LValue DecimalEmitter::areOfDecimal(Words words)
{
    return m_out.notEqual(words.upper, m_out.constInt64(Decimal128::upperOfNoValue));
}

auto DecimalEmitter::wordsOrLeave(Words words) -> Words
{
    leaveUnless(Condition(areOfDecimal(words)));
    return words;
}

auto DecimalEmitter::wordsOfEither(NOESCAPE const Invocable<Words(LBasicBlock)> auto& first, NOESCAPE const Invocable<Words()> auto& otherwise) -> Words
{
    LBasicBlock otherCase = m_fastPathGenerator.newBlock();
    LBasicBlock continuation = m_fastPathGenerator.newBlock();
    auto handOn = [&](Words words) {
        std::pair anchors { m_out.anchor(words.low), m_out.anchor(words.upper) };
        m_out.jump(continuation);
        return anchors;
    };
    auto [low, upper] = handOn(first(otherCase));
    m_fastPathGenerator.appendTo(otherCase);
    auto [otherLow, otherUpper] = handOn(otherwise());
    m_fastPathGenerator.appendTo(continuation);
    return { m_out.phi(Machine::Int64, low, otherLow), m_out.phi(Machine::Int64, upper, otherUpper) };
}

auto DecimalEmitter::wordsInLineOr(NOESCAPE const Invocable<Words(LBasicBlock)> auto& inLine, NOESCAPE const Invocable<Words()> auto& byCall) -> Words
{
    return m_computesInLine ? wordsOfEither(inLine, byCall) : byCall();
}

auto DecimalEmitter::decimalOfInteger(const ValueContext& context, Integer integer, bool mayBeNegative, TrailingZeros result) -> Words
{
    if (result == TrailingZeros::Some)
        return m_fastPaths.fromInteger(integer.value(), mayBeNegative, result, nullptr);
    return wordsInLineOr([&](LBasicBlock slowCase) {
        return m_fastPaths.fromInteger(integer.value(), mayBeNegative, result, slowCase);
    }, [&] {
        return callForWords(context, RuntimeOperation::DecimalOfInteger, { integer });
    });
}

auto DecimalEmitter::decimalOfAnyValue(const ValueContext& context, Boxed value, const Type& type) -> Words
{
    Words words = callForWords(context, RuntimeOperation::DecimalOfPrimitive, { value });
    if (!(type.primitives & (Primitive::AnyString | Primitive::BigInt)))
        return type.isNumber() ? words : wordsOrLeave(words);
    return wordsOfEither([&](LBasicBlock asksAgain) {
        branchUnless(Condition(areOfDecimal(words)), asksAgain);
        return words;
    }, [&] {
        return wordsOrLeave(callForWords(context, RuntimeOperation::DecimalOfRopeOrBigInt, { value }));
    });
}

auto DecimalEmitter::construct(const ValueContext& context, TrailingZeros result) -> Words
{
    ValueID source = context->argumentsOperands()[0];
    Type type = typeOf(source);
    Operand operand = argument(source);
    if (operand.is<Word32>())
        return decimalOfInteger(context, signExtended(operand.as<Word32>()), type.integers.minimum < 0, result);
    if (operand.is<Integer>() && type.integers.isSubsetOf(Range::exact()))
        return decimalOfInteger(context, operand.as<Integer>(), type.integers.minimum < 0, result);

    Boxed value = boxedArgument(context, source);
    if (!(type.primitives & Primitive::Integer) || !m_computesInLine)
        return decimalOfAnyValue(context, value, type);
    return wordsOfEither([&](LBasicBlock isNoInteger) {
        LBasicBlock isInteger = m_fastPathGenerator.newBlock();
        m_fastPathGenerator.branch(isInt32(value).value(), isInteger, isNoInteger);
        m_fastPathGenerator.appendTo(isInteger);
        return decimalOfInteger(context, signExtended(unboxInt32(value)), true, result);
    }, [&] {
        return decimalOfAnyValue(context, value, type);
    });
}

auto DecimalEmitter::scale(const ValueContext& context, const DecimalCall& call, TrailingZeros result) -> Words
{
    KnownDecimal x = decimalIn(call.receiver);
    bool rounds = call.operation == DecimalOperation::Round;
    auto byCallOf = [&](RuntimeOperation operation, auto... words) {
        Boxed count = boxedArgument(context, call.count);
        Words result = rounds
            ? callForWords(context, operation, { Integer(words)..., count, word32(static_cast<int32_t>(call.mode)) })
            : callForWords(context, operation, { Integer(words)..., count });
        return typeOf(call.count).isInteger() ? result : wordsOrLeave(result);
    };
    if (x.divisor)
        return byCallOf(RuntimeOperation::DecimalDivideAndRound, x.words.low, x.words.upper, x.divisor->low, x.divisor->upper);

    Operand count = argument(call.count);
    return wordsInLineOr([&](LBasicBlock slowCase) {
        Word32 narrow;
        if (count.is<Word32>())
            narrow = count.as<Word32>();
        else if (count.is<Integer>()) {
            narrow = Word32(m_out.castToInt32(count.value));
            branchUnless(equal(signExtended(narrow), count.as<Integer>()), slowCase);
        } else {
            branchUnless(isInt32(count.as<Boxed>()), slowCase);
            narrow = unboxInt32(count.as<Boxed>());
        }
        return rounds ? m_fastPaths.round(x.words, x.trailingZeros, narrow.value(), call.mode, result, slowCase) : m_fastPaths.scale10(x.words, narrow.value(), slowCase);
    }, [&] {
        return byCallOf(rounds ? RuntimeOperation::DecimalRound : RuntimeOperation::DecimalScale10, x.words.low, x.words.upper);
    });
}

auto DecimalEmitter::compute(const ValueContext& context, const DecimalCall& call, TrailingZeros result) -> Words
{
    Words x = decimalIn(call.receiver).words;
    Words y = call.otherDecimal ? decimalIn(call.otherDecimal).words : Words { };
    auto byCallOf = [&](RuntimeOperation operation) {
        return [&, operation] {
            return callForWords(context, operation, { Integer(x.low), Integer(x.upper), Integer(y.low), Integer(y.upper) });
        };
    };
    auto isTotal = [&](ValueID operand) {
        return result == TrailingZeros::Some && m_reused.shareObject(operand, context.id());
    };
    switch (call.operation) {
    case DecimalOperation::Add:
        return wordsInLineOr([&](LBasicBlock slowCase) {
            if (isTotal(call.receiver))
                return m_fastPaths.addToTotal(x, y, false, slowCase);
            return isTotal(call.otherDecimal) ? m_fastPaths.addToTotal(y, x, false, slowCase) : m_fastPaths.add(x, y, result, slowCase);
        }, byCallOf(RuntimeOperation::DecimalAdd));
    case DecimalOperation::Subtract:
        return wordsInLineOr([&](LBasicBlock slowCase) {
            return isTotal(call.receiver) ? m_fastPaths.addToTotal(x, y, true, slowCase) : m_fastPaths.subtract(x, y, result, slowCase);
        }, byCallOf(RuntimeOperation::DecimalSubtract));
    case DecimalOperation::Divide: {
        std::optional<Words> reciprocal = m_fastPaths.reciprocalOf(y);
        if (!reciprocal)
            return byCallOf(RuntimeOperation::DecimalDivide)();
        y = *reciprocal;
        [[fallthrough]];
    }
    case DecimalOperation::Multiply:
        return wordsInLineOr([&](LBasicBlock slowCase) { return m_fastPaths.multiply(x, y, result, slowCase); }, byCallOf(RuntimeOperation::DecimalMultiply));
    case DecimalOperation::Remainder:
        return byCallOf(RuntimeOperation::DecimalRemainder)();
    case DecimalOperation::Abs:
        return m_fastPaths.abs(x);
    case DecimalOperation::Negate:
        return m_fastPaths.negate(x);
    case DecimalOperation::Mantissa:
        return callForWords(context, RuntimeOperation::DecimalMantissa, { Integer(x.low), Integer(x.upper) });
    case DecimalOperation::Round:
    case DecimalOperation::Scale10:
        return scale(context, call, result);
    case DecimalOperation::Equals:
    case DecimalOperation::NotEquals:
    case DecimalOperation::LessThan:
    case DecimalOperation::LessThanOrEqual:
    case DecimalOperation::GreaterThan:
    case DecimalOperation::GreaterThanOrEqual:
    case DecimalOperation::IsNaN:
    case DecimalOperation::IsFinite:
    case DecimalOperation::Compare:
    case DecimalOperation::Exponent:
    case DecimalOperation::ToString:
    case DecimalOperation::ToExponential:
    case DecimalOperation::ToFixed:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename T>
T DecimalEmitter::compare(const DecimalCall& call, NOESCAPE const Invocable<T(LValue, LValue)> auto& ofIntegers, NOESCAPE const Invocable<T(Integer)> auto& ofOrder)
{
    Words x = decimalIn(call.receiver).words;
    Words y = decimalIn(call.otherDecimal).words;
    auto byCall = [&] {
        return ofOrder(callLeaf<Integer>(RuntimeOperation::DecimalCompare, { Integer(x.low), Integer(x.upper), Integer(y.low), Integer(y.upper) }));
    };
    if (!m_computesInLine)
        return byCall();

    LBasicBlock slowCase = m_fastPathGenerator.newBlock();
    ScopeFork scope = m_out.forkScope();
    Join<T> join(m_out);
    m_fastPaths.compare(x, y, slowCase, [&](LValue left, LValue right) {
        join.add(ofIntegers(left, right));
    });
    m_fastPathGenerator.appendTo(slowCase);
    join.add(byCall());
    return join.finishBehind(scope);
}

Condition DecimalEmitter::test(DecimalOperation operation, const DecimalCall& call)
{
    static_assert(unorderedDecimals == 2);
    switch (operation) {
    case DecimalOperation::IsNaN:
        return Condition(m_fastPaths.isNaN(decimalIn(call.receiver).words));
    case DecimalOperation::IsFinite:
        return Condition(m_fastPaths.isFinite(decimalIn(call.receiver).words));
    case DecimalOperation::Equals:
        return Condition(m_fastPaths.equals(decimalIn(call.receiver).words, decimalIn(call.otherDecimal).words));
    case DecimalOperation::NotEquals:
        return Condition(m_fastPaths.notEquals(decimalIn(call.receiver).words, decimalIn(call.otherDecimal).words));
    case DecimalOperation::LessThan:
        return compare<Condition>(call, [&](LValue x, LValue y) { return Condition(m_out.lessThan(x, y)); }, [&](Integer order) { return lessThan(order, 0); });
    case DecimalOperation::LessThanOrEqual:
        return compare<Condition>(call, [&](LValue x, LValue y) { return Condition(m_out.lessThanOrEqual(x, y)); }, [&](Integer order) { return lessThanOrEqual(order, 0); });
    case DecimalOperation::GreaterThan:
        return compare<Condition>(call, [&](LValue x, LValue y) { return Condition(m_out.greaterThan(x, y)); }, [&](Integer order) { return equal(order, 1); });
    case DecimalOperation::GreaterThanOrEqual:
        return compare<Condition>(call, [&](LValue x, LValue y) { return Condition(m_out.greaterThanOrEqual(x, y)); }, [&](Integer order) { return belowOrEqual(order, 1); });
    case DecimalOperation::Add:
    case DecimalOperation::Subtract:
    case DecimalOperation::Multiply:
    case DecimalOperation::Divide:
    case DecimalOperation::Remainder:
    case DecimalOperation::Abs:
    case DecimalOperation::Negate:
    case DecimalOperation::Mantissa:
    case DecimalOperation::Round:
    case DecimalOperation::Scale10:
    case DecimalOperation::Compare:
    case DecimalOperation::Exponent:
    case DecimalOperation::ToString:
    case DecimalOperation::ToExponential:
    case DecimalOperation::ToFixed:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed DecimalEmitter::answer(const ValueContext& context, const DecimalCall& call)
{
    if (call.operation == DecimalOperation::Compare) {
        return compare<Boxed>(call, [&](LValue x, LValue y) {
            return boxInt32(Word32(m_out.sub(m_out.greaterThan(x, y), m_out.lessThan(x, y))));
        }, [&](Integer order) {
            return Boxed(m_out.select(equal(order, unorderedDecimals).value(), boxedConstant(jsNaN()).value(), boxInt32(Word32(m_out.castToInt32(order.value()))).value()));
        });
    }

    Words x = decimalIn(call.receiver).words;
    if (call.operation == DecimalOperation::Exponent) {
        auto byCall = [&] {
            return callLeaf<Boxed>(RuntimeOperation::DecimalExponent, { Integer(x.low), Integer(x.upper) });
        };
        if (!m_computesInLine)
            return byCall();
        LBasicBlock slowCase = m_fastPathGenerator.newBlock();
        ScopeFork scope = m_out.forkScope();
        Join<Boxed> join(m_out);
        join.add(boxInt32(Word32(m_fastPaths.exponent(x, slowCase))));
        m_fastPathGenerator.appendTo(slowCase);
        join.add(byCall());
        return join.finishBehind(scope);
    }

    if (call.operation != DecimalOperation::ToFixed)
        return boxCell(callOperation<CellPointer>(context, call.operation == DecimalOperation::ToString ? RuntimeOperation::DecimalToString : RuntimeOperation::DecimalToExponential, { Integer(x.low), Integer(x.upper) }));

    Boxed digits = call.options ? boxedArgument(context, call.count) : boxedConstant(jsNumber(0));
    Boxed mode = call.modeInOptions ? boxedArgument(context, call.modeInOptions) : call.options ? boxedConstant(JSValue()) : undefined();
    Boxed text = boxCell(callOperation<CellPointer>(context, RuntimeOperation::DecimalToFixed, { Integer(x.low), Integer(x.upper), digits, mode }));
    leaveUnless(isNotEmpty(text));
    return text;
}

void DecimalEmitter::call(const ValueContext& context, const DecimalRun::Part& part)
{
    DecimalCall call = *m_runs.callAt(context.id());
    if (!m_run->has(context->calleeOperand(), DecimalRun::Step::ReadsMethod))
        requireWordOfClass(boxed(context->calleeOperand()), offsetOfDecimalMethod(call.operation));
    decimalIn(call.receiver);
    if (call.otherDecimal)
        decimalIn(call.otherDecimal);
    if (!takesTrailingZeros(m_runs.operationAsComputed(*m_run, context.id()))) {
        stripTrailingZerosOf(call.receiver);
        if (call.otherDecimal)
            stripTrailingZerosOf(call.otherDecimal);
    }

    if (call.operation == DecimalOperation::Divide && part.isOnlyRounded && !m_fastPaths.reciprocalOf(decimalIn(call.otherDecimal).words)) {
        m_decimals.append({ context.id(), decimalIn(call.receiver).words, TrailingZeros::Some, decimalIn(call.otherDecimal).words });
        return;
    }
    if (call.givesDecimal()) {
        TrailingZeros result = part.mustLackTrailingZeros ? TrailingZeros::None : TrailingZeros::Some;
        m_decimals.append({ context.id(), compute(context, call, result), result });
        return;
    }
    if (call.givesBoolean()) {
        Condition holds = test(call.operation, call);
        m_truths.append({ context.id(), holds });
        define(context, boxBoolean(holds));
        return;
    }
    if (ValueID user = m_code.onlyUserOf(context.id()); !user || !m_run->has(user, DecimalRun::Step::AsksOrder))
        define(context, answer(context, call));
}

void DecimalEmitter::generate(const ValueContext& context, const DecimalRun::Part& part)
{
    using Step = DecimalRun::Step;
    auto noteConstant = [&](Decimal128 constant) {
        auto [low, upper] = constant.words();
        m_decimals.append({ context.id(), { m_out.constInt64(low), m_out.constInt64(upper) }, TrailingZeros::None });
    };
    switch (part.step) {
    case Step::ReadsMethod:
        decimalIn(context->objectOperand());
        requireClassAsMade();
        if (part.isReadBehindRun)
            define(context, Boxed(m_out.load64(mutableInstanceField(offsetOfDecimalMethod(m_runs.callAt(m_code.onlyUserOf(context.id()))->operation)))));
        return;
    case Step::CallsMethod:
        call(context, part);
        return;
    case Step::ReadsConstructor: {
        Boxed constructor = emitter<AccessEmitter>().constantOfGlobalBindingOrEmpty(context);
        requireWordOfClass(constructor, AOT_INSTANCE_OFFSET(decimals.constructor));
        define(context, constructor);
        return;
    }
    case Step::Constructs: {
        if (!m_run->has(context->calleeOperand(), Step::ReadsConstructor))
            requireWordOfClass(boxed(context->calleeOperand()), AOT_INSTANCE_OFFSET(decimals.constructor));
        if (std::optional<Decimal128> constant = m_runs.constantAt(context.id())) {
            noteConstant(*constant);
            return;
        }
        TrailingZeros result = part.mustLackTrailingZeros ? TrailingZeros::None : TrailingZeros::Some;
        m_decimals.append({ context.id(), construct(context, result), result });
        return;
    }
    case Step::IsLiteral:
        noteConstant(*m_runs.constantAt(context.id()));
        return;
    case Step::AsksTruth:
        define(context, std::ranges::find(m_truths, context->valueOperand(), &std::pair<ValueID, Condition>::first)->second);
        return;
    case Step::AsksOrder:
        define(context, test(*m_runs.testOfOrderAt(context.id()), *m_runs.callAt(context->leftOperand())));
        return;
    case Step::IsReadThrough:
        return;
    case Step::IsMadeTwice:
    case Step::IsMadeAhead:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer DecimalEmitter::allocate(const ValueContext& context, Words words)
{
    if (!m_computesInLine)
        return callOperation<CellPointer>(context, RuntimeOperation::NewDecimal, { Integer(words.low), Integer(words.upper) });

    LBasicBlock slowCase = m_fastPathGenerator.newBlock();
    ScopeFork scope = m_out.forkScope();
    Join<CellPointer> join(m_out);
    CellPointer cell;
    {
        CodeGenerator::InitializationScope initialization(generator());
        cell = emitter<CellAllocationEmitter>().allocateObject(InlineAllocation::Decimal, slowCase);
        m_out.store64(words.low, cell.value(), m_heaps.DecimalObject_low);
        m_out.store64(words.upper, cell.value(), m_heaps.DecimalObject_upper);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    join.add(cell);

    m_fastPathGenerator.appendTo(slowCase);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::NewDecimal, { Integer(words.low), Integer(words.upper) }, { .isOnRarePath = true }));
    return join.finishBehind(scope);
}

void DecimalEmitter::makeObject(const ValueContext& context)
{
    auto known = std::ranges::find(m_decimals, context.id(), &KnownDecimal::origin);
    if (known == m_decimals.end() || generator().wordOf(context.id()))
        return;
    Words words = known->words;
    if (!m_reused.contains(context.id())) {
        define(context, boxCell(allocate(context, words)));
        return;
    }

    LBasicBlock hasNone = m_fastPathGenerator.newBlock();
    ScopeFork scope = m_out.forkScope();
    Join<Boxed> join(m_out);
    unsigned object = m_reused.objectOf(context.id());
    Boxed reused(m_out.load64(wordThatNamesObjectOf(object)));
    branchUnless(isNotEmpty(reused), hasNone);
    m_out.store64(words.low, wordOfReusedObject(unboxCell(reused), DecimalObject::offsetOfLow()));
    m_out.store64(words.upper, wordOfReusedObject(unboxCell(reused), DecimalObject::offsetOfUpper()));
    join.add(reused);

    m_fastPathGenerator.appendTo(hasNone);
    Boxed made = boxCell(allocate(context, words));
    m_out.store64(made.value(), wordThatNamesObjectOf(object));
    join.add(made);
    define(context, join.finishBehind(scope));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
