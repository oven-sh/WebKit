/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiler.h"
#include "AOTEmitter.h"
#include "AOTImage.h"
#include "AOTTypedPasses.h"
#include "B3PatchpointValue.h"
#include "B3ValueInlines.h"

namespace JSC { namespace AOT { namespace Typed {

// A value of the back end in a representation. To take one for another does not compile.
template<Rep representation>
class Lowered {
public:
    static constexpr Rep rep = representation;

    Lowered() = default;
    explicit Lowered(LValue value)
        : m_value(value)
    {
    }

    LValue value() const { return m_value; }
    explicit operator bool() const { return m_value; }

private:
    LValue m_value { nullptr };
};

// A boolean, or 32 bits on the way from or to an integer.
using Word32 = Lowered<Rep::Int32>;
using Integer = Lowered<Rep::Int64>;
using Number = Lowered<Rep::Double>;
using OptionalInteger = Lowered<Rep::OptionalInt64>;
using OptionalNumber = Lowered<Rep::OptionalDouble>;
using CellPointer = Lowered<Rep::Cell>;
using Boxed = Lowered<Rep::JSValue>;

// An address that is no value of the language.
class Pointer {
public:
    Pointer() = default;
    explicit Pointer(LValue value)
        : m_value(value)
    {
    }

    LValue value() const { return m_value; }

private:
    LValue m_value { nullptr };
};

// A value in the representation its type calls for, which is only known when the compiler runs.
struct Operand {
    Operand() = default;
    Operand(Rep rep, LValue value)
        : rep(rep)
        , value(value)
    {
    }
    template<Rep representation>
    Operand(Lowered<representation> lowered)
        : rep(representation)
        , value(lowered.value())
    {
    }

    template<typename T>
    T as() const
    {
        RELEASE_ASSERT(rep == T::rep);
        return T(value);
    }

    Rep rep { Rep::Void };
    LValue value { nullptr };
};

// Where the ways to come by one value meet. In Rep::Void: the ways to do one thing.
class Join {
public:
    Join(Output& out, Rep rep, LType type)
        : m_out(out)
        , m_rep(rep)
        , m_type(type)
        , m_continuation(out.newBlock())
    {
    }

    // Ends the block.
    void add(Operand operand)
    {
        RELEASE_ASSERT(operand.rep == m_rep);
        if (m_rep != Rep::Void)
            m_values.append(m_out.anchor(operand.value));
        m_out.jump(m_continuation);
    }

    Operand finish()
    {
        m_out.appendTo(m_continuation);
        if (m_rep == Rep::Void)
            return { };
        return Operand(m_rep, m_out.phi(m_type, m_values));
    }

private:
    Output& m_out;
    Rep m_rep;
    LType m_type;
    LBasicBlock m_continuation;
    Vector<ValueFromBlock, 4> m_values;
};

template<typename T> struct ParameterOfOperation;
template<> struct ParameterOfOperation<EncodedJSValue> { using Type = Boxed; };
template<> struct ParameterOfOperation<JSCell*> { using Type = CellPointer; };
template<> struct ParameterOfOperation<uint32_t> { using Type = Word32; };
template<> struct ParameterOfOperation<int32_t> { using Type = Word32; };
template<> struct ParameterOfOperation<uint64_t> { using Type = Integer; };
template<> struct ParameterOfOperation<double> { using Type = Number; };
template<> struct ParameterOfOperation<const EncodedJSValue*> { using Type = Pointer; };

template<typename T> struct ResultOfOperation;
template<> struct ResultOfOperation<void> { using Type = void; };
template<> struct ResultOfOperation<EncodedJSValue> { using Type = Boxed; };
template<> struct ResultOfOperation<JSCell*> { using Type = CellPointer; };
template<> struct ResultOfOperation<size_t> { using Type = Word32; };
template<> struct ResultOfOperation<double> { using Type = Number; };
template<> struct ResultOfOperation<void*> { using Type = Pointer; };

template<typename T>
struct PlainResultOfOperation {
    using Type = T;
    static constexpr bool carriesException = false;
};
template<typename T>
struct PlainResultOfOperation<ExceptionOperationResult<T>> {
    using Type = T;
    static constexpr bool carriesException = true;
};

template<typename Function> struct OperationSignature;
template<typename Result, typename... Parameters>
struct OperationSignature<Result(Instance*, Parameters...)> {
    using ResultType = typename ResultOfOperation<typename PlainResultOfOperation<Result>::Type>::Type;
    // A double is handed back as it is even by what throws.
    static constexpr bool mayThrow = PlainResultOfOperation<Result>::carriesException || std::is_same_v<Result, double>;
    static Vector<LValue, 8> list(typename ParameterOfOperation<Parameters>::Type... arguments) { return { arguments.value()... }; }
};
template<typename Result, typename... Parameters>
struct OperationSignature<Result(Parameters...)> {
    using ResultType = typename ResultOfOperation<Result>::Type;
    static constexpr bool mayThrow = false;
    static Vector<LValue, 8> list(typename ParameterOfOperation<Parameters>::Type... arguments) { return { arguments.value()... }; }
};

struct LoweringContext {
    StubCalls stubCalls;
    Vector<ImageKey> knownCallees;
    UncheckedKeyHashMap<uint32_t, uint32_t, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> calleeIndices;
    IntegersThatLookLikeAddresses wideIntegerConstants;
    IndexReferences indexReferences;
    Vector<Site> sites;
    // The names the sites go by.
    Vector<StringID> identifiers;
    // The calls that a handler of the function watches, in the order of their numbers, each with the number of the handler.
    Vector<std::pair<uint32_t, uint32_t>> catchEntrypointsForCallSites;
    unsigned numberOfCatchEntrypoints { 0 };
    bool emitsCalls { false };
    bool readsCallerFrame { false };
};

// What the back end is told of a call.
struct OperationEffects {
    Writes writes { Writes::Heap };
    bool exitsSideways { true };
    // See Function::writesShapeWords. What writes the heap is taken to run code that is not known, unless it says otherwise.
    bool writesShapeWords { writes == Writes::Heap };
    // With Writes::Fields: of all fields it writes order words only.
    bool writesOnlyOrderWords { false };
    // With Writes::TypedArrayProperties: it allocates nothing and comes to no safepoint, so that not even what a write barrier goes by changes.
    bool writesOnlyTypedArrayProperties { false };
};

class Lowering final : public Emitter {
public:
    Lowering(B3::Procedure&, const Program&, FunctionID, LoweringContext&);

    bool run();
    ASCIILiteral whyFailed() const { return m_whyFailed; }
    uint8_t genericEntrypointParameterCount() const { return m_genericEntrypointParameterCount; }

private:
    // Core.
    void fail(ASCIILiteral why)
    {
        if (m_whyFailed.isNull())
            m_whyFailed = why;
    }
    Operand failWith(ASCIILiteral why)
    {
        fail(why);
        return undefined();
    }
    // Of the type it is given, or of the type a test has shown it to be of where this code runs: see Assumption.
    const Type& typeOf(ValueID id) const
    {
        for (size_t i = m_assumptions.size(); i--;) {
            if (m_assumptions[i].value == id)
                return *m_assumptions[i].type;
        }
        return m_program.at(m_function.at(id).type);
    }
    Operand get(ValueID);
    void define(ValueID, Operand);
    static LType b3TypeOf(Rep);
    LBasicBlock newColdBlock();
    std::span<const Target> successorsOf(const Terminator&) const;
    Vector<BlockID> reversePostorder() const;
    void computeLoopBlocks(const Vector<BlockID>& order);
    bool lowerEntryParameters();

    // Exceptions.
    struct Handler {
        BlockID block;
        Vector<ValueID> liveIn;
        unsigned firstParameterWord;
        LBasicBlock entry;
    };
    struct CatchSpill {
        B3::Variable* variable;
        unsigned word;
    };
    void computeCatchEntrypoints(const Vector<BlockID>& order);
    TypedPointer catchSpillSlot(unsigned);
    void storeToCatchSpillSlot(unsigned word, Operand);
    Operand loadFromCatchSpillSlot(unsigned word, Rep);
    void spillForCatch(const Target&);
    void lowerCatchEntrypoint(Handler&);
    void lowerBlock(BlockID);
    void lowerTerminator(const Terminator&);
    LBasicBlock edgeTo(const Target&, bool isBranch);
    Operand lower(ValueID, const Value&);
    Operand lowerLanguageOperation(ValueID result, const Value&);

    template<typename Functor> void require(Word32 condition, const Functor& otherwise);
    template<typename T, typename Fast, typename Slow> T either(Word32 condition, const Fast&, const Slow&);
    template<typename Fast, typename Slow> Operand eitherOperand(Word32 condition, Rep, const Fast&, const Slow&);
    template<typename Fast, typename Slow> Pointer eitherPointer(Word32 condition, const Fast&, const Slow&);
    // Does one of the two: as a rule the first.
    template<typename Usual, typename Rare> void ifElse(Word32 condition, const Usual&, const Rare&);
    // For what has several ways out to one general path: goes on in the block, which newColdBlock() made. The functor ends it.
    template<typename Functor> void inColdBlock(LBasicBlock, const Functor&);

    // Constants and arithmetic on representations.
    Word32 word32(int32_t value) { return Word32(m_out.constInt32(value)); }
    Word32 boolean(bool value) { return word32(value); }
    Integer integer(int64_t);
    Number number(double value) { return Number(m_out.constDouble(value)); }
    Boxed boxedConstant(JSValue value) { return Boxed(m_out.constInt64(JSValue::encode(value))); }
    Boxed undefined() { return boxedConstant(jsUndefined()); }
    // What computes goes through these two, so that it says in what representation it takes its operands and yields its result.
    template<typename Result, typename T> Result unary(LValue (Output::*operation)(LValue), T a) { return Result((m_out.*operation)(a.value())); }
    template<typename Result, typename T> Result binary(LValue (Output::*operation)(LValue, LValue), T a, T b) { return Result((m_out.*operation)(a.value(), b.value())); }

    Integer add(Integer a, Integer b) { return binary<Integer>(&Output::add, a, b); }
    Integer sub(Integer a, Integer b) { return binary<Integer>(&Output::sub, a, b); }
    Integer mul(Integer a, Integer b) { return binary<Integer>(&Output::mul, a, b); }
    // The divisor is not zero.
    Integer mod(Integer a, Integer b) { return binary<Integer>(&Output::mod, a, b); }
    Integer neg(Integer a) { return unary<Integer>(&Output::neg, a); }
    Word32 lessThanOrEqual(Word32 a, Word32 b) { return binary<Word32>(&Output::lessThanOrEqual, a, b); }
    Word32 greaterThanOrEqual(Word32 a, Word32 b) { return binary<Word32>(&Output::greaterThanOrEqual, a, b); }
    Word32 lessThan(Integer a, Integer b) { return binary<Word32>(&Output::lessThan, a, b); }
    Word32 lessThanOrEqual(Integer a, Integer b) { return binary<Word32>(&Output::lessThanOrEqual, a, b); }
    Word32 greaterThan(Integer a, Integer b) { return binary<Word32>(&Output::greaterThan, a, b); }
    Word32 greaterThanOrEqual(Integer a, Integer b) { return binary<Word32>(&Output::greaterThanOrEqual, a, b); }
    Word32 isZero(Integer a) { return unary<Word32>(&Output::isZero64, a); }
    Word32 isNotZero(Integer a) { return unary<Word32>(&Output::notZero64, a); }
    // All bits clear: a hole in an array, a variable that holds nothing yet.
    Word32 isEmpty(Boxed a) { return unary<Word32>(&Output::isZero64, a); }
    Word32 isNotEmpty(Boxed a) { return unary<Word32>(&Output::notZero64, a); }

    Number add(Number a, Number b) { return binary<Number>(&Output::doubleAdd, a, b); }
    Number sub(Number a, Number b) { return binary<Number>(&Output::doubleSub, a, b); }
    Number mul(Number a, Number b) { return binary<Number>(&Output::doubleMul, a, b); }
    Number div(Number a, Number b) { return binary<Number>(&Output::doubleDiv, a, b); }
    Number min(Number a, Number b) { return binary<Number>(&Output::doubleMin, a, b); }
    Number max(Number a, Number b) { return binary<Number>(&Output::doubleMax, a, b); }
    Number neg(Number a) { return unary<Number>(&Output::doubleNeg, a); }
    Number abs(Number a) { return unary<Number>(&Output::doubleAbs, a); }
    Number floor(Number a) { return unary<Number>(&Output::doubleFloor, a); }
    Number ceil(Number a) { return unary<Number>(&Output::doubleCeil, a); }
    Number trunc(Number a) { return unary<Number>(&Output::doubleTrunc, a); }
    Number sqrt(Number a) { return unary<Number>(&Output::doubleSqrt, a); }
    Word32 equal(Number a, Number b) { return binary<Word32>(&Output::doubleEqual, a, b); }
    Word32 notEqualOrUnordered(Number a, Number b) { return binary<Word32>(&Output::doubleNotEqualOrUnordered, a, b); }
    Word32 lessThan(Number a, Number b) { return binary<Word32>(&Output::doubleLessThan, a, b); }
    Word32 lessThanOrEqual(Number a, Number b) { return binary<Word32>(&Output::doubleLessThanOrEqual, a, b); }
    Word32 greaterThan(Number a, Number b) { return binary<Word32>(&Output::doubleGreaterThan, a, b); }
    Word32 greaterThanOrEqual(Number a, Number b) { return binary<Word32>(&Output::doubleGreaterThanOrEqual, a, b); }
    Number toNumber(Word32 a) { return unary<Number>(&Output::intToDouble, a); }
    // By the conversion itself, wherever the integer came from.
    Number convertToNumber(Integer a) { return unary<Number>(&Output::intToDouble, a); }
    Word32 isIntegral(Number value) { return equal(convertToNumber(truncateToInteger(value)), value); }

    Word32 bitAnd(Word32 a, Word32 b) { return binary<Word32>(&Output::bitAnd, a, b); }
    Word32 bitOr(Word32 a, Word32 b) { return binary<Word32>(&Output::bitOr, a, b); }
    Word32 bitXor(Word32 a, Word32 b) { return binary<Word32>(&Output::bitXor, a, b); }
    Integer bitAnd(Integer a, Integer b) { return binary<Integer>(&Output::bitAnd, a, b); }
    Integer bitOr(Integer a, Integer b) { return binary<Integer>(&Output::bitOr, a, b); }
    Integer bitXor(Integer a, Integer b) { return binary<Integer>(&Output::bitXor, a, b); }
    Word32 shiftLeft(Word32 a, Word32 b) { return binary<Word32>(&Output::shl, a, b); }
    Word32 shiftRight(Word32 a, Word32 b) { return binary<Word32>(&Output::aShr, a, b); }
    Word32 shiftRightUnsigned(Word32 a, Word32 b) { return binary<Word32>(&Output::lShr, a, b); }
    Word32 mul(Word32 a, Word32 b) { return binary<Word32>(&Output::mul, a, b); }
    Word32 countLeadingZeros(Word32 a) { return unary<Word32>(&Output::ctlz32, a); }
    Word32 hasBits(Word32 a, uint32_t mask) { return binary<Word32>(&Output::testNonZero32, a, word32(mask)); }
    Word32 lacksBits(Word32 a, uint32_t mask) { return binary<Word32>(&Output::testIsZero32, a, word32(mask)); }

    // Of bits, whatever they stand for. Numbers are compared as numbers, above.
    template<typename T> static constexpr bool isComparedAsBits = !std::is_same_v<T, Number> && !std::is_same_v<T, OptionalNumber>;
    template<typename T> requires isComparedAsBits<T> Word32 equal(T a, T b) { return binary<Word32>(&Output::equal, a, b); }
    template<typename T> requires isComparedAsBits<T> Word32 notEqual(T a, T b) { return binary<Word32>(&Output::notEqual, a, b); }
    template<typename T> requires isComparedAsBits<T> Word32 below(T a, T b) { return binary<Word32>(&Output::below, a, b); }
    template<typename T> requires isComparedAsBits<T> Word32 belowOrEqual(T a, T b) { return binary<Word32>(&Output::belowOrEqual, a, b); }
    template<typename T> requires isComparedAsBits<T> Word32 aboveOrEqual(T a, T b) { return binary<Word32>(&Output::aboveOrEqual, a, b); }

    Word32 logicalNot(Word32 value) { return bitXor(value, word32(1)); }
    Word32 logicalAnd(Word32 a, Word32 b) { return bitAnd(a, b); }
    Word32 logicalOr(Word32 a, Word32 b) { return a ? bitOr(a, b) : b; }
    template<typename T> T select(Word32 condition, T a, T b) { return T(m_out.select(condition.value(), a.value(), b.value())); }
    Word32 isWithin(Integer, Range);

    // Conversions.
    Operand convert(Operand, const Type&, Rep);
    template<typename T> T convertTo(Operand operand, const Type& type) { return convert(operand, type, T::rep).template as<T>(); }
    Operand in(ValueID id, Rep rep) { return convert(get(id), typeOf(id), rep); }
    template<typename T> T in(ValueID id) { return in(id, T::rep).template as<T>(); }
    Operand convertOptional(Operand, const Type&, Rep);
    Integer bitsOf(Operand);
    Operand fromBits(Integer, Rep);
    // What stands for Primitive::Undefined, Null or Absent in a representation that has room for it.
    static int64_t bitsOfMarker(uint16_t kind, Rep);
    Word32 isMarker(Operand);
    Number purified(Number);
    Integer truncateToInteger(Number);
    Number toNumber(Integer);
    Boxed box(Number value) { return Boxed(boxDouble(value.value())); }
    Boxed boxNumber(Number);
    Boxed boxInt32(Word32 value) { return Boxed(Emitter::boxInt32(value.value())); }
    Boxed box(CellPointer value) { return Boxed(value.value()); }
    Number unboxedDouble(Boxed value) { return Number(unboxDouble(value.value())); }
    Word32 unboxedBoolean(Boxed value) { return Word32(unboxBoolean(value.value())); }
    // What stands for a number where it is one.
    Number assumedNumber(OptionalNumber value) { return Number(value.value()); }
    Integer assumedInteger(OptionalInteger value) { return Integer(value.value()); }
    CellPointer asCell(Boxed value)
    {
        m_takenForCells.add(value.value());
        return CellPointer(value.value());
    }
    Integer signExtended(Word32 value) { return Integer(m_out.signExt32To64(value.value())); }
    Integer zeroExtended(Word32 value) { return Integer(m_out.zeroExt(value.value(), B3::Int64)); }
    Word32 low32(Integer value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 low32(Boxed value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 toInt32(Number);
    Word32 toInt32(ValueID);
    Word32 toInt32OrElse(ValueID, LBasicBlock otherwise);

    // Variables.
    TypedPointer addressOf(VariableID);
    void initializeModuleVariables();
    void checkInitialized(Operand raw);
    void findStoresToVariables();
    Operand lowerGetModuleVariable(ValueID result, const Value&);
    void lowerPutModuleVariable(const Value&);
    Operand lowerGetEnvironmentField(ValueID result, const Value&);
    void lowerPutEnvironmentField(const Value&);

    // Tests and checks.
    Word32 isCell(Boxed value) { return Word32(Emitter::isCell(value.value())); }
    Word32 isInt32(Boxed value) { return Word32(Emitter::isInt32(value.value())); }
    Word32 isNumber(Boxed value) { return Word32(Emitter::isNumber(value.value())); }
    Word32 isBoolean(Boxed value) { return Word32(Emitter::isBoolean(value.value())); }
    Word32 isUndefinedOrNull(Boxed value) { return Word32(isOther(value.value())); }
    Word32 typeOfCell(CellPointer);
    Word32 add(Word32 a, Word32 b) { return binary<Word32>(&Output::add, a, b); }
    Word32 isZero(Word32 a) { return unary<Word32>(&Output::isZero32, a); }
    // The 32 bits every cell has at offsetOfShapeWord and at offsetOfOrderWord. The cell may be any object, string or symbol: see AOTTypedData.h.
    Word32 shapeWordOf(CellPointer);
    Word32 orderWordOf(CellPointer);
    Word32 passes(Word32 shapeWord, const ShapeTest&, LBasicBlock otherwise = nullptr);
    Word32 passesOneOf(Word32 shapeWord, const Vector<LayoutID, 1>& views, LBasicBlock otherwise = nullptr);
    Word32 lowerHasShapeOf(const Value&);
    Word32 isClosureOf(CellPointer, FunctionID);
    Word32 lowerIsClosureOf(const Value&);

    // Holding values to types. A test reads the type and the shape word of a cell at most and writes nothing. Where one fails and a
    // TypeError is thrown, nothing comes back: so the back end folds a test that is repeated, and what it has loaded stays loaded.

    // The one way an operation makes sure that what it yields is of the type of its value. The operand is of the type `guarantee`, which is
    // what the operation vouches for by itself, in any representation that holds it. Yields it in repOf(type). Within the type: no code
    // but the conversion. The position of the TypeError is that of the value that is being lowered, which ends its block where a handler watches.
    Operand ensure(Operand, const Type& guarantee, const Type&);
    // Nothing is vouched for.
    Operand ensure(Boxed value, const Type& type) { return ensure(value, m_program.at(m_program.types.anything), type); }
    // Goes on where the operand, of type `from`, is of type `to`, with it in repOf(to). Where it is not, goes to the block.
    Operand fitOrElse(Operand, const Type& from, const Type& to, LBasicBlock otherwise);
    // The same as a boolean.
    Word32 fits(Operand, const Type& from, const Type& to);
    // Ends the block that is being appended to: nothing comes back.
    void throwTypeMismatch(Operand, const Type& from, const Type& to);
    // The receiver of an access or a call, which is cells and maybe `undefined` or `null`: the cell. For those two the TypeError of the language,
    // which names the property if there is a name. The test is isCell() of the value, whoever asks.
    CellPointer nonNil(Operand, const Type&, StringID name);
    CellPointer nonNil(ValueID receiver, StringID name) { return nonNil(get(receiver), typeOf(receiver), name); }
    // Ends the block.
    void throwNullish(Boxed, StringID name);
    // Options::validateAOTTypedFacts(). The operand is the value that has just been lowered.
    void validateFact(Operand, ValueID, const Type&);

    Word32 isCallable(CellPointer);
    Word32 cellFits(CellPointer, const Type& from, const Type& to, LBasicBlock otherwise);
    Word32 nonCellFits(Boxed, const Type& from, const Type& to);
    Operand fitBoxedOrElse(Boxed, const Type& from, const Type& to, LBasicBlock otherwise);
    Operand fitBoxedToIntegerOrElse(Boxed, const Type& from, const Type& to, LBasicBlock otherwise);
    Integer integralOrElse(Number, Range, LBasicBlock otherwise);
    using Emitter::orElse;
    void orElse(Word32 condition, LBasicBlock otherwise) { Emitter::orElse(condition.value(), otherwise); }
    Operand coerce(Operand, const Type& from, const Value& cast);
    bool mayPass(const Type&, const ShapeTest&) const;
    Operand lowerCoercion(ValueID result, const Value& cast);
    Operand lowerNonNull(ValueID result, const Value& cast);
    Operand lowerCast(ValueID result, const Value&);
    Word32 lowerIsUndefinedOrNull(ValueID);
    Word32 lowerToBoolean(ValueID);
    Word32 lowerIsInstanceOfClass(const Value&);

    // Operators.
    Number asNumericOperand(ValueID);
    bool isNumericByItsType(ValueID) const;
    Operand yieldNumeric(Operand computed, const Type& guarantee, const Type&, bool typeIsFact);
    Operand lowerBinary(ValueID result, const Value&);
    Operand lowerNumericBinary(Opcode, ValueID a, ValueID b, const Type& result);
    Operand yieldInt32OrElse(Integer, const Type& result, LBasicBlock otherwise);
    Operand lowerBinaryOfInt32(Opcode, ValueID a, ValueID b, const Type& result, LBasicBlock otherwise);
    Integer lowerBitwise(Opcode, ValueID a, ValueID b);
    Operand lowerUnary(ValueID result, const Value&);
    Word32 lowerRelational(const Value&);
    Word32 lowerStrictEq(ValueID a, ValueID b);
    Word32 lowerLooseEq(ValueID a, ValueID b);
    Word32 toBooleanGenerally(ValueID);
    CellPointer toStringCell(ValueID);

    // Memory.
    enum LoadFlag : uint8_t {
        // It cannot trap wherever it is moved to.
        IsSafe = 1,
        // What it reads never changes.
        IsFixed = 2,
    };
    LValue marked(LValue load, unsigned flags);
    // A field of an object or of a structure of the engine, in the representation asked for.
    template<typename T, typename Base> T loadField(Base, const B3::AbstractHeap&, unsigned flags = 0);
    template<typename Base> Word32 loadByte(Base base, const B3::AbstractHeap& field, unsigned flags = 0) { return Word32(marked(m_out.load8ZeroExt32(base.value(), field), flags)); }
    template<typename T, typename Base> void storeField(Base, const B3::AbstractHeap&, T);
    Word32 lacksBits(Pointer a, uintptr_t mask) { return Word32(m_out.testIsZeroPtr(a.value(), m_out.constIntPtr(mask))); }
    Pointer offsetBy(Pointer base, Integer index, unsigned logSize) { return Pointer(m_out.add(base.value(), logSize ? m_out.shl(index.value(), m_out.constInt32(logSize)) : index.value())); }
    // The size of the smallest object that is looked at as the layout says.
    uint32_t leastSizeOf(LayoutID) const;
    unsigned safeIfReaches(CellPointer, unsigned end) const;
    unsigned safeIfAllMembersHave(CellPointer, const Field&) const;
    Operand load(Store, TypedPointer, unsigned flags);
    void store(Store, Operand, TypedPointer);
    const B3::AbstractHeap& heapOf(SlotID slot) { return m_heaps.AOTTyped_slots[slot.index()]; }
    void barrier(CellPointer owner);
    // After the last store that makes a new cell what it is, before anyone else gets to see it.
    void fenceAfterInitialization();
    // After every initializing store: fences after the last one.
    void didLowerInitializingStore();

    // Accesses.
    Word32 isAbsent(Operand raw);
    bool isLoweringInitializingStore() const { return isInitializingStore(m_program, m_function, *m_currentBlock, m_currentIndexInBlock); }
    // The field an access by name comes down to: the receiver is looked at as one type of the program, which has it. Null: none.
    const Field* fieldFor(const Type& receiver, StringID) const;
    const Field* fieldExpectedFor(const Type& expected, StringID) const;
    CellPointer withFieldOrElse(ValueID object, LayoutID view, const Field&, LBasicBlock otherwise);
    bool hasMemberThatKeepsField(LayoutID view, StringID) const;
    bool mayBeFrozen(LayoutID view) const;
    // Of an object that has the field.
    Operand readField(CellPointer, const Field&, const Type&);
    Word32 hasField(CellPointer, const Field&);
    bool isBornWith(const Allocation&, StringID name) const;
    void writeField(CellPointer, ValueID object, const Field&, ValueID stored);
    bool isAllocatedAsMemberOf(ValueID object, LayoutID view) const;
    // Of an object that is looked at as the view says: one of its members may have no such field, and where the view admits foreign objects
    // it may be any object. `lacks` is for all that is not known to have the field.
    template<typename Has, typename Lacks> Operand throughView(CellPointer, ValueID object, LayoutID view, const Field&, Rep, const Has&, const Lacks&);
    // What goes by the members of the view: `inPlace`. Where the view admits foreign objects, that is only for an object of a member, and
    // `generally` asks any other.
    template<typename InPlace, typename Generally> Operand ifOfMember(CellPointer, ValueID object, LayoutID view, Rep, const InPlace&, const Generally&);
    template<typename Passes, typename Fails> Operand byShape(CellPointer, const ShapeTest&, Rep, const Passes&, const Fails&);
    Operand lowerGetField(ValueID result, const Value&);
    void lowerPutField(ValueID, const Value&);
    Operand lowerGetById(ValueID result, const Value&);
    void lowerPutById(ValueID, const Value&);
    Word32 lowerInById(const Value&);
    Operand lowerNewObject(ValueID, const Value&);
    TypedPointer fieldAt(CellPointer cell, const Field& field) { return m_out.address(heapOf(field.slot), cell.value(), field.offset); }

    // Calls.
    uint32_t nextSite()
    {
        if (m_currentCatchEntrypoint)
            m_context.catchEntrypointsForCallSites.append({ m_nextSite, *m_currentCatchEntrypoint });
        return CallSiteIndex(BytecodeIndex(m_nextSite++)).bits();
    }
    LValue callEntry(Entry, LType, bool mayThrow, OperationEffects, const Vector<LValue, 8>& arguments);
    OperationEffects effectsOfBuiltinCall(const Value&);
    // Every call says what it may change through this, and every store that others can see through didEmitWrite.
    void applyEffects(B3::PatchpointValue*, OperationEffects);
    void didEmitWrite(Writes, bool writesShapeWords = false);
    template<auto operation, typename... Arguments> auto callOperation(Entry, OperationEffects, Arguments...);
    LValue callFunction(FunctionID, LType, const Vector<LValue, 8>& arguments);
    Boxed callValue(Stub, const Vector<LValue, 8>& operands);
    template<typename T, typename Functor> T lazilyInitialized(ptrdiff_t table, uint32_t index, const Functor& fill);
    // By its number among Program::allocations: a cell with its header, and both words if the language sees it as an object. Its fields hold nothing yet.
    CellPointer allocate(uint32_t allocation);
    CellPointer stringConstant(StringID);
    void poll();
    std::pair<Word32, Pointer> argumentBuffer(const Arguments&);
    struct Argument {
        Operand operand;
        const Type* type;
    };
    Argument argument(ValueID id) { return { get(id), &typeOf(id) }; }
    Argument argumentFor(const Function& callee, unsigned parameter, ValueID);
    const Type& typePassedAs(const Function& callee, unsigned index) const;
    // The arguments as the entry block of the function takes them. One that is missing is `undefined`.
    Operand callCompiled(FunctionID, const Vector<Argument, 8>&, const Type& result);
    struct MethodTarget {
        // None: the object has a field of the name, which holds what is called.
        FunctionID function;
        Vector<uint32_t, 4> shapes;
    };
    using MethodTargets = Vector<MethodTarget, 4>;
    // What a call by the name comes down to in the members of the view, each with the shapes for which it does. Nothing: that is not known
    // for all of them, or they are too many.
    std::optional<MethodTargets> methodTargets(LayoutID view, StringID) const;
    // The arguments are those of the call in the source.
    Operand callMethod(FunctionID, CellPointer thisObject, const Arguments&, const Type& result);
    Operand callMethodOfMember(CellPointer, const Type& receiver, const MethodTargets&, const Value&, const Type& result);
    Boxed callGenerally(Boxed callee, Boxed thisValue, const Arguments&);
    Boxed callMethodGenerally(Boxed object, StringID, const Arguments&);
    bool mayBeCalledInPlaceOfItsClosure(FunctionID, unsigned numberOfArguments) const;
    Operand callClosure(FunctionID, CellPointer closure, ValueID thisValue, const Arguments&, const Type& result, LBasicBlock otherwise);
    Operand lowerCallDirect(ValueID result, const Value&);
    Operand lowerCall(ValueID result, const Value&);
    Operand lowerCallMethod(ValueID result, const Value&);
    Operand lowerConstruct(ValueID result, const Value&);
    // The arguments: the function, the object that was made for it, what it is passed.
    Operand lowerConstructWith(ValueID result, const Value&);
    FunctionID genericEntrypointFor(FunctionID);
    CellPointer scopeOfClosure(CellPointer);
    LValue loadRuntimeState32(ptrdiff_t offsetInInstance) { return m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, offsetInInstance)); }
    CellPointer closureConstant(FunctionID);
    CellPointer lowerNewClosure(const Value&);

    // The generic tier.
    void loadICData();
    std::optional<unsigned> allocateSite(StringID name, unsigned extra);
    Pointer slotAddress(unsigned slot);
    LValue callICStub(Stub, LType, const Vector<std::pair<LValue, GPRReg>, 4>& operands, std::optional<std::pair<GPRReg, uint32_t>> immediate = std::nullopt, OperationEffects = { });
    Boxed emitGetById(Boxed base, StringID);
    Boxed emitGetLength(Boxed base);
    void emitPutById(Boxed base, StringID, Boxed value);
    Boxed emitGetByVal(Boxed base, Boxed key);
    void emitPutByVal(Boxed base, Boxed key, Boxed value);
    Boxed emitGenericArithmetic(Stub, Boxed, Boxed, OperationEffects);
    Word32 emitGenericCompare(Stub, Boxed, Boxed, OperationEffects);

    // Predictions. See AOTTypedLoweringGeneric.cpp.

    // What a test has shown of a value. It holds while code is emitted that only runs where the test has passed: there typeOf() and get()
    // answer with it, so that the operation is lowered as it is for an operand whose type says as much.
    struct Assumption {
        ValueID value;
        Operand operand;
        const Type* type;
    };
    struct Expectation {
        ValueID value;
        const Type* type;
        // Of an instance of a class of the engine: it is also to be as the engine makes them, see isAsMade().
        bool isAsMade { false };
    };
    std::optional<Type> predictionOf(ValueID, uint16_t without = Primitive::UndefinedOrNull) const;
    Type expectedTypeOf(ValueID) const;
    static bool canTellWhetherAsMade(const EngineClass&);
    Word32 isAsMade(CellPointer, const EngineClass&);
    Operand expectOrElse(const Expectation&, LBasicBlock otherwise);
    // Where the values are what is expected of them: `inPlace`, for which they are of those types. It is handed the block to go to with
    // whatever it has no code for. There, and where a value is something else: `generally`. Both yield the representation.
    template<typename InPlace, typename Generally> Operand ifAsExpected(const Vector<Expectation, 4>&, Rep, const InPlace&, const Generally&);
    bool areExpectedToBeNumbers(std::initializer_list<ValueID>) const;
    // For operands for which areExpectedToBeNumbers() holds. Where all are int32: `onInt32`, if there is such code, which is handed the
    // block to go to where integers will not do. Where all are numbers: `onNumbers`. Else `generally`.
    template<typename OnInt32, typename OnNumbers, typename Generally> Operand ifNumbersAsExpected(std::initializer_list<ValueID>, Rep, bool hasCodeForInt32, const OnInt32&, const OnNumbers&, const Generally&);

    // Containers. See AOTTypedLoweringContainers.cpp.
    Integer indexOrElse(ValueID key, LBasicBlock otherwise);
    std::optional<Operand> absentElement(const Type&);
    Boxed getByValGenerally(ValueID object, ValueID key);
    void putByValGenerally(ValueID object, ValueID key, ValueID stored);
    Pointer butterflyOf(CellPointer);
    Integer publicLength(Pointer butterfly);
    Integer vectorLength(Pointer butterfly);
    Integer arrayLength(CellPointer);
    Word32 isPlainArray(CellPointer array);
    Word32 isChangedArray(CellPointer);
    Word32 indexingModeOf(CellPointer array, unsigned mask);
    bool mayBeIndex(ValueID key) const;
    Operand getArrayElement(CellPointer array, ValueID key, const Type&, LBasicBlock otherwise = nullptr, bool looksForNumbersLast = false);
    template<typename Functor> void storeBoxedArrayElement(CellPointer array, std::optional<Integer> index, Boxed stored, LBasicBlock otherwise, const Functor& afterwards);
    template<typename Functor> void storeArrayElement(CellPointer array, std::optional<Integer> index, ValueID stored, LBasicBlock otherwise, const Functor& afterwards);
    void putArrayElement(CellPointer array, ValueID key, ValueID stored, LBasicBlock otherwise = nullptr);
    Operand pushArrayElement(CellPointer array, ValueID pushed, const Type&);
    void ensureStillAliveHere(CellPointer owner);
    struct TypedArrayReceiver {
        // What mode, length and vector are loaded from, with these LoadFlags.
        CellPointer view;
        unsigned flags { 0 };
        // Of a value that may be `undefined` or `null`: whether it is neither. If it is one of them, `view` is something else that can be loaded from.
        Word32 isView;
        Boxed value;
    };
    TypedArrayReceiver typedArrayReceiver(ValueID);
    TypedArrayReceiver typedArrayReceiver(CellPointer);
    void turnAwayNil(const TypedArrayReceiver&);
    Word32 hasLengthOfItsOwn(const TypedArrayReceiver&);
    Pointer findTypedArrayElement(const TypedArrayReceiver&, Integer index, LBasicBlock otherwise, LBasicBlock outside);
    Operand getTypedArrayElement(const TypedArrayReceiver&, ValueID key, TypedArrayType, const Type&, LBasicBlock otherwise = nullptr);
    void putTypedArrayElement(const TypedArrayReceiver&, ValueID key, TypedArrayType, ValueID stored, LBasicBlock otherwise = nullptr);
    Operand lowerGetByVal(ValueID result, const Value&);
    void lowerPutByVal(const Value&);
    Word32 isArrayStandingForItsIterator(ValueID);
    Operand lowerIteratorOpen(ValueID result, const Value&);
    Word32 lowerIteratorHasNext(const Value&);
    Operand lowerIteratorValue(ValueID result, const Value&);
    void lowerIteratorClose(const Value&);

    // Strings, maps and sets. See AOTTypedLoweringStrings.cpp.
    struct StringReceiver {
        // What is loaded from, with these LoadFlags.
        CellPointer string;
        unsigned flags { 0 };
        // Of a value that may be `undefined` or `null`: whether it is neither. If it is one of them, `string` is the empty string.
        Word32 isString;
        Boxed value;
    };
    struct Characters {
        // Where the first one is.
        Pointer data;
        // Of the StringImpl that has them: whether they are 8-bit. Whether it is an atom only shows for a string that is all of it.
        Word32 flags;
        // They are there for as long as the collector finds this.
        CellPointer owner;
    };
    // A string that another is made of. Of the flags only how wide the characters are is looked at.
    struct StringPart {
        CellPointer string;
        Integer length;
        Word32 flags;
        bool mayBeEmpty { true };
    };
    StringReceiver stringReceiver(ValueID);
    StringReceiver stringReceiver(CellPointer);
    void turnAwayNil(const StringReceiver&, StringID name);
    Pointer fiberOf(const StringReceiver&);
    Integer stringLength(const StringReceiver&);
    Characters charactersOf(const StringReceiver&);
    Word32 codeUnitAt(const Characters&, Integer index);
    Word32 codePointAt(const Characters&, Integer index, Integer length);
    CellPointer stringOfCodeUnit(Word32);
    Operand getStringElement(const StringReceiver&, ValueID key, const Type&, LBasicBlock otherwise = nullptr);
    Integer positionOf(ValueID);
    Integer placeInString(ValueID, Integer length, bool isRelativeToEnd);
    CellPointer allocateRope(LBasicBlock slowPath);
    CellPointer partOfString(const StringReceiver&, Integer length, Integer from, Integer to);
    // The text, if the value is defined as a string of the program that has ASCII characters only.
    std::optional<std::span<const uint8_t>> constantASCIIText(ValueID) const;
    StringPart stringPartOf(ValueID);
    StringPart stringPartOf(CellPointer);
    StringPart concatenate(const StringPart&, const StringPart&, const StringPart* third = nullptr);
    CellPointer lowerStrcat(const Value&);
    CellPointer integerToString(Integer, const Type&);
    Word32 hasText(Pointer data, std::span<const uint8_t> text);
    Word32 hasTextAt(const StringReceiver&, Integer place, ValueID constant, std::span<const uint8_t> text);
    Word32 isStringWithText(ValueID, ValueID constant, std::span<const uint8_t> text);
    Word32 areStringsEqual(ValueID, ValueID);
    Operand lowerStringBuiltin(InlineBuiltin, ValueID result, const Value&);
    Boxed mapKey(ValueID);
    Word32 hashOfMapKey(Boxed);
    Pointer findInMapOrSet(CellPointer table, bool isMap, Boxed key, Word32 hash, LBasicBlock absent);
    Integer sizeOfMapOrSet(CellPointer, bool isMap);
    Operand lowerMapOrSetBuiltin(InlineBuiltin, ValueID result, const Value&, Writes inPlace);

    // The table.
    CellPointer builtinCell(unsigned);
    Boxed callFunctionOfEngine(const Value&, OperationEffects);
    Operand callMethodOfArray(const Value&, const Type&);
    Operand lowerCallBuiltin(ValueID result, const Value&);
    std::optional<Operand> lowerInline(InlineBuiltin, ValueID result, const Value&);
    Operand lowerContainerBuiltin(InlineBuiltin, ValueID result, const Value&, Writes inPlace);
    // What a call or a read by name comes down to if its operands are what is expected of them.
    struct ExpectedBuiltin {
        // As resolveBuiltins() would have it.
        Value call;
        // What is expected of the receiver. Never: there is none, or its type says what it is.
        Type receiver;
        Vector<Type, 4> arguments;
    };
    BuiltinName globalDefinedBy(ValueID) const;
    std::optional<ExpectedBuiltin> expectedBuiltin(BuiltinKind, ValueID receiver, BuiltinName, const Arguments&) const;
    bool chooseArgumentsToExpect(ExpectedBuiltin&) const;
    template<typename Generally> Operand lowerExpectedBuiltin(ValueID result, const ExpectedBuiltin&, std::optional<Boxed> callee, const Generally&);

    const Program& m_program;
    const Function& m_function;
    FunctionID m_id;
    bool m_isEntry;
    LoweringContext& m_context;
    const Builtins& m_builtins;
    Vector<Operand> m_values;
    Vector<Assumption, 4> m_assumptions;
    Vector<LBasicBlock> m_blocks;
    Vector<Vector<LValue, 4>> m_phis;
    // For an integer that was made from a double: that double.
    UncheckedKeyHashMap<LValue, LValue> m_asDouble;
    // For what is a cell from where it is defined, so that a load from it cannot trap wherever it is moved to: its type.
    UncheckedKeyHashMap<LValue, TypeID> m_typeOfCell;
    // What was defined as a boxed value and is a cell only behind the test that showed it.
    UncheckedKeyHashSet<LValue> m_takenForCells;
    StringID m_length;
    B3::Variable* m_data { nullptr };
    BitVector m_loopBlocks;
    Vector<Handler> m_catchEntrypoints;
    UncheckedKeyHashMap<uint32_t, unsigned, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_catchEntrypointForBlock;
    UncheckedKeyHashMap<uint32_t, CatchSpill, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_catchSpills;
    unsigned m_catchSpillSlotCount { 0 };
    LValue m_catchSpillArea { nullptr };
    std::optional<unsigned> m_currentCatchEntrypoint;
    // By layout.
    mutable UncheckedKeyHashMap<uint32_t, uint32_t, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_leastSizes;
    mutable UncheckedKeyHashMap<uint32_t, bool, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_viewsThatMayBeFrozen;
    // See findStoresToVariables(). A field goes by the number of its layout and its own.
    bool m_didFindStoresToVariables { false };
    BitVector m_variablesStoredTo;
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_fieldsStoredTo;
    // What is being stored to was made by this run of the function: to whoever calls it, such a store is none.
    bool m_isStoringToLocalAllocation { false };
    // While a value is fitted that something is made of where it does not fit (`as`, `!`): an object fits a view only if it is of a member,
    // also where the view admits foreign objects.
    bool m_foreignObjectsDoNotFit { false };
    // validateFact() asks what holds of a value, which for an array is not what it takes to be taken for one: see EngineClass::type.
    bool m_isValidatingFact { false };
    // Where the value that is being lowered is.
    const Block* m_currentBlock { nullptr };
    size_t m_currentIndexInBlock { 0 };
    // Of a general entry that takes a list.
    Word32 m_argumentCount;
    Pointer m_argumentBuffer;
    bool m_isInLoop { false };
    uint32_t m_nextSite { 1 };
    ASCIILiteral m_whyFailed;
    uint8_t m_genericEntrypointParameterCount { 0 };
};

template<typename Functor>
void Lowering::require(Word32 condition, const Functor& otherwise)
{
    LBasicBlock bad = newColdBlock();
    LBasicBlock good = m_out.newBlock();
    m_out.branch(condition.value(), usually(good), rarely(bad));
    m_out.appendTo(bad);
    otherwise();
    m_out.unreachable();
    m_out.appendTo(good);
}

template<typename Functor>
void Lowering::inColdBlock(LBasicBlock block, const Functor& functor)
{
    m_out.appendTo(block);
    functor();
}

template<typename T, typename Base>
T Lowering::loadField(Base base, const B3::AbstractHeap& field, unsigned flags)
{
    if constexpr (std::is_same_v<T, Word32>)
        return Word32(marked(m_out.load32(base.value(), field), flags));
    else {
        static_assert(!std::is_same_v<T, Number> && !std::is_same_v<T, OptionalNumber>);
        return T(marked(m_out.load64(base.value(), field), flags));
    }
}

template<typename T, typename Base>
void Lowering::storeField(Base base, const B3::AbstractHeap& field, T value)
{
    if constexpr (std::is_same_v<T, Word32>)
        m_out.store32(value.value(), base.value(), field);
    else {
        static_assert(!std::is_same_v<T, Number> && !std::is_same_v<T, OptionalNumber>);
        m_out.store64(value.value(), base.value(), field);
    }
}

template<typename Usual, typename Rare>
void Lowering::ifElse(Word32 condition, const Usual& usual, const Rare& rare)
{
    LBasicBlock yes = m_out.newBlock();
    LBasicBlock no = newColdBlock();
    LBasicBlock join = m_out.newBlock();
    m_out.branch(condition.value(), usually(yes), rarely(no));
    m_out.appendTo(yes);
    usual();
    m_out.jump(join);
    m_out.appendTo(no);
    rare();
    m_out.jump(join);
    m_out.appendTo(join);
}

template<typename Fast, typename Slow>
Operand Lowering::eitherOperand(Word32 condition, Rep rep, const Fast& fast, const Slow& slow)
{
    LBasicBlock yes = m_out.newBlock();
    LBasicBlock no = newColdBlock();
    LBasicBlock join = m_out.newBlock();
    m_out.branch(condition.value(), usually(yes), rarely(no));
    m_out.appendTo(yes);
    Operand fastResult = fast();
    RELEASE_ASSERT(fastResult.rep == rep);
    ValueFromBlock fromFast = m_out.anchor(fastResult.value);
    m_out.jump(join);
    m_out.appendTo(no);
    Operand slowResult = slow();
    RELEASE_ASSERT(slowResult.rep == rep);
    ValueFromBlock fromSlow = m_out.anchor(slowResult.value);
    m_out.jump(join);
    m_out.appendTo(join);
    return Operand(rep, m_out.phi(b3TypeOf(rep), fromFast, fromSlow));
}

template<typename Passes, typename Fails>
Operand Lowering::byShape(CellPointer cell, const ShapeTest& test, Rep rep, const Passes& ifItPasses, const Fails& ifItFails)
{
    if (test.passesNothing())
        return ifItFails();
    LBasicBlock passing = m_out.newBlock();
    LBasicBlock failing = m_out.newBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    m_out.branch(passes(shapeWordOf(cell), test, failing).value(), unsure(passing), unsure(failing));
    m_out.appendTo(passing);
    join.add(ifItPasses());
    m_out.appendTo(failing);
    join.add(ifItFails());
    return join.finish();
}

template<typename Has, typename Lacks>
Operand Lowering::throughView(CellPointer cell, ValueID object, LayoutID view, const Field& field, Rep rep, const Has& has, const Lacks& lacks)
{
    // The members that have the field are members.
    if (field.isLackedBySomeMember)
        return byShape(cell, field.membersWithIt, rep, has, lacks);
    return ifOfMember(cell, object, view, rep, has, lacks);
}

template<typename InPlace, typename Generally>
Operand Lowering::ifOfMember(CellPointer cell, ValueID object, LayoutID view, Rep rep, const InPlace& inPlace, const Generally& generally)
{
    if (!m_program.at(view).admitsForeignObjects || isAllocatedAsMemberOf(object, view))
        return inPlace();
    return byShape(cell, m_program.at(view).test, rep, inPlace, generally);
}

template<typename T, typename Fast, typename Slow>
T Lowering::either(Word32 condition, const Fast& fast, const Slow& slow)
{
    static_assert(std::is_same_v<decltype(fast()), T> && std::is_same_v<decltype(slow()), T>);
    return eitherOperand(condition, T::rep, [&]() -> Operand {
        return fast();
    }, [&]() -> Operand {
        return slow();
    }).template as<T>();
}

template<typename Fast, typename Slow>
Pointer Lowering::eitherPointer(Word32 condition, const Fast& fast, const Slow& slow)
{
    static_assert(std::is_same_v<decltype(fast()), Pointer> && std::is_same_v<decltype(slow()), Pointer>);
    return Pointer(eitherOperand(condition, Rep::Int64, [&] {
        return Operand(Rep::Int64, fast().value());
    }, [&] {
        return Operand(Rep::Int64, slow().value());
    }).value);
}

template<typename InPlace, typename Generally>
Operand Lowering::ifAsExpected(const Vector<Expectation, 4>& expectations, Rep rep, const InPlace& inPlace, const Generally& generally)
{
    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    size_t numberOfAssumptions = m_assumptions.size();
    for (auto& expectation : expectations) {
        // What is known of it says as much.
        if (!expectation.isAsMade && typeOf(expectation.value).isWithin(*expectation.type, m_program))
            continue;
        m_assumptions.append(Assumption { expectation.value, expectOrElse(expectation, otherwise), expectation.type });
    }
    join.add(inPlace(otherwise));
    m_assumptions.shrink(numberOfAssumptions);
    m_out.appendTo(otherwise);
    join.add(generally());
    return join.finish();
}

template<typename OnInt32, typename OnNumbers, typename Generally>
Operand Lowering::ifNumbersAsExpected(std::initializer_list<ValueID> operands, Rep rep, bool hasCodeForInt32, const OnInt32& onInt32, const OnNumbers& onNumbers, const Generally& generally)
{
    auto& int32Type = m_program.at(m_program.types.int32);
    auto& numberType = m_program.at(m_program.types.number);
    // An operand that is a number by its type is kept as an integer or as a double, and is not looked at again.
    bool mayAllBeInt32 = hasCodeForInt32;
    bool mayBeOtherNumbers = false;
    for (ValueID id : operands) {
        auto prediction = predictionOf(id);
        const Type& expected = prediction ? *prediction : typeOf(id);
        bool isInt32 = expected.isInteger() && expected.integers.isWithin(Range::int32());
        mayAllBeInt32 &= isInt32 || (prediction && expected.primitives & Primitive::Int);
        mayBeOtherNumbers |= !isInt32;
    }
    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    size_t numberOfAssumptions = m_assumptions.size();
    auto expect = [&](const Type& type, LBasicBlock whereNot) {
        for (ValueID id : operands) {
            if (predictionOf(id))
                m_assumptions.append(Assumption { id, expectOrElse(Expectation { id, &type }, whereNot), &type });
        }
    };
    if (mayAllBeInt32) {
        LBasicBlock next = mayBeOtherNumbers ? m_out.newBlock() : otherwise;
        expect(int32Type, next);
        join.add(onInt32(next));
        m_assumptions.shrink(numberOfAssumptions);
        if (mayBeOtherNumbers)
            m_out.appendTo(next);
    }
    if (!mayAllBeInt32 || mayBeOtherNumbers) {
        expect(numberType, otherwise);
        join.add(onNumbers());
        m_assumptions.shrink(numberOfAssumptions);
    }
    m_out.appendTo(otherwise);
    join.add(generally());
    return join.finish();
}

template<typename Generally>
Operand Lowering::lowerExpectedBuiltin(ValueID result, const ExpectedBuiltin& expected, std::optional<Boxed> callee, const Generally& generally)
{
    ValueID receiver = expected.receiver.isNever() ? ValueID() : expected.call.child(0);
    InlineBuiltin which = m_builtins.at(expected.call.builtin()).implementation.inlined;
    Vector<Expectation, 4> expectations;
    // What has been read from the receiver under the name is looked at itself. Otherwise the receiver has to be one that finds what its class
    // has. Its length every array has itself, and keeps.
    if (receiver)
        expectations.append(Expectation { receiver, &expected.receiver, !callee && which != InlineBuiltin::ArrayLength });
    for (unsigned i = 0; i < expected.arguments.size(); ++i)
        expectations.append(Expectation { expected.call.arguments[i], &expected.arguments[i] });
    return ifAsExpected(expectations, repOf(typeOf(result)), [&](LBasicBlock otherwise) {
        if (receiver && callee)
            orElse(equal(*callee, box(builtinCell(m_builtins.cellOf(expected.call.builtin())))), otherwise);
        return *lowerInline(which, result, expected.call);
    }, generally);
}

// The arguments have to be in the representations the declaration of the operation calls for, which also says whether it may throw.
template<auto operation, typename... Arguments>
auto Lowering::callOperation(Entry entry, OperationEffects effects, Arguments... arguments)
{
    using Signature = OperationSignature<std::remove_pointer_t<decltype(operation)>>;
    using Result = typename Signature::ResultType;
    Vector<LValue, 8> list = Signature::list(arguments...);
    if constexpr (std::is_void_v<Result>)
        callEntry(entry, B3::Void, Signature::mayThrow, effects, list);
    else if constexpr (std::is_same_v<Result, Word32>)
        return Word32(m_out.castToInt32(callEntry(entry, B3::Int64, Signature::mayThrow, effects, list)));
    else if constexpr (std::is_same_v<Result, Number>)
        return Number(callEntry(entry, B3::Double, Signature::mayThrow, effects, list));
    else
        return Result(callEntry(entry, B3::Int64, Signature::mayThrow, effects, list));
}

// An entry of a table of the instance, which is filled the first time it is asked for.
template<typename T, typename Functor>
T Lowering::lazilyInitialized(ptrdiff_t table, uint32_t index, const Functor& fill)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    // What fills it writes the state of the run time: a load that was moved out of a loop would find it empty every time around.
    LValue value = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(table), static_cast<ptrdiff_t>(index) * sizeof(void*)));
    ValueFromBlock fast = m_out.anchor(value);
    m_out.branch(m_out.notZero64(value), usually(continuation), rarely(slowPath));
    m_out.appendTo(slowPath);
    T filled = fill();
    ValueFromBlock slow = m_out.anchor(filled.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return T(m_out.phi(B3::Int64, fast, slow));
}

template<auto operation>
constexpr OperationEffects effectsOfOperation()
{
    return { WritesOf<operation>::value, OperationSignature<std::remove_pointer_t<decltype(operation)>>::mayThrow };
}

#define AOT_TYPED_OPERATION(name, ...) callOperation<operationAOTTyped ## name>(Entry::operationAOTTyped ## name, effectsOfOperation<operationAOTTyped ## name>() __VA_OPT__(,) __VA_ARGS__)
// The operation does what an entry of the table describes, which says more than the declaration of the operation can.
#define AOT_TYPED_OPERATION_WITH_EFFECTS(name, effects, ...) callOperation<operationAOTTyped ## name>(Entry::operationAOTTyped ## name, effects __VA_OPT__(,) __VA_ARGS__)

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
